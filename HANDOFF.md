# 引き継ぎメモ（claude.ai のチャットから Claude Code へ）

このリポジトリは、3DS「マリオカート7」用の CTRPF プラグイン（.3gx）「MK7_Cheat」のソースです。
ユーザー（yk）が作った AR コード（cheats の txt）を、claude.ai のチャットで C++ に移植してきました。
この文書は、その経緯と今の状態をまとめたものです。ユーザーへの説明はすべて日本語で行ってください。

---

## 1. ユーザーの方針（必ず守る）

- **テスト用のコードを出すときは、各行の OFF 値（元の値）も併記する。**
- **動作した・効果があった・副産物のコードは、コード集のドキュメントに記録する。** 動かなかったコードとテストコードは記録しない。
  - コード集：https://claude.ai/artifact/CZ4FsJ5gv5e5JYuP41fLBd （claude.ai の Docs。Claude Code から触れない場合は、追記すべき内容をユーザーに伝える）
  - 記録は裏方で行い、共有や追記の報告は不要。
- コードの OFF 値は、基本的にこのコード集に載っている。新しいエントリを作るときは、まずコード集の OFF 値を使う。

## 2. 対象

| 版 | タイトルID | 対応 |
| --- | --- | --- |
| 日本版 Ver1.2 | 0004000000030600 | すべてのエントリ |
| EUR | 0004000000030700 | 共通エントリのみ |
| USA | 0004000000030800 | 共通エントリのみ |

- 起動時に `DetectRegion()` で版を判定する。
- アドレスは日本版で書き、`C()`（コード部：USA −0x28、EUR −0x08）、`S()`（影・光の色：USA/EUR とも −8）でずらす。
- ムーンジャンプの常時書き込み行とプレイヤーポインタの条件・オフセットは `kRegions[]` に版ごとに持つ。
- 日本版だけのエントリは `InitMenu` 内の `if (jp)` の中で登録する。

## 3. ファイル構成

| 場所 | 中身 |
| --- | --- |
| `Sources/main.cpp` | ほぼ全エントリとメニュー構築、カラーピッカー、Plugin設定 |
| `Sources/ItemRain.cpp` / `Includes/ItemRain.hpp` | アイテムレイン・アイテムドロップ（ユーザー作、こちらで設定画面などを追加） |
| `ctrpf-patch/KeyboardImpl.cpp` / `.hpp` | 日本語キーボード入りに改造した CTRPF の内部ファイル |
| `tools/skk2ctrpf.py` | SKK 辞書を `skk_jisyo.dic` に変換（SD の `/luma/plugins/` に置く） |
| `.github/workflows/build.yml` | GitHub Actions でのビルド |
| `Makefile`、`.plgInfo` | テンプレート（CTRPluginFramework-BlankTemplate）のもの |

## 4. ビルド

- このセッションには devkitPro がないので、ビルドは GitHub Actions で行う。
- 流れ：変更 → コミット → push → `gh run watch` → 失敗なら `gh run view --log-failed` → 成功なら `gh run download -n MK7_Cheat-3gx`。
- Actions は毎回、devkitPro の Docker イメージ上で次を行う（PC の devkitPro には影響しない）：
  1. 公式の libctrpf と 3gxtool を dkp-pacman で入れる
  2. CTRPF のソースを取得し、`ctrpf-patch/` の KeyboardImpl で上書き、`Library/Makefile` の `-Werror` を消してビルド
  3. できた `libctrpf*.a` と `include` を `$DEVKITPRO/libctrpf` に上書き
  4. プラグインを `make`
- **まだ一度も Actions で動かしていない。** 最初のビルドで次の点が外れている可能性がある：
  - Linux 用 3gxtool のパッケージ置き場 `https://thepixellizeross.gitlab.io/packages/x86_64/linux`（Windows 用からの推測）
  - KeyboardImpl のコピー先 `Library/source/CTRPluginFrameworkImpl/Menu/` と `Library/include/CTRPluginFrameworkImpl/Menu/`
- `CTRPF_REF` は今 `develop`。ユーザーの PC で動作確認したコミットに固定するのが望ましい（ユーザーに `ctrpf-dev` で `git rev-parse HEAD` を実行してもらう）。
- 経緯：CTRPF の 0.8.0 タグや OP-CTRPF 1.1.0（CTGP-7 用の派生）でビルドしたライブラリでは、プラグインの読み込み時にクラッシュした。**develop（pacman の 0.8.0.r1444 相当）でのみ動作した。**

## 5. メニュー構成（main.cpp の InitMenu）

- **レース**：いつでもドリフト、ダート無効、地上化*、壁衝突無効*、壁貫通*、影の色*、光の色*、水中ライト、スーパードリフト、［日本］グライダーボード常時起動、グライダーボード操作（ホットキー Y、キノピオサーキット / GBAクッパキャッスル1）、ポイント換算即終了*
- **カスタムモード**［日本］：ふうせんレース*、コインレース*（コイン枚数で順位決定 47 行を含む）、アイテムレイン、アイテムドロップ（ホットキー ← → ↑）
- **メニュー**［日本］：メニュー高速化*（28 行）、メニュー速度変更（2/3/4/6/8 倍、レース外のみ）
- **プレイヤー**：金ハンドル、［日本］GPタイマー*、ヘリモード*、グライダー押し戻し無し、コイン上限99（8人分）、コイン即復活*、アイテムボックス即復活*、アイテム残る、ゲッソー無効、無敵時間、コイン固定（コースアウト行は日本のみ）、ムーンジャンプ、リスポーン、アイテム即確定、自動装備、自動JA、バックミラー*、［日本］CTGP-7式ライブリプレイ、ポーズ制限解除
- **Miiの色変更**［日本］：髪・メガネ・肌・目・口（通常＋濃い方）・好きな色*（カラーピッカー）
- **BETA**［日本］：自分以外の時間停止*（ホットキー Y で停止/解除、動作不安定）
- **Plugin設定**（全版）：背景の色（背景・ノイズ・縁、カラーピッカー、`MK7_Cheat_theme.bin` に保存）

`*` = チェックを外すと OFF 値を書き戻すエントリ（`ApplyToggle` / `ApplyToggleOrdered`）。

## 6. 空き領域と排他（新しいコードを足す前に必ず確認）

| アドレス | 使うエントリ |
| --- | --- |
| 0x5EBD24〜0x5EBD28 | アイテムレイン / アイテムドロップの中継（フック 0x2D9668） |
| 0x5EBD30〜0x5EBD44 | GPタイマー |
| 0x5EBD50〜0x5EBD58 | ふうせんレース / コインレース |
| 0x5EBD9C〜0x5EBDD0 | メニュー高速化 |
| 0x5EBE18〜0x5EBE24 | バックミラー（← を押している間） |
| 0x5EBD30〜0x5EBF2C | BETA 時間停止（上の多くと重なる） |
| 0x6BE980 / 0x6BE984 | CTGP-7式ライブリプレイの作業用メモリ |

- 排他は `DisableIfOn()` で実装：時間停止 ⇔ GPタイマー・バックミラー・ふうせん・コイン・メニュー高速化、ふうせん ⇔ コイン。
- 空き領域を使うコードは、ON では「空き領域 → 分岐」、OFF では「分岐 → 空き領域」の順に書く（`ApplyToggleOrdered`）。途中で中身のない場所に飛ばないようにするため。
- AR 版のアイテムレイン（0x5EBE10〜、0x5EBD60〜、0x5EBF80〜）は 0x5EBD60〜 がメニュー高速化などと重なるので、プラグインには入れていない。

## 7. 書き方の決まり

- ノート（説明文）は日本語。ホットキーはシステムフォントのボタン記号（`BTN_A`、`BTN_UP` など、U+E000〜）で書く。
- コード名の色は txt の `~#RRGGBB~` と同じ色を `Col(0xRRGGBB, "名前")` で付ける。
- ゲームのメモリは `Process::Read32` / `Write32` 経由で読み書きする（ソフト終了時に壊れたポインタを読んでも落ちないように）。ItemRain.cpp はゲームのスレッドから直接読む作りなので例外。
- 3DS のシステムフォントにない文字（〜 U+301C、▶ ◀ など）は文字化けする。キーボードやノートでは ～（U+FF5E）、→ ← を使う。
- `main()` の最後で `delete menu;` をしない。ソフト終了時に `MenuFolder` の片付けで壊れたポインタを読んでクラッシュしたため外した（addr2line で `MenuFolderImpl::~MenuFolderImpl` → `MenuFolder::~MenuFolder` を確認）。
- ソフト終了時は `OnProcessExit` で `ItemRainShutdown()` を呼び、0x2D9668 のフックを外す。

## 8. 日本語キーボード（ctrpf-patch）

- CTRPF develop の KeyboardImpl を改造。英字ページ右下の「あ」でかなページ（ひらがな・カタカナ各2ページ）、「色」で文字色の挿入（プリセット / カラーピッカー / 色を戻す）。
- かなを打つたびに、入力欄とキーの間の予測変換バーに候補を出す（SKK 辞書を二分探索、一致 → ひらがな → カタカナ → 前方一致）。候補はタッチで確定、変換中は ←→/↑↓ で選択、A 確定、B でよみに戻る。
- 予測変換バーの場所を空けるため、QWERTY の全キーを 20 ドット下げている（`QWERTY_OFFSET_Y`）。ページごとのキー範囲は `s_pageStart[]` で管理し、`_GetEffectiveKeyRange()` で返す。
- 色コードは CTRPF 内部の 0x1B r g b（0 は 1 にする）と 0x18（色を戻す）。アクションリプレイのコード名を変更したとき、cheats の txt にどう保存されるかは未確認。

## 9. 未確認・推測の点

- `FwkSettings::Get()` と、ノイズの色が `BackgroundSecondaryColor` であること（Plugin設定）
- `Keyboard::ChangeSelectedEntry`（アイテムスイッチの画面）。ビルドエラーならその1行を消す
- Mii 髪色の色形式は FFBBGGRR（コード集で確認済み）
- 影・光の色の中間値（00/FF 以外）の見え方

## 10. まだ移植していない txt のコード

- AR 版アイテムレイン（プラグイン版があるため）
- コインドロップ試作、時間停止試作（試作のため）

ユーザーから追加の指示があるまで入れない。
