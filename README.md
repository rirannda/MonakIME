# MonakIME 0.2.0 — Rust版

入力中は**Mozcでライブ変換**し、**Space・変換キーを押したときだけCPUのZenzモデルで候補を追加**するfcitx5用の日本語IMEです。ローマ字・英日分節・補助辞書・学習・入力状態・非同期処理・CLIをRustで実装しています。fcitx5とllama.cppの接続には小さなC++アダプターを使います。

**開発中の実験的な実装です。** 主な対象はLinux x86_64とfcitx5 5.1以降です。Windows・macOS向けの入力メソッドは含みません。実アプリでのWayland/X11の手動確認は未完了です。現在の制約は[検証と現在の制約](#検証と現在の制約)を参照してください。

英日分節の手法は[Meltype](https://github.com/yksr-melt/Meltype)を参考にしており、Mozc接続にはMeltypeのネイティブヘルパーを使用しています。MonakIMEは別のプロジェクトです。依存元・利用箇所は[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)にまとめています。

## ビルド後に試す

以下のコマンドは、取得したソースのルートディレクトリーで実行します。初回は先に[ビルドとテスト](#ビルドとテスト)の手順を実行してください。ビルド済みファイル・モデル・生成済み辞書はGitリポジトリに含みません。

```sh
./build/monakime-cli --live --json 'hashidegohanwotaberu'
./build/monakime-cli --mozc --json 'hashidegohanwotaberu'
./build/monakime-cli --diagnose
./scripts/run-fcitx.sh
```

最初のCLIはMozcだけで「橋でご飯を食べる」、次はZenzを併用して「箸でご飯を食べる」を返す機能確認例です。結果は設定・辞書・学習状態によって変わることがあります。Zenzはv3.2-xsmall Q5_K_MをCPUで動かします。モデルや依存物の取得にはネットワークが必要ですが、通常の変換処理では外部通信を行いません。

起動スクリプトは既存のfcitx5を置き換えて起動し、ビルドしたアドオンと標準アドオンの探索パスを設定します。**fcitx5共通設定のShift単独による英数・日本語切替も無効にします。他の入力メソッドにも影響します。** 設定ファイルが存在する場合、初回変更前のバックアップを `~/.config/fcitx5/config.monakime-before-shift` に保存します（XDG_CONFIG_HOMEに対応）。Shift+SpaceやShift+矢印は引き続き使えます。

DBus・Wayland/X11のデスクトップセッションが必要です。起動後、fcitx5設定を開き直して **MonakIME（日本語）** を追加・選択します。「現在の言語のみ表示」が有効なら解除します。通常のfcitx5へ戻す方法は[試用の終了・アンインストール](#試用の終了アンインストール)を参照してください。

## 操作

| キー | 動作 |
|---|---|
| ローマ字入力 | Mozcでリアルタイム変換。続けて入力すると未確定のまま再変換 |
| Space / 変換 | Mozc候補を表示し、CPUのZenz候補を非同期で追加 |
| Space / Shift+Space、↓↑、Tab / Shift+Tab | 候補の選択 |
| 1〜9 / 候補クリック | 表示候補を選択 |
| ←→ | 文節ごとの候補選択へ移動 |
| Shift+←→ | 文節の読みを1文字縮める・伸ばす |
| Enter | 現在の表記を確定。Zenzは要求しない |
| Esc | 変換をかなに戻す。もう一度で入力を消去 |
| Backspace | 変換の取消、または読みを仮名1文字ずつ削除（未完成子音・英字は1文字） |
| F6 / F7 / F10 | ひらがな / カタカナ / 入力した英字 |
| 無変換 | 入力中は英字表示。空なら英数モード |
| 変換 / ひらがなカタカナ | 日本語モードへ戻る |

`Google`・`GitHub`で始まる混在文、`CPU`・`12.5%`・`256MB`の表記を保持します。英単語がローマ字の音節を分断しないようにし、URLや識別子も保持します。フォーカス喪失・リセットでは入力と文脈を破棄します。パスワード・機密欄はアプリケーションからのフラグに従って処理対象から外します。

## 構成と省資源化

Mozcは独立したネイティブヘルパーとして常駐し、Meltypeの.NETアプリは起動しません。入力から既定25ms待って最新の要求だけを送り、古い要求を積み上げません。MozcとZenzのワーカーは標準ライブラリーの条件変数で眠り、待機中の定期ポーリングを行いません。

モデルは初回の明示変換までロードしません。ZenzはCPU既定2スレッド、context 512、生成最大128トークン、推論期限500msです。GPUは使いません。Mozcキャッシュは最大32件・概算512KiB、Zenzキャッシュは最大32件です。補助辞書は約11MB・30万件の読み取り専用mmap、学習は最大4,096件です。保存は2秒の待ちでまとめます。

Rustへの変更だけで軽量化を保証することはできません。実測値とC++版との比較は [性能の測定](docs/performance.md)、処理の詳細は [ライブ変換](docs/live-conversion.md) と [Zenz](docs/zenzai.md) にあります。

## ビルドとテスト

Rust/Cargo、C/C++20コンパイラー、CMake 3.22以降、makeまたはNinja、pkg-config、fcitx5 5.1以降の本体・開発ファイルが必要です。セットアップにはBash、Git、curl、Python 3.10以降、`sha256sum` を使います。Cargo.tomlはedition 2024・最低Rust 1.85を指定していますが、検証に使用したのはRust 1.95.0です。1.85でのビルドは未確認です。

fcitx5統合テストにはtestfrontend/testui/testimのヘッダーとライブラリーも必要です。ディストリビューションの開発パッケージに含まれていない場合は、これらを含むfcitx5の開発環境を用意するか、CMakeに `-DBUILD_TESTING=OFF` を追加して統合テストを省略します。その場合、以下の `ctest` は実行しません。

```sh
./scripts/bootstrap.sh                 # 固定llama.cppとxsmallモデル
./scripts/setup-mozc.sh                # 公式Mozcヘルパー（x86_64 Linux）
./scripts/setup-dictionary.sh          # 補助辞書を生成
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

`bootstrap.sh` は固定したllama.cppとモデルを取得し、`cargo fetch --locked` を実行します。`setup-mozc.sh` はMeltype v1.3.0のLinuxアーカイブからヘルパーを配置します。これらはfcitx5やRust自体をインストールするスクリプトではありません。

Cargo依存は `Cargo.lock` で固定しています。取得済みのCargoキャッシュだけを使う場合はCMakeに `-DMONAKIME_CARGO_OFFLINE=ON` を追加します。`setup-dictionary.sh /path/to/mozc` はローカルチェックアウトの固定commitから公式辞書を取り出し、作業中のUT追加を取り込みません。

```sh
# Rustの通常テストとCLI（既定ではZenzをリンクしない）
cargo test --locked
cargo run --release --locked -- --live --json 'nihongo'
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings

# Zenzを外したfcitx5ビルド
cmake -S . -B build-mozc -DMONAKIME_WITH_LLAMA=OFF
```

Zenzを外す構成では `bootstrap.sh` によるllama.cpp・モデルの取得は不要です。CLIだけならCargoを直接使うか、CMakeの `-DMONAKIME_WITH_FCITX5=OFF` を使えます。Cargoの既定ビルドにニューラル推論は含まれません。Mozcを使うCLI例には `setup-mozc.sh` が必要です。外部バックエンドなしで試す場合は `cargo run --release --locked -- --no-model --json 'nihongonyuuryoku'` を使います。

標準のCPU推論ビルドはAVX2などを使うx86_64向けです。異なる環境では対応するMozcヘルパーを用意し、llama.cppのCPUオプションも対象CPUに合わせてビルドする必要があります。[Mozcヘルパーの説明](third_party/mozc/README.md)を参照してください。

## 設定と学習

`data/config.example` を `~/.config/monakime/config` に配置できます。変更の反映にはfcitx5の再起動が必要です。ファイル内のパスには絶対パスを指定します（`~` や環境変数の展開は行いません）。

旧名NewIMEの設定にも対応します。旧 `~/.config/newime/config` を読み込んだ後、新しい設定ファイルの同名項目を優先します。ユーザー辞書・学習ファイル・Mozcプロファイルは、新しい場所に存在しない場合だけ旧ディレクトリー内の既存データを使います。自動移動は行いません。

```ini
threads=2
inference_budget_ms=500
max_input_chars=128
learning=true
zenz=true
live_conversion=true
live_delay_ms=25
# model=/absolute/path/to/zenz-v3.2-small-Q5_K_M.gguf
# mozc_helper=/absolute/path/to/meltype_mozc_helper
```

環境変数は `MONAKIME_MODEL`、`MONAKIME_THREADS`、`MONAKIME_BUDGET_MS`、`MONAKIME_ZENZ`、`MONAKIME_LEARNING`、`MONAKIME_DATA_DIR`、`MONAKIME_DICTIONARY`、`MONAKIME_SYSTEM_DICTIONARY`、`MONAKIME_ENGLISH_DICTIONARY`、`MONAKIME_USER_DICTIONARY`、`MONAKIME_LEARNING_PATH`、`MONAKIME_MAX_INPUT_CHARS`、`MONAKIME_LIVE_CONVERSION`、`MONAKIME_LIVE_DELAY_MS`、`MONAKIME_MOZC_HELPER`、`MONAKIME_MOZC_PROFILE` です。旧 `NEWIME_*` も使用でき、同じ項目では `MONAKIME_*` が優先されます。環境変数が設定ファイルより優先されます。`./build/monakime-cli --diagnose` で実装・パス・機能を確認できます。

ユーザー辞書は `~/.config/monakime/user.tsv` に `読み<TAB>表記<TAB>コスト` の形式で作成します。学習した読み・表記は `~/.local/share/monakime/learning.tsv` に権限0600で保存します。ユーザーが作成する辞書の権限は自動変更しません。XDG_CONFIG_HOME / XDG_DATA_HOMEに対応します。学習を読み書きしない場合は `learning=false` または `MONAKIME_LEARNING=0` を指定します。既存の学習ファイルは削除しません。

```sh
./scripts/setup-model.sh small
MONAKIME_MODEL="$PWD/models/zenz-v3.2-small-Q5_K_M.gguf" ./scripts/run-fcitx.sh
```

## インストールとアーカイブ

```sh
sudo cmake --install build --strip
./scripts/package.sh
```

`sudo cmake --install` は構成時のインストール先（上の例では `/usr`）へ書き込みます。起動スクリプトによる試用だけなら、この操作は不要です。

`package.sh` はビルドのインストール内容を一時ディレクトリーに配置し、`build/monakime-0.2.0-linux-x86_64.tar.gz` とSHA-256ファイルを作成します。パッケージ作成だけならシステムは書き換えません。アドオン、Rust CLI、説明・ライセンス、MonakIMEのソースを収録し、Mozcヘルパー・生成済み辞書・xsmallモデルは取得済みの場合に収録します。CLIのみ・Zenzなしなど、ビルド構成によって内容は変わります。このアーカイブはLinux全般で動作する自己完結型のバイナリではなく、互換性のあるfcitx5やシステムライブラリーが必要です。

ソースは配置先の `share/doc/monakime/` 以下にも含まれます。llama.cppとMozc本体のソース一式は同梱しておらず、固定リビジョンと取得・ビルド手順を付けています。

インストール後にfcitx5を再起動し、MonakIMEを選択します。旧プロファイルの `newime` は **MonakIME（旧ID互換）** として動作します。通常は **MonakIME（日本語）** を選びます。旧開発コマンド `build/newime-cli` も新CLIへのリンクとして残します。

## 試用の終了・アンインストール

fcitx5設定で普段の入力メソッドへ切り替え、MonakIMEを入力メソッド一覧から外します。起動スクリプトで試用した場合は、fcitx5を終了してデスクトップの通常の方法で起動し直すか、ログインし直すと開発用アドオンの探索パスが解除されます。インストールしていなければ、システムから削除するMonakIMEのファイルはありません。

Shift単独の切替を戻す場合は、fcitx5設定の「グローバルオプション」で切替キーを設定し直します。設定ファイル全体を起動前へ戻す場合は、fcitx5を終了してから `config.monakime-before-shift` を `config` に戻し、通常のfcitx5を起動します。バックアップは初回分のみで、それ以降の設定変更も戻るため、内容を比較してから復元してください。`run-fcitx.sh` を再実行すると再びShift単独の切替を無効にします。

`cmake --install` でインストールした場合、自動アンインストール用スクリプトはありません。システムへインストールした直後の `build/install_manifest.txt` を保存し、MonakIMEの入力メソッドを一覧から外してfcitx5を終了した後、その一覧を確認して対象ファイルだけを削除してください。`package.sh` もこのマニフェストを更新します。マニフェストには通常のインストール先が記録されますが、パッケージ作成だけでシステムへインストールしたことにはなりません。設定・辞書・学習データは別管理で、この一覧には含まれません。

## 検証と現在の制約

Rustの回帰テスト、実Mozc・実GGUF、fcitx5の入力・非同期反映・取消・選択の保持・フォーカスと機密欄、起動スクリプトを確認します。旧版と同じ手書き10例、英字混在2例の結果も `reports/` に保存しています。これは汎用コーパスでの変換精度評価ではありません。

```sh
./scripts/evaluate.py --output build/conversion-local.json
./scripts/measure.py --output build/performance-local.json \
  --model models/zenz-v3.2-xsmall-Q5_K_M.gguf
./scripts/measure-live.py --output build/live-mozc-local.json
```

未確定ローマ字列の途中へのカーソル移動、Mozcの境界をまたぐユーザー辞書の反映には制約があります。文節の手動リサイズには補助変換器を使います。ニューラル候補の読みは辞書で照合しますが、未知の漢字語の読みは保証できません。実アプリでのWayland/X11の手動確認と、実消費電力の測定は未実施です。

不具合を報告する際は、OS・デスクトップ・Wayland/X11の別、対象アプリ、再現する入力とキー操作、期待した結果と実際の結果を添えてください。`--diagnose` の出力にはユーザーディレクトリーのパスが含まれるため、公開前に確認してください。実際の私的な入力文や学習ファイルを公開する必要はありません。

開発者向けの入口は[Rustコードを読む順序と練習](docs/rust-guide.md)です。移植前のコードは `reference/cpp/` に保存しています。

## ライセンス

Copyright (C) 2026 rirannda

MonakIMEの自作部分は[GPL-3.0-or-later](LICENSE)（GPL第3版、またはそれ以降の版）で公開します。ライセンスの条件に従って改変・再配布できます。本ソフトウェアは無保証で提供します。適用範囲と許諾・免責の表示は[COPYRIGHT](COPYRIGHT)を参照してください。

第三者のコード・辞書・モデルには、それぞれの著作権表示とライセンスが適用されます。依存元と利用箇所は[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)にまとめています。
