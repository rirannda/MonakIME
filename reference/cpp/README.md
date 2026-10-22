# C++版の参照用スナップショット

Rustへ移す直前のNewIME 0.1.0のソースです。現在の実装はルートの `src/*.rs` と `native/`、ビルド定義はルートのCargo/CMakeにあります。

`src/romaji.cpp`、`converter.cpp`、`dictionary.cpp` とRust版を比較して、文字列の所有権、参照、候補の型、入力状態、ワーカーの違いを確認できます。

旧CLIの測定時の配置先は `build/baseline-cpp/newime-cli` でした。この実行ファイルはGitリポジトリに含みません。測定結果はルートの [reports/cpp-before-rust.json](../../reports/cpp-before-rust.json) と [性能の測定](../../docs/performance.md) に保存しています。

このディレクトリーはコード参照用で、旧CMakeが参照する `data/`・`scripts/`・外部依存物などの一式は含みません。単独でのビルド・インストールを案内するものではありません。現行版はルートの [README](../../README.md#ビルドとテスト) の手順でビルドしてください。
