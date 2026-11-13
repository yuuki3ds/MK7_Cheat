// RT Menu: ゲームを止めずに、下画面に出したチート一覧でエントリを ON/OFF する
//  - SELECT (いつでも) か、CTRPF 下画面メニューの「RT Menu」ボタンで開く / SELECT で閉じる
//  - 十字キー ↑↓ 選択、←→ ページ送り、A 決定 (フォルダを開く / ON・OFF)、B もどる (一番上なら閉じる)
//  - 中身は上画面のメニューと同じ (PluginMenu のフォルダとエントリをそのまま使う)
//  - ギア・キーボードアイコンの設定画面は開けない (ゲームが動いたままなので)。ON/OFF のみ
//  - CTRPF 側の改造は ctrpf-patch/patch-home.sh (Game Guide ボタン → RT Menu)

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "CTRPluginFrameworkImpl/Graphics/Renderer.hpp"
#include <string>
#include <vector>

namespace CTRPluginFramework
{
    namespace
    {
        const int kRows   = 10;        // 1ページの行数
        const int kRowH   = 18;
        const int kListY  = 24;

        bool  g_built = false;
        std::vector<MenuFolder *>                g_folders;
        std::vector<std::vector<MenuEntry *>>    g_entries;
        std::vector<MenuEntry *>                 g_rootEntries;   // フォルダに入っていないエントリ

        volatile bool g_open = false;
        int   g_folder  = -1;          // -1 = 一番上
        int   g_sel     = 0;
        int   g_rootSel = 0;
        int   g_scroll  = 0;
        u32   g_holdFrames = 0;

        void Build(void)
        {
            if (g_built) return;
            PluginMenu *menu = PluginMenu::GetRunningInstance();
            if (!menu) return;
            g_folders = menu->GetFolderList();
            g_rootEntries = menu->GetEntryList();
            g_entries.clear();
            for (MenuFolder *f : g_folders)
                g_entries.push_back(f->GetEntryList());
            g_built = true;
        }

        int Count(void)
        {
            if (g_folder < 0) return (int)(g_folders.size() + g_rootEntries.size());
            return (int)g_entries[g_folder].size();
        }

        // 一番上の i 番目がエントリなら返す (フォルダなら nullptr)
        MenuEntry *EntryAt(int i)
        {
            if (g_folder >= 0) return (i >= 0 && i < (int)g_entries[g_folder].size()) ? g_entries[g_folder][i] : nullptr;
            i -= (int)g_folders.size();
            return (i >= 0 && i < (int)g_rootEntries.size()) ? g_rootEntries[i] : nullptr;
        }

        void Clamp(void)
        {
            const int n = Count();
            if (n == 0) { g_sel = 0; g_scroll = 0; return; }
            if (g_sel < 0) g_sel = n - 1;
            if (g_sel >= n) g_sel = 0;
            if (g_sel < g_scroll) g_scroll = g_sel;
            if (g_sel >= g_scroll + kRows) g_scroll = g_sel - kRows + 1;
        }

        // 押した瞬間と、押しっぱなし (約0.4秒後から 4 フレームごと)
        bool Repeat(Key k)
        {
            if (Controller::IsKeyPressed(k)) { g_holdFrames = 0; return true; }
            if (!Controller::IsKeyDown(k)) return false;
            ++g_holdFrames;
            return g_holdFrames > 24 && (g_holdFrames % 4) == 0;
        }

        // ---- 描画 (ゲームの下画面に毎フレーム描く) ----
        bool Draw(const Screen &screen)
        {
            if (!g_open || screen.IsTop) return false;

            const FwkSettings &st = FwkSettings::Get();
            const Color &bg   = st.BackgroundMainColor;
            const Color &bd   = st.BackgroundBorderColor;
            const Color &txt  = st.MainTextColor;
            const Color  sel(0x40, 0x40, 0x60);
            const Color  on(0x01, 0xE3, 0x0E);
            const Color  off(0x96, 0x96, 0x96);

            Renderer::SetTarget(BOTTOM);
            Renderer::DrawRect(0, 0, 320, 240, bg);
            Renderer::DrawRect(0, 20, 320, 1, bd);
            Renderer::DrawRect(0, 219, 320, 1, bd);

            int y = 2;
            const std::string title = g_folder < 0 ? std::string("RT Menu") : "RT Menu > " + g_folders[g_folder]->Name();
            Renderer::DrawSysString(title.c_str(), 6, y, 316, txt);

            const int n = Count();
            char page[16];
            snprintf(page, sizeof(page), "%d/%d", n ? g_sel + 1 : 0, n);
            y = 2;
            Renderer::DrawSysString(page, 314 - (int)Renderer::GetTextSize(page), y, 316, txt);

            for (int r = 0; r < kRows; r++)
            {
                const int i = g_scroll + r;
                if (i >= n) break;
                const int rowY = kListY + r * kRowH;
                if (i == g_sel) Renderer::DrawRect(2, rowY - 1, 316, kRowH, sel);

                y = rowY;
                MenuEntry *e = EntryAt(i);
                if (!e)
                {   // フォルダ
                    const std::string s = " " + g_folders[i]->Name();
                    Renderer::DrawSysString(s.c_str(), 8, y, 316, txt);
                    continue;
                }
                const bool usable = e->GetGameFunc() != nullptr;
                const bool act = e->IsActivated();
                int x = Renderer::DrawSysString(!usable ? "  -  " : (act ? "[ON] " : "[OFF]"), 8, y,
                                                316, !usable ? off : (act ? on : off));
                y = rowY;
                Renderer::DrawSysString(e->Name().c_str(), x + 6, y, 316, usable ? txt : off);
            }

            y = 222;
            Renderer::DrawSysString(g_folder < 0
                    ? " 選択  決定 SELECT 閉じる"
                    : " 選択  ON/OFF  もどる SELECT 閉じる",
                    6, y, 316, txt);
            return true;
        }

        // ---- 操作 (プラグインのループから毎フレーム) ----
        void Tick(void)
        {
            if (!g_open)
            {
                if (Controller::IsKeyPressed(Key::Select))
                {
                    Build();
                    g_open = true;
                }
                return;
            }

            if (Controller::IsKeyPressed(Key::Select)) { g_open = false; return; }

            if (Repeat(Key::DPadUp))        { g_sel--; Clamp(); }
            else if (Repeat(Key::DPadDown)) { g_sel++; Clamp(); }
            else if (Repeat(Key::DPadLeft)) { g_sel = g_sel >= kRows ? g_sel - kRows : 0; Clamp(); }
            else if (Repeat(Key::DPadRight))
            {
                const int n = Count();
                g_sel = (g_sel + kRows < n) ? g_sel + kRows : (n ? n - 1 : 0);
                Clamp();
            }

            if (Controller::IsKeyPressed(Key::A))
            {
                MenuEntry *e = EntryAt(g_sel);
                if (!e && g_folder < 0 && g_sel < (int)g_folders.size())
                {
                    g_rootSel = g_sel;
                    g_folder = g_sel;
                    g_sel = 0;
                    g_scroll = 0;
                    Clamp();
                }
                else if (e && e->GetGameFunc() != nullptr)
                {
                    if (e->IsActivated()) e->Disable();
                    else                  e->Enable();
                }
            }
            else if (Controller::IsKeyPressed(Key::B))
            {
                if (g_folder < 0) g_open = false;
                else
                {
                    g_folder = -1;
                    g_sel = g_rootSel;
                    g_scroll = 0;
                    Clamp();
                }
            }
        }
    }

    // CTRPF 下画面メニューの「RT Menu」ボタンから呼ばれる (ctrpf-patch/patch-home.sh)
    void RtMenuOpen(void)
    {
        Build();
        g_open = true;
    }

    // main() から呼ぶ: 操作のコールバックと描画の登録
    void RtMenuInit(PluginMenu &menu)
    {
        menu += Tick;
        OSD::Run(Draw);
    }
}
