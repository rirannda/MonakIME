# MonakIMEで使用するMozcヘルパー

このディレクトリーには、[Meltype v1.3.0のMozcヘルパー](https://github.com/yksr-melt/Meltype/tree/v1.3.0/native/mozc)のソース・ビルド定義・ライセンス・取得元情報を保存しています。Meltypeの.NETアプリは起動せず、`meltype_mozc_helper` を独立したプロセスとして使います。MonakIMEからの通信と設定は[ライブ変換の説明](../../docs/live-conversion.md)を参照してください。

| ファイル | 内容 |
| --- | --- |
| `meltype_mozc_helper.cc` | ヘルパーのソース。標準入出力で変換（`C`）・学習（`L`）・保存（`S`）を受け付けます。MonakIMEは変換命令を使用します |
| `BUILD.fragment` | Mozcの `src/converter/BUILD.bazel` に追加するBazel定義 |
| `MOZC_COMMIT` | ビルド対象のMozcリビジョン |
| `build-mozc-helper.sh` | 上流から取得したLinux向けビルドスクリプト |
| `MOZC-LICENSE.txt` / `MOZC-CREDITS.html` | Mozcと依存物のライセンス・クレジット |
| `provenance.json` | 公式リリースの取得元、アーカイブと抽出ファイルのSHA-256 |

ヘルパーのソースとビルドスクリプトは Copyright (C) 2026 Yukishiro、GPL-3.0-or-laterです。Mozc本体はBSD-3-Clauseと辞書・依存物それぞれのライセンスに従います。詳細は[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)を参照してください。このREADMEはMonakIMEでの利用手順です。上流のWindows向け手順は[Meltype v1.3.0のREADME](https://github.com/yksr-melt/Meltype/blob/v1.3.0/native/mozc/README.md)にあります。

## x86_64 Linux向けバイナリの取得

実行ファイルはGitリポジトリに含みません。MonakIMEのソースのルートディレクトリーから実行します。

```sh
./scripts/setup-mozc.sh
# 同じ公式アーカイブをローカルに取得済みの場合
./scripts/setup-mozc.sh /path/to/Meltype-1.3.0-linux.zip
```

Meltype v1.3.0の公式Linuxアーカイブと抽出ファイルを固定SHA-256で検証し、`third_party/mozc/meltype_mozc_helper` に配置します。使用するMozcは `a069a88d4cb5c011de0f9aebb6c149a1c808d904` のOSS辞書版です。取得にはBash・curl・Python 3.10以降が必要です。実行には互換性のあるLinux環境が必要で、x86_64以外ではこの取得スクリプトは使用できません。

## ソースからのビルド

Git、Bazelisk（既定のコマンド名は `bazel`）、C++コンパイラー、MozcのLinuxビルド用依存物が必要です。[Mozc公式のLinuxビルド手順](https://github.com/google/mozc/blob/master/docs/build_mozc_for_linux.md)も参照してください。リンク先は最新版の説明なので、実際の依存物・設定は `MOZC_COMMIT` のリビジョンに合わせます。初回はネットワークと、MonakIME本体より大きい依存物の取得・ビルドが必要です。

ソースのルートディレクトリーから、Mozc取得用に未使用のディレクトリーを指定します。スクリプトは既存チェックアウトを固定commitに戻す処理を行わず、ビルド定義へヘルパーを追加するため、普段使用しているMozcの作業ツリーは指定しないでください。

```sh
./third_party/mozc/build-mozc-helper.sh "$PWD/build-mozc-source"
MONAKIME_MOZC_HELPER="$PWD/third_party/mozc/bin/linux/meltype_mozc_helper" \
  ./scripts/run-fcitx.sh
```

出力先は `third_party/mozc/bin/linux/` です。取得スクリプトの配置先と異なるため、実行ファイルを `MONAKIME_MOZC_HELPER` または設定ファイルの `mozc_helper` で指定します。通常のCMakeインストールが収録するのは `third_party/mozc/meltype_mozc_helper` なので、自前ビルドの結果は自動収録されません。x86_64以外のアーキテクチャでのビルド・動作は未検証です。

プロファイルは通常 `~/.local/share/monakime/mozc/` を使用し、既存のMozcやMeltypeのプロファイルとは分けます。XDG_DATA_HOMEと旧名NewIMEのプロファイルへの互換動作は、ルートの[README](../../README.md#設定と学習)を参照してください。
