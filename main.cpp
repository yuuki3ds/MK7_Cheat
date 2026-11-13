// MK7_Cheat Ver.6.23 / MK7_CheatUS Ver.2.01 / MK7_CheatEU Ver.1.01 By yk
//   ->  CTRPluginFramework (.3gx) 移植版  Plugin Ver.0.0.1
// 対象: マリオカート7 JPN(0004000000030600) / EUR(0004000000030700) / USA(0004000000030800)
//       USA・EUR はコード名が一致するものだけ (起動時に自動判定)
//
// 変換方針
//  - 常時書き込みコード        : エントリON中は毎フレーム書き込み(ARと同じ)
//  - Y+→ / Y+← のON/OFFコード : メニューのチェックでON、外すとOFF値を書き戻す
//  - 影の色 / 光の色 / 髪色     : エントリのギアから色を選択、外すと元に戻す
//  - ボタン条件付き(DD)コード  : 元と同じボタン操作をそのまま再現

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstdio>
#include <string>
#include <vector>
#include "ItemRain.hpp"

namespace CTRPluginFramework
{
    // ---------------------------------------------------------------
    //  ヘルパー
    // ---------------------------------------------------------------
    struct Patch  { u32 addr; u32 value; };
    struct Toggle { u32 addr; u32 on; u32 off; };

    static inline void W32(u32 a, u32 v) { Process::Write32(a, v); }
    static inline void W16(u32 a, u16 v) { Process::Write16(a, v); }
    static inline u32  R32(u32 a)        { u32 v = 0; Process::Read32(a, v); return v; }
    static inline bool Held(u32 keys)    { return Controller::IsKeysDown(keys); }

    template <size_t N>
    static void WriteAll(const Patch (&p)[N])
    {
        for (const auto &x : p) W32(x.addr, x.value);
    }

    // ON中はon値、エントリを外した時(最後の1回の呼び出し)はoff値を書く
    template <size_t N>
    static void ApplyToggle(MenuEntry *e, const Toggle (&p)[N])
    {
        const bool on = e->IsActivated();
        for (const auto &x : p) W32(x.addr, on ? x.on : x.off);
    }

    static std::string Col(u32 rgb, const std::string &s)
    {
        return Color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF) << s;
    }

    // 3DSシステムフォントのボタン記号 (txtのノートと同じ文字)
    #define BTN_A     "\uE000"
    #define BTN_B     "\uE001"
    #define BTN_X     "\uE002"
    #define BTN_Y     "\uE003"
    #define BTN_L     "\uE004"
    #define BTN_R     "\uE005"
    #define BTN_UP    "\uE079"
    #define BTN_DOWN  "\uE07A"
    #define BTN_LEFT  "\uE07B"
    #define BTN_RIGHT "\uE07C"

    // ---------------------------------------------------------------
    //  カラーピッカー (rgb は 0xRRGGBB)
    //  下画面: 色相バー + R/G/B バー (十字キー / タッチ)
    //  上画面: 変更前 / 変更後 のプレビュー
    // ---------------------------------------------------------------
    static inline Color ToColor(u32 rgb)
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
        int h = h6 / 6;
        return h > 255 ? 255 : h;
    }

    static void WaitAllReleased()
    {
        do {
            Controller::Update();
            svcSleepThread(16666667LL);
        } while (Controller::GetKeysDown() != 0 || Touch::IsDown());
    }

    static bool ColorPicker(const std::string &title, u32 &rgb)
    {
        const u32 before = rgb & 0xFFFFFF;
        int val[4];                          // 0:色相 1:R 2:G 3:B
        val[1] = (before >> 16) & 0xFF;
        val[2] = (before >> 8) & 0xFF;
        val[3] = before & 0xFF;
        val[0] = RGBToHue(before);
        if (val[0] < 0) val[0] = 0;

        const char *rowName[4] = { "色相", "R", "G", "B" };
        const int   rowY[4]    = { 40, 82, 124, 166 };
        const int   BX = 34, BW = 256, BH = 20;
        const int   okX = 30, ngX = 170, btnY = 206, btnW = 120, btnH = 28;

        int row = 1, hold = 0;
        WaitAllReleased();

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

        while (true)
        {
            Controller::Update();
            const u32 cur = (val[1] << 16) | (val[2] << 8) | val[3];

            // ---- 入力 ----
            if (Controller::IsKeyPressed(Key::A)) { rgb = cur; break; }
            if (Controller::IsKeyPressed(Key::B)) { WaitAllReleased(); return false; }
            if (Controller::IsKeyPressed(Key::DPadUp))   row = (row + 3) % 4;
            if (Controller::IsKeyPressed(Key::DPadDown)) row = (row + 1) % 4;

            int dir = 0;
            if (Controller::IsKeyDown(Key::DPadRight))     dir = 1;
            else if (Controller::IsKeyDown(Key::DPadLeft)) dir = -1;
            if (dir)
            {
                int step = 0;
                if (hold == 0)      step = 1;            // 1回押し
                else if (hold > 45) step = 4;            // 長押しで加速
                else if (hold > 15) step = 1;            // 長押しで連続
                hold++;
                if (step) setRow(row, val[row] + dir * step);
            }
            else hold = 0;

            if (Touch::IsDown())
            {
                const UIntVector p = Touch::GetPosition();
                const int x = p.x, y = p.y;
                for (int i = 0; i < 4; i++)
                {
                    if (y >= rowY[i] - 6 && y < rowY[i] + BH + 6 && x >= BX - 8 && x < BX + BW + 8)
                    {
                        row = i;
                        setRow(i, x - BX);
                    }
                }
                if (y >= btnY && y < btnY + btnH)
                {
                    if (x >= okX && x < okX + btnW) { rgb = cur; break; }
                    if (x >= ngX && x < ngX + btnW) { WaitAllReleased(); return false; }
                }
            }

            // ---- 下画面 ----
            const Screen &bot = OSD::GetBottomScreen();
            bot.DrawRect(0, 0, 320, 240, Color::Black);
            bot.DrawSysfont(title, 6, 6, Color::White);

            for (int i = 0; i < 4; i++)
            {
                const int y = rowY[i];
                bot.DrawSysfont(rowName[i], 4, y + 2, i == row ? Color::Yellow : Color::White);

                for (int x = 0; x < BW; x++)
                {
                    u32 c;
                    if (i == 0)      c = HueToRGB(x);
                    else if (i == 1) c = (x << 16) | (val[2] << 8) | val[3];
                    else if (i == 2) c = (val[1] << 16) | (x << 8) | val[3];
                    else             c = (val[1] << 16) | (val[2] << 8) | x;
                    bot.DrawRect(BX + x, y, 1, BH, ToColor(c));
                }
                bot.DrawRect(BX - 2, y - 2, BW + 4, BH + 4,
                             i == row ? Color::Yellow : Color::Gray, false);

                // つまみ
                const int mx = BX + val[i];
                bot.DrawRect(mx - 2, y - 5, 5, BH + 10, Color::White);
                bot.DrawRect(mx - 1, y - 4, 3, BH + 8, Color::Black);

                char buf[8];
                sprintf(buf, "%3d", val[i]);
                bot.DrawSysfont(buf, BX + BW + 4, y + 2, Color::White);
            }

            bot.DrawRect(okX, btnY, btnW, btnH, Color(0, 110, 40));
            bot.DrawSysfont(BTN_A " 決定", okX + 32, btnY + 6, Color::White);
            bot.DrawRect(ngX, btnY, btnW, btnH, Color(130, 20, 20));
            bot.DrawSysfont(BTN_B " キャンセル", ngX + 18, btnY + 6, Color::White);

            // ---- 上画面 ----
            const Screen &top = OSD::GetTopScreen();
            top.DrawRect(0, 0, 400, 240, Color::Black);
            top.DrawSysfont(title, 10, 8, Color::White);

            top.DrawSysfont("変更前", 40, 36, Color::Gray);
            top.DrawRect(38, 56, 144, 124, Color::Gray, false);
            top.DrawRect(40, 58, 140, 120, ToColor(before));

            top.DrawSysfont("変更後", 220, 36, Color::White);
            top.DrawRect(218, 56, 144, 124, Color::White, false);
            top.DrawRect(220, 58, 140, 120, ToColor(cur));

            char hex[32];
            sprintf(hex, "#%06lX", (unsigned long)before);
            top.DrawSysfont(hex, 80, 186, Color::Gray);
            sprintf(hex, "#%06lX", (unsigned long)cur);
            top.DrawSysfont(hex, 260, 186, Color::White);

            top.DrawSysfont(BTN_UP BTN_DOWN ": 項目  " BTN_LEFT BTN_RIGHT ": 調整(長押しで加速)  タッチ操作可", 10, 214, Color::Gray);

            OSD::SwapBuffers();
            svcSleepThread(16666667LL);
        }

        WaitAllReleased();
        return true;
    }

    // ---------------------------------------------------------------
    //  リージョン自動判定
    //  JPN 0004000000030600 / EUR 0004000000030700 / USA 0004000000030800
    //  コード中のアドレスは日本版で書き、C()/S() で各版にずらす
    // ---------------------------------------------------------------
    enum Region { REGION_JPN, REGION_EUR, REGION_USA };

    struct RegionInfo
    {
        const char *name;
        const char *cheatTitle; // txt の1行目 (チート集の名前とバージョン)
        int  codeDelta;   // 0x2B〜0x3E台のコード (日本版との差)
        int  shadeDelta;  // 影・光の色 (0x2614xx〜0x2615xx)
        u32  moonExtra;   // ムーンジャンプの常時書き込み行
        u32  playerCond;  // プレイヤーポインタの条件 (0 = 条件なし)
        u32  playerOff;   // [0x140005AC] からのオフセット
    };

    static const RegionInfo kRegions[] = {
        { "JPN", "MK7_Cheat Ver.6.23",    0x00,  0, 0x00444C5C, 0x147909F4, 0x650 },
        { "EUR", "MK7_CheatEU Ver.1.01", -0x08, -8, 0x00444CE0, 0,          0x628 },
        { "USA", "MK7_CheatUS Ver.2.01", -0x28, -8, 0x00444C64, 0x1478FDF4, 0x650 },
    };

    static Region            g_region = REGION_JPN;
    static const RegionInfo *g_rg     = &kRegions[REGION_JPN];

    static inline u32 C(u32 jpAddr) { return jpAddr + g_rg->codeDelta; }
    static inline u32 S(u32 jpAddr) { return jpAddr + g_rg->shadeDelta; }

    static void DetectRegion()
    {
        switch (Process::GetTitleID())
        {
            case 0x0004000000030700ULL: g_region = REGION_EUR; break;
            case 0x0004000000030800ULL: g_region = REGION_USA; break;
            default:                    g_region = REGION_JPN; break;
        }
        g_rg = &kRegions[g_region];
    }

    // AR: D3000000 10000000 / 947909F4 00000001 / B40005AC / B0000650 / DC delta
    // USA: 9478FDF4 / B0000650、EUR: 条件行なし / B0000628
    static bool GetPlayerPtr(u32 delta, u32 &out)
    {
        if (g_rg->playerCond != 0)
        {
            u16 flag = 0;
            if (!Process::Read16(g_rg->playerCond, flag) || flag != 1) return false;
        }
        u32 p = 0;
        if (!Process::Read32(0x140005AC, p)) return false;
        if (!Process::Read32(p + g_rg->playerOff, p)) return false;
        out = p + delta;
        return true;
    }

    // ---------------------------------------------------------------
    //  レース
    // ---------------------------------------------------------------
    static void DriftAnytime(MenuEntry *)
    {
        W32(0x006655FC, 0x00000000);
    }

    static void NoDirt(MenuEntry *)
    {
        W16(0x00665676, 0x3F80);
        W16(0x0066567A, 0x3F80);
    }

    static void Grounded(MenuEntry *e)
    {
        static const Toggle t[] = {
            { C(0x002DB8AC), 0xE1A00000, 0xED900A00 },
        };
        ApplyToggle(e, t);
    }

    static void NoWallHit(MenuEntry *e)
    {
        static const Toggle t[] = {
            { C(0x002D656C), 0x0A00000B, 0x1A00000B },
        };
        ApplyToggle(e, t);
    }

    static void WallClip(MenuEntry *e)
    {
        if (e->IsActivated())
        {
            // 元コードで常時書き込みの2行
            W32(C(0x002F5700), 0xE3520001);
            W32(C(0x002F9868), 0xE5850241);
        }
        static const Toggle t[] = {
            { C(0x002D64D8), 0xE3A00000, 0xE3A00003 },
            { C(0x002D731C), 0xE3800000, 0xE3800040 },
            { C(0x002F9850), 0xE3800000, 0xE3800040 },
            { C(0x002F5738), 0xE3A00000, 0x13800502 },
            { C(0x002F5744), 0xE5C40C30, 0xE5840C30 },
            { C(0x002E1C0C), 0xE3A02001, 0xE3A02000 },
        };
        ApplyToggle(e, t);
    }

    // ---- 影の色 / 光の色 ----
    //  影: 0x00261548〜0x00261564 (元コードの R+↑ 部分)
    //  光: 0x002614A0〜0x002614B4 (元コードの R+→ 部分)
    struct NamedColor { const char *name; u32 rgb; };
    static const NamedColor kShadeColors[] = {
        { "赤", 0xFF0000 }, { "青", 0x0000FF }, { "緑", 0x00FF00 },
        { "黄", 0xFFFF00 }, { "紫", 0xFF00FF }, { "水", 0x00FFFF },
        { "白", 0xFFFFFF },
    };
    static u32 g_shadow = 0xFF0000; // 初期値: 赤
    static u32 g_light  = 0xFF0000; // 初期値: 赤

    static void ShadowColorFunc(MenuEntry *e)
    {
        if (!e->IsActivated())
        {
            static const Patch off[] = {
                { S(0x00261548), 0x0A000007 },
                { S(0x0026155C), 0xE1A05225 },
                { S(0x00261560), 0xE1855501 },
                { S(0x00261564), 0xE1A01C26 },
            };
            WriteAll(off);
            return;
        }
        W32(S(0x00261548), 0xE1A00000);
        W32(S(0x0026155C), 0xE3A05600 | ((g_shadow >> 16) & 0xFF));
        W32(S(0x00261560), 0xE3855B00 | ((g_shadow >> 8) & 0xFF));
        W32(S(0x00261564), 0xE3A01000 | (g_shadow & 0xFF));
    }

    static void LightColorFunc(MenuEntry *e)
    {
        if (!e->IsActivated())
        {
            static const Patch off[] = {
                { S(0x002614A0), 0xE1A06A06 },
                { S(0x002614A4), 0xE1865505 },
                { S(0x002614B4), 0xE185052C },
            };
            WriteAll(off);
            return;
        }
        W32(S(0x002614A0), 0xE3A06600 | ((g_light >> 16) & 0xFF));
        W32(S(0x002614A4), 0xE3865B00 | ((g_light >> 8) & 0xFF));
        W32(S(0x002614B4), 0xE3850000 | (g_light & 0xFF));
    }

    // 中間値(00/FF以外)の見え方は未検証
    static void PickShadeColor(const std::string &name, u32 &rgb)
    {
        std::vector<std::string> opts;
        opts.push_back(Col(rgb, "■") + " カラーピッカーで指定");
        for (const auto &c : kShadeColors)
            opts.push_back(Col(c.rgb, "■") + " " + c.name);

        Keyboard kb(name + "を選択", opts);
        int idx = kb.Open();
        if (idx < 0) return;

        if (idx == 0) { ColorPicker(name, rgb); return; }
        rgb = kShadeColors[idx - 1].rgb;
    }

    // 色を選んだら、コード名もその色で表示する
    static void ShadowColorMenu(MenuEntry *e)
    {
        PickShadeColor("影の色", g_shadow);
        e->Name() = Col(g_shadow, "影の色");
    }
    static void LightColorMenu(MenuEntry *e)
    {
        PickShadeColor("光の色", g_light);
        e->Name() = Col(g_light, "光の色");
    }

    static void UnderwaterLight(MenuEntry *)
    {
        static const Patch p[] = {
            { C(0x003E36A8), 0xE320F000 },
            { C(0x003E3774), 0xE3A01001 },
            { C(0x003E37F4), 0xE3A01001 },
        };
        WriteAll(p);
    }

    static void SuperDrift(MenuEntry *)
    {
        static const Patch p[] = {
            { C(0x003E4CAC), 0xE3A09069 },
            { C(0x003E4CBC), 0x13A09069 },
            { C(0x003E4F84), 0xE3A09067 },
            { C(0x003E4F94), 0x13A09067 },
        };
        WriteAll(p);
    }

    static void GliderBoard(MenuEntry *)
    {
        W32(0x0036748C, 0xEE300A40);
        W32(0x0036178C, 0xEE300A40);
    }

    // ---- グライダーボード操作 ----
    // キノピオサーキット(TcBoard) / GBAクッパキャッスル1(GbaBoard)
    // ボード +0x1B4: 状態(0格納中 1上昇中 2起動中 3下降中 4格納済み) / +0x1B0: タイマー
    static bool GetCourseMgr(u32 &out)
    {
        u16 flag = 0;
        if (!Process::Read16(0x147909F4, flag) || flag != 1) return false;   // 947909F4
        u32 p = 0;
        if (!Process::Read32(0x0065C528, p) || p == 0) return false;          // 6065C528
        static const u32 chain[] = { 0x38, 0x04, 0x1C, 0x18 };
        for (u32 off : chain)
            if (!Process::Read32(p + off, p)) return false;
        out = p;
        return true;
    }

    static u32 FindBoard(u32 mgr)
    {
        u32 b = 0;
        // キノピオサーキット: 管理役 +0x108 (vtable 0062FF04)
        if (Process::Read32(mgr + 0x108, b) && b != 0 && R32(b) == 0x0062FF04)
            return b;
        // GBAクッパキャッスル1: +0x58 のリストの先頭 (vtable 006317AC)
        u32 list = 0;
        if (Process::Read32(mgr + 0x58, list) && list != 0
            && Process::Read32(list, b) && b != 0 && R32(b) == 0x006317AC)
            return b;
        return 0;
    }

    static void GliderBoardControl(MenuEntry *e)
    {
        u32 mgr = 0;
        if (!GetCourseMgr(mgr)) return;
        const u32 b = FindBoard(mgr);
        if (b == 0) return;

        const u32 st = b + 0x1B4, tm = b + 0x1B0;
        // 押していない間: 起動中ならすぐ下降させる
        if (R32(st) == 2) W32(tm, 1);

        if (e->Hotkeys[0].IsDown())
        {
            if (R32(st) == 2) W32(tm, 0x7FFFFFFF);        // 起動状態を維持
            if (R32(st) == 4) { W32(st, 0); W32(tm, 0); } // 格納済み → 上昇へ
            if (R32(st) == 0) W32(tm, 0);
        }
    }

    // ---------------------------------------------------------------
    //  プレイヤー
    // ---------------------------------------------------------------
    static void GoldHandle(MenuEntry *)
    {
        u32 p = 0;
        if (!Process::Read32(0x0065C528, p) || p == 0) return;   // 6065C528 00000000
        W16(p + 0x774, 0x0100);
        W16(p + 0x77C, 0x0100);

        u32 q = 0;
        if (!Process::Read32(0x006673C8, q)) return;              // B06673C8
        // C0000000 00000019 + D6 : 0x19+1 = 26回、4バイトずつ
        for (u32 i = 0; i <= 0x19; i++)
            W32(q + 0x6BC4 + i * 4, 0x65656565);
    }

    // ---- Miiの色変更 ----
    // 値は FFBBGGRR (RGBを逆順)。反映にはMiiの作り直しが必要
    struct PalSlot { const char *name; u32 addr; u32 orig; }; // orig は 0xRRGGBB

    static inline u32 RGBtoMii(u32 rgb) // 0xRRGGBB -> 0xFFBBGGRR
    {
        return 0xFF000000 | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
    }

    // 髪 (Mii作成画面のパレット順)
    static const PalSlot kHair[] = {
        { "黒",       0x00601650, 0x1E1A18 },
        { "こげ茶",   0x00601654, 0x402010 },
        { "赤茶",     0x00601658, 0x5C180A },
        { "茶",       0x0060165C, 0x7C3A14 },
        { "灰",       0x00601660, 0x787880 },
        { "オリーブ", 0x00601664, 0x4E3E10 },
        { "明るい茶", 0x00601668, 0x885818 },
        { "金",       0x0060166C, 0xD0A04A },
    };
    // メガネ (推定)
    static const PalSlot kGlasses[] = {
        { "1", 0x00601670, 0x181818 }, { "2", 0x00601674, 0x603810 },
        { "3", 0x00601678, 0xA81008 }, { "4", 0x0060167C, 0x203068 },
        { "5", 0x00601680, 0xA86000 }, { "6", 0x00601684, 0x787068 },
    };
    static const PalSlot kSkin[] = {
        { "1", 0x00601B44, 0xFFD3AD }, { "2", 0x00601B48, 0xFFB66B },
        { "3", 0x00601B4C, 0xDE7942 }, { "4", 0x00601B50, 0xFFAA8C },
        { "5", 0x00601B54, 0xAD5129 }, { "6", 0x00601B58, 0x632C18 },
    };
    static const PalSlot kEyes[] = {
        { "1", 0x00601B5C, 0x000000 }, { "2", 0x00601B60, 0x6C7070 },
        { "3", 0x00601B64, 0x663C2C }, { "4", 0x00601B68, 0x605E30 },
        { "5", 0x00601B6C, 0x4654A8 }, { "6", 0x00601B70, 0x387058 },
    };
    // 口: 通常と濃い方を1つにまとめる
    static const PalSlot kMouth[] = {
        { "1",      0x00601B74, 0xD85208 }, { "1 (濃)", 0x00601B88, 0x823018 },
        { "2",      0x00601B78, 0xF00C08 }, { "2 (濃)", 0x00601B8C, 0x780C0C },
        { "3",      0x00601B7C, 0xF54848 }, { "3 (濃)", 0x00601B90, 0x882028 },
        { "4",      0x00601B80, 0xF09A74 }, { "4 (濃)", 0x00601B94, 0xDC7850 },
        { "5",      0x00601B84, 0x8C5040 }, { "5 (濃)", 0x00601B98, 0x461E0A },
    };
    static const PalSlot kFavorite[] = {
        { "赤",       0x00601B9C, 0xD21E14 }, { "オレンジ", 0x00601BA0, 0xFF6E19 },
        { "黄",       0x00601BA4, 0xFFD820 }, { "黄緑",     0x00601BA8, 0x78D220 },
        { "緑",       0x00601BAC, 0x007830 }, { "青",       0x00601BB0, 0x0A48B4 },
        { "水色",     0x00601BB4, 0x3CAADE }, { "ピンク",   0x00601BB8, 0xF55A7D },
        { "紫",       0x00601BBC, 0x7328AD }, { "茶",       0x00601BC0, 0x483818 },
        { "白",       0x00601BC4, 0xE0E0E0 }, { "黒",       0x00601BC8, 0x181814 },
    };

    template <size_t N>
    struct Palette
    {
        const char    *title;
        const PalSlot (&slots)[N];
        u32            cur[N];

        Palette(const char *t, const PalSlot (&s)[N], bool initPurple = false)
            : title(t), slots(s)
        {
            for (size_t i = 0; i < N; i++) cur[i] = initPurple ? 0x800080 : s[i].orig;
        }
    };

    static Palette<8>  g_palHair    ("髪",       kHair, true); // 初期値: 紫 (元コード FF800080)
    static Palette<6>  g_palGlasses ("メガネ",   kGlasses);
    static Palette<6>  g_palSkin    ("肌",       kSkin);
    static Palette<6>  g_palEyes    ("目",       kEyes);
    static Palette<10> g_palMouth   ("口",       kMouth);
    static Palette<12> g_palFavorite("好きな色", kFavorite);

    // ON中は設定色、チェックを外したら元の色(OFF値)を書く
    template <size_t N>
    static void ApplyPalette(MenuEntry *e, const Palette<N> &p)
    {
        const bool on = e->IsActivated();
        for (size_t i = 0; i < N; i++)
            W32(p.slots[i].addr, RGBtoMii(on ? p.cur[i] : p.slots[i].orig));
    }

    // 1色選ぶ。戻り値: 0=キャンセル 1=色を選んだ 2=元の色に戻す
    static int PickMiiColor(const std::string &title, u32 &rgb)
    {
        static const NamedColor presets[] = {
            { "赤", 0xFF0000 }, { "青", 0x0000FF }, { "緑", 0x00FF00 },
            { "黄", 0xFFFF00 }, { "紫", 0x800080 }, { "水", 0x00FFFF },
            { "白", 0xFFFFFF }, { "黒", 0x000000 },
        };
        std::vector<std::string> opts;
        opts.push_back(Col(rgb, "■") + " カラーピッカーで指定");
        opts.push_back("元の色に戻す");
        for (const auto &c : presets)
            opts.push_back(Col(c.rgb, "■") + " " + c.name);

        Keyboard kb(title + " の色を選択", opts);
        int idx = kb.Open();
        if (idx < 0) return 0;
        if (idx == 0) return ColorPicker(title, rgb) ? 1 : 0;
        if (idx == 1) return 2;
        rgb = presets[idx - 2].rgb;
        return 1;
    }

    template <size_t N>
    static void PaletteMenu(Palette<N> &p)
    {
        std::vector<std::string> opts;
        opts.push_back("全色まとめて変更");
        for (size_t i = 0; i < N; i++)
        {
            // 元の色 → 現在の設定色
            opts.push_back(Col(p.slots[i].orig, "■") + "→" + Col(p.cur[i], "■")
                         + " " + p.title + " " + p.slots[i].name);
        }

        Keyboard kb(std::string("変更する") + p.title + "の色を選択", opts);
        int idx = kb.Open();
        if (idx < 0) return;

        if (idx == 0)
        {
            u32 c = p.cur[0];
            const int r = PickMiiColor(std::string(p.title) + " 全色", c);
            for (size_t i = 0; i < N; i++)
            {
                if (r == 1) p.cur[i] = c;
                if (r == 2) p.cur[i] = p.slots[i].orig;
            }
            return;
        }

        const size_t i = idx - 1;
        u32 c = p.cur[i];
        const int r = PickMiiColor(std::string(p.title) + " " + p.slots[i].name, c);
        if (r == 1) p.cur[i] = c;
        if (r == 2) p.cur[i] = p.slots[i].orig;
    }

    static void MiiHair(MenuEntry *e)         { ApplyPalette(e, g_palHair); }
    static void MiiGlasses(MenuEntry *e)      { ApplyPalette(e, g_palGlasses); }
    static void MiiSkin(MenuEntry *e)         { ApplyPalette(e, g_palSkin); }
    static void MiiEyes(MenuEntry *e)         { ApplyPalette(e, g_palEyes); }
    static void MiiMouth(MenuEntry *e)        { ApplyPalette(e, g_palMouth); }
    static void MiiFavorite(MenuEntry *e)     { ApplyPalette(e, g_palFavorite); }
    static void MiiHairMenu(MenuEntry *)      { PaletteMenu(g_palHair); }
    static void MiiGlassesMenu(MenuEntry *)   { PaletteMenu(g_palGlasses); }
    static void MiiSkinMenu(MenuEntry *)      { PaletteMenu(g_palSkin); }
    static void MiiEyesMenu(MenuEntry *)      { PaletteMenu(g_palEyes); }
    static void MiiMouthMenu(MenuEntry *)     { PaletteMenu(g_palMouth); }
    static void MiiFavoriteMenu(MenuEntry *)  { PaletteMenu(g_palFavorite); }

    // 空き領域 0x5EBD30〜 を共有するエントリ (同時に使うとクラッシュするので排他にする)
    static MenuEntry *g_entGPTimer  = nullptr;
    static MenuEntry *g_entRearView = nullptr;
    static MenuEntry *g_entTimeStop = nullptr;

    static void DisableIfOn(MenuEntry *e)
    {
        if (e && e->IsActivated()) e->Disable();
    }

    // ---------------------------------------------------------------
    //  カスタムモード: ふうせんレース / コインレース (日本版のみ)
    //  同じアドレスと空き領域 0x5EBD50〜 を使うので、2つと時間停止(BETA)は排他
    // ---------------------------------------------------------------
    static MenuEntry *g_entBalloon  = nullptr;
    static MenuEntry *g_entCoinRace = nullptr;

    // ON は先頭から、OFF は末尾から書く (空き領域 → 分岐 の順に張り、分岐 → 空き領域 の順に外す)
    template <size_t N>
    static void ApplyToggleOrdered(MenuEntry *e, const Toggle (&p)[N])
    {
        if (e->IsActivated())
            for (size_t i = 0; i < N; i++) W32(p[i].addr, p[i].on);
        else
            for (size_t i = N; i-- > 0; ) W32(p[i].addr, p[i].off);
    }

    #define CUSTOM_RACE_COMMON(SCORE_A, SCORE_B)                                        \
        { 0x005EBD50, 0xE3500003, 0x00000000 },                                         \
        { 0x005EBD54, 0x13500000, 0x00000000 },                                         \
        { 0x005EBD58, 0xE12FFF1E, 0x00000000 },                                         \
        { 0x00419E18, 0xE3510000, 0xE3510003 },                                         \
        { 0x00419E20, 0xE3510003, 0x13510007 },                                         \
        { 0x00419E24, 0x13510007, 0x0590116C },                                         \
        { 0x00419E28, 0x1A000047, 0x03510001 },                                         \
        { 0x00419E2C, 0xE590116C, 0x0A000047 },                                         \
        { 0x00419E30, 0xE3510001, 0xE5901168 },                                         \
        { 0x00419E34, 0x0A000045, 0xE3510003 },                                         \
        { 0x00419E38, 0xE3510000, 0x15901168 },                                         \
        { 0x00419E3C, 0x1A000042, 0x13510007 },                                         \
        { 0x00419E40, 0xE320F000, 0x0590016C },                                         \
        { 0x00419E44, 0xE320F000, 0x03500000 },                                         \
        { 0x00419E48, 0xE320F000, 0x1A00003F },                                         \
        { 0x0041D47C, 0xE320F000, 0x1A000102 },                                         \
        { 0x0047A81C, 0xE2800C0B, 0xE2800B01 },                                         \
        { 0x0047A820, 0xE0651185, 0xE28000F8 },                                         \
        { 0x0047A824, 0xE0800201, 0x30800105 },                                         \
        { 0x0047A828, SCORE_A,    0xE5901000 },                                         \
        { 0x0047A860, 0xE2800C0B, 0xE2800B01 },                                         \
        { 0x0047A864, 0xE0651185, 0xE28000F8 },                                         \
        { 0x0047A868, 0xE0800201, 0x30800105 },                                         \
        { 0x0047A86C, SCORE_A,    0xE5901000 },                                         \
        { 0x0047E9C4, 0xE2811C0B, 0xE3500008 },                                         \
        { 0x0047E9C8, 0xE0602180, 0xE2811B01 },                                         \
        { 0x0047E9CC, 0xE0811202, 0xE28110F8 },                                         \
        { 0x0047E9D0, SCORE_B,    0x30811100 },                                         \
        { 0x0047E9D4, 0xE320F000, 0xE5912000 },                                         \
        { 0x00478CCC, 0xEB05CC1F, 0xE3500003 },  /* ↓ 空き領域を呼ぶ分岐は最後 */      \
        { 0x0047E948, 0xEB05B500, 0xE3500003 },                                         \
        { 0x0047EA6C, 0xEB05B4B7, 0xE3500003 },                                         \
        { 0x004748F4, 0xEB05DD15, 0xE3500003 }

    static void BalloonRace(MenuEntry *e)
    {
        if (e->WasJustActivated()) { DisableIfOn(g_entCoinRace); DisableIfOn(g_entTimeStop); }
        static const Toggle t[] = {
            { 0x00419E1C, 0x0A00004B, 0x15901168 },
            { 0x0041F82C, 0xE320F000, 0x1A00001E },
            CUSTOM_RACE_COMMON(0xE1D012B4, 0xE1D122B4),   // 本家と同じスコア表示
        };
        ApplyToggleOrdered(e, t);
    }

    // コインレース: コイン枚数で順位を決める (順位計算 0x464574 の中身を置き換え、47行)
    static const Toggle kCoinRank[] = {
            { 0x0046457C, 0xE590C1E4, 0xE2807C01 },
            { 0x00464580, 0xE6FFC07C, 0xE59021E4 },
            { 0x00464584, 0xE35C0008, 0xE5D014E7 },
            { 0x00464588, 0x83A0C008, 0xE28770FD },
            { 0x0046458C, 0xE59F10A0, 0xE3A03000 },
            { 0x00464590, 0xE5911000, 0xE6FFC072 },
            { 0x00464594, 0xE3510000, 0xE3510000 },
            { 0x00464598, 0x0A000022, 0xDA000010 },
            { 0x0046459C, 0xE2811D2F, 0xE0801083 },
            { 0x004645A0, 0xE2802F7D, 0xE2811B01 },
            { 0x004645A4, 0xE3A03000, 0xE2834001 },
            { 0x004645A8, 0xE0634183, 0xE1D11EF8 },
            { 0x004645AC, 0xE0814204, 0xE1A02007 },
            { 0x004645B0, 0xE1D440B2, 0xE20440FF },
            { 0x004645B4, 0xE0825003, 0xE3510008 },
            { 0x004645B8, 0xE5D55009, 0x30872001 },
            { 0x004645BC, 0xE3A06001, 0xE3540009 },
            { 0x004645C0, 0xE3A07000, 0xE5C24000 },
            { 0x004645C4, 0xE0678187, 0xE2802F7D },
            { 0x004645C8, 0xE0818208, 0x30822004 },
            { 0x004645CC, 0xE1D880B2, 0xE2833001 },
            { 0x004645D0, 0xE0829007, 0xE5C21000 },
            { 0x004645D4, 0xE5D99009, 0xE5D014E7 },
            { 0x004645D8, 0xE1580004, 0xE1510003 },
            { 0x004645DC, 0x82866001, 0xCAFFFFEE },
            { 0x004645E0, 0x1A000001, 0xE5D01028 },
            { 0x004645E4, 0xE1590005, 0xE3510000 },
            { 0x004645E8, 0x32866001, 0xE3A01000 },
            { 0x004645EC, 0xE2877001, 0xDA00000F },
            { 0x004645F0, 0xE157000C, 0xE0802081 },
            { 0x004645F4, 0x3AFFFFF2, 0xE04C4001 },
            { 0x004645F8, 0xE7CD6003, 0xE1D222FA },
            { 0x004645FC, 0xE2833001, 0xE20440FF },
            { 0x00464600, 0xE153000C, 0xE2811001 },
            { 0x00464604, 0x3AFFFFE7, 0xE3520008 },
            { 0x00464608, 0xE3A03000, 0x30875002 },
            { 0x0046460C, 0xE7DD6003, 0x21A05007 },
            { 0x00464610, 0xE0825003, 0xE3540009 },
            { 0x00464614, 0xE5C56009, 0xE5C54000 },
            { 0x00464618, 0xE7C23006, 0xE2805F7D },
            { 0x0046461C, 0xE2833001, 0x30855004 },
            { 0x00464620, 0xE153000C, 0xE5C52000 },
            { 0x00464624, 0x3AFFFFF8, 0xE5D02028 },
            { 0x00464628, 0xE28DD008, 0xE1520001 },
            { 0x0046462C, 0xE8BD03F0, 0xCAFFFFEF },
            { 0x00464630, 0xE12FFF1E, 0xE5D02028 },
            { 0x00464634, 0x0065C528, 0xE5D014E7 }
    };

    static void CoinRace(MenuEntry *e)
    {
        if (e->WasJustActivated()) { DisableIfOn(g_entBalloon); DisableIfOn(g_entTimeStop); }
        static const Toggle t[] = {
            { 0x00419E1C, 0x0A00000A, 0x15901168 },
            { 0x0041EDAC, 0xE320F000, 0x1A000004 },
            { 0x00415A00, 0xE320F000, 0x0A00001D },
            { 0x00415A34, 0xE310000A, 0xE3100002 },
            CUSTOM_RACE_COMMON(0xE1D011B6, 0xE1D121B6),
        };
        ApplyToggleOrdered(e, t);
        ApplyToggle(e, kCoinRank);   // 順位計算の置き換え (普通のレースでは必ずOFFになるよう一緒に戻す)
    }

    // ---------------------------------------------------------------
    //  メニュー (日本版のみ)
    // ---------------------------------------------------------------
    static MenuEntry *g_entMenuCut = nullptr;

    // メニュー高速化 (メニューカット まとめ版)。空き領域 0x5EBD9C〜0x5EBDD0 を使う
    static void MenuSpeedup(MenuEntry *e)
    {
        if (e->WasJustActivated()) DisableIfOn(g_entTimeStop);
        static const Toggle t[] = {
            // 空き領域 (先に書く)
            { 0x005EBD9C, 0xE5940000, 0 }, { 0x005EBDA0, 0xE59010D8, 0 },
            { 0x005EBDA4, 0xE59F2020, 0 }, { 0x005EBDA8, 0xE1510002, 0 },
            { 0x005EBDAC, 0x159F201C, 0 }, { 0x005EBDB0, 0x11510002, 0 },
            { 0x005EBDB4, 0x0AFB8F88, 0 }, { 0x005EBDB8, 0xE1A00004, 0 },
            { 0x005EBDBC, 0xE12FFF31, 0 }, { 0x005EBDC0, 0xE3500000, 0 },
            { 0x005EBDC4, 0x0AFB8F52, 0 }, { 0x005EBDC8, 0xEAFB8F83, 0 },
            { 0x005EBDCC, 0x00472C04, 0 }, { 0x005EBDD0, 0x0049B314, 0 },
            // 本体
            { 0x004CFAFC, 0xA3E00000, 0xA2400001 },
            { 0x004CFB64, 0xE320F000, 0x0AFFFFEA },
            { 0x004CFBC0, 0xE320F000, 0x0AFFFFD3 },
            { 0x004D38D8, 0x8A000003, 0x1A000003 },
            { 0x004D38F0, 0xE2810001, 0xE2800001 },
            { 0x004815B0, 0xE1500000, 0xE1510000 },
            { 0x004815F4, 0xEEF00A40, 0xEE700AC1 },
            { 0x00481658, 0xEEF00A40, 0xEE700A81 },
            { 0x004DC5F0, 0xE3A02001, 0xE584100C },
            { 0x004DC5F4, 0xE584200C, 0xE3A02001 },
            { 0x004DC654, 0xE3A02003, 0xE580100C },
            { 0x004DC658, 0xE580200C, 0xE3A02003 },
            { 0x00401E54, 0xE3A06000, 0xE3A06001 },
            // 空き領域へ飛ぶ分岐 (最後に書き、最初に戻す)
            { 0x004CFBC4, 0xEA047074, 0xE5940000 },
        };
        ApplyToggleOrdered(e, t);
    }

    // メニュー速度変更: レース外のときシーン更新の回数 ([[0x6789BC]+4]+0x304) を増やす
    static u32 g_menuSpeed = 4;
    static bool GetSceneSpeedAddr(u32 &out)
    {
        u32 p = 0;
        if (!Process::Read32(0x006789BC, p) || p == 0) return false;
        if (!Process::Read32(p + 4, p) || p == 0) return false;
        out = p + 0x304;
        return true;
    }

    static void MenuSpeed(MenuEntry *e)
    {
        u32 race = 0, addr = 0;
        Process::Read32(0x0065C528, race);
        if (!GetSceneSpeedAddr(addr)) return;

        if (!e->IsActivated())
        {
            if (race == 0) W32(addr, 1);              // OFF: 通常の 1 に戻す
            return;
        }
        if (race == 0)
            W32(addr, g_menuSpeed);                   // レース外: 倍速
        else if (R32(addr) == g_menuSpeed)
            W32(addr, 1);                             // レースに入ったら 1 に戻す
    }

    static void MenuSpeedMenu(MenuEntry *e)
    {
        static const u32 speeds[] = { 2, 3, 4, 6, 8 };
        std::vector<std::string> opts;
        for (u32 v : speeds)
            opts.push_back(std::string(v == g_menuSpeed ? "\u25CF " : "\u3000") + std::to_string(v) + "倍速");
        Keyboard kb("メニューの速度\n(16倍はゲームの早送りと同じ値なので選べません)", opts);
        int idx = kb.Open();
        if (idx >= 0)
        {
            g_menuSpeed = speeds[idx];
            e->Name() = Col(0x007AFF, "メニュー速度変更 (" + std::to_string(g_menuSpeed) + "倍速)");
        }
    }

    // リザルトのポイント換算を1フレームで終わらせる (日本版のみ)
    static void ResultPointSkip(MenuEntry *e)
    {
        static const Toggle t[] = {
            { 0x004DB118, 0xE3A01801, 0xE1A01008 },
        };
        ApplyToggle(e, t);
    }

    static void GPTimer(MenuEntry *e)
    {
        if (!e->IsActivated())
        {
            // OFF: フックを先に戻してから空き領域を消す
            W32(0x004DB8E0, 0xEBFE8772);
            for (u32 a = 0x005EBD30; a <= 0x005EBD44; a += 4) W32(a, 0);
            return;
        }
        if (e->WasJustActivated()) DisableIfOn(g_entTimeStop);

        static const Patch p[] = {
            { 0x005EBD30, 0xE92D4010 },
            { 0x005EBD34, 0xE1A04000 },
            { 0x005EBD38, 0xEBFA465C },
            { 0x005EBD3C, 0xE1A00004 },
            { 0x005EBD40, 0xEBFA48F8 },
            { 0x005EBD44, 0xE8BD8010 },
            { 0x004DB8E0, 0xEB044112 }, // フック本体は最後に書く
        };
        WriteAll(p);
    }

    static void NoGliderPushback(MenuEntry *)
    {
        W32(0x002DCB00, 0xED85BA9F);
        W32(0x002DCB04, 0xEA000062);
    }

    static void ItemStays(MenuEntry *)      { W32(C(0x002D2558), 0xE12FFF1E); }
    static void NoBlooper(MenuEntry *)      { W32(C(0x002BFA5C), 0xE3A00000); }
    static void Invincible(MenuEntry *)     { W32(C(0x002E15A8), 0xE3510001); }
    static void CoinLock(MenuEntry *)
    {
        W32(C(0x002E19E8), 0xE35A0000);     // 被弾時
        if (g_region == REGION_JPN)
            W32(0x004149A4, 0xE3A01000);    // コースアウト時 (日本版のみ)
    }

    // コイン上限99 (日本版のみ)
    static void Coin99(MenuEntry *)
    {
        W32(0x00410F5C, 0xE3520063);
        W32(0x004163C4, 0xE3560063);
        W32(0x004163D8, 0xE2663063);

        u32 p = 0;                                   // 6065C528 00000000
        if (Process::Read32(0x0065C528, p) && p != 0) // B065C528
        {
            for (u32 i = 0; i < 8; i++)              // 10000BC4〜10000ED4 (8人分、0x70おき)
                W16(p + 0xBC4 + i * 0x70, 0x0063);
        }
    }

    // アイテムボックス即復活 (日本版のみ): 復活までの待ち時間 120F → 30F
    static void ItemBoxRespawn(MenuEntry *e)
    {
        static const Toggle t[] = {
            { 0x0035CDEC, 0xE3A0101E, 0xE1D010B4 },
        };
        ApplyToggle(e, t);
    }

    // ヘリモード (日本版のみ)
    static void HeliMode(MenuEntry *e)
    {
        static const Toggle t[] = {
            { 0x002DAA98, 0xE3B09000, 0xE1B094A0 },
            { 0x006651C8, 0x4E6E6B28, 0x44160000 },
            { 0x002DB23C, 0xE320F000, 0x0A000034 },
            { 0x002DB250, 0xEA00000A, 0x0A00000A },
            { 0x002DB29C, 0xEEB00A4B, 0xED940A9C },
            { 0x00665288, 0x3F666666, 0x3DCCCCCD },
        };
        ApplyToggle(e, t);
    }

    // コイン即復活 (日本版のみ): 復活までの時間 300F → 1F
    static void CoinRespawn(MenuEntry *e)
    {
        static const Toggle t[] = {
            { 0x00415A48, 0xE3500001, 0xE3500F4B },
        };
        ApplyToggle(e, t);
    }

    static void MoonJump(MenuEntry *)
    {
        u32 p = 0;
        if (Held(Key::R | Key::DPadUp)   && GetPlayerPtr(0xFFFFE2A4, p)) W32(p, 0x41100000);
        if (Held(Key::R | Key::DPadDown) && GetPlayerPtr(0xFFFFE2A4, p)) W32(p, 0xC1400000);
        W32(g_rg->moonExtra, 0xE3A01000);
    }

    static void Respawn(MenuEntry *)
    {
        if (!Held(Key::L | Key::DPadDown)) return;
        W32(C(0x002F9868), 0xE5850241);
        u32 p = 0;
        if (GetPlayerPtr(0xFFFFEE98, p)) W32(p, 0x00000040);
    }

    static void InstantItem(MenuEntry *)
    {
        W32(C(0x002BE520), 0xE3580001);
        W32(C(0x002BE530), 0xE3500001);
    }

    static void AutoEquip(MenuEntry *)
    {
        W32(C(0x002BE2FC), 0x13A01000);
        W32(C(0x002BE300), 0x03A01001);
    }

    static void AutoJA(MenuEntry *)         { W32(C(0x002DBC0C), 0xE1A00000); }

    static void RearView(MenuEntry *e)
    {
        if (!e->IsActivated()) { W32(C(0x002F1244), 0xED901A97); return; }
        if (e->WasJustActivated()) DisableIfOn(g_entTimeStop);

        W32(C(0x002F1244), 0xED901A97);
        W32(C(0x002F1250), 0xE1A00000);
        if (Held(Key::DPadLeft))
        {
            // E05EBE18 00000010 の中身を先に書いてから分岐を張る
            W32(C(0x005EBE18), 0xE59F7004);
            W32(C(0x005EBE1C), 0xEE017A10);
            W32(C(0x005EBE20), 0xEAF41508);
            W32(C(0x005EBE24), 0xC24C0000);
            W32(C(0x002F1244), 0xEA0BEAF3);
        }
    }

    static void LiveReplay(MenuEntry *)
    {
        const u32 S = 0x006BE980; // 状態
        const u32 T = 0x006BE984; // Xボタン押下管理

        W32(0x004DA158, 0xE35000FF);
        W32(0x00475C70, 0xE320F000);
        W32(0x004D9234, 0xE320F000);
        W32(0x004DB8C4, 0xE320F000);
        W32(0x004DB8CC, 0xE35000FF);

        if (R32(S) != 1)
        {
            W32(0x002B2E94, 0xE320F000);
            W32(0x002B2E9C, 0xE320F000);
            W32(0x002B2F68, 0xE320F000);
        }
        if (R32(S) == 1)
        {
            W32(0x002B2E94, 0x0A000019);
            W32(0x002B2E9C, 0x0A000026);
            W32(0x002B2F68, 0x0A00000B);
        }
        if (R32(T) == 1) W32(T, 2);

        if (Held(Key::X))
        {
            if (R32(T) == 0)
            {
                // ARと同じく上から順に評価(元コードの挙動をそのまま再現)
                if (R32(S) == 0) W32(S, 2);
                if (R32(S) == 1) W32(S, 0);
                if (R32(S) == 2) W32(S, 1);
                W32(T, 1);
            }
            if (R32(T) == 2) W32(T, 1);
        }
        if (R32(T) == 2) W32(T, 0);
    }

    static void PauseUnlock(MenuEntry *)
    {
        static const Patch p[] = {
            { 0x0047E6C0, 0xE2200001 },
            { 0x0047E6C4, 0xE3500003 },
            { 0x0047E6C8, 0x2A000034 },
            { 0x0047E75C, 0xE320F000 },
            { 0x0047E768, 0xE320F000 },
        };
        WriteAll(p);
    }

    // ---------------------------------------------------------------
    //  BETA: 自分以外の時間停止 (最終試作版 v6改 / 未完成・動作不安定)
    //  空き領域 0x5EBD30〜0x5EBF2C、作業用メモリ 0x6BEC00 を使用
    // ---------------------------------------------------------------
    static const u32 kTimeStopCode[] = {
        0xE59512F4, 0xE3110C01, 0x0A000004, 0xE3500002, 0x13500002, 0x13500008, // 5EBD30
        0x0AF9BD13, 0xEAF9BCFB, 0xE1DA1001, 0x0AF9BD10, 0xEAF9BCF8, 0xE59FC11C, // 5EBD48
        0xE59CC000, 0xE59CC004, 0xE59CC2F0, 0xE31C0C01, 0x112FFF1E, 0xE12FFF12, // 5EBD60
        0xE59FC100, 0xE59CC000, 0xE59CC004, 0xE59CC2F0, 0xE31C0C01, 0x112FFF1E, // 5EBD78
        0xE92D5FF0, 0xEAF9DF50,                                                 // 5EBD90
    };
    static const u32 kTimeStopCode2[] = {                                        // 5EBDBC〜
        0xE59FC0BC, 0xE59CC000, 0xE59CC004, 0xE59CC2F0, 0xE31C0C01, 0x1A000001,
        0xE92D4FF8, 0xEAF43A4F, 0xE92D41F0, 0xE1A04000, 0xE59F8098, 0xE5988000,
        0xE3580000, 0x0A000021, 0xE5988650, 0xE3580000, 0x0A00001E, 0xE598C1D8,
        0xE5945028, 0xE594602C, 0xE3A07000, 0xE1570005, 0xAA000018, 0xE7960107,
        0xE2877001, 0xE3500000, 0x0AFFFFF9, 0xE590102C, 0xE151000C, 0x1AFFFFF6,
        0xE2472001, 0xE594805C, 0xE7981102, 0xE24DD008, 0xE58D0000, 0xE58D1004,
        0xE3A01001, 0xE5841028, 0xE1A0100D, 0xE584102C, 0xE28D1004, 0xE584105C,
        0xE1A00004, 0xEBFFFFD9, 0xE5845028, 0xE584602C, 0xE584805C, 0xE28DD008,
        0xE8BD81F0, 0x006789BC, 0x140005AC,                                     // 〜5EBE84
    };
    static const u32 kTimeStopCode3[] = {                                        // 5EBE90〜
        0xE59FC090, 0xE59CC000, 0xE59CC004, 0xE59CC2F0, 0xE31C0C01, 0x1A000003,
        0xE59FC07C, 0xE3A02000, 0xE58C2000, 0xE12FFF11, 0xE5902070, 0xE3520000,
        0x012FFF1E, 0xE59FC060, 0xE59C3000, 0xE28CC004, 0xE52D4004, 0xE2533001,
        0x4A000008, 0xE49C4008, 0xE1540000, 0x1AFFFFFA, 0xE51C4004, 0xE1540002,
        0xE50C2004, 0xE49D4004, 0x012FFF1E, 0xE12FFF11, 0xE59F4024, 0xE5943000,
        0xE3530078, 0xAA000003, 0xE2833001, 0xE5843000, 0xE58C0000, 0xE58C2004,
        0xE49D4004, 0xE12FFF11, 0x006789BC, 0x006BEC00,                         // 〜5EBF2C
    };
    static const Toggle kTimeStopHooks[] = {
        { 0x0045B134, 0xEA0642FD, 0xE59512F4 },
        { 0x0036DF4C, 0xEB09F782, 0xE12FFF32 },
        { 0x00463AD8, 0xEA0620A6, 0xE92D5FF0 },
        { 0x002FA718, 0xEA0BC5A7, 0xE92D4FF8 },
        { 0x005D32E8, 0xE5950008, 0xE5950008 },
        { 0x005D3318, 0xEB0062DC, 0xE12FFF31 },
    };

    template <size_t N>
    static void WriteBlock(u32 base, const u32 (&v)[N])
    {
        for (size_t i = 0; i < N; i++) W32(base + i * 4, v[i]);
    }

    // 停止フラグ: [[0x6789BC]+4]+0x2F0 (全体の時間停止と同じ)
    static bool GetTimeStopFlagAddr(u32 &out)
    {
        u32 p = 0;
        if (!Process::Read32(0x006789BC, p)) return false;
        if (!Process::Read32(p + 4, p))      return false;
        out = p + 0x2F0;
        return true;
    }

    static void SetTimeStopFlag(u32 value)
    {
        u32 a = 0;
        if (GetTimeStopFlagAddr(a)) W32(a, value);
    }

    static void TimeStopBeta(MenuEntry *e)
    {
        if (!e->IsActivated())
        {
            // OFF: 停止解除 → フックを戻す → 空き領域を消す
            SetTimeStopFlag(0);
            for (const auto &h : kTimeStopHooks) W32(h.addr, h.off);
            for (u32 a = 0x005EBD30; a <= 0x005EBF2C; a += 4) W32(a, 0);
            return;
        }
        if (e->WasJustActivated())
        {
            DisableIfOn(g_entGPTimer);
            DisableIfOn(g_entRearView);
            DisableIfOn(g_entBalloon);
            DisableIfOn(g_entCoinRace);
            DisableIfOn(g_entMenuCut);
        }

        // 空き領域のコードを先に書いてからフックを張る
        WriteBlock(0x005EBD30, kTimeStopCode);
        WriteBlock(0x005EBDBC, kTimeStopCode2);
        WriteBlock(0x005EBE90, kTimeStopCode3);
        for (const auto &h : kTimeStopHooks) W32(h.addr, h.on);

        // ホットキーを押した瞬間に 停止 ⇔ 解除 を切り替え
        static bool prev = false;
        const bool down = e->Hotkeys[0].IsDown();
        if (down && !prev)
        {
            u32 a = 0;
            if (GetTimeStopFlagAddr(a))
                W32(a, (R32(a) & 0x100) ? 0x000 : 0x100);
        }
        prev = down;
    }

    // ---------------------------------------------------------------
    //  メニュー構築
    // ---------------------------------------------------------------
    static void InitMenu(PluginMenu &menu)
    {
        MenuFolder *race = new MenuFolder(Col(0x01E30E, "レース"));
        *race += new MenuEntry(Col(0xFF8F01, "いつでもドリフト"), DriftAnytime,
                               "スピードや地面の状況に左右されずドリフトできます");
        *race += new MenuEntry(Col(0x01FF01, "ダート無効"), NoDirt,
                               "悪路による減速を無効化します");
        *race += new MenuEntry(Col(0x7BFF01, "地上化"), Grounded,
                               "水中を地上判定にします\n"
                               "チェックでON / 外すとOFF");
        *race += new MenuEntry(Col(0xFFD901, "壁衝突無効"), NoWallHit,
                               "壁の衝突判定を無効化\n"
                               "壁に当たってもスピードやドリフトが途切れません\n"
                               "チェックでON / 外すとOFF");
        *race += new MenuEntry(Col(0xFF01E9, "壁貫通"), WallClip,
                               "壁判定を無効化\n"
                               "コースアウト判定も消しコースをある程度自由に散策できます\n"
                               "チェックでON / 外すとOFF");
        *race += new MenuEntry(Col(g_shadow, "影の色"), ShadowColorFunc, ShadowColorMenu,
                               "3Dモデルの縁の影を任意の色に指定できます\n"
                               "ギアアイコンから色を選択\nチェックを外すと元に戻る");
        *race += new MenuEntry(Col(g_light, "光の色"), LightColorFunc, LightColorMenu,
                               "3Dモデルに当たっている光の色を任意の色に指定できます\n"
                               "ギアアイコンから色を選択\nチェックを外すと元に戻る");
        *race += new MenuEntry(Col(0xFFB601, "水中ライト"), UnderwaterLight,
                               "カートのスクリューにファイアボールのエフェクトを追加します。\n"
                               "水中時にライトを点灯しているような見た目になります");
        *race += new MenuEntry(Col(0x00F7FF, "スーパードリフト"), SuperDrift,
                               "ドリフト時のエフェクトを変更しかっこいい見た目にします");
        const bool jp = (g_region == REGION_JPN);   // 日本版だけのコード
        if (jp)
        {
            *race += new MenuEntry(Col(0x00EAFF, "グライダーボード常時起動"), GliderBoard,
                                   "キノピオサーキット\n"
                                   "GBAクッパキャッスル1で有効\n"
                                   "秒数で起動のボードを最初から起動させます");
            MenuEntry *board = new MenuEntry(Col(0x00EAFF, "グライダーボード操作"), GliderBoardControl,
                                             "キノピオサーキット\n"
                                             "GBAクッパキャッスル1で有効\n"
                                             "ボードを出したりしまったりできます");
            board->Hotkeys += Hotkey(Key::Y, "ボードを起動");
            *race += board;
            *race += new MenuEntry(Col(0xFFCD00, "ポイント換算即終了"), ResultPointSkip,
                                   "リザルト画面のポイントのカウントアップを\n"
                                   "1フレームで終わらせ、すぐ次の演出に進みます");
        }
        menu += race;

        // カスタムモード (日本版のみ)
        if (jp)
        {
            const std::string exclusive = "\nふうせんレース・コインレースは同時に使えません\n(ONにするともう片方は自動でOFFになります)";
            MenuFolder *custom = new MenuFolder(Col(0xFF8F01, "カスタムモード"));
            g_entBalloon = new MenuEntry(Col(0x001AFF, "ふうせんレース"), BalloonRace,
                                         "GPレースにふうせんバトルのルールを追加します。\n"
                                         "3つ割れるとコースアウト判定になります。\n"
                                         "ポイントは関係ありません\n"
                                         "レースに入る前に有効にしてください" + exclusive);
            *custom += g_entBalloon;
            g_entCoinRace = new MenuEntry(Col(0xFFE701, "コインレース"), CoinRace,
                                          "GPレースにコインバトルのルールを追加します。\n"
                                          "頭の上にコインが表示され、被弾で落としたコインを\n"
                                          "他の人が拾えます。順位はコインの枚数で決まります\n"
                                          "レースに入る前に有効にしてください" + exclusive);
            *custom += g_entCoinRace;
            *custom += new MenuEntry(Col(0x01FF8F, "アイテムレイン"), ItemRain, ItemRainSettings,
                                     "各カートの前方上空からアイテムを降らせます\n"
                                     "キーボードアイコンから降る量とアイテムスイッチを設定");
            MenuEntry *drop = new MenuEntry(Col(0x01FF8F, "アイテムドロップ"), ItemDropKey,
                                            std::string("自分のカートのすぐ前にアイテムを1つずつ落とします\n")
                                            + Col(0xFFE100, BTN_LEFT " コイン") + "\n"
                                            + Col(0xFF8F01, BTN_RIGHT " アイテムボックス") + "\n"
                                            + Col(0xF2F101, BTN_UP " バナナ") + "\n"
                                            + "ボタンはホットキーのアイコンから変更できます");
            drop->Hotkeys += Hotkey(Key::DPadLeft,  "コインを落とす");
            drop->Hotkeys += Hotkey(Key::DPadRight, "アイテムボックスを落とす");
            drop->Hotkeys += Hotkey(Key::DPadUp,    "バナナを落とす");
            *custom += drop;
            menu += custom;

            // メニュー (日本版のみ)
            MenuFolder *menuFolder = new MenuFolder(Col(0x007AFF, "メニュー"));
            g_entMenuCut = new MenuEntry(Col(0x007AFF, "メニュー高速化"), MenuSpeedup,
                                         "メニューの完了待ち・選択肢の流れ・暗転・退場待ちをカットし、\n"
                                         "cc選択の背景のマリオプレビューを消します");
            *menuFolder += g_entMenuCut;
            *menuFolder += new MenuEntry(Col(0x007AFF, "メニュー速度変更 (4倍速)"), MenuSpeed, MenuSpeedMenu,
                                         "レース外のとき、メニュー全体を速く動かします\n"
                                         "ボタン・キャラ・画面切り替え・フェードがすべて速くなります\n"
                                         "キーボードアイコンから速度を選べます");
            menu += menuFolder;
        }

        MenuFolder *player = new MenuFolder(Col(0x0186FF, "プレイヤー"));
        *player += new MenuEntry(Col(0xFFCA01, "金ハンドル"), GoldHandle);
        if (jp)
        {
            g_entGPTimer = new MenuEntry(Col(0xFFC400, "GPタイマー"), GPTimer,
                                         "GP時にもタイムアタックのタイマーを表示します");
            *player += g_entGPTimer;
            *player += new MenuEntry(Col(0x00EFFF, "ヘリモード"), HeliMode,
                                     "グライダー中にどこでも自由に方向転換でき、\n"
                                     "上昇の制限がなく、操作しない限り高度が下がりません\n"
                                     "下入力で降りられます");
            *player += new MenuEntry(Col(0xD6FF00, "グライダー押し戻し無し"), NoGliderPushback,
                                     "グライダーのコース正規ルートに戻ろうとする動きを無効化します");
            *player += new MenuEntry(Col(0xFFE100, "コイン上限99"), Coin99,
                                     "コインの取得上限を99枚にします\n"
                                     "10枚を超えた分もスピードは速くなります");
            *player += new MenuEntry(Col(0xFFE100, "コイン即復活"), CoinRespawn,
                                     "コインを取っても即復活し爆速で無限取得できます");
            *player += new MenuEntry(Col(0x00DFFF, "アイテムボックス即復活"), ItemBoxRespawn,
                                     "取られたアイテムボックスがすぐに復活します");
        }
        *player += new MenuEntry(Col(0xFF0106, "アイテム残る"), ItemStays,
                                 "サンダー被弾時やコースアウトしてもアイテムが消えません");
        *player += new MenuEntry("ゲッソー無効", NoBlooper,
                                 "ゲッソーの墨を非表示にします");
        *player += new MenuEntry(Col(0x0164FF, "無敵時間"), Invincible,
                                 "バトルモードの被弾直後の無敵時間を全モードに適用します");
        *player += new MenuEntry(Col(0xFFE701, "コイン固定"), CoinLock,
                                 jp ? "被弾してもコインを落としません"
                                    : "被弾してもコインを落としません\n"
                                      "(コースアウトは対象外)");   // USA/EUR はコースアウト時の行が未移植
        *player += new MenuEntry(Col(0xF7F901, "ムーンジャンプ"), MoonJump,
                                 Col(0xFF0108, BTN_R "+" BTN_UP " 上") + "\n"
                               + Col(0x01AFFF, BTN_R "+" BTN_DOWN " 下"));
        *player += new MenuEntry(Col(0x01E315, "リスポーン"), Respawn,
                                 std::string("その場でコースアウト判定にし、即リスポーンします\n")
                                 + Col(0x01F214, BTN_L "+" BTN_DOWN));
        *player += new MenuEntry(Col(0x01E1FF, "アイテム即確定"), InstantItem,
                                 "アイテムルーレットを短縮し、獲得直後に確定します");
        *player += new MenuEntry(Col(0xF7F901, "自動装備"), AutoEquip,
                                 "装備可能なアイテムを所持した状態で使用すると自動で装備します\n"
                                 "長押しは不要です。もう一度入力で使用されます");
        *player += new MenuEntry(Col(0x01FFE1, "自動JA"), AutoJA,
                                 "ボタンを押さなくても自動でジャンプアクションします");
        g_entRearView = new MenuEntry(Col(0x01C7FF, "バックミラー"), RearView,
                                 Col(0x01F214, BTN_LEFT));
        *player += g_entRearView;
        if (jp)
        {
            *player += new MenuEntry(Col(0xFFCD00, "CTGP-7式ライブリプレイ"), LiveReplay,
                                     std::string("リプレイ再生前に有効にすること。\n"
                                                 "アングルをプレイアングルとリプレイアングルに切り替えます。\n")
                                     + Col(0xFFCD00, BTN_X " 切替"));
            *player += new MenuEntry(Col(0xFF7700, "ポーズ制限解除"), PauseUnlock,
                                     "カウントダウン時、ゴール直後にもポーズメニューを開けるようにします");
        }
        menu += player;

        if (!jp) return;  // 以下は日本版のみ

        const std::string miiNote = "ギアアイコンから色を指定\n"
                                    "チェックを外すと元の色に戻る\n"
                                    "反映にはMiiの作り直しが必要\n(マイページを開いて戻るなど)";
        MenuFolder *mii = new MenuFolder(Col(0xC900FF, "Miiの色変更"));
        *mii += new MenuEntry(Col(0xC900FF, "髪の色"),       MiiHair,     MiiHairMenu,     miiNote);
        *mii += new MenuEntry(Col(0xC900FF, "メガネの色"),   MiiGlasses,  MiiGlassesMenu,  miiNote);
        *mii += new MenuEntry(Col(0xC900FF, "肌の色"),       MiiSkin,     MiiSkinMenu,     miiNote);
        *mii += new MenuEntry(Col(0xC900FF, "目の色"),       MiiEyes,     MiiEyesMenu,     miiNote);
        *mii += new MenuEntry(Col(0xC900FF, "口の色"),       MiiMouth,    MiiMouthMenu,    miiNote);
        *mii += new MenuEntry(Col(0xC900FF, "好きな色"),     MiiFavorite, MiiFavoriteMenu, miiNote);
        menu += mii;

        MenuFolder *beta = new MenuFolder(Col(0xFF5C5F, "BETA"));
        g_entTimeStop = new MenuEntry(Col(0xFF5C5F, "自分以外の時間停止"), TimeStopBeta,
                                      Col(0xFF0104, "動作が不安定です") + "\n"
                                      "GPタイマー・バックミラー・ふうせんレース・コインレース・\nメニュー高速化とは同時に使えません\n"
                                      "(ONにすると自動でOFFになります)");
        g_entTimeStop->Hotkeys += Hotkey(Key::Y, "停止 / 解除の切り替え");
        *beta += g_entTimeStop;
        menu += beta;
    }

    // ---------------------------------------------------------------
    //  Plugin設定: メニュー背景の色 (全リージョン共通)
    //  色は SD のプラグインフォルダに保存し、次回起動時に読み込む
    // ---------------------------------------------------------------
    struct ThemeSlot { const char *name; Color FwkSettings::*field; };
    static const ThemeSlot kThemeSlots[] = {
        { "背景の色",         &FwkSettings::BackgroundMainColor },
        { "背景のノイズの色", &FwkSettings::BackgroundSecondaryColor },
        { "背景の縁の色",     &FwkSettings::BackgroundBorderColor },
    };
    static const int  kThemeCount = sizeof(kThemeSlots) / sizeof(kThemeSlots[0]);
    static const char *kThemePath = "MK7_Cheat_theme.bin";
    static u32 g_themeDefault[kThemeCount];

    static inline u32 ColorToRGB(const Color &c) { return (c.r << 16) | (c.g << 8) | c.b; }

    static void SaveTheme(void)
    {
        File file;
        if (File::Open(file, kThemePath, File::RWC | File::TRUNCATE) != 0)
            return;
        u32 data[1 + kThemeCount];
        data[0] = 0x48544B4D; // "MKTH"
        for (int i = 0; i < kThemeCount; i++)
            data[1 + i] = ColorToRGB(FwkSettings::Get().*(kThemeSlots[i].field));
        file.Write(data, sizeof(data));
        file.Close();
    }

    // 起動時 (PatchProcess) に呼ぶ: 初期の色を覚えてから、保存した色を反映
    static void LoadTheme(FwkSettings &settings)
    {
        for (int i = 0; i < kThemeCount; i++)
            g_themeDefault[i] = ColorToRGB(settings.*(kThemeSlots[i].field));

        File file;
        if (File::Open(file, kThemePath, File::READ) != 0)
            return;
        u32 data[1 + kThemeCount] = { 0 };
        if (file.Read(data, sizeof(data)) == 0 && data[0] == 0x48544B4D)
        {
            for (int i = 0; i < kThemeCount; i++)
                settings.*(kThemeSlots[i].field) = ToColor(data[1 + i] & 0xFFFFFF);
        }
        file.Close();
    }

    static void ThemeMenu(MenuEntry *)
    {
        for (;;)
        {
            FwkSettings &settings = FwkSettings::Get();
            std::vector<std::string> opts;
            for (int i = 0; i < kThemeCount; i++)
                opts.push_back(Col(ColorToRGB(settings.*(kThemeSlots[i].field)), "■") + " " + kThemeSlots[i].name);
            opts.push_back("初期の色に戻す");
            opts.push_back("もどる");

            Keyboard kb("プラグインの背景の色\n変更した色は保存され、次回も使われます", opts);
            int idx = kb.Open();
            if (idx < 0 || idx == kThemeCount + 1)
                break;

            if (idx == kThemeCount)
            {
                for (int i = 0; i < kThemeCount; i++)
                    settings.*(kThemeSlots[i].field) = ToColor(g_themeDefault[i]);
                SaveTheme();
                continue;
            }

            u32 rgb = ColorToRGB(settings.*(kThemeSlots[idx].field));
            if (ColorPicker(kThemeSlots[idx].name, rgb))
            {
                settings.*(kThemeSlots[idx].field) = ToColor(rgb);
                SaveTheme();
            }
        }
    }

    static void AddPluginSettings(PluginMenu &menu)
    {
        MenuFolder *settings = new MenuFolder(Col(0xAAAAAA, "Plugin設定"));
        *settings += new MenuEntry(Col(0xDD00FF, "背景の色"), nullptr, ThemeMenu,
                                   "プラグインのメニューの背景・ノイズ・縁の色を\n"
                                   "カラーピッカーで変更します\n"
                                   "変更した色は保存され、次回起動時も使われます");
        menu += settings;
    }

    // ---------------------------------------------------------------
    //  CTRPF 必須関数
    // ---------------------------------------------------------------
    void PatchProcess(FwkSettings &settings)
    {
        LoadTheme(settings);
    }

    void OnProcessExit(void)
    {
        ItemRainShutdown(); // アイテムレインのフックを外す
    }

    int main(void)
    {
        DetectRegion();
        // タイトル: txt の1行目と同じ表記 / 右上のバージョン: プラグイン Ver.0.0.1
        const std::string title = Col(0xFF5C5F, std::string(g_rg->cheatTitle) + " ")
                                + Col(0xDD00FF, "By yk");
        const std::string about = std::string(g_rg->cheatTitle) + " By yk\n"
                                + "Plugin Ver.0.0.1\n"
                                + "JPN / USA / EUR 自動対応 (現在: " + g_rg->name + ")";
        PluginMenu *menu = new PluginMenu(title, 0, 0, 1, about);
        menu->SynchronizeWithFrame(true);
        InitMenu(*menu);
        AddPluginSettings(*menu);
        menu->Run();
        // ソフト終了時のクラッシュ対策:
        // delete menu でフォルダを片付ける処理 (MenuFolderImpl → ~MenuFolder) が
        // 壊れたポインタを読んで落ちるので、メニューは消さずにそのまま終了する。
        // (プラグインのメモリはプロセス終了時にまとめて解放される)
        return 0;
    }
}
