# MK7_Cheat (3DS マリオカート7 用 CTRPF プラグイン)

## 構成
- `Sources/` プラグインのソース (main.cpp, ItemRain.cpp など)
- `Includes/` ヘッダー
- `ctrpf-patch/` 日本語キーボード入りに改造した CTRPF の KeyboardImpl.cpp / .hpp
- `.github/workflows/build.yml` GitHub Actions でのビルド

## ビルド
- このセッションには devkitPro がないので、ビルドは GitHub Actions で行う。
- 変更したらコミットして push し、`gh run watch` でビルドを待つ。
- 失敗したら `gh run view --log-failed` でエラーを確認して直す。
- 成功したら `gh run download -n MK7_Cheat-3gx` で .3gx を取得し、ユーザーに渡す。

## 注意
- 対象は日本版 Ver1.2 (0004000000030600)。USA (030800) / EUR (030700) は main.cpp の C()/S() でアドレスをずらして対応。
- 空き領域 0x5EBD24〜 を複数のエントリが使うので、新しいコードを足すときは重ならないか確認する。
- ユーザーへの説明は日本語で行う。
