# Issue #20: TFT 深度マップの左右反転設定

## Status

- Phase: completed
- Approval: approved (2026-11-04)
- Issue: [#20 TFT LCD の左右の動きについて](https://github.com/tkumata/prone-guard/issues/20)
- Branch: `feat/issue-20`
- ユーザは計画全体を承認し、実装を指示した。
- 設定、保存互換性、管理画面、描画の実装は完了した。
- 現行文書と自動テストを更新した。
- 2026-11-04 にユーザが実機の動作確認と完了を報告した。
- ユーザの動作確認をもって、本変更を受け入れ済みとする。
- 個別結果が未報告の試験は、実施済みとして扱わない。

## Goal

開発中の対面配置で、深度マップの左右の動きを合わせる。
管理画面から TFT の左右反転を切り替えられるようにする。
既定の表示方向は現在のままとする。
再起動後も保存済みの選択を復元する。

## Scope

既存の管理設定、保存処理、描画処理に設定を追加する。
新しい外部ライブラリは追加しない。

| 対象 | 変更内容 |
| --- | --- |
| `main/index.html` | 「左右を反転する」のチェックボックス、読込み、保存 |
| `main/monitor_settings.h` | 左右反転設定の追加 |
| `main/monitor_settings_json.c` | JSON boolean の検証と省略時の互換性 |
| `main/monitor_settings.c` | 保存、復元、旧形式の読込み |
| `main/main.c` | 設定 API と表示タスクへの接続 |
| `main/depth_map.c`、`.h` | 描画時だけ列の参照順序を反転 |
| `tests/test_monitor.c` | 設定の有効・無効、省略、不正入力 |
| `tests/test_depth_map.c` | 左右反転と既存描画の回帰検証 |
| `tests/test_ui.cjs` | 設定取得、切替、保存、再取得の動作 |
| `tests/test_monitor_settings.c`、`tests/fakes/nvs.h` | NVS の保存・復元・旧形式・失敗の検証用に追加 |
| `tests/run.sh` | NVS 保存処理のホストテストを接続 |
| `README.md`、`docs/current/SETUP.md` | 操作と実機確認手順 |
| `docs/current/REQUIREMENTS.md` | 左右反転の要求と受け入れ条件 |
| `docs/current/SPECIFICATIONS.md` | 設定 API、保存互換性、表示の対応関係 |
| `docs/current/DESIGN.md` | 設定と描画の責務、スナップショットの扱い |

以下の既存呼出し元と入力も互換性確認に含める。

- `main/device_security.c` の USB 初期設定は設定デコーダーを共用する。
- `tools/setup.py` は左右反転項目のない設定 JSON を生成する。
- `tools/provision_device.py` は設定 JSON を USB 用に包む。
- 描画関数の呼出し元は `main/main.c` と `tests/test_depth_map.c` にある。
- 既存の観測 fixture とエッジ側の契約は変更しない。

## Non-goals

- 上下反転、回転、表示方向の自動検出。
- TFT 全体や文字の鏡像表示。
- 深度取得、カメラ、JSONL、SQLite、Jev 入力の変更。
- エッジの設置情報 `depth_map_order` の変更。
- USB 初期設定やエッジ画面への新しい操作項目の追加。
- ST7735 の初期化、配線、色順、列オフセットの変更。
- 無関係な文書リンクや過去の完了計画の修正。

## Required Behavior

Issue で指定された確定事項は次のとおりとする。

- MUST: 管理画面に「左右を反転する」を追加する。
- MUST: 未設定時は現在の表示方向を使用する。
- MUST: 選択を不揮発メモリに保存できるようにする。
- MUST: 有効・無効の切替で深度マップの左右の動きを変える。

以下の具体的な API と反映方法は承認済みとする。

### 管理画面と API

- MUST: 既存の設定フォーム内にチェックボックスを配置する。
- MUST: 既存の「保存」操作で左右反転設定も保存する。
- MUST: API の項目名を `tft_mirror_x` とする。
- MUST: `GET /settings` は保存済みの値を JSON boolean で返す。
- MUST: `POST /settings` は JSON boolean だけを受け付ける。
- MUST: 文字列、数値、`null`、重複キーは `400` で拒否する。
- MUST: POST で項目を省略した場合は現在値を維持する。
- MUST: USB 初期設定で項目を省略した場合は `false` とする。
- MUST: フォーム取得時と保存成功時に、応答値を選択状態へ反映する。
- MUST: 保存失敗を管理画面に表示する。
- MUST: 保存失敗時は入力値を保持し、再試行できるようにする。
- MUST: HTTPS、認証、Origin、本文サイズの制限を維持する。
- MUST: Wi-Fi パスワードとデバイスキーの非公開契約を維持する。

### 保存と互換性

変更前の実装は構造体全体を `edge_settings` blob に保存していた。
変更前の読込みは、blob と構造体のサイズの一致を要求していた。
本変更では旧形式も検証し、反転無効として復元する。

- MUST: 暗号化 `secure_nvs` 内の管理設定として保存する。
- MUST: 旧 blob の既存項目を復元し、新項目だけを `false` とする。
- MUST: 旧形式は既知のサイズと既存項目の検証で識別する。
- MUST: 次回の設定保存で新形式として保存する。
- MUST: 再起動後に `true` と `false` の両方を復元する。
- MUST: 不正なサイズや値を、正常な旧形式として扱わない。
- MUST: 読込み・保存エラーを返し、NVS を自動消去しない。
- MUST: 保存失敗時は実行中の設定と表示方向を変更しない。
- MUST: 既存の Wi-Fi、送信先、資格情報を保持する。
- MUST: 既存の USB 設定 JSON を変更なしで受け付ける。

### 描画と反映

- MUST: 無効時は `[row][col]` を現在の表示列 `col` に配置する。
- MUST: 有効時は `[row][col]` を表示列 `7-col` に配置する。
- MUST: 行の対応関係を変更しない。
- MUST: 距離と target_status を同じセルから参照する。
- MUST: 無効セルの印も対応する反転先へ移す。
- MUST: 下部の状態文字と凡例は現在の向きで表示する。
- MUST: 階調、無効値、欠測、更新停止の判定を維持する。
- MUST: 保存成功後は、再起動せず次回の表示更新で反映する。
- MUST: 表示タスクはフレームごとに設定を一度コピーする。
- MUST: 全 10 区画の描画中は同じ設定値を使用する。
- MUST: 描画と SPI 転送中は共有状態の mutex を保持しない。
- MUST: 観測スナップショットと送信データを並べ替えない。
- MUST: 1 秒周期と表示障害時の取得・送信継続を維持する。

## Tasks

### Task 1: 管理設定と永続化の拡張

- Status: completed
- Result: boolean の検証、省略時の互換性、新旧 NVS の読込みを実装した。
- Validation: 設定 JSON と NVS のホストテストは成功した。
- Validation: 既存 provision テスト 1 件、setup テスト 31 件は成功した。
- Validation: 通常 Python で省略された仮想 eFuse テストは SDK Python で成功した。
- Objective: 左右反転設定を検証、保存、復元できるようにする。
- Scope: 設定型、JSON デコーダー、NVS、設定 API と対応テスト。
- Acceptance Criteria:
  - `true`、`false`、項目省略が指定どおりに処理される。
  - 不正な型と重複キーを拒否する。
  - 旧 blob の既存項目を保持し、反転無効で読み込む。
  - 両方の設定値を保存後に読み戻せる。
  - 読込みと保存の失敗を成功扱いしない。
  - 保存失敗時に実行中の設定を変更しない。
  - 既存の USB 初期設定 JSON を受け付ける。
- Verification:
  - `tests/test_monitor.c` で設定 JSON の観測可能な動作を検証する。
  - NVS の代替実装で新旧 blob と失敗条件を検証する。
  - `tests/run.sh` に保存処理のホストテストを追加する。
  - Python の既存 provision/setup テストで入力の互換性を確認する。

### Task 2: 管理画面と深度マップ描画への接続

- Status: completed
- Result: チェックボックス、設定 API、フレーム単位の反映を接続した。
- Validation: UI の取得、両方向の保存、再取得、保存失敗のテストは成功した。
- Validation: 全画素の左右対応、文字領域、入力不変性のテストは成功した。
- Validation: ESP-IDF 6.0.3 のビルドで設定 API とタスクの接続を確認した。
- Objective: 管理画面から保存した方向を TFT に反映する。
- Scope: 設定フォーム、表示タスク、描画関数と全呼出し元、対応テスト。
- Acceptance Criteria:
  - 保存済みの選択を管理画面に表示する。
  - 有効・無効の両方を保存できる。
  - 全 64 セルが指定の表示列へ対応する。
  - 無効セルの印が距離と同じ列へ移る。
  - 状態文字、凡例、上下方向を変更しない。
  - 1 フレーム内に反転前後の設定を混在させない。
  - 描画で入力データを変更しない。
- Verification:
  - Task 1 完了後に UI テストで取得、両方向の保存、失敗を検証する。
  - 描画テストで左右非対称の深度と無効セルを両設定で検証する。
  - 描画テストで文字領域と入力データの不変性を検証する。
  - ESP-IDF ビルドで実際の設定 API とタスクの接続を確認する。

### Task 3: 仕様と操作手順の同期、結合検証

- Status: completed
- Result: README と要件、仕様、設計、操作手順を同期した。
- Validation: 自動テストと実機確認の結果を分けて記録した。
- Validation: `tests/run.sh` の全ホストテストは成功した。
- Validation: `git diff --check` は成功した。
- Validation: ESP-IDF 6.0.3 のビルドは成功した。
- Validation: アプリは 0x26f020 bytes、最小パーティションの空きは 59% だった。
- Validation: sandbox 内の初回ビルドは `sysctl` の権限制限で停止した。
- Validation: 制限外で同じ SDK と既存設定を使用したビルドは成功した。
- Validation: 2026-11-04 にユーザが実機の動作確認と完了を報告した。
- Validation: 再起動後の保持と旧設定移行の個別試験結果は未報告とする。
- Objective: 操作と互換性を文書化し、確認済みの範囲を記録する。
- Scope: Scope に列挙した現行文書、本計画、既定の検証手段。
- Acceptance Criteria:
  - 表示仕様の「反転を加えない」を、設定による例外と整合させる。
  - API、既定値、旧設定の読込み、反映時期を文書へ記載する。
  - 実機確認と自動検証の結果を分けて記録する。
  - 未実施の確認を完了扱いしない。
- Verification:
  - Task 1 と Task 2 完了後に以下を実行する。
  - `IDF_BUILD_DIR=build/setup ./tests/run.sh`
  - `node tests/test_ui.cjs`
  - `python3 -m unittest discover -s tests -p 'test_provision.py'`
  - `python3 -m unittest discover -s tests -p 'test_setup.py'`
  - ESP-IDF 6.0.3 環境で以下を実行する。
  - `idf.py -B build/setup -DSDKCONFIG="$PWD/build/setup/sdkconfig" build`
  - `git diff --check`
  - 同一差分で成功済みの検証は、追加変更や懸念がない限り再実行しない。
  - 実機で左右移動と上下移動を、反転の有効・無効で確認する。
  - 実機で保存後の反映、画面再取得、再起動後の保持を確認する。
  - 実機で旧設定を保持した更新後も取得・送信できるか確認する。
  - 実機の書込みと確認は、利用環境と実施権限がある段階で行う。

## Open Questions

- 現時点で未決定の質問はない。
- API 名、旧形式の読込み、再起動不要の反映は承認済みとする。
- ユーザは実機の動作確認を終え、本変更の完了を承認した。
- 再起動後の保持と旧設定移行の個別試験は確認済みにしない。

## References

- [Issue #20](https://github.com/tkumata/prone-guard/issues/20)
- [要件](../../current/REQUIREMENTS.md)
- [仕様: 管理 API と TFT](../../current/SPECIFICATIONS.md)
- [設計: マイコンと設定保存](../../current/DESIGN.md)
- [開発者向け手順](../../current/SETUP.md)
- [既存 TFT の完了計画](../completed/2026-11-04-issue-17-st7735-depth-display.md)
