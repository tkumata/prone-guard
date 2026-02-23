# 顔認識監視デバイス 仕様書

## 1. 対象機能

- ESP32-S3 上の HTTP 映像配信
- ESP-DL による顔認識推論
- 顔未認識時の障害遷移

## 2. 設定仕様

1. 固定設定
   - `WIFI_SSID`: 文字列、必須、空文字禁止
   - `WIFI_PASSWORD`: 文字列、必須、8 文字以上を推奨

2. 既定値
   - `FRAME_WIDTH = 320`
   - `FRAME_HEIGHT = 240`
   - `FRAME_INTERVAL_MS = 500`
   - `FACE_CONFIDENCE_TH = 0.70`
   - `FACE_MISS_FAULT_SEC = 3`
   - `WIFI_RETRY_INTERVAL_SEC = 5`

3. 境界値
   - `FRAME_INTERVAL_MS`: 200 〜 1000
   - `FACE_CONFIDENCE_TH`: 0.50 〜 0.95
   - `FACE_MISS_FAULT_SEC`: 1 〜 10

## 3. HTTP 仕様

1. `GET /`
   - 役割: プレビュー画面を返す。
   - 応答: `text/html`
   - 内容: `<img src="/stream">` を含む最小ページ

2. `GET /stream`
   - 役割: MJPEG ストリーム配信
   - 応答: `multipart/x-mixed-replace; boundary=frame`
   - フレーム内容:
     - 常に元画像を配信する

3. `GET /health`
   - 役割: 状態確認
   - 応答: `application/json`
   - 例:

```json
{
  "state": "MONITORING",
  "wifi": "connected",
  "camera": "ok",
  "inference": "ok"
}
```

## 4. 推論仕様

- 入力: カメラフレームをモデル入力サイズへ前処理したデータ
- 出力:
  - `is_face_detected` (`true` / `false`)
  - `confidence` (0.0 〜 1.0)
- 判定:
  - `confidence >= FACE_CONFIDENCE_TH` かつ `is_face_detected == true` を正常候補とする。

## 5. 監視判定仕様

1. 正常判定
   - `confidence >= FACE_CONFIDENCE_TH` かつ `is_face_detected == true` を正常とみなす。

2. 障害成立
   - 非正常状態が `FACE_MISS_FAULT_SEC` 秒継続で `FAULT_INFERENCE` に遷移する。

3. 障害解除
   - 正常判定を再取得したフレームで `MONITORING` に戻す。

## 6. 描画仕様

- 推論結果に応じたオーバーレイ描画は行わない。
- `/stream` はカメラ JPEG をそのまま返す。

## 7. 状態遷移仕様

- 初期状態: `BOOT`
- `BOOT -> WIFI_CONNECTING`
- `WIFI_CONNECTING -> READY`
- `READY -> MONITORING`
- `MONITORING -> FAULT_INFERENCE`
- `FAULT_INFERENCE -> MONITORING`
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
   - 条件: 顔が `FACE_MISS_FAULT_SEC` 秒以上認識できない
   - 挙動: `FAULT_INFERENCE` へ遷移
   - 影響: 映像配信は継続し、`/health` で障害状態を返す

## 9. 互換性と移行

- 既存外部 API は未公開のため互換性制約はない。
- 将来 HTTPS 化や認証追加時は `/health` の JSON 契約維持を優先する。
