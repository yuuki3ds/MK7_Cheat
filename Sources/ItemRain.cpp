// アイテムレイン（プラグイン版） MK7 日本版 Ver1.2 (0004000000030600) 専用
// アイテム／コイン／アイテムボックスを各カートの前方上空から降らせる。
// ARコード版アイテムレイン（002D9668 のフック）とは同時に使えないので、AR版はOFFにすること。
//
// main.cpp のカスタムモードフォルダに「アイテムレイン」として登録済み (日本版のみ)。
// ソフト終了時は OnProcessExit から ItemRainShutdown() を呼び、フックを外す。

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>
#include "ItemRain.hpp"

namespace CTRPluginFramework
{
    // ---- ゲーム側アドレス（日本版1.2） ----
    static const u32 kHookAddr      = 0x002D9668;   // カート毎フレーム更新の2命令目 (mov r4, r0)
    static const u32 kHookOrig      = 0xE1A04000;
    static const u32 kVeneerAddr    = 0x005EBD24;   // 空き領域（2語だけ使う）
    static const u32 kRoot          = 0x006789B8;
    static const u32 kKey           = 0x75F1B26B;
    static const u32 kFnGetItemMgr  = 0x002BE564;   // (itemDir, type) -> mgr
    static const u32 kFnDropItem    = 0x002CDAD0;   // (mgr, playerIdx, pos*, vel*)
    static const u32 kFnCoinSlot    = 0x00415498;   // (coinMgr, playerIdx) -> slot
    static const u32 kFnCoinStart   = 0x004203A4;   // (coinObj, pos*)
    static const u32 kVtItemBox     = 0x0062EFA8;
    static const u32 kFnBroadUpdate = 0x003DB3BC;   // (登録, 位置*, 半径) 絞り込み表の更新

    enum { KIND_COIN = 0x10, KIND_BOX = 0x11 };

    // ---- 設定 ----
    struct Kind { const char *name; u8 id; bool on; };
    static Kind g_kinds[] = {
        { "バナナ",           0x00, true  },
        { "ミドリこうら",     0x01, true  },
        { "アカこうら",       0x02, true  },
        { "キノコ",           0x03, true  },
        { "ボムへい",         0x04, true  },
        { "ゲッソー",         0x05, true  },
        { "トゲゾーこうら",   0x06, true  },
        { "スター",           0x08, true  },
        { "ファイアフラワー", 0x0C, true  },
        { "コイン",           KIND_COIN, false },
        { "アイテムボックス", KIND_BOX,  false },
        // 09 キラー・0A サンダーはクラッシュ、0D しっぽは動作不良のため入れていない
    };
    static const int kNumKinds = sizeof(g_kinds) / sizeof(g_kinds[0]);

    static int   g_amountSel = 1;       // 量の選択 (0 少なめ / 1 ふつう / 2 多め / 3 大量)
    static u32   g_interval  = 241;     // カート更新の回数で数える（8台×60fps＝480/秒）。8で割って1余る値
    static float g_height    = 120.0f;  // 降らせる高さ
    static float g_fallSpeed = 3.0f;    // アイテムボックスの落下速度（1フレームあたり）
    static float g_boxFloat  = 15.0f;   // ボックスが止まる高さ（カートの高さ＋この値）

    // ---- 内部状態 ----
    static bool g_installed = false;
    static bool g_running   = false;   // レイン動作中
    static u32  g_counter   = 0;
    static u32  g_rng       = 0x1234ABCD;
    static u8   g_table[32];
    static int  g_tableLen  = 0;

    struct Fall { u32 obj; float ground; };
    static Fall g_falls[8];
    static u32  g_lastObjDir = 0;

    static inline bool ValidPtr(u32 p) { return p >= 0x14000000 && p < 0x18000000 && (p & 3) == 0; }

    static void RebuildTable(void)
    {
        g_tableLen = 0;
        for (int i = 0; i < kNumKinds; i++)
            if (g_kinds[i].on && g_tableLen < (int)sizeof(g_table))
                g_table[g_tableLen++] = g_kinds[i].id;
    }

    static u32 Rand(void)
    {
        g_rng = g_rng * 1664525u + 1013904223u;
        return g_rng;
    }

    // レースの管理表（アイテム管理 +0x38、オブジェクト管理 +0x18）
    static u32 GetDirectorTable(void)
    {
        u32 a = *(u32 *)(kRoot + 4);
        if (!a) return 0;
        a = *(u32 *)(a + 4);
        if (!a) return 0;
        a += 0x1E0;
        if (*(u8 *)(a + 8) == 0) return 0;
        u32 svc = *(u32 *)(a + 4) ^ kKey;
        if (!ValidPtr(svc)) return 0;
        u32 x = *(u32 *)(svc + 0x1C);
        return ValidPtr(x) ? x : 0;
    }

    // ---- アイテムボックス ----
    static void SetBoxPos(u32 box, float x, float y, float z)
    {
        float *p = (float *)(box + 0x48);    // 現在位置
        float *q = (float *)(box + 0x54);    // 現在位置の写し
        float *h = (float *)(box + 0x1F8);   // 復活時の基準位置（現在位置より15低い）
        p[0] = x; p[1] = y; p[2] = z;
        q[0] = x; q[1] = y; q[2] = z;
        h[0] = x; h[1] = y - 15.0f; h[2] = z;
        *(u32 *)(box + 0x6C) |= 1;           // 位置の更新要求

        // 当たり判定（球）の中心も動かす
        u32 col = *(u32 *)(box + 0x14);
        if (ValidPtr(col))
        {
            float *c = (float *)(col + 0x40);
            c[0] = x; c[1] = y; c[2] = z;

            // 当たり判定の「近くにいるか」の絞り込み表を新しい位置で登録し直す。
            // ボックスは動かない物体の扱いで、ゲーム側は開始時に一度しか登録しない。
            u32 ent = *(u32 *)(box + 0x84);
            if (ValidPtr(ent))
                ((void (*)(u32, float *, float))kFnBroadUpdate)(ent, c, *(float *)(box + 0x88));
        }

        // カートとの当たり判定に使われる球（行列の平行移動と中心。高さは見た目より17低い）
        u32 hit = *(u32 *)(box + 0x260);
        if (ValidPtr(hit))
        {
            float *m = (float *)hit;
            m[5]  = x;          // +0x14
            m[9]  = y - 17.0f;  // +0x24
            m[13] = z;          // +0x34
            m[24] = x;          // +0x60
            m[25] = y - 17.0f;  // +0x64
            m[26] = z;          // +0x68
        }
    }

    static void UpdateBoxes(u32 objDir)
    {
        if (objDir != g_lastObjDir)
        {   // レースが変わったら落下中リストを捨てる
            for (Fall &f : g_falls) f.obj = 0;
            g_lastObjDir = objDir;
        }
        for (Fall &f : g_falls)
        {
            if (!f.obj) continue;
            if (!ValidPtr(f.obj) || *(u32 *)f.obj != kVtItemBox) { f.obj = 0; continue; }
            float *p = (float *)(f.obj + 0x48);
            float y = p[1] - g_fallSpeed;
            if (y <= f.ground) { y = f.ground; }
            SetBoxPos(f.obj, p[0], y, p[2]);
            if (y <= f.ground) f.obj = 0;
        }
    }

    static void SpawnBox(u32 objDir, u32 rnd, const float *pos, float groundY)
    {
        if (!objDir) return;
        u32 count = *(u32 *)(objDir + 0x44);
        u32 arr   = *(u32 *)(objDir + 0x4C);
        if (!ValidPtr(arr) || count == 0 || count > 0x400) return;

        u32 boxes[64]; u32 n = 0;
        for (u32 i = 0; i < count && n < 64; i++)
        {
            u32 o = *(u32 *)(arr + i * 4);
            if (ValidPtr(o) && *(u32 *)o == kVtItemBox) boxes[n++] = o;
        }
        if (n == 0) return;

        Fall *slot = nullptr;
        for (Fall &f : g_falls) if (!f.obj) { slot = &f; break; }
        if (!slot) return;

        u32 box = boxes[(rnd >> 4) % n];
        for (Fall &f : g_falls) if (f.obj == box) return;   // 落下中の物は使わない

        SetBoxPos(box, pos[0], pos[1], pos[2]);
        slot->obj = box;
        slot->ground = groundY;
    }

    // ---- コイン ----
    static void SpawnCoin(u32 kart, u32 idx, float *pos)
    {
        u32 mgr = *(u32 *)(kart + 0x120C);
        if (!ValidPtr(mgr) || idx >= 8) return;
        int slot = ((int (*)(u32, u32))kFnCoinSlot)(mgr, idx);
        if (slot < 0) return;
        u32 base = mgr + idx * 44 + 0x48F4;
        if (*(u32 *)base <= (u32)slot) return;
        u32 coin = *(u32 *)(*(u32 *)(base + 8) + slot * 4);
        if (!ValidPtr(coin)) return;

        float *v = (float *)(coin + 0x44);          // 速度
        v[0] = v[1] = v[2] = 0.0f;
        float *m = (float *)(coin + 0x104);         // 向き（単位行列）
        for (int i = 0; i < 9; i++) m[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        ((void (*)(u32, float *))kFnCoinStart)(coin, pos);
    }

    // ---- カート更新ごとに呼ばれる本体 ----
    extern "C" void ItemRain_Tick(u32 kart)
    {
        if (!g_installed || !ValidPtr(kart)) return;

        u32 idx = *(u32 *)(kart + 0x84);
        u32 x = GetDirectorTable();
        if (!x) return;
        u32 itemDir = *(u32 *)(x + 0x38);
        u32 objDir  = *(u32 *)(x + 0x18);
        if (!ValidPtr(objDir)) objDir = 0;

        if (idx == 0 && objDir) UpdateBoxes(objDir);   // 1フレームに1回

        if (!g_running || g_tableLen == 0 || g_interval == 0) return;
        if (++g_counter < g_interval) return;
        g_counter = 0;

        u32 rnd  = Rand();
        u8  kind = g_table[(rnd >> 24) % g_tableLen];

        // 位置：カートの前方 150〜661、左右 ±128、上空 g_height
        const float *k = (const float *)kart;
        float d = 150.0f + (float)(rnd & 0x1FF);
        float l = (float)((rnd >> 9) & 0xFF) - 128.0f;
        float pos[3], vel[3] = { 0.0f, 0.0f, 0.0f };
        pos[0] = k[9]  + k[6] * d + k[0] * l;
        pos[1] = k[10] + k[7] * d + g_height;
        pos[2] = k[11] + k[8] * d + k[2] * l;

        if (kind == KIND_COIN)
        {
            SpawnCoin(kart, idx, pos);
        }
        else if (kind == KIND_BOX)
        {
            // ボックスは、いまカートがいる場所の真上から落とす（地面の高さが確実に分かるため）
            float bl = (float)((rnd >> 9) & 0x7F) - 64.0f;
            float bpos[3] = { k[9] + k[0] * bl, k[10] + g_height, k[11] + k[2] * bl };
            SpawnBox(objDir, rnd, bpos, k[10] + g_boxFloat);
        }
        else
        {
            if (!ValidPtr(itemDir)) return;
            u32 mgr = ((u32 (*)(u32, u32))kFnGetItemMgr)(itemDir, kind);
            if (!ValidPtr(mgr)) return;
            ((void (*)(u32, u32, float *, float *))kFnDropItem)(mgr, idx, pos, vel);
        }
    }

    // ---- フック用スタブ（ARM） ----
    extern "C" void ItemRain_Stub(void);
    __asm__(
        ".arm\n"
        ".align 2\n"
        ".global ItemRain_Stub\n"
        ".type ItemRain_Stub, %function\n"
        "ItemRain_Stub:\n"
        "    push  {r0-r3, r12, lr}\n"
        "    vpush {d0-d7}\n"
        "    mov   r4, sp\n"            // r4 はこの直後に上書きされるので自由に使える
        "    bic   sp, sp, #7\n"
        "    bl    ItemRain_Tick\n"
        "    mov   sp, r4\n"
        "    vpop  {d0-d7}\n"
        "    pop   {r0-r3, r12, lr}\n"
        "    mov   r4, r0\n"            // 上書きした元の命令
        "    ldr   pc, =0x002D966C\n"
        "    .ltorg\n"
    );

    static void Install(void)
    {
        if (g_installed) return;
        for (Fall &f : g_falls) f.obj = 0;
        g_lastObjDir = 0;
        g_counter = 0;
        RebuildTable();
        // 空き領域に中継（ldr pc, [pc, #-4] / スタブのアドレス）
        Process::Patch(kVeneerAddr,     0xE51FF004);
        Process::Patch(kVeneerAddr + 4, (u32)ItemRain_Stub);
        // b kVeneerAddr
        u32 br = 0xEA000000 | (((kVeneerAddr - kHookAddr - 8) >> 2) & 0x00FFFFFF);
        Process::Patch(kHookAddr, br);
        g_installed = true;
    }

    static void Uninstall(void)
    {
        if (!g_installed) return;
        g_running = false;
        Process::Patch(kHookAddr, kHookOrig);
        Process::Patch(kVeneerAddr,     0);
        Process::Patch(kVeneerAddr + 4, 0);
        g_installed = false;
    }

    // ---- メニュー用 ----
    void ItemRain(MenuEntry *entry)
    {
        if (Process::GetTitleID() != 0x0004000000030600ULL) return;

        if (!entry->IsActivated())
        {
            Uninstall();
            return;
        }
        if (!g_installed) Install();
        g_running = true;
    }

    // ソフト終了時に呼ぶ (フックを元に戻す)
    void ItemRainShutdown(void)
    {
        Uninstall();
    }

    void ItemRainSettings(MenuEntry *entry)
    {
        (void)entry;
        static const char *amountNames[] = {
            "少なめ（約1個/秒）", "ふつう（約2個/秒）", "多め（約5個/秒）", "大量（約10個/秒）"
        };
        static const u32 amountVals[] = { 481, 241, 97, 49 };
        static const char *heightNames[] = { "低い（60）", "ふつう（120）", "高い（240）" };
        static const float heightVals[] = { 60.0f, 120.0f, 240.0f };

        for (;;)
        {
            int onCount = 0;
            for (int i = 0; i < kNumKinds; i++)
                if (g_kinds[i].on) onCount++;

            const std::string amountLabel = std::string("降る量：") + amountNames[g_amountSel];
            const std::string switchLabel = "アイテムスイッチ：" + std::to_string(onCount) + "種類ON";
            int hSel = 1;
            for (int i = 0; i < 3; i++)
                if (g_height == heightVals[i]) hSel = i;
            const std::string heightLabel = std::string("高さ：") + heightNames[hSel];
            Keyboard top("アイテムレインの設定", { amountLabel, switchLabel, heightLabel, "もどる" });
            int c = top.Open();
            if (c == 0)
            {
                std::vector<std::string> opts;
                for (int i = 0; i < 4; i++)
                    opts.push_back(std::string(i == g_amountSel ? "● " : "　") + amountNames[i]);
                Keyboard kb("降る量", opts);
                int s = kb.Open();
                if (s >= 0 && s < 4)
                {
                    g_amountSel = s;
                    g_interval = amountVals[s];
                    g_counter = 0;
                }
            }
            else if (c == 1)
            {
                // アイテムスイッチ: 押すたびに ON/OFF
                int sel = 0;
                for (;;)
                {
                    std::vector<std::string> opts;
                    for (int i = 0; i < kNumKinds; i++)
                    {
                        const std::string mark = g_kinds[i].on
                            ? (Color(0x01, 0xE3, 0x0E) << std::string("[ON] "))
                            : (Color(0x96, 0x96, 0x96) << std::string("[OFF]"));
                        opts.push_back(mark + (Color(0xFF, 0xFF, 0xFF) << (std::string(" ") + g_kinds[i].name)));
                    }
                    opts.push_back("すべてON");
                    opts.push_back("すべてOFF");
                    opts.push_back("もどる");
                    Keyboard kb("アイテムスイッチ\n押すとON/OFFが切り替わります", opts);
                    kb.ChangeSelectedEntry(sel);
                    int s = kb.Open();
                    if (s < 0 || s == kNumKinds + 2) break;
                    if (s == kNumKinds)          for (int i = 0; i < kNumKinds; i++) g_kinds[i].on = true;
                    else if (s == kNumKinds + 1) for (int i = 0; i < kNumKinds; i++) g_kinds[i].on = false;
                    else                         g_kinds[s].on = !g_kinds[s].on;
                    sel = s;
                }
                RebuildTable();
            }
            else if (c == 2)
            {
                std::vector<std::string> opts;
                for (int i = 0; i < 3; i++)
                    opts.push_back(std::string(i == hSel ? "● " : "　") + heightNames[i]);
                Keyboard kb("降らせる高さ", opts);
                int s = kb.Open();
                if (s >= 0 && s < 3) g_height = heightVals[s];
            }
            else
                break;
        }
    }
}
