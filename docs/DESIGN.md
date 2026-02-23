# 乳児うつ伏せ検知デバイス 設計書

## 1. 設計方針

- 映像配信と推論を同一デバイスで完結させる。
- 検知結果はフレーム上へ直接重畳し、クライアント側の追加処理を不要にする。
- Wi-Fi 切断や推論失敗時もプロセスを止めず、自己復旧を優先する。

## 2. 全体構成

1. 通信層
   - `wifi_sta_manager`: STA 接続、切断検知、再接続を管理する。

2. 取得層
   - `camera_capture`: カメラ初期化と JPEG フレーム取得を担当する。

3. 推論層
   - `prone_inference`: ESP-DL でフレーム推論し、検知候補の矩形と信頼度を返す。

4. 判定層
   - `prone_judge`: 連続時間判定を行い、`MONITORING` と `ALERT` を決定する。

5. 描画層
   - `overlay_renderer`: `ALERT` 時のみ赤い四角を JPEG へ描画する。

6. 配信層
   - `http_stream_server`: HTML と MJPEG ストリームを配信する。

## 3. タスク分割

- `net_task`: Wi-Fi 接続監視
- `capture_task`: フレーム取得
- `inference_task`: 推論処理
- `stream_task`: HTTP 配信
- `watchdog_task`: フリーズ検知

共有データはリングバッファ 2 本で受け渡す。

- `raw_frame_queue`: カメラ取得後フレーム
- `render_frame_queue`: 描画済みフレーム

## 4. 状態設計

- `BOOT`: 起動直後
- `WIFI_CONNECTING`: STA 接続中
- `READY`: 配信準備完了
- `MONITORING`: 通常監視中
- `ALERT`: うつ伏せ継続検知中
- `FAULT_CAMERA`: カメラ障害
- `FAULT_INFERENCE`: 推論障害

主な遷移条件:

- `BOOT -> WIFI_CONNECTING`: 起動完了
- `WIFI_CONNECTING -> READY`: Wi-Fi 接続成功
- `READY -> MONITORING`: HTTP サーバ開始成功
- `MONITORING -> ALERT`: 信頼度 0.70 以上の検知が 10 秒継続
- `ALERT -> MONITORING`: 検知解除が 3 秒継続
- `MONITORING/ALERT -> FAULT_CAMERA`: カメラ取得失敗連続 5 回
- `MONITORING/ALERT -> FAULT_INFERENCE`: 推論失敗連続 10 回

## 5. データ設計

1. コンパイル時定数
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `PRONE_CONFIDENCE_TH = 0.70f`
   - `PRONE_HOLD_SEC = 10`
   - `FRAME_INTERVAL_MS = 500`

2. 実行時構造体
   - `detection_box_t`: `x`, `y`, `w`, `h`, `confidence`
   - `inference_result_t`: `is_prone`, `confidence`, `box`, `timestamp_ms`
   - `system_state_t`: 現在状態、連続検知開始時刻、失敗カウンタ

## 6. 処理フロー

1. `capture_task` が JPEG フレームを取得する。
2. `inference_task` が縮小画像で ESP-DL 推論を実行する。
3. `prone_judge` が連続時間判定を更新する。
4. `overlay_renderer` が必要時のみ赤枠を描画する。
5. `stream_task` が MJPEG チャンクでブラウザへ送出する。

## 7. 異常系設計

- Wi-Fi 切断:
  - `WIFI_CONNECTING` へ戻し、再接続試行を継続する。
  - 再接続までは既存 HTTP セッションを切断する。
- カメラ障害:
  - 5 秒ごとに再初期化を試行する。
  - 復旧時は `READY` に戻す。
- 推論障害:
  - 障害フレームは赤枠なしで配信する。
  - 10 回連続失敗時のみ `FAULT_INFERENCE` とする。

## 8. 運用設計

- ログレベル: `ERROR`, `WARN`, `INFO`
- 必須ログ:
  - Wi-Fi 接続成功/失敗
  - HTTP サーバ開始/停止
  - 状態遷移
  - 連続失敗カウンタ
- 機密情報:
  - SSID とパスワードはログ出力禁止

## 9. 現時点の実装差分

- 設計上の各モジュールは未実装。
- `main/main.c` は空である。
- 本設計は初期実装の基準として採用する。
