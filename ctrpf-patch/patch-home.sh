#!/bin/sh
# CTRPF の PluginMenuHome.cpp を書き換える
#  - 下画面の「Mapper」ボタン → 「FTP Server」。押すとプラグイン側の FtpServerMenu() (Sources/FtpServer.cpp)
#  - 下画面の「Game Guide」ボタン → 「RT Menu」。押すと RtMenuSettings() (Sources/RtMenu.cpp) で RT Menu の設定画面を開く
# 使い方: sh patch-home.sh <CTRPF のソースのルート>
set -e
F="$1/Library/source/CTRPluginFrameworkImpl/Menu/PluginMenuHome.cpp"

check() { grep -qF -- "$1" "$F" || { echo "patch-home.sh: '$1' が見つかりません ($2)"; exit 1; }; }

check '"Mapper"' 'ボタン名'
check '_hidMapperBtn.Lock();' 'ボタンの無効化'
check '// _hidMapperBtn();' 'ボタンを押したときの処理'
check '//_hidMapperBtn.Update(isTouched, touchPos);' 'ボタンの更新'
check '"Game Guide"' 'Game Guide のボタン名'
check '_gameGuideBtn.Lock();' 'Game Guide の無効化'
check 'if (_gameGuideBtn()) _gameGuideBtn_OnClick();' 'Game Guide を押したときの処理'

sed -i 's/"Mapper"/"FTP Server"/' "$F"
sed -i 's|^\([[:space:]]*\)_hidMapperBtn\.Lock();|\1// _hidMapperBtn.Lock(); (FTP Server として使う)|' "$F"
sed -i 's|// _hidMapperBtn();|if (_hidMapperBtn() \&\& FtpServerMenu != nullptr) FtpServerMenu();|' "$F"
sed -i 's|//_hidMapperBtn\.Update(isTouched, touchPos);|_hidMapperBtn.Update(isTouched, touchPos);|' "$F"

sed -i 's/"Game Guide"/"RT Menu"/' "$F"
sed -i 's|^\([[:space:]]*\)_gameGuideBtn\.Lock();|\1// _gameGuideBtn.Lock(); (RT Menu として使う)|' "$F"
sed -i 's|if (_gameGuideBtn()) _gameGuideBtn_OnClick();|if (_gameGuideBtn() \&\& RtMenuSettings != nullptr) RtMenuSettings();|' "$F"

# プラグイン側で定義する関数 (定義がなければ nullptr になる弱いシンボル)
sed -i '0,/^namespace CTRPluginFramework/s//namespace CTRPluginFramework { void FtpServerMenu(void) __attribute__((weak)); void RtMenuSettings(void) __attribute__((weak)); }\n\nnamespace CTRPluginFramework/' "$F"

grep -n 'FtpServerMenu\|RtMenuSettings\|FTP Server\|RT Menu\|_hidMapperBtn\|_gameGuideBtn' "$F"
