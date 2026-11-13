MK7_Cheat (CTRPF 3gx プラグイン) ソース一式

Sources/      main.cpp, ItemRain.cpp      → テンプレートの Sources に上書き
Includes/     ItemRain.hpp                → テンプレートの Includes に入れる
ctrpf-patch/  KeyboardImpl.cpp / .hpp     → 日本語キーボード入り CTRPF (ctrpf-dev の Library 内の同名ファイルに上書き)
tools/        skk2ctrpf.py                → SKK辞書を skk_jisyo.dic に変換 (sd:/luma/plugins/ に置く)
.github/      GitHub Actions のビルド設定
CLAUDE.md     Claude Code 用の説明
.gitignore    ビルドでできるファイルを除外

テンプレートの Makefile と .plgInfo はこの zip に含まれていません (テンプレートのものを使う)。
