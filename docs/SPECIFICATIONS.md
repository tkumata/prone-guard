# prone-like 監視デバイス 仕様書

## 1. 対象機能

- ESP32-S3 上の HTTP 映像配信
- ESP-DL による顔検知推論
- 顔矩形の時系列からの `prone-like` 推定

## 2. 設定仕様

1. 固定設定
   - `WIFI_SSID`: 文字列、必須、空文字禁止
   - `WIFI_PASSWORD`: 文字列、必須、8 文字以上を推奨

2. 既定値
   - `FRAME_WIDTH = 320`
   - `FRAME_HEIGHT = 240`
   - `FRAME_INTERVAL_MS = 500`
   - `FACE_CONFIDENCE_TH = 0.45`
   - `PRONE_LIKE_MISSING_MS_TH = 2000`
   - `PRONE_LIKE_LONG_MISSING_MS_TH = 5000`
   - `PRONE_LIKE_PRE_DISAPPEAR_MOVE_TH = 24.0`
   - `PRONE_LIKE_AREA_DROP_TH = 0.20`
   - `PRONE_LIKE_SCORE_TH = 0.70`
   - `WIFI_RETRY_INTERVAL_SEC = 5`

3. 境界値
   - `FRAME_INTERVAL_MS`: 200 〜 1000
   - `FACE_CONFIDENCE_TH`: 0.40 〜 0.95
   - `PRONE_LIKE_MISSING_MS_TH`: 1000 〜 5000
   - `PRONE_LIKE_LONG_MISSING_MS_TH`: 3000 〜 10000

## 3. HTTP 仕様

1. `GET /`
   - 役割: プレビュー画面を返す。
   - 応答: `text/html`
   - 内容: `<img src="/stream">` を含む最小ページ

2. `GET /stream`
   - 役割: MJPEG ストリーム配信
   - 応答: `multipart/x-mixed-replace; boundary=frame`
   - フレーム内容:
     - 顔検知成立時は検知領域に赤枠を重畳した JPEG を配信する
     - 顔未検知時、または描画失敗時は元画像を配信する

3. `GET /health`
   - 役割: 状態確認
   - 応答: `application/json`
   - 例:

```json
{
  "state": "MONITORING",
  "wifi": "connected",
  "camera": "ok",
  "inference": "ok",
  "monitor_status": "unknown",
  "pre_disappear_move_px": 0.0
}
```

## 4. 推論仕様

- 入力: カメラフレームをモデル入力サイズへ前処理したデータ
- 利用モデル:
  - `human_face_detect_msr_s8_v1.espdl`
  - `human_face_detect_mnp_s8_v1.espdl`
- 出力:
  - `is_face_detected` (`true` / `false`)
  - `confidence` (0.0 〜 1.0)
- 判定:
  - `confidence >= FACE_CONFIDENCE_TH` かつ `is_face_detected == true` を顔検知成立とする。
  - `missing_ms`、`pre_disappear_move_px`、`pre_disappear_area_drop` から `prone_like_score` を算出する。

## 5. 監視判定仕様

1. `OK`
   - `confidence >= FACE_CONFIDENCE_TH` かつ `is_face_detected == true`

2. `UNKNOWN`
   - 顔未検知かつ `prone_like_score < PRONE_LIKE_SCORE_TH`
   - 想定用途は短時間の見失い

3. `NG`
   - 顔未検知かつ `prone_like_score >= PRONE_LIKE_SCORE_TH`
   - `move_px` / `area_drop` が弱くても、長時間の顔消失で到達しうる

## 6. 描画仕様

- 描画条件: `is_face_detected == true` かつ顔矩形が有効な場合
- 描画色: RGB(255,0,0)
- 線幅: 2px
- 描画範囲: 推論結果の顔矩形をフレーム境界内にクリップした領域
- 失敗時: 描画なしで元 JPEG を返し、配信は継続する

## 7. 状態遷移仕様

- 初期状態: `BOOT`
- `BOOT -> WIFI_CONNECTING`
- `WIFI_CONNECTING -> READY`
- `READY -> MONITORING`
- `MONITORING -> ALERT`
- `ALERT -> MONITORING`
- `MONITORING/FAULT_INFERENCE -> FAULT_CAMERA`

禁止遷移:

- `BOOT -> FAULT_INFERENCE`
- `FAULT_CAMERA -> MONITORING`

## 8. エラー仕様

1. Wi-Fi
   - 条件: 接続失敗または切断
   - 挙動: 5 秒間隔で再接続
   - 影響: `/stream` は接続復旧まで中断

2. カメラ
   - 条件: 初期化失敗または取得失敗連続 5 回
   - 挙動: `FAULT_CAMERA` へ遷移し 5 秒ごとに再初期化
   - 影響: 画像配信停止

3. 推論
   - 条件: モデル実行に失敗する
   - 挙動: `FAULT_INFERENCE` へ遷移
   - 影響: 映像配信は継続し、`/health` で障害状態を返す

## 9. 互換性と移行

- 既存外部 API は未公開のため互換性制約はない。
- 将来 HTTPS 化や認証追加時は `/health` の JSON 契約維持を優先する。
