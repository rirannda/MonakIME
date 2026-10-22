# Rust 0.2.0の検証とC++版との比較

2026-10-22、AMD Ryzen 7 7735HS / Linux x86_64、Rust 1.95.0、GCC 16.2.1、fcitx5 5.1.23で測定しました。両版とも同じ30万件の補助辞書、Mozcヘルパー、xsmall Q5_K_M、CPU 2スレッドを使用します。RustのReleaseビルドは最適化・thin LTO・codegen-units=1です。以下の数値はNewIMEからMonakIMEへ改名する前の測定記録であり、その後の変更を含む最新版の測定結果ではありません。

比較対象の旧CLIは `build/baseline-cpp/newime-cli`、旧ソースは `reference/cpp/` に保存しています。設定・学習形式とモデルは共通です。学習を無効にし、Mozcのプロファイルは測定ごとに一時ディレクトリーへ分けています。ビルドと測定は重ねていません。

## Mozc込みのライブ変換

`scripts/measure-live.py` による、CLI＋実Mozc子プロセスの合算です。Zenzはロードしません。5つの短文を繰り返し、後続100要求を測定しました。25msの入力待ちとfcitx5の描画時間は含みません。OSのファイルキャッシュは消していません。

| 項目 | C++ 0.1 | Rust 0.2 |
|---|---:|---:|
| 初回要求・プロセス初期化とヘルパー起動を含む | 29.13 ms | 91.10 ms |
| 後続100要求の中央値 | 2.16 ms | 1.70 ms |
| 後続100要求の95%点 | 5.75 ms | 4.77 ms |
| 100要求のCPU時間・親＋子 | 0.29 s | 0.23 s |
| CLIのRSS | 14,432 KiB | 11,148 KiB |
| MozcのRSS | 21,588 KiB | 21,396 KiB |
| 合計RSS | 36,020 KiB（35.2 MiB） | 32,544 KiB（31.8 MiB） |
| 1秒待機時のCPU時間増分 | 0 tick | 0 tick |

この測定では継続変換のメモリ・CPU時間は減り、初回応答は遅くなりました。Rust版は辞書のUTF-8検証も行います。また、CLIでMozcが成功したときの不要な補助変換を省き、候補の重複排除で一時文字列を複製する処理も減らしました。結果にはこうした実装変更が含まれ、言語だけの比較ではありません。

RSSには共有ページの重複計上が含まれる可能性があります。待機計測はclock tick（1/100秒）単位です。長期のCPU使用量やfcitx5全体が厳密にゼロになることを示す値ではありません。

生データ：[C++](../reports/live-mozc-cpp.json)、[Rust](../reports/live-mozc-rust.json)。

## 補助辞書とZenzの個別測定

`scripts/measure.py` のCLI測定です。ライブ変換のMozcは使いません。補助辞書は8つの入力を2,000回処理しました。

| 項目 | C++ 0.1 | Rust 0.2 |
|---|---:|---:|
| 補助辞書変換・中央値 | 57.10 µs | 76.18 µs |
| 補助辞書変換・95%点 | 198.92 µs | 217.27 µs |
| 2,000要求のCPU時間 | 0.141 s | 0.195 s |
| 補助辞書のみ・変換後RSS | 10,136 KiB | 10,204 KiB |
| xsmall込み・変換後RSS | 46,164 KiB | 46,436 KiB |
| xsmall初回モデル呼び出し | 56.94 ms | 41.31 ms |
| 後続9例のモデル呼び出し中央値 | 25.94 ms | 18.65 ms |

補助辞書だけの変換時間はこの測定では増えています。通常のライブ変換はMozcを使います。モデル時間は同じllama.cpp・モデルでの単一試行であり、CPU周波数や他の処理の影響もあります。モデルの時間差をRustへの移植効果とは断定しません。

生データ：[C++](../reports/cpp-before-rust.json)、[Rust](../reports/performance-rust.json)。

## 機能確認

Rust版でも、手書き10例の期待する先頭候補は補助辞書で7/10、xsmall併用で10/10でした。大文字の英字混在2例は両方一致しました。これは機能確認であり、汎用コーパスでの変換精度や他のIMEとの優劣を示す値ではありません。

- [10例の結果](../reports/conversion-rust.json)
- [英字混在2例の結果](../reports/mixed-case-rust.json)

現在のテストでは、Rustの回帰テストに加えて、実Mozc・実CPUモデル、辞書生成・読込・学習、fcitx5の基本操作、ライブ変換、非同期モデル、起動スクリプト、改名互換、Shift単独切替の設定を確認します。テスト数は機能追加とビルド構成によって変わります。ライブ変換のテストは補助辞書を無効にし、実Mozcから応答が来ること、入力中にGGUFがロードされずSpace後にロードされることも確認します。古い入力欄の破棄が新しい欄の変換を取り消さないことも対象です。

ASan/UBSanビルドの対象はC++アダプターとC++統合テストです。Rustの通常コード全体をサニタイザ付きコンパイラーでビルドしたものではありません。環境のptrace制約によりLeakSanitizerは使えず、`ASAN_OPTIONS=detect_leaks=0` で検査します。Rust側は通常テストとClippyを確認します。

電力・電力量は測定していません。CPU時間を電力の実測値として扱いません。Wayland/X11の各アプリでの手動確認は、自動統合テストとは別に残っています。

## 現行版の再測定

ルートの[README](../README.md#ビルドとテスト)に従って依存物の取得とビルドを済ませてから、ソースのルートディレクトリーで実行します。保存済みの比較記録を上書きしないよう、新しい結果は `build/` に出力します。測定はビルドや他の推論と同時に実行しないでください。

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
./scripts/measure-live.py --binary build/monakime-cli --output build/live-mozc-local.json
./scripts/measure.py --binary build/monakime-cli --output build/performance-local.json \
  --model models/zenz-v3.2-xsmall-Q5_K_M.gguf
```

旧比較用バイナリ `build/baseline-cpp/newime-cli` は配布ソースに含みません。[C++版のスナップショット](../reference/cpp/README.md)はコード参照用であり、このバイナリを生成する単独ビルド一式ではありません。旧版の数値は保存済みのJSONを参照してください。
