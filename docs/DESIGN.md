# 乳児うつ伏せ検知デバイス 設計書

## 1. 設計方針

- 映像配信とローカル推論は同一デバイスで実行する。
- Jev モードでは HTTPS 経由で外部の姿勢判定を利用する。
- 検知結果はフレーム上へ直接重畳し、クライアント側の追加処理を不要にする。
- Wi-Fi 切断や推論失敗時もプロセスを止めず、自己復旧を優先する。

## 2. 全体構成

1. 通信層
   - `wifi_sta_manager`: STA 接続、切断検知、再接続を管理する。

2. 取得層
   - `camera_capture`: カメラ初期化と JPEG フレーム取得を担当する。

3. 推論層
   - `prone_inference`: ESP-DL でフレーム推論し、検知候補の矩形・信頼度・ランドマーク座標を返す。

4. 判定層
   - `prone_judge`: ランドマーク方向メトリクス（ヨー非対称度・縦比率・短縮比）と時系列判定を行い、`MONITORING` と `ALERT` を決定する。

5. 描画層
   - `overlay_renderer`: 顔検知成立時に赤い四角を JPEG へ描画する。

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
   - `CONFIG_WIFI_SSID`
   - `CONFIG_WIFI_PASSWORD`
   - `PRONE_CONFIDENCE_TH = 0.70f`
   - `PRONE_HOLD_SEC = 10`
   - `FRAME_INTERVAL_MS = 500`

2. 実行時構造体
   - `detection_box_t`: `x`, `y`, `w`, `h`, `confidence`
   - `inference_result_t`: `is_prone`, `confidence`, `box`, `timestamp_ms`
   - `system_state_t`: 現在状態、連続検知開始時刻、失敗カウンタ
   - `face_orientation_t`: ランドマークから算出した方向メトリクス。※ランドマーク座標のうち左口角X (`lmx`) と右口角X (`rmx`) は現時点で使用していないが、今後の拡張（口角の傾きによる回転推定など）のために取得・予約している。

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

- 初期設計のタスクやモジュール構成と、現在の実装には差がある。
- `main/main.c` は Wi-Fi、撮影、配信、監視状態の接続を担当する。
- 撮影タスクは最新画像を公開する。
- 推論タスクと配信処理は、最新画像を専用バッファへコピーする。
- `prone_inference_bridge.cpp` は縮小とローカル顔推論を担当する。
- `jev_protocol.c` は質問、応答検証、判定変換、周期計算を担当する。
- `jev_client.c` は HTTPS、multipart、通信期限を担当する。
- `monitor_settings_json.c` は設定入力の検証を担当する。
- `monitor_settings.c` は NVS の保存と復元を担当する。
- 設定と判定状態は mutex で保護する。
- 設定変更時に世代を更新し、旧世代の判定結果を破棄する。
- 詳細な外部動作は `SPECIFICATIONS.md` の Issue #10 節を参照する。
