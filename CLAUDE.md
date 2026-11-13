# MK7_Cheat (3DS マリオカート7 用 CTRPF プラグイン)

最初に `HANDOFF.md` を読むこと。これまでの経緯、メニュー構成、空き領域の使い方、未確認の点がまとまっている。

## 要点
- ユーザーへの説明は日本語で行う。
- テスト用のコードを出すときは、各行の OFF 値（元の値）も併記する。
- 対象は日本版 Ver1.2 (0004000000030600)。USA (030800) / EUR (030700) は main.cpp の C()/S() でアドレスをずらして対応。
- 空き領域 0x5EBD24〜 を複数のエントリが使うので、新しいコードを足すときは HANDOFF.md の表で重ならないか確認する。

## 構成
- `Sources/` プラグインのソース (main.cpp, ItemRain.cpp)
- `Includes/` ヘッダー
- `ctrpf-patch/` 日本語キーボード入りに改造した CTRPF の KeyboardImpl.cpp / .hpp
- `tools/skk2ctrpf.py` SKK 辞書の変換スクリプト
- `.github/workflows/build.yml` GitHub Actions でのビルド

## ビルド
- このセッションには devkitPro がないので、ビルドは GitHub Actions で行う。
- 変更したらコミットして push し、`gh run watch` でビルドを待つ。
- 失敗したら `gh run view --log-failed` でエラーを確認して直す。
- 成功したら `gh run download -n MK7_Cheat-3gx` で .3gx を取得し、ユーザーに渡す。
