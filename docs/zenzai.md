# zenz CPUバックエンド

この実装は、Zenzaiで使われるかな漢字変換モデル **zenz-v3.2** を Rustからネイティブアダプター経由でllama.cppへ渡します。AzooKeyのSwiftエンジン全体の移植ではありません。入力中はMozcでライブ変換し、ユーザーがSpace・変換キーで変換を要求した場合だけ非同期でニューラル候補を追加します。Enterではニューラル推論を要求しません。

## 配布元と固定リビジョン

2026-10-22に公式配布元で確認しました。

| モデル | 固定revision | ファイルサイズ | SHA256 |
|---|---|---:|---|
| [Miwa-Keita/zenz-v3.2-xsmall-gguf](https://huggingface.co/Miwa-Keita/zenz-v3.2-xsmall-gguf) | `4f5423f0fad41a73b1242eb96fe5c12ae4fdca83` | 20,970,304 bytes | `00c64b3d318045a708d0cad5434faccab10f5481a49e6362864551fd0995fa58` |
| [Miwa-Keita/zenz-v3.2-small-gguf](https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf) | `c67e03e07d215c869f591b274c1631170d3e11fe` | 73,871,936 bytes | `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673` |

いずれも `ggml-model-Q5_K_M.gguf`、モデルカードのライセンス表示は **Apache-2.0** です。作者はMiwa-Keita氏です。取得したモデルの改変は行いません。ライセンス本文は `data/licenses/Apache-2.0.txt` に保存しています。モデルはGitリポジトリに含まず別途取得しますが、既定の場所に取得したxsmallモデルはCMakeのインストールとパッケージに収録されます。

llama.cppは [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp/tree/d88040c704c8855c497d46c20675a3d492f70f94)、commit `d88040c704c8855c497d46c20675a3d492f70f94`、MITライセンスです。ソースツリーのLICENSEを保持します。このcommitのC APIを対象にしています。

このモデルの `tokenizer.ggml.pre=gpt2-small-japanese-char` は、ここで固定しているllama.cppリビジョンでは未対応です。MonakIMEはモデルを改変せず、[ネイティブアダプター](../native/llama_bridge.cpp)でロード時のKV overrideに `gpt-2` を指定します。実GGUFを用いた変換テストを行っています。

```sh
./scripts/setup-model.sh                  # xsmall、約21MB
./scripts/setup-model.sh small            # small、約74MB
./scripts/setup-model.sh xsmall /path/to/models
```

取得先はHTTPSの公式Hugging Faceのみです。固定revision・サイズ・SHA256を確認してから原子的に配置します。既存ファイルが一致すれば再ダウンロードしません。

## プロンプト

[AzooKey公式ドキュメント](https://github.com/azooKey/AzooKeyKanaKanjiConverter/blob/main/Docs/zenzai.md) のv3プロトコルに従います。

```text
\uEE02<直前の確定文脈>\uEE00<カタカナの読み>\uEE01
```

文脈が空なら `\uEE02` の部分を省略します。モデル向けのプロフィール情報やクラウド通信は使いません。バックエンドが受け付ける左文脈は直近96文字、読みは最大128文字です。fcitx5側が渡す文脈は、その入力欄で確定した直近64文字です。周辺の文書全体を取得する処理は含みません。入力に含まれる私用領域の制御マーカーは受け付けず、文脈からも除去します。[公式実装のZenzContext](https://github.com/azooKey/AzooKeyKanaKanjiConverter/blob/main/Sources/KanaKanjiConverterModule/ConversionAlgorithms/Zenzai/Zenz/ZenzContext.swift)も参考にし、空白を全角空白に正規化します。トークナイズ時にはBOS追加を有効にし、特殊トークン文字列の解釈を無効にします。

## 負荷の制御

- 初回の変換要求までモデルをロードしません。`mmap` を使い、GPUへの配置・KV offload・演算offloadを無効にします。
- 既定はCPU 2スレッド、context 512、最大128生成トークン、推論500msです。logits出力は1位置に限定して不要なメモリを確保しません。CMakeでCUDA/Vulkan/Metal/BLAS/OpenMPを無効にしています。
- 専用ggml threadpoolに `poll=0` を指定し、要求終了時にpauseします。待機中にCPUを回し続けません。
- 生成中およびモデルロード中のキャンセルに対応します。推論期限はcold load完了後に開始します。初回ロード時間は推論期限の対象外ですが、UI threadでは実行しません。
- 期限切れ、キャンセル、生成上限、UTF-8不正、制御文字、不自然な長さの出力を破棄します。途中までの文章を候補として返しません。
- KVメモリは要求終了時に消去します。ニューラル側で文章を永続保存しません。

ニューラル生成だけでは漢字の読み一致は保証できません。変換器側で辞書の読み・表記を有界DPで照合し、確認できればCLIの `reading_verified=true` を返します。ASCII英数字の変更や、出力がかなだけで読みが異なる場合は候補を破棄します。辞書にない漢字語の読みは未検証のまま候補として許可するため、`reading_verified=false` の出力には誤変換の可能性があります。候補はユーザーの操作で確定し、辞書候補も保持します。学習済みの表記はニューラル候補より優先します。

入力は最大128文字です。フロントエンド側の入力上限を大きくしても、128文字を超える読みはニューラル推論をせず辞書候補を利用します。成功結果のキャッシュは32件、プロセス内だけで保持します。

## 検証

モデル不要のプロトコル・入力検証・キャンセル・フォールバックテストはRustの `cargo test` で行います。実推論は次で確認できます。

```sh
ctest --test-dir build -R rust_real_backends --output-on-failure
```

実行結果のレイテンシはその時点のPC・ビルド設定での値です。全環境での上限を保証する値ではありません。
