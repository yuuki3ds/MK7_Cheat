// RT Menu: ゲームを止めずに、下画面に出したチート一覧でエントリを ON/OFF する
//  - 開くボタン (初期値 SELECT) でいつでも開閉
//  - 十字キー ↑↓ 選択、←→ ページ送り、A 決定 (フォルダを開く / ON・OFF)、B もどる (一番上なら閉じる)
//  - 中身は上画面のメニューと同じ。ただし RT で使えない項目 (設定画面だけのエントリ) と、
//    それしかないフォルダは出さない
//  - CTRPF 下画面メニューの「RT Menu」ボタン (Game Guide を置き換え) で設定画面を開く
//    (有効 / 無効、開くボタン)。設定は MK7_Cheat_rtmenu.bin に保存
//  - CTRPF 側の改造は ctrpf-patch/patch-home.sh

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "CTRPluginFrameworkImpl/Graphics/Renderer.hpp"
#include <string>
#include <vector>

namespace CTRPluginFramework
{
    namespace
    {
        const int kRows  = 8;          // 1ページの行数
        const int kRowH  = 16;
        const int kHeadH = 18;
        const int kW     = 320;

        struct OpenKey { const char *name; u32 keys; };
        const OpenKey kOpenKeys[] = {
            { "SELECT",          Key::Select },
            { "L + SELECT",      Key::L | Key::Select },
            { "R + SELECT",      Key::R | Key::Select },
            { "Y + SELECT",      Key::Y | Key::Select },
            { "L + R",           Key::L | Key::R },
            { "L + R + SELECT",  Key::L | Key::R | Key::Select },
        };
        const int kOpenKeyCount = sizeof(kOpenKeys) / sizeof(kOpenKeys[0]);
        const char *kSavePath = "MK7_Cheat_rtmenu.bin";

        bool  g_enabled = true;
        int   g_keySel  = 0;

        // 表示する項目 (Build で作る)
        struct Folder { std::string label; std::vector<MenuEntry *> entries; };
        bool                g_built = false;
        std::vector<Folder> g_folders;

        volatile bool g_open = false;
        int   g_folder  = -1;          // -1 = 一番上 (フォルダ一覧)
        int   g_sel     = 0;
        int   g_rootSel = 0;
        int   g_scroll  = 0;
        u32   g_holdFrames = 0;

        void Save(void)
        {
            File f;
            if (File::Open(f, kSavePath, File::RWC | File::TRUNCATE) != 0) return;
            u32 data[3] = { 0x544D5452 /* "RTMT" */, (u32)g_enabled, (u32)g_keySel };
            f.Write(data, sizeof(data));
            f.Close();
        }

        void Load(void)
        {
            File f;
            if (File::Open(f, kSavePath, File::READ) != 0) return;
            u32 data[3] = { 0 };
            if (f.Read(data, sizeof(data)) == 0 && data[0] == 0x544D5452)
            {
                g_enabled = data[1] != 0;
                g_keySel  = (data[2] < (u32)kOpenKeyCount) ? (int)data[2] : 0;
            }
            f.Close();
        }

        bool Usable(MenuEntry *e) { return e && e->GetGameFunc() != nullptr && e->IsVisible(); }

        void Build(void)
        {
            if (g_built) return;
            PluginMenu *menu = PluginMenu::GetRunningInstance();
            if (!menu) return;
            for (MenuFolder *mf : menu->GetFolderList())
            {
                Folder f;
                for (MenuEntry *e : mf->GetEntryList())
                    if (Usable(e)) f.entries.push_back(e);
                if (f.entries.empty()) continue;   // RT で使える項目がないフォルダは出さない
                f.label = " " + mf->Name();
                g_folders.push_back(f);
            }
            g_built = true;
        }

        int Count(void)
        {
            return g_folder < 0 ? (int)g_folders.size() : (int)g_folders[g_folder].entries.size();
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

        bool OpenKeyPressed(void)
        {
            const u32 keys = kOpenKeys[g_keySel].keys;
            return (Controller::GetKeysDown() & keys) == keys && (Controller::GetKeysPressed() & keys) != 0;
        }

        // ---- 描画 (ゲームの下画面に毎フレーム描く。描く範囲は項目の数だけにして軽くする) ----
        bool Draw(const Screen &screen)
        {
            if (!g_open || screen.IsTop) return false;

            const FwkSettings &st = FwkSettings::Get();
            const Color &bg  = st.BackgroundMainColor;
            const Color &bd  = st.BackgroundBorderColor;
            const Color &txt = st.MainTextColor;
            static const Color sel(0x40, 0x40, 0x60);
            static const Color on(0x01, 0xE3, 0x0E);
            static const Color off(0x96, 0x96, 0x96);

            const int n = Count();
            const int rows = n < kRows ? n : kRows;
            const int h = kHeadH + rows * kRowH + 2;

            Renderer::SetTarget(BOTTOM);
            Renderer::DrawRect(0, 0, kW, h, bg);
            Renderer::DrawRect(0, kHeadH - 1, kW, 1, bd);

            int y = 1;
            Renderer::DrawSysString(g_folder < 0 ? "RT Menu" : g_folders[g_folder].label.c_str() + 4, 4, y, kW - 4, txt);

            for (int r = 0; r < rows; r++)
            {
                const int i = g_scroll + r;
                if (i >= n) break;
                const int rowY = kHeadH + r * kRowH;
                if (i == g_sel) Renderer::DrawRect(0, rowY, kW, kRowH, sel);

                y = rowY;
                if (g_folder < 0)
                {
                    Renderer::DrawSysString(g_folders[i].label.c_str(), 6, y, kW - 4, txt);
                    continue;
                }
                MenuEntry *e = g_folders[g_folder].entries[i];
                const bool act = e->IsActivated();
                int x = Renderer::DrawSysString(act ? "ON" : "OFF", 6, y, kW - 4, act ? on : off);
                y = rowY;
                Renderer::DrawSysString(e->Name().c_str(), x > 34 ? x + 6 : 40, y, kW - 4, txt);
            }
            return true;
        }

        // ---- 操作 (プラグインのループから毎フレーム) ----
        void Tick(void)
        {
            if (!g_enabled) { g_open = false; return; }

            if (OpenKeyPressed())
            {
                if (!g_open) Build();
                g_open = !g_open;
                return;
            }
            if (!g_open) return;

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
                if (g_folder < 0)
                {
                    if (g_sel < (int)g_folders.size())
                    {
                        g_rootSel = g_sel;
                        g_folder = g_sel;
                        g_sel = 0;
                        g_scroll = 0;
                    }
                }
                else
                {
                    MenuEntry *e = g_folders[g_folder].entries[g_sel];
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

    // CTRPF 下画面メニューの「RT Menu」ボタンから呼ばれる設定画面 (ctrpf-patch/patch-home.sh)
    void RtMenuSettings(void)
    {
        for (;;)
        {
            const std::string s0 = std::string("RT Menu：") + (g_enabled ? "有効" : "無効");
            const std::string s1 = std::string("開くボタン：") + kOpenKeys[g_keySel].name;
            Keyboard kb("RT Menu の設定\n"
                        "ゲームを止めずに、下画面のチート一覧で ON/OFF できます\n"
                        " 選択  ページ  決定  もどる",
                        { s0, s1, "もどる" });
            const int c = kb.Open();
            if (c == 0)
            {
                g_enabled = !g_enabled;
                if (!g_enabled) g_open = false;
                Save();
            }
            else if (c == 1)
            {
                std::vector<std::string> opts;
                for (int i = 0; i < kOpenKeyCount; i++)
                    opts.push_back(std::string(i == g_keySel ? "● " : "　") + kOpenKeys[i].name);
                Keyboard k2("RT Menu を開くボタン", opts);
                const int s = k2.Open();
                if (s >= 0 && s < kOpenKeyCount) { g_keySel = s; Save(); }
            }
            else
                break;
        }
    }

    // main() から呼ぶ: 設定の読み込み、操作のコールバックと描画の登録
    void RtMenuInit(PluginMenu &menu)
    {
        Load();
        menu += Tick;
        OSD::Run(Draw);
    }
}
