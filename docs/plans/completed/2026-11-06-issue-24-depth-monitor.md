# Issue #24 深度センサーモニターの修正

## Status

- Phase: verification
- Approval: approved

## Goal

無効セルは黒地の赤い `×` で、有効な遠距離セルと区別できるようにする。
README と現行文書で、現在のコードベースの動作と運用手順を確認できるようにする。

## Scope

- `main/depth_map.c` の無効セルの色と、描画テストの期待値を変更対象とする。
- README と `docs/current/` の全 5 文書を対象に、現在のコードベースと記述を照合する。無効セルの色は Task 1 の変更後のコードに合わせる。

## Non-goals

- センサー取得、測距値、JSONL、判定入力、管理画面を変更すること。
- パネルの初期化、色順、配線、表示の更新周期を変更すること。
- ADR、過去の計画、検証記録を書き換えること。

## Required Behavior

- MUST: 有効セルは現行の 64 階調で表示する。表示範囲は 0〜4000 mm とする。
- MUST: 0 mm は白、4000 mm は黒とする。
- MUST: 無効セルは黒地に赤い対角線 2 本で `×` を描く。
- MUST: 顔検知結果の色、凡例、左右反転、深度の状態表示を維持する。
- MUST: 現行の動作は、実装、設定例、テストで確認できる内容に合わせて記述する。
- MUST: 商品向けの要件や設計に未実装の項目がある場合、現在利用できる機能と区別する。

## Tasks

### Task 1: 無効セルの `×` を赤くする

- Objective: 無効セルを黒地の赤い `×` で表示する。
- Scope: `main/depth_map.c` と `tests/test_depth_map.c` を変更する。無効セルの赤には、既存の `FACE x` と同じ第 3 成分を使う。
- Acceptance Criteria: 無効セルは対角線だけを赤とし、残りを黒とする。既存の階調、有効性判定、取得状態、左右反転、顔表示を維持する。
- Verification: `sh tests/run.sh` で無効セルの画素と既存動作を確認する。`idf.py build` でファームウェアをビルドする。実機 TFT で無効セルの背景が黒く、`×` が `FACE x` と同じ赤に見えることを確認する。

### Task 2: 現行文書をコードベースに合わせる

- Objective: 現行の動作と運用手順を、現在のコードベースから説明する。
- Scope: README と `docs/current/` の `REQUIREMENTS.md`、`SPECIFICATIONS.md`、`DESIGN.md`、`SETUP.md`、`CONNECTION.md` を点検する。表示に加え、観測、管理 API と設定、機器登録、エッジ処理、セットアップを照合する。
- Acceptance Criteria: 現行の API、既定値、コマンド、運用手順がコードベースと一致する。変更履歴や過去の確認状況を現行の動作説明から整理する。移動済みの計画への参照を解消する。未検証の動作を確認済みと記述しない。
- Verification: 記述を `main/`、`edge/`、`tools/` の実装、設定例、テストと照合する。変更した相対リンクの参照先と、コマンドのオプションを確認する。

## Open Questions

- なし。

## 検証結果

- Task 1 の `sh tests/run.sh` と ESP-IDF 6.0.3 の `idf.py build` が成功した。
- Task 2 では README と現行文書 5 件を実装、設定例、テストと照合した。リポジトリ内の参照先と、セットアップ用 CLI のオプションを確認した。
- 管理画面、エッジ、USB 初期設定、セットアップの自動テストが成功した。仮想 eFuse のテストは ESP-IDF の Python で確認した。
- ユーザが VS Code で ESP-IDF の環境を再選択した。ESP-IDF 6.0.3 でのビルドと実機への書き込み成功をログで確認した。
- ユーザから動作確認の報告があった。実機 TFT の発色など、個別の確認項目は報告に含まれていない。

## References

- [Issue #24 深度センサーモニターの修正](https://github.com/tkumata/prone-guard/issues/24)
- [現行の TFT 表示仕様](../../current/SPECIFICATIONS.md#7-tft-深度マップ)
- [TFT 描画コード](../../../main/depth_map.c)
- [TFT 描画テスト](../../../tests/test_depth_map.c)
