#include "CTRPluginFrameworkImpl/Menu/KeyboardImpl.hpp"
#include "CTRPluginFrameworkImpl/System/ProcessImpl.hpp"
#include "CTRPluginFrameworkImpl/Preferences.hpp"
#include "CTRPluginFramework/Menu/Keyboard.hpp"

#include <cmath>
#include "3ds.h"
#include "CTRPluginFramework/Utils/Utils.hpp"
#include "CTRPluginFrameworkImpl/Menu/PluginMenuImpl.hpp"
#include "CTRPluginFramework/Graphics/CustomIcon.hpp"
#include "CTRPluginFramework/System/File.hpp"
#include "CTRPluginFramework/System/Controller.hpp"
#include "CTRPluginFramework/System/Touch.hpp"

#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace CTRPluginFramework
{
    #define USER_VALID  0
    #define USER_ABORT  -1
    #define SLEEP_ABORT -2
    // [JP] 追加キー (Keyboard::SpecialCodePoints と重ならない値)
    #define KEY_KANA        (-0x4A501) //<- 英字 ⇔ かな
    #define KEY_KANA_TYPE   (-0x4A502) //<- ひらがな ⇔ カタカナ
    #define KEY_DAKUTEN     (-0x4A503) //<- ゛゜小 の切り替え
    #define KEY_KANA_PAGE   (-0x4A504) //<- かなページ送り
    #define KEY_COLOR       (-0x4A505) //<- 文字色の挿入

    // [JP] QWERTY キーボードのページ (_QwertyKeys に この順で並ぶ)
    enum QwertyPage
    {
        PG_LOW, PG_UP, PG_SYM1, PG_SYM2, PG_NIN1, PG_NIN2,
        PG_HIRA1, PG_HIRA2, PG_KATA1, PG_KATA2, PG_COUNT
    };
    static int  s_pageStart[PG_COUNT + 1] = { 0 };

    // [JP] 入力エリアとキーボードの間に予測変換バーを置くため、QWERTY のキーを下にずらす
    #define QWERTY_OFFSET_Y 20
    #define SUGGEST_BAR_Y   36      //<- 予測変換バーの位置 (入力エリアのすぐ下)
    #define SUGGEST_BAR_H   20

    // ---------------------------------------------------------------
    //  [JP] 日本語入力の補助関数
    // ---------------------------------------------------------------
    namespace JpInput
    {
        // SKK辞書 (PC用スクリプト skk2ctrpf.py で変換したもの)
        static const char *DictPath = "/luma/plugins/skk_jisyo.dic";

        static void AppendUtf8(std::string &out, u32 code)
        {
            u8 buf[4];
            ssize_t n = encode_utf8(buf, code);
            if (n > 0) out.append(reinterpret_cast<char *>(buf), n);
        }

        // toKata = true: ひらがな → カタカナ / false: カタカナ → ひらがな
        static std::string ShiftKana(const std::string &str, bool toKata)
        {
            std::string out;
            const u8 *p = reinterpret_cast<const u8 *>(str.c_str());
            while (*p)
            {
                u32 code = 0;
                ssize_t n = decode_utf8(&code, p);
                if (n <= 0) break;
                if (toKata && code >= 0x3041 && code <= 0x3096) code += 0x60;
                else if (!toKata && code >= 0x30A1 && code <= 0x30F6) code -= 0x60;
                AppendUtf8(out, code);
                p += n;
            }
            return out;
        }

        static std::string ColorEsc(u32 r, u32 g, u32 b)
        {
            // 0 は文字列の終端になってしまうので 1 にする
            std::string esc(4, '\0');
            esc[0] = 0x1B;
            esc[1] = static_cast<char>((r & 0xFF) ? (r & 0xFF) : 1);
            esc[2] = static_cast<char>((g & 0xFF) ? (g & 0xFF) : 1);
            esc[3] = static_cast<char>((b & 0xFF) ? (b & 0xFF) : 1);
            return esc;
        }

        // 辞書の形式:
        //   "SKKU" + u32 件数 + u32 オフセット[件数] + "よみ\t候補1/候補2/...\n" の行 (よみのバイト順)
        //
        // よみが key と一致する候補を exact に、key で始まる長いよみの候補を predict に入れる (予測変換)
        static bool Predict(const std::string &key, std::vector<std::string> &exact,
                            std::vector<std::string> &predict, u32 maxCands = 30, u32 maxLines = 24)
        {
            File file;
            if (File::Open(file, DictPath, File::READ) != 0)
                return false;

            char    header[8];
            u32     count = 0;
            if (file.Read(header, 8) != 0 || std::memcmp(header, "SKKU", 4) != 0)
            {
                file.Close();
                return false;
            }
            std::memcpy(&count, header + 4, 4);

            auto readOffset = [&](u32 index, u32 &offset) -> bool
            {
                return file.Seek(8 + index * 4, File::SET) == 0 && file.Read(&offset, 4) == 0;
            };
            auto readKey = [&](u32 offset, std::string &k) -> bool
            {
                char buf[128];
                if (file.Seek(offset, File::SET) != 0 || file.Read(buf, sizeof(buf)) != 0)
                    return false;
                k.clear();
                for (u32 i = 0; i < sizeof(buf) && buf[i] != '\t' && buf[i] != '\n'; i++)
                    k += buf[i];
                return true;
            };
            auto readLine = [&](u32 offset, std::string &line) -> bool
            {
                char buf[128];
                line.clear();
                while (line.size() < 4096)
                {
                    if (file.Seek(offset, File::SET) != 0 || file.Read(buf, sizeof(buf)) != 0)
                        return false;
                    u32 i = 0;
                    for (; i < sizeof(buf) && buf[i] != '\n' && buf[i] != '\0'; i++)
                        line += buf[i];
                    if (i < sizeof(buf)) break;
                    offset += sizeof(buf);
                }
                return true;
            };
            auto addUnique = [](std::vector<std::string> &v, const std::string &str)
            {
                for (const std::string &c : v)
                    if (c == str) return;
                v.push_back(str);
            };

            // key 以上の最初のよみを二分探索
            u32 lo = 0, hi = count;
            std::string k;
            while (lo < hi)
            {
                const u32 mid = (lo + hi) / 2;
                u32 offset = 0;
                if (!readOffset(mid, offset) || !readKey(offset, k))
                {
                    file.Close();
                    return false;
                }
                if (k.compare(key) < 0) lo = mid + 1;
                else                    hi = mid;
            }

            // key で始まるよみを順に読む
            for (u32 i = lo, lines = 0; i < count && lines < maxLines
                 && exact.size() + predict.size() < maxCands; i++, lines++)
            {
                u32 offset = 0;
                std::string line;
                if (!readOffset(i, offset) || !readLine(offset, line))
                    break;
                const size_t tab = line.find('\t');
                if (tab == std::string::npos)
                    break;
                const std::string reading = line.substr(0, tab);
                if (reading.compare(0, key.size(), key) != 0)
                    break;

                std::vector<std::string> &dst = (reading == key) ? exact : predict;
                size_t start = tab + 1;
                while (start < line.size() && exact.size() + predict.size() < maxCands)
                {
                    size_t slash = line.find('/', start);
                    if (slash == std::string::npos) slash = line.size();
                    if (slash > start)
                        addUnique(dst, line.substr(start, slash - start));
                    start = slash + 1;
                }
            }
            file.Close();
            return true;
        }

        // ゛゜小 の切り替えグループ (ひらがな。カタカナは変換して扱う)
        static const char16_t *DakutenGroups[] =
        {
            u"かが", u"きぎ", u"くぐ", u"けげ", u"こご",
            u"さざ", u"しじ", u"すず", u"せぜ", u"そぞ",
            u"ただ", u"ちぢ", u"つっづ", u"てで", u"とど",
            u"はばぱ", u"ひびぴ", u"ふぶぷ", u"へべぺ", u"ほぼぽ",
            u"あぁ", u"いぃ", u"うぅゔ", u"えぇ", u"おぉ",
            u"やゃ", u"ゆゅ", u"よょ", u"わゎ",
        };

        static u32 NextDakuten(u32 code)
        {
            bool kata = (code >= 0x30A1 && code <= 0x30F6);
            u32  hira = kata ? code - 0x60 : code;
            for (const char16_t *group : DakutenGroups)
            {
                for (int i = 0; group[i]; i++)
                {
                    if (group[i] != hira) continue;
                    u32 next = group[i + 1] ? group[i + 1] : group[0];
                    return kata ? next + 0x60 : next;
                }
            }
            return 0;
        }

        // -----------------------------------------------------------
        //  カラーピッカー (プラグインの影の色などと同じ操作)
        //  下画面: 色相バー + R/G/B バー (十字キー / タッチ)
        //  上画面: 変更前 / 変更後 のプレビュー
        // -----------------------------------------------------------
        static Color ToColor(u32 rgb)
        {
            return Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }

        static u32 HueToRGB(int h) // h: 0-255 (彩度・明度最大)
        {
            const int region = h * 6 / 256;
            const u32 up   = (h * 6) % 256;
            const u32 down = 255 - up;
            switch (region)
            {
                case 0:  return 0xFF0000 | (up << 8);
                case 1:  return (down << 16) | 0x00FF00;
                case 2:  return 0x00FF00 | up;
                case 3:  return (down << 8) | 0x0000FF;
                case 4:  return (up << 16) | 0x0000FF;
                default: return 0xFF0000 | down;
            }
        }

        static int RGBToHue(u32 rgb) // 0-255、無彩色なら -1
        {
            const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
            const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            const int d  = mx - mn;
            if (d == 0) return -1;
            int h6;
            if (mx == r)      { h6 = 256 * (g - b) / d; if (h6 < 0) h6 += 1536; }
            else if (mx == g)   h6 = 256 * (b - r) / d + 512;
            else                h6 = 256 * (r - g) / d + 1024;
            const int h = h6 / 6;
            return h > 255 ? 255 : h;
        }

        // rgb は 0xRRGGBB。決定で true
        static bool PickColor(const std::string &title, u32 &rgb)
        {
            const u32   before = rgb & 0xFFFFFF;
            int         val[4];                 // 0:色相 1:R 2:G 3:B
            val[1] = (before >> 16) & 0xFF;
            val[2] = (before >> 8) & 0xFF;
            val[3] = before & 0xFF;
            val[0] = RGBToHue(before);
            if (val[0] < 0) val[0] = 0;

            const char  *rowName[4] = { "H", "R", "G", "B" };
            const int   rowY[4] = { 34, 74, 114, 154 };
            const int   BX = 38, BW = 256, BH = 20;
            IntRect okButton(30, 190, 120, 26);
            IntRect ngButton(170, 190, 120, 26);

            const Color white(255, 255, 255), gray(128, 128, 128), yellow(255, 255, 0), black(0, 0, 0);
            const Color &textColor = Preferences::Settings.MainTextColor;

            auto setRow = [&](int i, int v)
            {
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                val[i] = v;
                if (i == 0)
                {
                    const u32 c = HueToRGB(v);
                    val[1] = (c >> 16) & 0xFF; val[2] = (c >> 8) & 0xFF; val[3] = c & 0xFF;
                }
                else
                {
                    const int h = RGBToHue((val[1] << 16) | (val[2] << 8) | val[3]);
                    if (h >= 0) val[0] = h;
                }
            };

            Event           event;
            EventManager    manager(EventManager::EventGroups::GROUP_KEYS | EventManager::EventGroups::GROUP_TOUCH);
            int             row = 1, hold = 0, result = -1;
            bool            waitTouchRelease = Touch::IsDown(); // 色キーを押した指が離れるまで待つ

            while (result < 0)
            {
                // ---- 入力 ----
                while (manager.PollEvent(event))
                {
                    if (event.type != Event::KeyPressed)
                        continue;
                    if (event.key.code == Key::A)            result = 1;
                    else if (event.key.code == Key::B)       result = 0;
                    else if (event.key.code == Key::DPadUp)  row = (row + 3) % 4;
                    else if (event.key.code == Key::DPadDown) row = (row + 1) % 4;
                }

                int dir = 0;
                if (Controller::IsKeyDown(Key::DPadRight))     dir = 1;
                else if (Controller::IsKeyDown(Key::DPadLeft)) dir = -1;
                if (dir)
                {
                    int step = 0;
                    if (hold == 0)      step = 1;   // 1回押し
                    else if (hold > 45) step = 4;   // 長押しで加速
                    else if (hold > 15) step = 1;   // 長押しで連続
                    hold++;
                    if (step) setRow(row, val[row] + dir * step);
                }
                else
                    hold = 0;

                const bool touchDown = Touch::IsDown();
                if (waitTouchRelease && !touchDown)
                    waitTouchRelease = false;
                if (!waitTouchRelease && touchDown)
                {
                    IntVector p(Touch::GetPosition());
                    for (int i = 0; i < 4; i++)
                    {
                        if (p.y >= rowY[i] - 6 && p.y < rowY[i] + BH + 6 && p.x >= BX - 8 && p.x < BX + BW + 8)
                        {
                            row = i;
                            setRow(i, p.x - BX);
                        }
                    }
                    if (okButton.Contains(p.x, p.y)) result = 1;
                    if (ngButton.Contains(p.x, p.y)) result = 0;
                }

                const u32 cur = (val[1] << 16) | (val[2] << 8) | val[3];
                if (result == 1)
                    rgb = cur;

                // ---- 上画面 ----
                Renderer::SetTarget(TOP);
                Window::TopWindow.Draw();

                int y = 26;
                Renderer::DrawSysString(title.c_str(), 36, y, 364, textColor);

                y = 50; Renderer::DrawSysString("変更前", 60, y, 200, gray);
                y = 50; Renderer::DrawSysString("変更後", 220, y, 364, textColor);
                Renderer::DrawRect(IntRect(58, 68, 124, 84), gray, false);
                Renderer::DrawRect(IntRect(60, 70, 120, 80), ToColor(before));
                Renderer::DrawRect(IntRect(218, 68, 124, 84), white, false);
                Renderer::DrawRect(IntRect(220, 70, 120, 80), ToColor(cur));

                char buf[96];
                std::snprintf(buf, sizeof(buf), "#%06lX", static_cast<unsigned long>(before));
                y = 156; Renderer::DrawSysString(buf, 90, y, 200, gray);
                std::snprintf(buf, sizeof(buf), "#%06lX", static_cast<unsigned long>(cur));
                y = 156; Renderer::DrawSysString(buf, 250, y, 364, textColor);
                std::snprintf(buf, sizeof(buf), "R %3d   G %3d   B %3d", val[1], val[2], val[3]);
                y = 176; Renderer::DrawSysString(buf, 130, y, 364, textColor);
                y = 198; Renderer::DrawSysString(":項目  :調整  :決定  :キャンセル", 36, y, 364, gray);

                // ---- 下画面 ----
                Renderer::SetTarget(BOTTOM);
                Renderer::DrawRect(IntRect(20, 20, 280, 200), Preferences::Settings.Keyboard.Background);

                for (int i = 0; i < 4; i++)
                {
                    const int ry = rowY[i];
                    int ty = ry + 2;
                    Renderer::DrawSysString(rowName[i], 24, ty, 36, i == row ? yellow : white);

                    for (int x = 0; x < BW; x++)
                    {
                        u32 c;
                        if (i == 0)      c = HueToRGB(x);
                        else if (i == 1) c = (x << 16) | (val[2] << 8) | val[3];
                        else if (i == 2) c = (val[1] << 16) | (x << 8) | val[3];
                        else             c = (val[1] << 16) | (val[2] << 8) | x;
                        Renderer::DrawRect(IntRect(BX + x, ry, 1, BH), ToColor(c));
                    }
                    Renderer::DrawRect(IntRect(BX - 2, ry - 2, BW + 4, BH + 4), i == row ? yellow : gray, false);

                    // つまみ
                    const int mx = BX + val[i];
                    Renderer::DrawRect(IntRect(mx - 2, ry - 5, 5, BH + 10), white);
                    Renderer::DrawRect(IntRect(mx - 1, ry - 4, 3, BH + 8), black);
                }

                Renderer::DrawRect(okButton, Color(0, 110, 40));
                y = okButton.leftTop.y + 5;
                Renderer::DrawSysString(" 決定", okButton.leftTop.x + 34, y, okButton.leftTop.x + 120, white);
                Renderer::DrawRect(ngButton, Color(130, 20, 20));
                y = ngButton.leftTop.y + 5;
                Renderer::DrawSysString(" キャンセル", ngButton.leftTop.x + 18, y, ngButton.leftTop.x + 120, white);

                Renderer::EndFrame();
            }

            SoundEngine::PlayMenuSound(result == 1 ? SoundEngine::Event::ACCEPT : SoundEngine::Event::CANCEL);
            return result == 1;
        }
    }

    std::vector<TouchKey>    KeyboardImpl::_DecimalKeys;
    std::vector<TouchKey>    KeyboardImpl::_HexaDecimalKeys;
    std::vector<TouchKey>    KeyboardImpl::_QwertyKeys;
    Clock                    KeyboardImpl::_inputClock;

    KeyboardImpl::KeyboardImpl(const std::string &text)
    {
        _owner = nullptr;

        _text = text;
        _error = "";
        _userInput = "";
        _canChangeLayout = false;
        _canAbort = true;
        _isOpen = false;
        _userAbort = false;
        _mustRelease = false;
        _useCaps = false;
        _useSymbols = false;
        _useNintendo = false;
        _errorMessage = false;
        _askForExit = false;
        _isHex = false;
        _offset = 0.f;
        _max = 0;
        _layout = HEXADECIMAL;
        _cursorPositionInString = 0;
        _cursorPositionOnScreen = 0;
        _showCursor = true;
        _keys = nullptr;
        _convert = nullptr;
        _compare = nullptr;
        _onKeyboardEvent = nullptr;

        _customKeyboard = false;
        _displayScrollbar = false;
        _currentPosition = 0;
        _scrollbarSize = 0;
        _scrollCursorSize = 10;
        _scrollPadding = 0.f;
        _scrollJump = 0.f;
        _scrollSize = 0.f;
        _scrollPosition = 0.f;
        _inertialVelocity = 0.f;
        _scrollStart = 0.f;
        _scrollEnd = 0.f;

        _symbolsPage = 0;
        _nintendoPage = 0;

        DisplayTopScreen = true;
    }

    KeyboardImpl::KeyboardImpl(Keyboard* kb, const std::string &text)
    {
        _owner = kb;

        _text = text;
        _error = "";
        _userInput = "";
        _canChangeLayout = false;
        _canAbort = true;
        _isOpen = false;
        _userAbort = false;
        _mustRelease = false;
        _useCaps = false;
        _useSymbols = false;
        _useNintendo = false;
        _errorMessage = false;
        _askForExit = false;
        _isHex = false;
        _offset = 0.f;
        _max = 0;
        _layout = HEXADECIMAL;
        _cursorPositionInString = 0;
        _cursorPositionOnScreen = 0;
        _showCursor = true;

        _convert = nullptr;
        _compare = nullptr;
        _onKeyboardEvent = nullptr;
        _keys = nullptr;

        _customKeyboard = false;
        _displayScrollbar = false;
        _currentPosition = 0;
        _scrollbarSize = 0;
        _scrollCursorSize = 10;
        _scrollPadding = 0.f;
        _scrollJump = 0.f;
        _scrollSize = 0.f;
        _scrollPosition = 0.f;
        _inertialVelocity = 0.f;
        _scrollStart = 0.f;
        _scrollEnd = 0.f;

        _symbolsPage = 0;
        _nintendoPage = 0;

        DisplayTopScreen = true;
    }

    KeyboardImpl::~KeyboardImpl(void)
    {
      //  for (KeyIter iter = _keys.begin(); iter != _keys.end(); ++iter)
      //      iter->Clear();
        for (TouchKeyString *tks : _strKeys)
            delete tks;
    }

    void    KeyboardImpl::SetLayout(Layout layout, bool fraction)
    {
        _canChangeLayout = false; ///< Reset state
        _layout = layout;
        _isHex = false;
        _decimalFraction = fraction;
        _userInput.clear();
        if (_layout == QWERTY) _Qwerty();
        else if (_layout == DECIMAL) _Decimal();
        else if (_layout == HEXADECIMAL)
        {
            _isHex = true;
            _Hexadecimal();
        }
    }

    void    KeyboardImpl::SetHexadecimal(bool isHex)
    {
        _isHex = isHex;
    }

    bool    KeyboardImpl::IsHexadecimal(void) const
    {
        return (_isHex);
    }

    void    KeyboardImpl::SetMaxInput(u32 max)
    {
        _max = max;
    }

    void    KeyboardImpl::CanAbort(bool canAbort)
    {
        _canAbort = canAbort;
    }

    std::string &KeyboardImpl::GetInput(void)
    {
        return (_userInput);
    }

    std::string &KeyboardImpl::GetMessage(void)
    {
        return (_text);
    }

    std::string& KeyboardImpl::GetTitle(void)
    {
        return _title;
    }

    void    KeyboardImpl::SetError(std::string &error)
    {
        _errorMessage = true;
        _error = error;
    }

    void    KeyboardImpl::SetStartingPage(Keyboard::QwertyKeyboardPage page)
    {
        _useSymbols = false;
        _useNintendo = false;
        _useCaps = false;
        _symbolsPage = 0;
        _nintendoPage = 0;
        _useKana = false;       // [JP]
        _kanaPage = 0;

        switch (page)
        {
        case Keyboard::QwertyKeyboardPage::LETTERS_LOWER:
            break;
        case Keyboard::QwertyKeyboardPage::LETTERS_UPPER:
            _useCaps = true;
            break;
        case Keyboard::QwertyKeyboardPage::SYMBOLS_PAGE1:
            _useSymbols = true;
            break;
        case Keyboard::QwertyKeyboardPage::SYMBOLS_PAGE2:
            _useSymbols = true;
            _symbolsPage = 1;
            break;
        case Keyboard::QwertyKeyboardPage::NINTENDO_PAGE1:
            _useNintendo = true;
            break;
        case Keyboard::QwertyKeyboardPage::NINTENDO_PAGE2:
            _useNintendo = true;
            _nintendoPage = 1;
            break;
        default:
            break;
        }
    }

    void    KeyboardImpl::SetBlockedKeys(const std::vector<int>& blockedKeys)
    {
        _allowedKeys.clear();
        _blockedKeys = blockedKeys;
    }

    void    KeyboardImpl::SetAllowedKeys(const std::vector<int>& allowedKeys)
    {
        _blockedKeys.clear();
        _allowedKeys = allowedKeys;
    }

    void    KeyboardImpl::SetConvertCallback(ConvertCallback callback)
    {
        _convert = callback;
    }

    void    KeyboardImpl::SetCompareCallback(CompareCallback callback)
    {
        _compare = callback;
    }

    void    KeyboardImpl::OnKeyboardEvent(OnEventCallback callback)
    {
        _onKeyboardEvent = callback;
    }

    void	KeyboardImpl::ChangeSelectedEntry(int entry) {
        if (!_customKeyboard)
            return;
        if (!_strKeys.size() || entry >= static_cast<int>(_strKeys.size()) || !_strKeys[entry]->CanUse())
            entry = -1;
        _ChangeManualKey(entry, false);
        // Update the position
        if (entry != -1 && _displayScrollbar) {
            int keyRow = _manualKey / (_isIconKeyboard ? 4 : 1);
            int positionRow = _currentPosition / (_isIconKeyboard ? 4 : 1);

            if (keyRow > positionRow + 6 - 2) {
                positionRow = keyRow - 4;
                _scrollSize = ((positionRow * 36.01f) + 15) / _scrollJump - _scrollPosition;
                _manualScrollUpdate = true;
                _UpdateScroll(0.f, true);
            }
            else if (keyRow < positionRow + 1) {
                positionRow = std::max(keyRow, 0);
                _scrollSize = ((positionRow * 36.01f) - 15) / _scrollJump - _scrollPosition;
                _manualScrollUpdate = true;
                _UpdateScroll(0.f, true);
            }
        }
    }

    void    KeyboardImpl::ChangeEntrySound(int entry, SoundEngine::Event soundEvent)
    {
        if (_customKeyboard && entry >= 0 && entry < static_cast<int>(_strKeys.size()) && _strKeys[entry]->CanUse())
        {
            _strKeys[entry]->SetAcceptSoundEvent(soundEvent);
        }
    }

    void    KeyboardImpl::Populate(const std::vector<std::string> &input, bool resetScroll)
    {
        bool mustReset = (_strKeys.size() != input.size()) || resetScroll || _isIconKeyboard;

        int count = input.size();

        if (mustReset)
            _ChangeManualKey(0, false);
        mustReset = (mustReset || count < 6);

        _customKeyboard = true;
        _isIconKeyboard = false;

        if (mustReset)
            _currentPosition = 0;

        std::vector<float> origPosY;

        for (TouchKeyString* tks : _strKeys)
        {
            if (!mustReset)
            {
                u16 posX; float posY;
                tks->GetPosition(posX, posY);
                origPosY.push_back(posY);
            }
            delete tks;
        }

        _strKeys.clear();


        int posY = (count < 6) ? (20 + (200 - ((30 * count) + 6 * (count - 1))) / 2) : 30;

        if (mustReset)
        {
            if (count < 6)
            {
                _displayScrollbar = false;
            }
            else
            {
                int height = 190;

                float lsize = 36.f * (float)count + 1;

                float padding = (float)height / lsize;
                int cursorSize = padding * height;
                float scrollTrackSpace = lsize - height;
                float scrollThumbSpace = height - cursorSize;

                _scrollJump = scrollTrackSpace / scrollThumbSpace;
                _scrollbarSize = height;

                if (cursorSize < 5)
                    cursorSize = 5;

                _scrollPadding = padding;
                _scrollCursorSize = cursorSize;
                _scrollPosition = 0.f;
                _scrollEnd = _scrollbarSize - _scrollCursorSize;
                _displayScrollbar = true;
            }
        }

        _scrollSize = 0;
        _inertialVelocity = 0;

        IntRect box(60, posY, 200, 30);
        int i = 0;
        for (const std::string &str : input)
        {
            TouchKeyString* tks = new TouchKeyString(str, box, true);

            if (!mustReset)
                tks->SetPosition(0xFFFF, origPosY[i++]);
            _strKeys.push_back(tks);
            if (mustReset) box.leftTop.y += 36;
        }

        origPosY.clear();
        _manualScrollUpdate = true;
        _UpdateScroll(0, true);
    }

    void    KeyboardImpl::Populate(const std::vector<CustomIcon>& input, bool resetScroll)
    {
        bool mustReset = (_strKeys.size() != input.size()) || resetScroll || _customKeyboard;

        int count = input.size() / 4;

        if (mustReset)
            _ChangeManualKey(0, false);

        mustReset = (mustReset || count < 6);

        _customKeyboard = true;
        _isIconKeyboard = true;

        if (mustReset)
            _currentPosition = 0;

        std::vector<float> origPosY;

        for (TouchKeyString* tks : _strKeys)
        {
            if (!mustReset)
            {
                u16 posX; float posY;
                tks->GetPosition(posX, posY);
                origPosY.push_back(posY);
            }
            delete tks;
        }
        _strKeys.clear();

        if (input.size() % 4 != 0) count++;

        int posY = (count < 6) ? (20 + (200 - ((30 * count) + 6 * (count - 1))) / 2) : 30;

        if (mustReset) {
            if (count < 6)
            {
                _displayScrollbar = false;
            }
            else if (mustReset)
            {
                int height = 190;


                float lsize = 36.f * (float)count + 1;

                float padding = (float)height / lsize;
                int cursorSize = padding * height;
                float scrollTrackSpace = lsize - height;
                float scrollThumbSpace = height - cursorSize;

                _scrollJump = scrollTrackSpace / scrollThumbSpace;
                _scrollbarSize = height;

                if (cursorSize < 5)
                    cursorSize = 5;

                _scrollPadding = padding;
                _scrollCursorSize = cursorSize;
                _scrollPosition = 0.f;
                _scrollEnd = _scrollbarSize - _scrollCursorSize;
                _displayScrollbar = true;
            }
        }

        _scrollSize = 0;
        _inertialVelocity = 0;

        IntRect box(91, posY, 30, 30);

        int i = 1;
        int j = 0;

        for (const CustomIcon& ico : input)
        {
            TouchKeyString* tks;

            if (ico.sizeX != 30 || ico.sizeY != 30) tks = new TouchKeyString(CustomIcon(Icon::DefaultCustomIcon.pixArray, Icon::DefaultCustomIcon.sizeX, Icon::DefaultCustomIcon.sizeY, ico.isEnabled), box, true);
            else tks = new TouchKeyString(ico, box, true);

            if (!mustReset)
                tks->SetPosition(0xFFFF, origPosY[j++]);

            _strKeys.push_back(tks);

            if (i == 0 && mustReset) box.leftTop.y += 36;
            box.leftTop.x = 91 + i * 36;
            if (i++ == 3) i = 0;
        }

        origPosY.clear();
        _manualScrollUpdate = true;
        _UpdateScroll(0, true);
    }

    void    KeyboardImpl::Clear(void)
    {
        _customKeyboard = false;
        _currentPosition = 0;
        for (TouchKeyString *tks : _strKeys)
            delete tks;
        _strKeys.clear();
    }

    int     KeyboardImpl::Run(void)
    {
        _isOpen = true;
        _userAbort = false;
        _askForExit = false;
        _compStart = -1;        // [JP]
        _compLen = 0;
        _converting = false;
        _convCands.clear();

        // Check if Process is paused
        if (!ProcessImpl::IsPaused)
        {
            _mustRelease  = true;
            ProcessImpl::Pause(false);
        }
        else
            _mustRelease = false;

        int                 ret = -1;
        Event               event;
        EventManager        manager(EventManager::EventGroups::GROUP_KEYS | EventManager::EventGroups::GROUP_TOUCH);
        Clock               clock;

        // Construct keyboard
        if (!_customKeyboard)
        {
            if (_layout == QWERTY) _Qwerty();
            else if (_layout == DECIMAL) _Decimal();
            else if (_layout == HEXADECIMAL) _Hexadecimal();

            if (!_allowedKeys.empty()) {
                for (auto it = _keys->begin(); it != _keys->end(); it++) {
                    int code = it->GetCodePoint();
                    if (std::find(_allowedKeys.begin(), _allowedKeys.end(), code) == _allowedKeys.end())
                        it->Enable(false);
                }
            }
            if (!_blockedKeys.empty()) {
                for (auto it = _keys->begin(); it != _keys->end(); it++) {
                    int code = it->GetCodePoint();
                    if (std::find(_blockedKeys.begin(), _blockedKeys.end(), code) != _blockedKeys.end())
                        it->Enable(false);
                }
            }
        }

        // Check start input
        _errorMessage = !_CheckInput();

        // Set cursor
        if (_showCursor)
        {
            _cursorPositionInString = _userInput.size();
            _ScrollUp();
        }

        // Loop until exit
        while (_isOpen)
        {
            while (manager.PollEvent(event))
            {
                _ProcessEvent(event);
                if (_userAbort)
                {
                    ret = USER_ABORT;
                    goto exit;
                }
            }

            // Update current keys
            _Update(clock.Restart().AsSeconds());

            // Render Top Screen
            if (DisplayTopScreen)
                _RenderTop();
            // Render Bottom Screen
            _RenderBottom();
            Renderer::EndFrame();

            // if it's a standard keyboard
            if (!_customKeyboard)
            {
                // Check keys
                bool inputChanged = _CheckKeys();

                if (_errorMessage && inputChanged)
                    _errorMessage = false;

                if (inputChanged)
                {
                    _errorMessage = !_CheckInput();
                    if (_onKeyboardEvent != nullptr && _owner != nullptr)
                        _onKeyboardEvent(*_owner, _KeyboardEvent);
                }

                // If user try to exit the keyboard
                if (_askForExit)
                {
                    // If input is invalid, user can't exit
                    if (_errorMessage)
                        _askForExit = false;
                    else
                    {
                        // Check input
                        _errorMessage = !_CheckInput();
                        if (!_errorMessage)
                        {
                            // input is valid, exit
                            _isOpen = false;
                            ret = 0;
                        }
                    }
                }
            }
            else
            {
                int  choice = -1;
                bool isSelected = _CheckButtons(choice);

                if (isSelected)
                {
                    ret = choice;
                    _isOpen = false;
                }
            }
            if (SystemImpl::IsSleeping()) {
                ret = SLEEP_ABORT;
                _isOpen = false;
            }
        }

    exit:
        _Select(nullptr);
        PluginMenu *menu = PluginMenu::GetRunningInstance();
        if (menu && !menu->IsOpen() && ret != SLEEP_ABORT)
            ScreenImpl::Clean();
        if (_mustRelease)
            ProcessImpl::Play(false);
        return (ret);
    }

    void    KeyboardImpl::Close(void)
    {
        _isOpen = false;
    }

    void    KeyboardImpl::_RenderTop(void)
    {
        const Color     &red = Color::Red;
        static IntRect  background1(30, 20, 340, 200);
        static IntRect  background2(50, 30, 300, 180);

        IntRect &background = _mustRelease ? background2 : background1;
        int     maxX = background.leftTop.x + background.size.x;
        int     maxY = background.leftTop.y + background.size.y;

        int   posY =  background.leftTop.y + 5;
        int   posX =  background.leftTop.x + 5;

        Renderer::SetTarget(TOP);
        Window::TopWindow.Draw();

        Renderer::DrawSysStringReturn(reinterpret_cast<const u8 *>(_text.c_str()), posX, posY, maxX, Preferences::Settings.MainTextColor, maxY);

        // IF error
        if (_errorMessage && !_error.empty())
        {
            if (posY < 120)
                posY += 48;
            Renderer::DrawSysStringReturn(reinterpret_cast<const u8 *>(_error.c_str()), posX, posY, maxX, red, maxY);
        }

        // [JP] 変換候補 / 操作ヒント
        if (_layout == QWERTY && !_customKeyboard)
            _RenderCandidates(background.leftTop.x + 5, maxX, maxY);
        if (_onKeyboardEvent != nullptr && _owner != nullptr) {
            Render::Interface interface = Renderer::GetInterface();
            _ClearKeyboardEvent();
            _KeyboardEvent.type = KeyboardEvent::EventType::FrameTop;
            _KeyboardEvent.renderInterface = &interface;
            _onKeyboardEvent(*_owner, _KeyboardEvent);
        }
    }

    void    KeyboardImpl::_RenderBottom(void)
    {
        static IntRect  background(20, 20, 280, 200);
        static IntRect  background2(22, 22, 276, 196);
        static IntRect  clampArea(22, 25, 270, 190);

        Renderer::SetTarget(BOTTOM);

        // Draw "normal" keyboard
        if (!_customKeyboard)
        {
            // Pointer to settings
            static auto    &theme = Preferences::Settings.Keyboard;

            int     posY = 20;
            int     posX = 25;

            // Clean background
            Renderer::DrawRect(background, theme.Background);

            // Draw input
            Renderer::DrawSysString(_userInput.c_str(), posX, posY, 300, theme.Input, _offset);

            // Draw cursor
            if (_showCursor && _blinkingClock.GetElapsedTime() < Seconds(0.5f))
                Renderer::DrawLine(_cursorPositionOnScreen + posX, 21, 1, theme.Cursor, 16);

            // Digit layout
            if (_layout != Layout::QWERTY)
            {
                // Draw keys
                for (TouchKey &key : *_keys)
                {
                    key.Draw();
                }
            }
            // Qwerty layout
            else
            {
                // [JP] 表示中のページだけ描画
                int start, end;
                std::tie(start, end) = _GetEffectiveKeyRange();
                for (; start < end; start++)
                    (*_keys)[start].Draw();
            }

            // [JP] 予測変換バー
            if (_layout == Layout::QWERTY)
                _RenderSuggestionBar();

            // [JP] 未確定文字列に下線
            if (_compStart >= 0 && _compLen > 0
                && _compStart + _compLen <= static_cast<int>(_userInput.size()))
            {
                std::string before = _userInput.substr(0, _compStart);
                std::string comp = _userInput.substr(_compStart, _compLen);
                int x0 = static_cast<int>(Renderer::GetTextSize(before.c_str()) - _offset);
                int w = static_cast<int>(Renderer::GetTextSize(comp.c_str()));
                if (x0 < 0) { w += x0; x0 = 0; }
                if (x0 + w > 270) w = 270 - x0;
                if (w > 0)
                    Renderer::DrawLine(posX + x0, 35, w, theme.Cursor, 1);
            }
        }
        // Draw custom keyboard
        else
        {
            // Pointer to settings
            static auto    &theme = Preferences::Settings.CustomKeyboard;

            Renderer::DrawRect2(background, theme.BackgroundMain, theme.BackgroundSecondary);
            Renderer::DrawRect(background2, theme.BackgroundBorder, false);

            size_t max = _strKeys.size();
            int offset = _isIconKeyboard ? 24 : 6;
            max = std::min(static_cast<int>(max), _currentPosition + offset);

            PrivColor::UseClamp(true, clampArea);

            for (size_t i = _currentPosition; i < max && i < _strKeys.size(); i++)
            {
                _strKeys[i]->ForcePressed(static_cast<int>(i) == _manualKey);
                _strKeys[i]->Draw();
            }

            PrivColor::UseClamp(false);

            if (_displayScrollbar)
            {
                // Draw scroll bar
                const Color &sbBackground = theme.ScrollBarBackground;
                const Color &sbThumb = theme.ScrollBarThumb;

                // Background
                int posX = 292;
                int posY = 25;

                Renderer::DrawLine(posX, posY + 1, 1, sbBackground, _scrollbarSize - 2);
                Renderer::DrawLine(posX + 1, posY, 1, sbBackground, _scrollbarSize);
                Renderer::DrawLine(posX + 2, posY + 1, 1,sbBackground, _scrollbarSize - 2);

                posY += (int)(_scrollPosition);

                // Draw thumb
                Renderer::DrawLine(posX, posY + 1, 1, sbThumb, _scrollCursorSize - 2);
                Renderer::DrawLine(posX + 1, posY, 1, sbThumb, _scrollCursorSize);
                Renderer::DrawLine(posX + 2, posY + 1, 1, sbThumb, _scrollCursorSize - 2);
            }
        }
        if (_onKeyboardEvent != nullptr && _owner != nullptr) {
            Render::Interface interface = Renderer::GetInterface();
            _ClearKeyboardEvent();
            _KeyboardEvent.type = KeyboardEvent::EventType::FrameBottom;
            _KeyboardEvent.renderInterface = &interface;
            _onKeyboardEvent(*_owner, _KeyboardEvent);
        }
    }

    void    KeyboardImpl::_ProcessEvent(Event &event)
    {
        bool inputPassedTime = _inputClock.HasTimePassed(Milliseconds(200));

        // [JP] 変換候補を選択中の操作 (キー選択の操作は止める)
        if (!_customKeyboard && _converting
            && (event.type == Event::KeyPressed || event.type == Event::KeyDown))
        {
            const u32 code = event.key.code;
            if (event.type == Event::KeyPressed)
            {
                if (code == Key::B)
                {
                    SoundEngine::PlayMenuSound(SoundEngine::Event::CANCEL);
                    _CancelConversion();
                    return;
                }
                if (code == Key::A)
                {
                    SoundEngine::PlayMenuSound(SoundEngine::Event::ACCEPT);
                    _CommitComposition();
                    return;
                }
                if (code == Key::DPadRight || code == Key::DPadDown || code == Key::DPadLeft || code == Key::DPadUp)
                {
                    SoundEngine::PlayMenuSound(SoundEngine::Event::DESELECT);
                    _Convert((code == Key::DPadRight || code == Key::DPadDown) ? 1 : -1);
                    return;
                }
            }
            if (code & (Key::Down | Key::Up | Key::Left | Key::Right | Key::L | Key::R))
                return;
        }

        // [JP] 予測変換バーのタッチ
        if (!_customKeyboard && _layout == QWERTY && event.type == Event::TouchBegan
            && event.touch.y >= SUGGEST_BAR_Y && event.touch.y < SUGGEST_BAR_Y + SUGGEST_BAR_H
            && event.touch.x >= 20 && event.touch.x < 300)
        {
            const int tx = event.touch.x;
            if (tx < 36 && _candFirst > 0)
                _candFirst = _candFirst > 3 ? _candFirst - 3 : 0;
            else if (tx >= 284 && _candHasMore)
                _candFirst += _candHit.empty() ? 1 : static_cast<int>(_candHit.size());
            else
            {
                for (size_t i = 0; i < _candHit.size(); i++)
                {
                    if (tx >= _candHit[i].first && tx < _candHit[i].second)
                    {
                        _ChooseCandidate(_candFirst + static_cast<int>(i));
                        break;
                    }
                }
            }
            return;
        }

        if (event.type == Event::KeyPressed)
        {
            if (event.key.code == Key::B)
            {
                if (_canAbort)
                {
                    _userAbort = true;
                    SoundEngine::PlayMenuSound(SoundEngine::Event::CANCEL);
                }

                return;
            }
            if (!_customKeyboard && event.key.code == Y && !_userInput.empty())
            {
                SoundEngine::PlayMenuSound(SoundEngine::Event::DESELECT);
                _compStart = -1; _compLen = 0; _converting = false; // [JP]
                _userInput.clear();
                _ClearKeyboardEvent();
                _KeyboardEvent.type = KeyboardEvent::EventType::InputWasCleared;
                _KeyboardEvent.codepoint = 0;
                _errorMessage = !_CheckInput();
                if (_onKeyboardEvent != nullptr && _owner != nullptr)
                    _onKeyboardEvent(*_owner, _KeyboardEvent);
            }
            if (event.key.code == Select && !_customKeyboard && _layout != QWERTY && _canChangeLayout)
            {
                SoundEngine::PlayMenuSound(SoundEngine::Event::DESELECT);
                _userInput.clear();
                _ClearKeyboardEvent();
                _KeyboardEvent.type = KeyboardEvent::EventType::InputWasCleared;
                _KeyboardEvent.codepoint = 0;
                _errorMessage = !_CheckInput();
                if (_onKeyboardEvent != nullptr && _owner != nullptr)
                    _onKeyboardEvent(*_owner, _KeyboardEvent);
                SetLayout(_layout == DECIMAL ? HEXADECIMAL : DECIMAL);
                _canChangeLayout = true;
            }
            if (_customKeyboard && (event.key.code & (Key::Down | Key::Up | Key::Left | Key::Right | Key::A))) {
                _HandleManualKeyPress((Key)(event.key.code & Key::A));
                inputPassedTime = true;
            }
        }

        if (event.type == Event::KeyDown)
        {
            // [JP] 未確定文字列はカーソル移動で確定
            if (_compStart >= 0 && (event.key.code == Key::L || event.key.code == Key::R))
                _CommitComposition();

            if (_showCursor && inputPassedTime)
            {
                if (event.key.code == Key::L) {
                    _inputClock.Restart();
                    _ScrollDown();
                }
                else if (event.key.code == Key::R) {
                    _inputClock.Restart();
                    _ScrollUp();
                }
            }
            if (_customKeyboard && inputPassedTime) {
                if (event.key.code & (Key::Down | Key::Up | Key::Left | Key::Right | Key::A)) {
                    _HandleManualKeyPress((Key)(event.key.code & ~(u32)Key::A));
                    _inputClock.Restart();
                }
            }
            if (!_customKeyboard && _allowKeyInput && inputPassedTime && (event.key.code & (Key::Down | Key::Up | Key::Left | Key::Right))) {
                if (_selectedKey == nullptr) {
                    _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                } else {
                    u16 posX, posY;
                    if (event.key.code & Key::Down) {
                        std::tie(posX, posY) = _selectedKey->GetSidePosition(TouchKey::Side::BOTTOM);
                        _CalcSelect(SelectMode::DOWN, posX, posY);
                    }
                    if (event.key.code & Key::Up) {
                        std::tie(posX, posY) = _selectedKey->GetSidePosition(TouchKey::Side::TOP);
                        _CalcSelect(SelectMode::UP, posX, posY);
                    }
                    if (event.key.code & Key::Left) {
                        std::tie(posX, posY) = _selectedKey->GetSidePosition(TouchKey::Side::LEFT);
                        _CalcSelect(SelectMode::LEFT, posX, posY);
                    }
                    if (event.key.code & Key::Right) {
                        std::tie(posX, posY) = _selectedKey->GetSidePosition(TouchKey::Side::RIGHT);
                        _CalcSelect(SelectMode::RIGHT, posX, posY);
                    }
                }
                _inputClock.Restart();
            }
        }
        if (_onKeyboardEvent != nullptr && _owner != nullptr && (event.type == Event::KeyDown || event.type == Event::KeyPressed || event.type == Event::KeyReleased)) {
            _ClearKeyboardEvent();
            if (event.type == Event::KeyPressed) _KeyboardEvent.type = KeyboardEvent::EventType::KeyPressed;
            else if (event.type == Event::KeyDown) _KeyboardEvent.type = KeyboardEvent::EventType::KeyDown;
            else if (event.type == Event::KeyReleased) _KeyboardEvent.type = KeyboardEvent::EventType::KeyReleased;
            _KeyboardEvent.affectedKey = event.key.code;
            _onKeyboardEvent(*_owner, _KeyboardEvent);
        }

        if (event.type == Event::TouchMoved || event.type == Event::TouchEnded)
        {
            _scrollSize = 0; _ChangeManualKey(-1, false);
        }
        if (!_customKeyboard && event.type == Event::TouchBegan)
        {
            _Select(nullptr);
        }

        if (!_displayScrollbar)
            return;

        static IntRect  buttons(60, 26, 200, 200);
        // Touch / Scroll
        if (event.type == Event::TouchBegan)
        {
            if (!buttons.Contains(event.touch.x, event.touch.y))
            {
                _inertialVelocity = 0;
                _lastTouch = IntVector(event.touch.x, event.touch.y);
                _touchTimer.Restart();
            }
        }

        if (event.type == Event::TouchMoved)
        {
            if (!buttons.Contains(event.touch.x, event.touch.y))
            {
                Time delta = _touchTimer.Restart();

                float moveDistance = (float)(_lastTouch.y - event.touch.y);
                _inertialVelocity = moveDistance / delta.AsSeconds();
                _lastTouch = IntVector(event.touch.x, event.touch.y);
            }
        }

        if (event.type == Event::TouchEnded)
        {
            if (!buttons.Contains(event.touch.x, event.touch.y))
            {
                if (_touchTimer.GetElapsedTime().AsSeconds() > 0.3f)
                    _inertialVelocity = 0.f;
            }
        }
    }

    #define INERTIA_SCROLL_FACTOR 0.9f
    #define INERTIA_ACCELERATION 0.75f
    #define INERTIA_THRESHOLD 1.0f

    void    KeyboardImpl::_UpdateScroll(float delta, bool ignoreTouch) {

        bool            isTouchDown = Touch::IsDown() && !ignoreTouch;
        IntVector       touchPos(ignoreTouch ? IntVector(0, 0) : (IntVector)Touch::GetPosition());

        if (_displayScrollbar)
        {
            if (!_manualScrollUpdate)
                _scrollSize = (_inertialVelocity * INERTIA_SCROLL_FACTOR * delta);

            _manualScrollUpdate = false;

            _scrollPosition += _scrollSize;

            if (_scrollPosition <= 0.f)
            {
                _scrollSize = _scrollSize - _scrollPosition;
                _scrollPosition = 0.f;
                _inertialVelocity = 0.f;
            }
            else if (_scrollPosition >= _scrollEnd)
            {
                _scrollSize -= (_scrollPosition - _scrollEnd);
                _scrollPosition = _scrollEnd;
                _inertialVelocity = 0.f;
            }


            _inertialVelocity += (0.98f) * delta;

            _inertialVelocity *= INERTIA_ACCELERATION;

            _currentPosition = (_scrollPosition * _scrollJump) / 36; //(_scrollPosition / 36);
            if (_isIconKeyboard) _currentPosition *= 4;

            if (std::abs(_inertialVelocity) < INERTIA_THRESHOLD)
                _inertialVelocity = 0.f;

            float scr = -_scrollSize * _scrollJump;
            _scrollSize = 0;

            for (TouchKeyString* tks : _strKeys)
            {
                tks->Scroll(scr);
                tks->Update(isTouchDown, touchPos);
            }
        }
        else
        {
            for (TouchKeyString* tks : _strKeys)
                tks->Update(isTouchDown, touchPos);
        }
    }

    void    KeyboardImpl::_Update(float delta)
    {
		bool			isTouchDown = Touch::IsDown();
		IntVector		touchPos(Touch::GetPosition());

        if (!_customKeyboard)
        {
            int start, end;
            std::tie(start, end) = _GetEffectiveKeyRange();

            for (int i = start; i < end; i++)
            {
                (*_keys)[i].Update(isTouchDown, touchPos);
            }

            if (_showCursor && _blinkingClock.HasTimePassed(Seconds(1.f)))
                _blinkingClock.Restart();

            _UpdateScrollInfos();
        }
        else ///< Custom Keyboard
        {
            _UpdateScroll(delta, false);
        }
    }

    /*
    ** _keys:
    **
    ** [0] = 'Q'
    ** [1] = 'W'
    ** [2] = 'E'
    ** [3] = 'R'
    ** [4] = 'T'
    ** [5] = 'Y'
    ** [6] = 'U'
    ** [7] = 'I'
    ** [8] = 'O'
    ** [9] = 'P'
    ** [10] = 'KEY_BACKSPACE'

    ** First Line : 11 keys **

    ** [11] = 'A'
    ** [12] = 'S'
    ** [13] = 'D'
    ** [14] = 'F'
    ** [15] = 'G'
    ** [16] = 'H'
    ** [17] = 'J'
    ** [18] = 'K'
    ** [19] = 'L'
    ** [20] = '''
    ** [21] = 'KEY_ENTER'

    ** Second Line : 11 keys **

     ** [22] = 'CAPS'
     ** [23] = 'Z'
     ** [24] = 'X'
     ** [25] = 'C'
     ** [26] = 'V'
     ** [27] = 'B'
     ** [28] = 'N'
     ** [29] = 'M'
     ** [30] = ','
     ** [31] = '.'
     ** [32] = '?'
     *
     ** Third Line : 11 keys **

     ** [33] = 'KEY_SYMBOL'
     ** [34] = 'SMILEY'
     ** [35] = 'SPACE'
     *
     ** Fourth Line : 3 keys **
    *************************/

    void    KeyboardImpl::_QwertyLowCase(void)
    {
        // Key rectangle
        IntRect pos(20, 36 + QWERTY_OFFSET_Y, 25, 40);

        s_pageStart[PG_LOW] = _QwertyKeys.size(); // [JP]

        /*low case*/
        _QwertyKeys.emplace_back('q', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('w', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('e', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('r', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('t', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('y', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('u', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('i', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('o', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('p', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 76 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back('a', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('s', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('d', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('f', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('g', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('h', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('j', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('k', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('l', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('\'', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

        pos.leftTop.x = 20;
        pos.leftTop.y = 116 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_CAPS, Icon::DrawCapsLockOn, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('z', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('x', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('c', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('v', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('b', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('n', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('m', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(',', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('.', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('?', pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 156 + QWERTY_OFFSET_Y;

        pos.size.x = 40;
        _QwertyKeys.emplace_back("+=@", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS); pos.leftTop.x += 40;
        _QwertyKeys.emplace_back("\uE008", pos, Keyboard::SpecialCodePoints::KEY_SMILEY); pos.leftTop.x += 40;
        pos.size.x = 120;
        _QwertyKeys.emplace_back("\uE057", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
        // [JP] 文字色 / かな
        pos.size.x = 50;
        _QwertyKeys.emplace_back("\u8272", pos, KEY_COLOR); pos.leftTop.x += 50;   // 色
        pos.size.x = 25;
        _QwertyKeys.emplace_back("\u3042", pos, KEY_KANA);                         // あ
    }

    void    KeyboardImpl::_QwertyUpCase(void)
    {
        // Key rectangle
        IntRect pos(20, 36 + QWERTY_OFFSET_Y, 25, 40);

        s_pageStart[PG_UP] = _QwertyKeys.size(); // [JP]

        /*low case*/
        _QwertyKeys.emplace_back('Q', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('W', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('E', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('R', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('T', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('Y', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('U', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('I', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('O', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('P', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 76 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back('A', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('S', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('D', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('F', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('G', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('H', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('J', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('K', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('L', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('"', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

        pos.leftTop.x = 20;
        pos.leftTop.y = 116 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_CAPS, Icon::DrawCapsLockOn, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('Z', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('X', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('C', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('V', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('B', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('N', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('M', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(';', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(':', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('!', pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 156 + QWERTY_OFFSET_Y;

        pos.size.x = 40;
        _QwertyKeys.emplace_back("+=@", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS); pos.leftTop.x += 40;
        _QwertyKeys.emplace_back("\uE008", pos, Keyboard::SpecialCodePoints::KEY_SMILEY); pos.leftTop.x += 40;
        pos.size.x = 120;
        _QwertyKeys.emplace_back("\uE057", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
        // [JP] 文字色 / かな
        pos.size.x = 50;
        _QwertyKeys.emplace_back("\u8272", pos, KEY_COLOR); pos.leftTop.x += 50;   // 色
        pos.size.x = 25;
        _QwertyKeys.emplace_back("\u3042", pos, KEY_KANA);                         // あ
    }

    void KeyboardImpl::_QwertySymbols(void)
    {
        // Key rectangle
        IntRect pos(20, 36 + QWERTY_OFFSET_Y, 25, 40);

        s_pageStart[PG_SYM1] = _QwertyKeys.size(); // [JP]

        /*page 1*/
        _QwertyKeys.emplace_back('?', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('!', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('@', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('#', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('$', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('%', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('&', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('1', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('2', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('3', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 76 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back('(', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(')', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('-', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('_', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('=', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00F7", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('+', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('4', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('5', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('6', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

        pos.leftTop.x = 20;
        pos.leftTop.y = 116 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\u2192", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS_PAGE); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('\\', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(';', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(':', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('"', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('*', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('/', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('7', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('8', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('9', pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 156 + QWERTY_OFFSET_Y;

        pos.size.x = 40;
        _QwertyKeys.emplace_back("+=@", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS); pos.leftTop.x += 40;
        _QwertyKeys.emplace_back("\uE008", pos, Keyboard::SpecialCodePoints::KEY_SMILEY); pos.leftTop.x += 40;
        pos.size.x = 120;
        _QwertyKeys.emplace_back("\uE057", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
        pos.size.x = 25;
        _QwertyKeys.emplace_back('0', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('.', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u3042", pos, KEY_KANA); // [JP] あ

        pos.leftTop.x = 20;
        pos.leftTop.y = 36 + QWERTY_OFFSET_Y;

        s_pageStart[PG_SYM2] = _QwertyKeys.size(); // [JP]

        /*page 2*/
        _QwertyKeys.emplace_back("\u2022", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00A9", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u20AC", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00A3", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00A5", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00B5", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00A7", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('1', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('2', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('3', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 76 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\u2122", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('<', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('>', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('[', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(']', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('{', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('}', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('4', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('5', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('6', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

        pos.leftTop.x = 20;
        pos.leftTop.y = 116 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\u2190", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS_PAGE); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('|', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00B2", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('`', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00B0", pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('~', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('^', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('7', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('8', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('9', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u00B1", pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 156 + QWERTY_OFFSET_Y;

        pos.size.x = 40;
        _QwertyKeys.emplace_back("+=@", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS); pos.leftTop.x += 40;
        _QwertyKeys.emplace_back("\uE008", pos, Keyboard::SpecialCodePoints::KEY_SMILEY); pos.leftTop.x += 40;
        pos.size.x = 120;
        _QwertyKeys.emplace_back("\uE057", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
        pos.size.x = 25;
        _QwertyKeys.emplace_back('0', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back('.', pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\u3042", pos, KEY_KANA); // [JP] あ
    }

    void KeyboardImpl::_QwertyNintendo()
    {
        // Key rectangle
        IntRect pos(20, 36 + QWERTY_OFFSET_Y, 25, 40);

        s_pageStart[PG_NIN1] = _QwertyKeys.size(); // [JP]

        /*page 1*/
        _QwertyKeys.emplace_back("\uE000" /* A */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE001" /* B */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE002" /* X */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE003" /* Y */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE004" /* L */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE005" /* R */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE054" /* ZL */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE055" /* ZR */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE006" /* DPAD */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE041" /* DPAD Wii*/, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 76 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\uE04C" /* a */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE04D" /* b */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE04E" /* x */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE04F" /* y */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE052" /* l */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE053" /* r */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE050" /* L Stick */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE051" /* R Stick */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE042" /* A Wii */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE043" /* B Wii */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

        pos.leftTop.x = 20;
        pos.leftTop.y = 116 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\u2192", pos, Keyboard::SpecialCodePoints::KEY_NINTENDO_PAGE); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE040" /* Power */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE044" /* Home */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE045" /* + */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE046" /* - */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE047" /* 1 */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE048" /* 2 */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE049" /* Stick */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE04A" /* C */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE04B" /* Z */, pos); pos.leftTop.x += 25;

        pos.leftTop.x = 20;
        pos.leftTop.y = 156 + QWERTY_OFFSET_Y;

        pos.size.x = 40;
        _QwertyKeys.emplace_back("+=@", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS); pos.leftTop.x += 40;
        _QwertyKeys.emplace_back("\uE008", pos, Keyboard::SpecialCodePoints::KEY_SMILEY); pos.leftTop.x += 40;

        pos.size.x = 120;
        _QwertyKeys.emplace_back("\uE057", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
        // [JP] 文字色 / かな
        pos.size.x = 50;
        _QwertyKeys.emplace_back("\u8272", pos, KEY_COLOR); pos.leftTop.x += 50;   // 色
        pos.size.x = 25;
        _QwertyKeys.emplace_back("\u3042", pos, KEY_KANA);                         // あ

        pos.leftTop.x = 20;
        pos.leftTop.y = 36 + QWERTY_OFFSET_Y;

        s_pageStart[PG_NIN2] = _QwertyKeys.size(); // [JP]

        /*page 2*/
        _QwertyKeys.emplace_back("\uE079" /* DPAD UP */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE07B" /* DPAD DOWN */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE07C" /* DPAD LEFT */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE07D" /* DPAD RIGHT */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE07E" /* DPAD UP&DOWN */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE07F" /* DPAD LEFT&RIGHT */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE077" /* Wii Stick */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE078" /* Wii Power */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE056" /* Enter */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE057" /* Space*/, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

        pos.leftTop.x = 20;
        pos.leftTop.y = 76 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\uE007" /* Clock */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE008" /* Happy */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE009" /* Angry */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE00A" /* Sad */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE00B" /* ExpressionLess */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE00C" /* Sun */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE00D" /* Cloud */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE00E" /* Umbrella */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE00F" /* Snowman */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE06B" /* ? */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

        pos.leftTop.x = 20;
        pos.leftTop.y = 116 + QWERTY_OFFSET_Y;

        _QwertyKeys.emplace_back("\u2190", pos, Keyboard::SpecialCodePoints::KEY_NINTENDO_PAGE); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE015" /*  */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE016" /*  */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE017" /*  */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE018" /*  */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE019" /* Arrow Right */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE01A" /* Arrow Left */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE01B" /* Arrow Up */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE01C" /* Arow Down */, pos); pos.leftTop.x += 25;
        _QwertyKeys.emplace_back("\uE01E" /* Camera */, pos); pos.leftTop.x += 25;

        pos.leftTop.x = 20;
        pos.leftTop.y = 156 + QWERTY_OFFSET_Y;

        pos.size.x = 40;
        _QwertyKeys.emplace_back("+=@", pos, Keyboard::SpecialCodePoints::KEY_SYMBOLS); pos.leftTop.x += 40;
        _QwertyKeys.emplace_back("\uE008", pos, Keyboard::SpecialCodePoints::KEY_SMILEY); pos.leftTop.x += 40;
        pos.size.x = 120;
        _QwertyKeys.emplace_back("\uE057", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
        // [JP] 文字色 / かな
        pos.size.x = 50;
        _QwertyKeys.emplace_back("\u8272", pos, KEY_COLOR); pos.leftTop.x += 50;   // 色
        pos.size.x = 25;
        _QwertyKeys.emplace_back("\u3042", pos, KEY_KANA);                         // あ
    }


    void    KeyboardImpl::_Qwerty(void)
    {
        _keys = &_QwertyKeys;

        if (_QwertyKeys.empty())
        {
            /* 0 - 35*/
            _QwertyLowCase();
            /*36 - 71*/
            _QwertyUpCase();
            /* 72 - 108 Page 1*/
            /* 109 - 146 Page 2*/
            _QwertySymbols();
            /* 147 - 181 Page 1*/
            /* 182 - 216 Page 2*/
            _QwertyNintendo();
            // [JP] ひらがな2ページ + カタカナ2ページ
            _QwertyKana();
            s_pageStart[PG_COUNT] = _QwertyKeys.size();
        }

        // Enable all keys
        for (TouchKey& key : *_keys)
            key.Enable();
    }

    // ---------------------------------------------------------------
    //  [JP] かなキーボード
    //   1～3段目: 10文字 + (BS / ENTER / ページ送り)
    //   4段目   : カナ切替 / ゛゜小 / 空白・変換 / 、 / 。 / 英字に戻る
    // ---------------------------------------------------------------
    void    KeyboardImpl::_QwertyKana(void)
    {
        static const char *page1[3][10] =
        {
            { "あ", "い", "う", "え", "お", "か", "き", "く", "け", "こ" },
            { "さ", "し", "す", "せ", "そ", "た", "ち", "つ", "て", "と" },
            { "な", "に", "ぬ", "ね", "の", "は", "ひ", "ふ", "へ", "ほ" },
        };
        static const char *page2[3][10] =
        {
            { "ま", "み", "む", "め", "も", "や", "ゆ", "よ", "「", "」" },
            { "ら", "り", "る", "れ", "ろ", "わ", "を", "ん", "ー", "！" },
            { "？", "・", "～", "…", "（", "）", "『", "』", "♪", "☆" },
        };

        for (int kata = 0; kata < 2; kata++)
        {
            for (int page = 0; page < 2; page++)
            {
                s_pageStart[PG_HIRA1 + kata * 2 + page] = _QwertyKeys.size();

                const char *(*table)[10] = page ? page2 : page1;
                auto label = [kata](const char *str)
                {
                    return kata ? JpInput::ShiftKana(str, true) : std::string(str);
                };

                IntRect pos(20, 36 + QWERTY_OFFSET_Y, 25, 40);

                // 1段目
                for (int c = 0; c < 10; c++) { _QwertyKeys.emplace_back(label(table[0][c]), pos); pos.leftTop.x += 25; }
                _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);

                // 2段目
                pos.leftTop.x = 20;
                pos.leftTop.y = 76 + QWERTY_OFFSET_Y;
                for (int c = 0; c < 10; c++) { _QwertyKeys.emplace_back(label(table[1][c]), pos); pos.leftTop.x += 25; }
                _QwertyKeys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
                _QwertyKeys.back().SetAcceptSoundEvent(SoundEngine::Event::ACCEPT);

                // 3段目
                pos.leftTop.x = 20;
                pos.leftTop.y = 116 + QWERTY_OFFSET_Y;
                _QwertyKeys.emplace_back(page ? "\u2190" : "\u2192", pos, KEY_KANA_PAGE); pos.leftTop.x += 25;
                for (int c = 0; c < 10; c++) { _QwertyKeys.emplace_back(label(table[2][c]), pos); pos.leftTop.x += 25; }

                // 4段目
                pos.leftTop.x = 20;
                pos.leftTop.y = 156 + QWERTY_OFFSET_Y;
                pos.size.x = 40;
                _QwertyKeys.emplace_back(kata ? "かな" : "カナ", pos, KEY_KANA_TYPE); pos.leftTop.x += 40;
                _QwertyKeys.emplace_back("゛゜小", pos, KEY_DAKUTEN); pos.leftTop.x += 40;
                pos.size.x = 120;
                _QwertyKeys.emplace_back("空白 / 変換", pos, Keyboard::SpecialCodePoints::KEY_SPACE); pos.leftTop.x += 120;
                pos.size.x = 25;
                _QwertyKeys.emplace_back("、", pos); pos.leftTop.x += 25;
                _QwertyKeys.emplace_back("。", pos); pos.leftTop.x += 25;
                _QwertyKeys.emplace_back("A", pos, KEY_KANA);
            }
        }
    }


    /*
    ** _keys:
    **
    ** [0] = 'A'
    ** [1] = 'B'
    ** [2] = 'C'
    ** [3] = 'D'
    ** [4] = 'E'
    ** [5] = 'F'
    ** [6] = '1'
    ** [7] = '2'
    ** [8] = '3'
    ** [9] = '4'
    ** [10] = '5'
    ** [11] = '6'
    ** [12] = '7'
    ** [13] = '8'
    ** [14] = '9'
    ** [15] = KEY_BACKSPACE
    ** [16] = KEY_ENTER
    ** [17] = '.'
    ** [18] = '0'
    *************************/

    void    KeyboardImpl::_DigitKeyboard(std::vector<TouchKey> &keys)
    {
        //Start
        IntRect pos(20, 36, 46, 46);

        // A - F
        char c = 'A';
        for (int i = 0; i < 6; i++, c++)
        {
            keys.emplace_back(c, pos);
            pos.leftTop.x += 46;

            if (i % 2)
            {
                pos.leftTop.x = 20;
                pos.leftTop.y += 46;
            }
        }

        pos.leftTop.y = 36;
        pos.leftTop.x = 112;

        // 1 - 9
        c = '1';
        for (int i = 0; i < 9; i++, c++)
        {
            keys.emplace_back(c, pos);
            pos.leftTop.x += 46;

            if (i % 3 == 2)
            {
                pos.leftTop.x = 112;
                pos.leftTop.y += 46;
            }
        }

        // Special keys
        pos.leftTop.y = 36;
        pos.leftTop.x = 250;

        // Clear key
        keys.emplace_back(Keyboard::SpecialCodePoints::KEY_BACKSPACE, Icon::DrawClearSymbol, pos);
        pos.leftTop.y += 46;

        // Enter key
        keys.emplace_back(Keyboard::SpecialCodePoints::KEY_ENTER, Icon::DrawEnterKey, pos);
        pos.leftTop.y += 46;

        // Dot key
        keys.emplace_back('.', pos);
        pos.leftTop.y += 46;

        // Plus-Minus key
        keys.emplace_back("\u00B1", pos);

        // 0 key
        pos.leftTop.y = 174;
        pos.leftTop.x = 112;
        pos.size.x = 138;
        keys.emplace_back('0', pos);
    }

    void    KeyboardImpl::_Decimal(void)
    {
        _keys = &_DecimalKeys;

        if (_DecimalKeys.empty())
            _DigitKeyboard(_DecimalKeys);

        // Enable all keys
        for (TouchKey& key : *_keys)
            key.Enable();

        // Disable Hex keys
        KeyIter  iter = _DecimalKeys.begin();
        KeyIter  end = iter;
        std::advance(end, 6);

        for (; iter != end; ++iter)
            (*iter).Enable(false);

        _keys->at(17).Enable(_decimalFraction);
    }

    void    KeyboardImpl::_Hexadecimal(void)
    {
        _keys = &_HexaDecimalKeys;

        if (_HexaDecimalKeys.empty())
            _DigitKeyboard(_HexaDecimalKeys);

        // Enable all keys
        for (TouchKey& key : *_keys)
            key.Enable();

        // Disable dot key
        _HexaDecimalKeys.at(17).Enable(false);

        // Disable pllus-minus key
        _HexaDecimalKeys.at(18).Enable(false);

        // Disable Hex keys if users asks so
        /*if (!_isHex)
        {
            KeyIter  iter = _HexaDecimalKeys.begin();
            KeyIter  end = iter;
            std::advance(end, 6);

            for (; iter != end; ++iter)
                (*iter).Enable(false);
        } */
    }

    static int UnitsToAdvance(const char *str, u32 target)
    {
        const u8 *s = reinterpret_cast<const u8 *>(str);
        int units = 0;

        // Skip UTF8 sig
        if (s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)
            s += 3;

        while (*s && target > 0)
        {
            if (*s == 0x18)
            {
                ++s;
                ++units;
                --target;
                continue;
            }

            if (*s == 0x1B)
            {
                s += 4;
                units += 4;
                target -= 4;
                continue;
            }

            u32 code;
            int unit = decode_utf8(&code, s);

            if (code == 0)
                break;
            if (unit == -1)
                return -1;

            s += unit;
            units += unit;
            target -= unit;
        }
        return units;
    }

    int     UnitsToNextChar(const char *cstr, int left)
    {
        int units = 0;

        while (*cstr && left > 0)
        {
            if (*cstr == 0x1B)
            {
                units += 4;
                left -= 4;
                cstr += 4;
                continue;
            }

            if (*cstr == 0x18)
            {
                ++units;
                --left;
                ++cstr;
                continue;
            }

            u32 code;
            int u = decode_utf8(&code, (u8 *)cstr);

           /* if (code == 0)
                break;*/
            if (u == -1)
                return u;

            units += u;
            break;
        }
        return units;
    }

    int     UnitsToPreviousChar(const char *cstr, int cursor)
    {
        int units = 0;

        --cstr;
        while (*cstr && cursor > 0)
        {
            --cursor;
            if (cursor > 4 && *(cstr - 3) == 0x1B)
            {
                units += 4;
                cursor -= 4;
                cstr -= 3;
                continue;
            }

            if (*cstr == 0x18)
            {
                ++units;
                --cursor;
                --cstr;
                continue;
            }

            u32 code;
            int u = decode_utf8(&code, (u8 *)cstr);

            if (u == -1 && !cursor)
                return u;

            if (u != -1)
            {
                units += u;
                break;
            }
            --cstr;
        }
        return units;
    }

    void    KeyboardImpl::_ScrollUp(void)
    {
        const u32   strLength = _userInput.size();
        const char  *cstr = _userInput.c_str();

        // If input is empty
        if (strLength == 0)
            goto empty;

        {
            // Get units to advance to go to the next char
            const int units = UnitsToNextChar(cstr + _cursorPositionInString, strLength - _cursorPositionInString);

            if (units < 0)
            {
                ///< Weird char being found, better clear the input than abort/crash later
                goto error;
            }

            // Increase cursor position
            _cursorPositionInString += units;
        }
        // cursor must be at most, after the last char of input
        if (_cursorPositionInString > static_cast<int>(strLength))
            _cursorPositionInString = strLength;

        _blinkingClock.Restart();
        return;

    error:
        _userInput.clear();
    empty:
        _offset = 0.f;
        _cursorPositionInString = _cursorPositionOnScreen = 0;
        return;

    }

    void    KeyboardImpl::_ScrollDown(void)
    {
        const u32   strLength = _userInput.size();
        const char  *cstr = _userInput.c_str();

        // If input is empty
        if (strLength == 0)
            goto empty;

        // If cursor is beyond the string, fix it (can happen when deleting a unicode)
        if (_cursorPositionInString > static_cast<int>(strLength))
            _cursorPositionInString = strLength;

        {
            // Get units to advance to go to the previous char
            const int units = UnitsToPreviousChar(cstr + _cursorPositionInString, _cursorPositionInString);

            if (units < 0)
            {
                ///< Weird char being found, better clear the input than abort/crash later
                goto error;
            }

            // Increase cursor position
            _cursorPositionInString -= units;
        }

        // cursor must be at most, at the begining of the string
        if (_cursorPositionInString < 0)
            _cursorPositionInString = 0;

        _blinkingClock.Restart();
        return;

    error:
        _userInput.clear();
    empty:
        _offset = 0.f;
        _cursorPositionInString = _cursorPositionOnScreen = 0;
        return;
    }

    void    KeyboardImpl::_UpdateScrollInfos(void)
    {
        const char *cstr = _userInput.c_str();
        const float strWidth = Renderer::GetTextSize(cstr);
        // Get units untils current cursor position
        const int unitsToCursor = UnitsToAdvance(cstr, _cursorPositionInString);

        // Weird character, might as well purge the string
        if (unitsToCursor < 0)
        {
            _offset = 0.f;
            _cursorPositionInString = _cursorPositionOnScreen = 0;
            _userInput.clear();
            return;
        }

        // Get width of both before and after cursor
        const float after = Renderer::GetTextSize(cstr + unitsToCursor);
        const float before = strWidth - after;

        // Compute offsets
        if (strWidth > 260.f)
        {
            if (before > 140.f)
                _offset = before + std::min(after, 140.f) - 260.f;
            else
                _offset = 0.f;
            _cursorPositionOnScreen = before - _offset;
        }
        else
        {
            _offset = 0;
            _cursorPositionOnScreen = before;
        }
    }

    enum
    {
        CLEAR_NOT_PRESSED = 0,
        CLEAR_JUST_PRESSED,
        CLEAR_QUICK_MODE
    };
    bool    KeyboardImpl::_CheckKeys(void)
    {
        static Clock    backspacetimer;
        static u32      backspaceFastMode = CLEAR_NOT_PRESSED;
        static Time     FastModeWaitTime = Seconds(0.5f);
        static Time     FastClearingFrame = Seconds(0.1f);

        int start, end;
        std::tie(start, end) = _GetEffectiveKeyRange();

        // Check cursor position, just in case
        if (_cursorPositionInString > static_cast<int>(_userInput.size()))
            _ScrollUp();

        for (int i = start; i < end; i++)
        {
            std::string  temp;

            int ret = (*_keys)[i](temp);

            if (ret != -1)
            {
                if (ret == ~Keyboard::SpecialCodePoints::KEY_BACKSPACE)
                {
                    backspaceFastMode = CLEAR_NOT_PRESSED;
                    goto _backspacePressed;
                }
                if (ret == Keyboard::SpecialCodePoints::KEY_BACKSPACE)
                {
                    if (backspaceFastMode == CLEAR_NOT_PRESSED)
                    {
                        backspaceFastMode = CLEAR_JUST_PRESSED;
                        backspacetimer.Restart();
                        return (false);
                    }
                    if (backspaceFastMode == CLEAR_JUST_PRESSED)
                    {
                        if (!backspacetimer.HasTimePassed(FastModeWaitTime))
                            return (false);
                        backspaceFastMode = CLEAR_QUICK_MODE;
                    }
                    if (backspaceFastMode == CLEAR_QUICK_MODE)
                    {
                        if (!backspacetimer.HasTimePassed(FastClearingFrame))
                            return (false);
                    }
                    backspacetimer.Restart();
                    goto _backspacePressed;
                }

                // Reset backspace state if any other key is pressed
                backspaceFastMode = CLEAR_NOT_PRESSED;

                if (ret == TouchKey::KEYCODE_STRING)
                {
                    if ((_layout == DECIMAL && _userInput.size() >= 18)
                        || (_layout == QWERTY && _max && Utils::GetSize(_userInput) >= _max))
                        return (false);

                    _ClearKeyboardEvent();
                    _KeyboardEvent.type = KeyboardEvent::CharacterAdded;
                    decode_utf8(&_KeyboardEvent.codepoint, (const u8 *)temp.c_str());

                    if (_KeyboardEvent.codepoint == 0x00B1) // ± key
                    {
                        if (_userInput[0] == '-')
                        {
                            _ScrollDown();
                            _userInput.erase(0, 1);
                        }
                        else
                        {
                            _userInput.insert(_userInput.begin(), '-');
                            _ScrollUp();
                        }
                    }
                    else
                    {
                        // [JP] かな入力中は未確定文字列に追加
                        if (_useKana)
                        {
                            if (_converting)
                                _CommitComposition();
                            if (_compStart < 0)
                            {
                                _compStart = _cursorPositionInString;
                                _compLen = 0;
                            }
                        }
                        else if (_compStart >= 0)
                            _CommitComposition();

                        _userInput.insert(_cursorPositionInString, temp);
                        if (_useKana)
                        {
                            _compLen += temp.size();
                            _UpdateSuggestions(); // [JP] 予測変換
                        }
                        _ScrollUp();
                    }
                    return (true);
                }
                if (ret == Keyboard::SpecialCodePoints::KEY_ENTER)
                {
                    // [JP] 未確定文字列があれば確定だけ
                    if (_compStart >= 0)
                    {
                        _CommitComposition();
                        return (false);
                    }
                    _askForExit = true;
                    return (false);
                }
                // [JP] かな入力中: 空白キーで変換
                else if (ret == Keyboard::SpecialCodePoints::KEY_SPACE && _useKana && _compStart >= 0 && _compLen > 0)
                {
                    _Convert(1);
                    return (true);
                }
                else if (ret == Keyboard::SpecialCodePoints::KEY_SPACE && (!_max || Utils::GetSize(_userInput) < _max))
                {
                    _userInput.insert(_cursorPositionInString, " ");
                    _ClearKeyboardEvent();
                    _KeyboardEvent.type = KeyboardEvent::CharacterAdded;
                    _KeyboardEvent.codepoint = ' ';
                    _ScrollUp();
                    return (true);
                }
                else if (ret == Keyboard::SpecialCodePoints::KEY_CAPS)
                {
                    _useCaps = !_useCaps;
                    if (_selectedKey) {
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                    }
                }
                // [JP] 英字 ⇔ かな
                else if (ret == KEY_KANA)
                {
                    _CommitComposition();
                    _useKana = !_useKana;
                    _useSymbols = false;
                    _useNintendo = false;
                    _useCaps = false;
                    _symbolsPage = 0;
                    _nintendoPage = 0;
                    _kanaPage = 0;
                    if (_selectedKey)
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                }
                else if (ret == KEY_KANA_TYPE)
                {
                    _useKatakana = !_useKatakana;
                    if (_selectedKey)
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                }
                else if (ret == KEY_KANA_PAGE)
                {
                    _kanaPage = !_kanaPage;
                    if (_selectedKey)
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                }
                else if (ret == KEY_DAKUTEN)
                {
                    return (_ApplyDakuten());
                }
                else if (ret == KEY_COLOR)
                {
                    return (_InsertColor());
                }
                else if (ret == Keyboard::SpecialCodePoints::KEY_SMILEY)
                {
                    _useNintendo = !_useNintendo;
                    _useKana = false; _CommitComposition(); // [JP]
                    _useSymbols = false;
                    _useCaps = false;
                    if (!_useNintendo)
                        _nintendoPage = 0;
                    _symbolsPage = 0;
                    if (_selectedKey) {
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                    }
                }
                else if (ret == Keyboard::SpecialCodePoints::KEY_SYMBOLS)
                {
                    _useSymbols = !_useSymbols;
                    _useKana = false; _CommitComposition(); // [JP]
                    _useNintendo = false;
                    _useCaps = false;
                    if (!_useSymbols)
                        _symbolsPage = 0;
                    _nintendoPage = 0;
                    if (_selectedKey) {
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                    }
                }
                else if (ret == Keyboard::SpecialCodePoints::KEY_SYMBOLS_PAGE)
                {
                    _symbolsPage = !_symbolsPage;
                    if (_selectedKey) {
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                    }
                }
                else if (ret == Keyboard::SpecialCodePoints::KEY_NINTENDO_PAGE)
                {
                    _nintendoPage = !_nintendoPage;
                    if (_selectedKey) {
                        _CalcSelect(SelectMode::CLOSEST, _lastSelectPos.first, _lastSelectPos.second);
                    }
                }
                else
                {
                    if (_layout == DECIMAL && _userInput.length() >= 18)
                        return (false);

                    if (_layout != Layout::QWERTY &&_cursorPositionInString == 0 && ret == '.')
                    {
                        _userInput.insert(0, "0.");
                        _ScrollUp();
                        _ScrollUp(); ///< Yeah I know, I'm f*cking lazy
                    }
                    else if (_max == 0 || Utils::GetSize(_userInput) < _max)
                    {
                        if (_compStart >= 0) _CommitComposition(); // [JP]
                        temp.clear();
                        temp += ret;
                        _userInput.insert(_cursorPositionInString, temp);
                        _ClearKeyboardEvent();
                        _KeyboardEvent.type = KeyboardEvent::CharacterAdded;
                        _KeyboardEvent.codepoint = ret;
                        _ScrollUp();
                    }
                    return (true);
                }
            }
        }

        backspaceFastMode = CLEAR_NOT_PRESSED;
        return (false);

    _backspacePressed:
        return _Backspace();
    }

    bool    KeyboardImpl::_CheckInput(void)
    {
        if (_layout == QWERTY)
        {
            if (_compare != nullptr)
                return (_compare((void *)&_userInput, _error));
            return (true);
        }

        // In case there's no convert function, always consider input as valid
        if (_convert == nullptr)
            return (true);

        // Always call convert function, can avoid overflow
        void *convertedInput = _convert(_userInput, _isHex);

        // In case there's no callback, always consider input as valid
        if (_compare == nullptr)
            return (true);

        return (_compare(convertedInput, _error));
    }

    bool    KeyboardImpl::_CheckButtons(int &ret)
    {
        bool res = false;
        for (int i = 0; i < static_cast<int>(_strKeys.size()); i++)
        {
            ret = (*_strKeys[i])();
            if (ret != -1)
            {
                ret = i;
                res = true;
                break;
            }
        }
        if (_userSelectedKey)
        {
            if (_manualKey != -1 && _strKeys[_manualKey]->CanUse()) {
                ret = _manualKey; res = true;
            }
            _userSelectedKey = false;
        }
        return res;
    }

    void    KeyboardImpl::_HandleManualKeyPress(Key key)
    {
        _inertialVelocity = 0;
        if (_isIconKeyboard)
        {
            if (_manualKey == -1)
            {
                if (key & (Key::Down | Key::Left))
                {
                    int tempKey = _displayScrollbar ? _currentPosition : 0;
                    if (!_strKeys[tempKey]->CanUse())
                        tempKey = 0;
                    _ChangeManualKey(tempKey);
                }
                else if (key & (Key::Up | Key::Right))
                {
                    int tempKey = _displayScrollbar ? std::min((int)_strKeys.size() - 1, _currentPosition + 20) : _strKeys.size() - 1;
                    if (!_strKeys[tempKey]->CanUse())
                        tempKey = (int)_strKeys.size() - 1;
                    _ChangeManualKey(tempKey);
                }
                else return;
            }
            if (key & (Key::Down | Key::Up | Key::Left | Key::Right))
            {
                if (key & Key::Down)
                {
                    int orig = _manualKey;
                    int tempKey = _manualKey;

                    do
                    {
                        tempKey += 4;
                    } while (tempKey < static_cast<int>(_strKeys.size()) && !_strKeys[tempKey]->CanUse() && tempKey - orig < 16);

                    if (tempKey >= static_cast<int>(_strKeys.size()) || tempKey - orig >= 16)
                        tempKey = orig;

                    _ChangeManualKey(tempKey);
                }
                else if (key & Key::Up)
                {
                    int orig = _manualKey;
                    int tempKey = _manualKey;

                    do
                    {
                        tempKey -= 4;
                    } while (tempKey > 0 && !_strKeys[tempKey]->CanUse() && orig - tempKey < 16);

                    if (tempKey < 0 || orig - tempKey >= 16)
                        tempKey = orig;

                    _ChangeManualKey(tempKey);
                }
                else if (key & Key::Right)
                {
                    int orig = _manualKey;
                    int tempKey = _manualKey;

                    do
                    {
                        tempKey++;
                    } while (tempKey < static_cast<int>(_strKeys.size()) && ((u32)tempKey & 3) != 0 && !_strKeys[tempKey]->CanUse());

                    if (tempKey >= static_cast<int>(_strKeys.size()) || ((u32)tempKey & 3) == 0)
                        tempKey = orig;

                    _ChangeManualKey(tempKey);
                }
                else if (key & Key::Left)
                {
                    int orig = _manualKey;
                    int tempKey = _manualKey;

                    do
                    {
                        tempKey--;
                    } while (tempKey > 0 && ((u32)tempKey & 3) != 3 && !_strKeys[tempKey]->CanUse());

                    if (tempKey < 0 || ((u32)tempKey & 3) == 3)
                        tempKey = orig;

                    _ChangeManualKey(tempKey);
                }

                if (_displayScrollbar)
                {
                    int keyRow = _manualKey / 4;
                    int positionRow = _currentPosition / 4;

                    if (keyRow > positionRow + 6 - 2)
                    {
                        positionRow = keyRow - 4;
                        _scrollSize = ((positionRow * 36.01f) + 15) / _scrollJump - _scrollPosition;
                        _manualScrollUpdate = true;
                        _UpdateScroll(0.f, true);
                    }
                    else if (keyRow < positionRow + 1)
                    {
                        positionRow = std::max(keyRow, 0);
                        _scrollSize = ((positionRow * 36.01f) - 15) / _scrollJump - _scrollPosition;
                        _manualScrollUpdate = true;
                        _UpdateScroll(0.f, true);
                    }
                }
            }
        }
        else
        {
            if (_manualKey == -1)
            {
                if (key & Key::Down)
                {
                    int tempKey = _displayScrollbar ? _currentPosition : 0;
                    if (!_strKeys[tempKey]->CanUse()) {
                        tempKey = 0;
                        if (_strKeys[tempKey]->CanUse()) {
                            _ChangeManualKey(tempKey);
                        }
                    } else {
                        _ChangeManualKey(tempKey);
                    }

                } else if (key & Key::Up)
                {
                    int tempKey = _displayScrollbar ? std::min((int)_strKeys.size() - 1, _currentPosition + 5) : _strKeys.size() - 1;
                    if (!_strKeys[tempKey]->CanUse()) {
                        tempKey = (int)_strKeys.size() - 1;
                        if (_strKeys[tempKey]->CanUse()) {
                            _ChangeManualKey(tempKey);
                        }
                    } else {
                        _ChangeManualKey(tempKey);
                    }
                }
                else return;
            }
            if (key & (Key::Down | Key::Up))
            {
                if (key & Key::Down)
                {
                    int orig = _manualKey;
                    int tempKey = _manualKey;

                    do {
                        tempKey++;
                    } while (tempKey < static_cast<int>(_strKeys.size()) && !_strKeys[tempKey]->CanUse() && tempKey - orig < 4);

                    if (tempKey >= static_cast<int>(_strKeys.size()) || tempKey - orig >= 4)
                        tempKey = orig;

                    _ChangeManualKey(tempKey);
                }
                else if (key & Key::Up)
                {
                    int orig = _manualKey;
                    int tempKey = _manualKey;

                    do {
                        tempKey--;
                    } while (tempKey > 0 && !_strKeys[tempKey]->CanUse() && orig - tempKey < 4);

                    if (tempKey < 0 || orig - tempKey >= 4)
                        tempKey = orig;

                    _ChangeManualKey(tempKey);
                }

                if (_displayScrollbar)
                {
                    int keyRow = _manualKey;
                    int positionRow = _currentPosition;

                    if (keyRow > positionRow + 6 - 2)
                    {
                        positionRow = keyRow - 4;
                        _scrollSize = ((positionRow * 36.01f) + 15) / _scrollJump - _scrollPosition;
                        _manualScrollUpdate = true;
                        _UpdateScroll(0.f, true);
                    }
                    else if (keyRow < positionRow + 1)
                    {
                        positionRow = std::max(keyRow, 0);
                        _scrollSize = ((positionRow * 36.01f) - 15) / _scrollJump - _scrollPosition;
                        _manualScrollUpdate = true;
                        _UpdateScroll(0.f, true);
                    }
                }
            }
        }

        if (key == A) {
            if (_manualKey != -1 && _strKeys[_manualKey]->CanUse())
                SoundEngine::PlayMenuSound(_strKeys[_manualKey]->GetAcceptSoundEvent());
            _userSelectedKey = true;
        }
    }

    void KeyboardImpl::_ClearKeyboardEvent()
    {
        _KeyboardEvent.codepoint = 0;
        _KeyboardEvent.selectedIndex = -1;
        _KeyboardEvent.affectedKey = (Key)0;
        _KeyboardEvent.renderInterface = nullptr;
    }

    void    KeyboardImpl::_ChangeManualKey(int newVal, bool playSound)
    {
        _manualKey = newVal;
        static bool preventRecursion = false;
        if (_manualKey != _prevManualKey && !preventRecursion && playSound)
            SoundEngine::PlayMenuSound(SoundEngine::Event::CURSOR);
        if (_onKeyboardEvent != nullptr && _owner != nullptr && _manualKey != _prevManualKey && !preventRecursion) {
            preventRecursion = true;
            _ClearKeyboardEvent();
            _KeyboardEvent.type = KeyboardEvent::EventType::SelectionChanged;
            _KeyboardEvent.selectedIndex = _manualKey;
            _onKeyboardEvent(*_owner, _KeyboardEvent);
            preventRecursion = false;
        }
        _prevManualKey = _manualKey;
    }

    std::pair<int, int> KeyboardImpl::_GetEffectiveKeyRange() {
        int start = 0;
        int end = _keys->size();

        // [JP] ページの範囲は s_pageStart から求める (キーを追加したため)
        if (_layout == Layout::QWERTY)
        {
            int page;
            if (_useKana)           page = (_useKatakana ? PG_KATA1 : PG_HIRA1) + (_kanaPage ? 1 : 0);
            else if (_useSymbols)   page = _symbolsPage ? PG_SYM2 : PG_SYM1;
            else if (_useNintendo)  page = _nintendoPage ? PG_NIN2 : PG_NIN1;
            else                    page = _useCaps ? PG_UP : PG_LOW;
            start = s_pageStart[page];
            end = s_pageStart[page + 1];
        }

        return std::make_pair(start, end);
    }

    void    KeyboardImpl::_Select(TouchKey* key)
    {
        if (_selectedKey != nullptr)
        {
            _selectedKey->Select(false);
        }
        _selectedKey = key;
        if (_selectedKey != nullptr)
        {
            _selectedKey->Select(true);
            _lastSelectPos = _selectedKey->GetSidePosition(TouchKey::Side::CENTER);
        }
    }

    void    KeyboardImpl::_CalcSelect(SelectMode mode, u16 origPosX, u16 origPosY)
    {
        int start, end;
        std::tie(start, end) = _GetEffectiveKeyRange();

        switch (mode)
        {
        case SelectMode::CLOSEST:
        {
            auto el = std::min_element(_keys->begin() + start, _keys->begin() + end, [origPosX, origPosY](const TouchKey& a, const TouchKey& b) {
                u16 posAx, posAy, posBx, posBy;
                std::tie(posAx, posAy) = a.GetSidePosition(TouchKey::Side::CENTER);
                std::tie(posBx, posBy) = b.GetSidePosition(TouchKey::Side::CENTER);
                float distAx = (s16)posAx - (s16)origPosX, distAy = (s16)posAy - (s16)origPosY;
                float distBx = (s16)posBx - (s16)origPosX, distBy = (s16)posBy - (s16)origPosY;
                return (distAx * distAx) + (distAy * distAy) < (distBx * distBx) + (distBy * distBy);
            });
            _Select(&(*el));
        }
        break;

        case SelectMode::UP:
        {
            TouchKey* closest = nullptr;
            float minDist = 100000000.f;
            for (auto key = _keys->begin() + start; key != _keys->begin() + end; key++)
            {
                u16 posX, posY;
                std::tie(posX, posY) = key->GetSidePosition(TouchKey::Side::BOTTOM);
                if (posY > origPosY)
                    continue;
                float distX = (s16)posX - (s16)origPosX;
                float distY = (s16)posY - (s16)origPosY;
                float dist = distX * distX + distY * distY;
                if (dist < minDist) {
                    minDist = dist;
                    closest = &(*key);
                }
            }
            if (closest) _Select(closest);
        }
        break;

        case SelectMode::DOWN:
        {
            TouchKey* closest = nullptr;
            float minDist = 100000000.f;
            for (auto key = _keys->begin() + start; key != _keys->begin() + end; key++)
            {
                u16 posX, posY;
                std::tie(posX, posY) = key->GetSidePosition(TouchKey::Side::TOP);
                if (posY < origPosY)
                    continue;
                float distX = (s16)posX - (s16)origPosX;
                float distY = (s16)posY - (s16)origPosY;
                float dist = distX * distX + distY * distY;
                if (dist < minDist) {
                    minDist = dist;
                    closest = &(*key);
                }
            }
            if (closest) _Select(closest);
        }
        break;

        case SelectMode::RIGHT:
        {
            TouchKey* closest = nullptr;
            float minDist = 100000000.f;
            for (auto key = _keys->begin() + start; key != _keys->begin() + end; key++)
            {
                u16 posX, posY;
                std::tie(posX, posY) = key->GetSidePosition(TouchKey::Side::LEFT);
                if (posX < origPosX)
                    continue;
                float distX = (s16)posX - (s16)origPosX;
                float distY = (s16)posY - (s16)origPosY;
                float dist = distX * distX + distY * distY;
                if (dist < minDist) {
                    minDist = dist;
                    closest = &(*key);
                }
            }
            if (closest) _Select(closest);
        }
        break;

        case SelectMode::LEFT:
        {
            TouchKey* closest = nullptr;
            float minDist = 100000000.f;
            for (auto key = _keys->begin() + start; key != _keys->begin() + end; key++)
            {
                u16 posX, posY;
                std::tie(posX, posY) = key->GetSidePosition(TouchKey::Side::RIGHT);
                if (posX > origPosX)
                    continue;
                float distX = (s16)posX - (s16)origPosX;
                float distY = (s16)posY - (s16)origPosY;
                float dist = distX * distX + distY * distY;
                if (dist < minDist) {
                    minDist = dist;
                    closest = &(*key);
                }
            }
            if (closest) _Select(closest);
        }
        break;

        default:
            break;
        }
    }

    bool    KeyboardImpl::_Backspace()
    {
        // [JP] 変換中は読みに戻す
        if (_converting)
        {
            _CancelConversion();
            return (true);
        }
        // [JP] 色コード (0x1B r g b) / 色リセット (0x18) はまとめて消す
        if (_cursorPositionInString >= 4 && _userInput[_cursorPositionInString - 4] == 0x1B)
        {
            _userInput.erase(_cursorPositionInString - 4, 4);
            _cursorPositionInString -= 4;
            _blinkingClock.Restart();
            return (true);
        }
        if (_cursorPositionInString >= 1 && _userInput[_cursorPositionInString - 1] == 0x18)
        {
            _userInput.erase(_cursorPositionInString - 1, 1);
            _cursorPositionInString -= 1;
            _blinkingClock.Restart();
            return (true);
        }

        const int sizeBefore = _userInput.size();
        std::string &&right = _userInput.substr(_cursorPositionInString);
        _userInput.erase(_cursorPositionInString);
        _ScrollDown(); ///< Scroll down before removing the char
        _ClearKeyboardEvent();
        _KeyboardEvent.codepoint = Utils::RemoveLastChar(_userInput);
        _userInput += right;

        // [JP] 未確定文字列の長さを合わせる
        if (_compStart >= 0)
        {
            _compLen -= sizeBefore - static_cast<int>(_userInput.size());
            if (_compLen <= 0 || _cursorPositionInString < _compStart)
                _CommitComposition();
            else
                _UpdateSuggestions();
        }

        if (_KeyboardEvent.codepoint != 0)
        {
            _KeyboardEvent.type = KeyboardEvent::CharacterRemoved;
            return (true);
        }
        return (false);
    }

    // ---------------------------------------------------------------
    //  [JP] 日本語入力
    // ---------------------------------------------------------------
    void    KeyboardImpl::_CommitComposition(void)
    {
        _compStart = -1;
        _compLen = 0;
        _converting = false;
        _convIndex = 0;
        _convReading.clear();
        _convCands.clear();
    }

    void    KeyboardImpl::_ReplaceComposition(const std::string &str)
    {
        if (_compStart < 0 || _compStart + _compLen > static_cast<int>(_userInput.size()))
        {
            _CommitComposition();
            return;
        }
        _userInput.replace(_compStart, _compLen, str);
        _compLen = str.size();
        _cursorPositionInString = _compStart + _compLen;
        _blinkingClock.Restart();
    }

    // 予測変換の候補: 一致する変換 → ひらがな → カタカナ → 予測 (よみが長い語)
    void    KeyboardImpl::_UpdateSuggestions(void)
    {
        _convCands.clear();
        _candFirst = 0;
        if (_converting || _compStart < 0 || _compLen <= 0
            || _compStart + _compLen > static_cast<int>(_userInput.size()))
            return;

        const std::string reading = _userInput.substr(_compStart, _compLen);
        const std::string hira = JpInput::ShiftKana(reading, false);

        std::vector<std::string> exact, predict;
        JpInput::Predict(hira, exact, predict);

        auto addUnique = [this](const std::string &str)
        {
            for (const std::string &c : _convCands)
                if (c == str) return;
            _convCands.push_back(str);
        };
        for (const std::string &c : exact)   addUnique(c);
        addUnique(hira);
        addUnique(JpInput::ShiftKana(reading, true));
        for (const std::string &c : predict) addUnique(c);
    }

    void    KeyboardImpl::_Convert(int direction)
    {
        if (_compStart < 0 || _compLen <= 0)
            return;

        if (!_converting)
        {
            if (_convCands.empty())
                _UpdateSuggestions();
            if (_convCands.empty())
                return;
            _Select(nullptr); // A で候補を確定するので、キーの選択を外しておく
            _convReading = _userInput.substr(_compStart, _compLen);
            _convIndex = 0;
            _converting = true;
        }
        else
        {
            const int n = _convCands.size();
            _convIndex = ((_convIndex + direction) % n + n) % n;
        }
        _ReplaceComposition(_convCands[_convIndex]);
    }

    void    KeyboardImpl::_CancelConversion(void)
    {
        if (!_converting)
            return;
        _converting = false;
        _ReplaceComposition(_convReading);
        _convIndex = 0;
        // 候補の一覧はそのまま (よみが同じなので)
    }

    // 候補をタッチして確定
    void    KeyboardImpl::_ChooseCandidate(int index)
    {
        if (index < 0 || index >= static_cast<int>(_convCands.size()) || _compStart < 0)
            return;
        SoundEngine::PlayMenuSound(SoundEngine::Event::ACCEPT);
        const std::string cand = _convCands[index];
        _converting = false;
        _ReplaceComposition(cand);
        _CommitComposition();
        _errorMessage = !_CheckInput();
    }

    // 入力エリアとキーボードの間の予測変換バー
    void    KeyboardImpl::_RenderSuggestionBar(void)
    {
        static auto &theme = Preferences::Settings.Keyboard;
        const Color gray(128, 128, 128);
        const Color &textColor = Preferences::Settings.MainTextColor;

        const int barY = SUGGEST_BAR_Y;
        Renderer::DrawRect(IntRect(20, barY, 280, SUGGEST_BAR_H), theme.Background);
        Renderer::DrawLine(22, barY + SUGGEST_BAR_H - 1, 276, gray, 1);
        _candHit.clear();
        _candHasMore = false;

        const int n = _convCands.size();
        if (n == 0)
            return;

        const int leftX = 36, rightX = 284;
        auto itemWidth = [this](int i) { return static_cast<int>(Renderer::GetTextSize(_convCands[i].c_str())) + 10; };

        // 変換中は選択中の候補が見えるようにずらす
        if (_candFirst >= n) _candFirst = 0;
        if (_converting && _convIndex >= 0)
        {
            if (_convIndex < _candFirst)
                _candFirst = _convIndex;
            while (_candFirst < _convIndex)
            {
                int x = leftX;
                for (int i = _candFirst; i <= _convIndex; i++)
                    x += itemWidth(i);
                if (x <= rightX) break;
                _candFirst++;
            }
        }

        int x = leftX;
        for (int i = _candFirst; i < n; i++)
        {
            int w = itemWidth(i);
            if (x + w > rightX)
            {
                if (i == _candFirst) w = rightX - x; // 長すぎる候補は切って表示
                else { _candHasMore = true; break; }
            }
            if (_converting && i == _convIndex)
                Renderer::DrawRect(IntRect(x, barY + 1, w, SUGGEST_BAR_H - 2), Color(0, 90, 200));
            int ty = barY + 2;
            Renderer::DrawSysString(_convCands[i].c_str(), x + 5, ty, x + w, textColor);
            Renderer::DrawLine(x + w, barY + 4, 1, gray, SUGGEST_BAR_H - 8);
            _candHit.push_back(std::make_pair(x, x + w));
            x += w;
        }

        int ty = barY + 2;
        if (_candFirst > 0)
            Renderer::DrawSysString("\u2190", 23, ty, leftX, textColor);         // ←
        ty = barY + 2;
        if (_candHasMore)
            Renderer::DrawSysString("\u2192", rightX + 2, ty, 300, textColor);   // →
    }

    // カーソル直前の文字を ゛゜小 の順に切り替える
    bool    KeyboardImpl::_ApplyDakuten(void)
    {
        if (_converting)
            _CancelConversion();

        const int cur = _cursorPositionInString;
        if (cur < 3 || cur > static_cast<int>(_userInput.size()))
            return (false);

        const u8 *p = reinterpret_cast<const u8 *>(_userInput.c_str()) + cur - 3;
        if ((p[0] & 0xF0) != 0xE0)
            return (false);

        u32 code = 0;
        if (decode_utf8(&code, p) != 3)
            return (false);

        const u32 next = JpInput::NextDakuten(code);
        if (next == 0)
            return (false);

        std::string rep;
        JpInput::AppendUtf8(rep, next);
        if (rep.size() != 3)
            return (false);

        _userInput.replace(cur - 3, 3, rep);
        if (_compStart >= 0 && cur > _compStart)
            _UpdateSuggestions();
        return (true);
    }

    // 文字色の挿入 (アクションリプレイのコード名の色付けなど)
    bool    KeyboardImpl::_InsertColor(void)
    {
        static u32 lastColor = 0xFF5C5F; // 前回カラーピッカーで選んだ色
        struct Preset { const char *name; u8 r, g, b; };
        static const Preset presets[] =
        {
            { "赤",       0xFF, 0x00, 0x00 }, { "オレンジ", 0xFF, 0x8F, 0x00 },
            { "黄",       0xFF, 0xE7, 0x00 }, { "緑",       0x01, 0xFF, 0x01 },
            { "水色",     0x00, 0xF7, 0xFF }, { "青",       0x01, 0x88, 0xFF },
            { "紫",       0xC9, 0x00, 0xFF }, { "ピンク",   0xFF, 0x01, 0xE9 },
            { "白",       0xFF, 0xFF, 0xFF }, { "灰",       0x96, 0x96, 0x96 },
        };

        std::vector<std::string> options;
        options.push_back(JpInput::ColorEsc(lastColor >> 16, lastColor >> 8, lastColor) + "\u25A0" + std::string("\x18") + " カラーピッカーで指定");
        options.push_back("色を戻す (ここから通常の色)");
        for (const Preset &p : presets)
            options.push_back(JpInput::ColorEsc(p.r, p.g, p.b) + "\u25A0 " + p.name);

        KeyboardImpl list("文字の色を選択\n\nカーソルの位置から後ろの文字に色が付きます。");
        list.Populate(options, true);
        const int choice = list.Run();
        if (choice < 0)
            return (false);

        std::string code;
        if (choice == 0)
        {
            u32 rgb = lastColor;
            if (!JpInput::PickColor("文字の色を選択", rgb))
                return (false);
            lastColor = rgb;
            code = JpInput::ColorEsc(rgb >> 16, rgb >> 8, rgb);
        }
        else if (choice == 1)
            code = "\x18";
        else
        {
            const Preset &p = presets[choice - 2];
            code = JpInput::ColorEsc(p.r, p.g, p.b);
        }

        _CommitComposition();
        if (_cursorPositionInString > static_cast<int>(_userInput.size()))
            _cursorPositionInString = _userInput.size();
        _userInput.insert(_cursorPositionInString, code);
        _cursorPositionInString += code.size();
        _blinkingClock.Restart();
        return (true);
    }

    // 上画面: 操作ヒント (候補は下画面の予測変換バーに表示)
    void    KeyboardImpl::_RenderCandidates(int posX, int maxX, int maxY)
    {
        const Color gray(128, 128, 128);
        int y = maxY - 20;
        if (_converting)
            Renderer::DrawSysString("\uE07B\uE07C:候補を選択  \uE000:確定  \uE001:よみに戻す", posX, y, maxX, gray);
        else if (_compStart >= 0)
            Renderer::DrawSysString("候補をタッチで確定  空白/変換:変換  \u21B5:かなのまま確定", posX, y, maxX, gray);
    }

    // WIll only be used in the hex editor, so no need to do a full implementation
    bool    KeyboardImpl::operator()(int &out)
    {
        _Update(0.f);

        if (!_CheckKeys())
            return (false);

        if (_userInput.size())
        {
            out = (int)_userInput[0];
            if (out >= '0' && out <= '9')
                out -= '0';
            if (out >= 'A' && out <= 'F')
                out = 10 + out - 'A';
            _userInput.pop_back();
            return (true);
        }
        return (false);
    }

    void    KeyboardImpl::CanChangeLayout(bool canChange)
    {
        _canChangeLayout = canChange;
    }


}
