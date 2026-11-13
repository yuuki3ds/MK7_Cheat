#ifndef ITEMRAIN_HPP
#define ITEMRAIN_HPP

#include <CTRPluginFramework.hpp>

namespace CTRPluginFramework
{
    // アイテムレイン本体（MenuEntry の GameFunc に指定）
    void ItemRain(MenuEntry *entry);
    // 降る量・アイテムスイッチの設定（MenuEntry の MenuFunc に指定）
    void ItemRainSettings(MenuEntry *entry);
    // ソフト終了時にフックを外す（OnProcessExit から呼ぶ）
    void ItemRainShutdown(void);
}

#endif
