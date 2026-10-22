# Rustコードを読む順序

MonakIME 0.2では、変換のロジックと入力状態をRustで実装しています。fcitx5とllama.cppの接続は `native/` のC++アダプターが受け持ちます。Rust側にはTokio、GC、Webサーバーを追加していません。直接の依存は `libc`・`memmap2`・CLIのJSON出力用 `serde_json` です。

最初はモデルもfcitx5も使わず、次を実行すると読み進められます。

```sh
cargo test
cargo test --test core english_japanese_regressions
cargo test --test core input_state_and_privacy
cargo run --release -- --no-model --json 'nihongonyuuryoku'
```

Cargoを直接使う場合の既定ビルドは、補助辞書とMozcを利用できます。CPUのZenzとfcitx5は、ルートのCMakeがネイティブライブラリーを準備してからCargoを呼び出してビルドします。

## 1. 小さい関数から読む

| ファイル | 注目するところ |
|---|---|
| [text.rs](../src/text.rs) | `chars()`、文字数とUTF-8のバイト位置、借用した `&str` の切り出し |
| [romaji.rs](../src/romaji.rs) | 変換表、`match`、未完成のローマ字を保持する分岐 |
| [compose.rs](../src/compose.rs) | `Vec` と構造体で、英語・日本語の2通りの経路を比較する動的計画法 |
| [converter.rs](../src/converter.rs) | 候補と文節の型、候補の合成、境界変更、ニューラル出力の読み照合 |

`romaji::convert` は入力を `&str` で借り、末尾を確定するかを `bool` で受け取り、結果を `String` で返します。結果の文字列は関数内で作って呼び出し元へ渡します。ここを起点にすると所有権の流れを追いやすくなります。

`compose.rs` は、ローマ字をどこで英単語として保持するかを決めます。`Googledeshirabete` の `Google` を保持し、残りを「でしらべて」にする部分です。`converter.rs` は、その読みから候補を作る補助変換器です。通常のライブ変換にはMozcを使います。

## 2. 入力状態を追う

[session.rs](../src/session.rs) の `Key` → `Session::key` → `Session::view` を読んでください。`Core` は設定・辞書・入力欄ごとの状態・ワーカーをまとめます。

入力欄の状態は `HashMap<u64, Session>` に入り、`Session` に次を保存しています。

- `raw`：編集する入力文字列。ライブ変換中もローマ字を保持します。Backspaceで音節の一部を消すときは、必要な部分を仮名に戻して残します。
- `live`：入力中に届いたMozcの結果。
- `conversion`：Space後に選択する候補と文節。
- `generation`：この入力が現在のものかを識別する番号。
- `converting`・`segmented`：候補選択中か、文節単位で操作中か。
- `source_spans`：文節編集を始めたときだけ作る、元入力と読みのバイト範囲の対応表。

`Context<'a>` は設定・辞書・ワーカーへの参照を一時的にまとめます。辞書への `&mut Dictionary` と、入力状態への `&mut Session` は別のフィールドから借りています。入力状態のために `RefCell` や全体を囲む `Mutex` を使う必要がありません。

`View` は描画に必要な情報と、確定文字列・キーを処理したかの結果です。fcitx5への依存がないので、[tests/core.rs](../tests/core.rs) から普通のRust関数として操作できます。

## 3. 外部変換と非同期処理を読む

| ファイル | 注目するところ |
|---|---|
| [mozc.rs](../src/mozc.rs) | `Command`、`Child`、`Result`、期限付きパイプ通信、`Drop`での終了 |
| [zenz.rs](../src/zenz.rs) | `Option`、入力・出力検証、機能フラグ `#[cfg(feature = "llama")]` |
| [worker.rs](../src/worker.rs) | `Arc`、`Mutex<Option<Job>>`、`Condvar`、世代番号による取消 |

`Option<Conversion>` は、採用できる結果があるときに `Some`、取消や失敗などで結果がないときに `None` になります。補助辞書は `unwrap_or_else` で必要になったときだけ実行します。

`worker.rs` の `Mutex<Option<Job>>` は最新の保留要求を1件だけ持ちます。連続入力で古い要求を積み上げません。ワーカーは条件変数で待ち、入力から25msの待ちが終わったらMozcへ送ります。この待ちはキー入力で更新され、常時CPUを回すループにはなりません。

Zenzの要求を作るのは `Session::begin` です。通常の文字入力ではMozcの要求だけを作り、Enterは現在の表記を確定します。

## 4. 保存と境界部分を読む

[dictionary.rs](../src/dictionary.rs) は、読みで二分探索するメモリマップ辞書と、最大4,096件の学習を扱います。学習は権限0600の一時ファイルへ保存し、書き込みが成功してから置き換えます。`Config` の読み込みは [config.rs](../src/config.rs)、CLIは [main.rs](../src/main.rs) にあります。

最後に [ffi.rs](../src/ffi.rs) を読んでください。C++へ渡す構造体に `#[repr(C)]` を付け、文字列を `CString` に変換しています。C++は受け取った表示情報を使い終わると、Rustの `monakime_view_free` に返します。

`unsafe` は、C ABI、OSのパイプ・シグナル操作、読み取り専用のメモリマップに集めています。それぞれの近くに、ポインターの寿命などの前提をコメントしています。通常のローマ字処理・分節・入力状態のコードには `unsafe` はありません。

## 小さい練習

1. `romaji.rs` に `gwi → ぐぃ` を追加し、同じファイルのテストを増やします。
2. `tests/core.rs` に、英単語と日本語を混ぜた短文を1つ追加し、`compose` の経路選択を追います。
3. `input_state_and_privacy` にF7相当の `Key::Katakana` とEscを追加し、`Session::key` が表示と元入力をどう扱うか確認します。

```sh
cargo fmt --all
cargo clippy --all-targets -- -D warnings
cargo test
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

通常の `cargo test` はモデル不要のテストを実行します。実Mozc・GGUFが必要な2件は `#[ignore]` で分け、CMakeのCTestから実行します。`rust_real_backends` とfcitx5統合テストでは、実際のバックエンドも確認します。

移植前の実装は [reference/cpp](../reference/cpp/) に保存しています。Rustのモジュールと同名のC++ファイルを比較できます。現在のビルドにはルートの `Cargo.toml` と `CMakeLists.txt` を使います。
