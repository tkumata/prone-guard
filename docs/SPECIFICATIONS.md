# 乳児うつ伏せ検知デバイス 仕様書

## 1. 対象機能

- ESP32-S3 上の HTTP 映像配信
- ESP-DL によるうつ伏せ推論
- うつ伏せ継続検知時の赤枠オーバーレイ

## 2. 設定仕様

1. 固定設定
   - `WIFI_SSID`: 文字列、必須、空文字禁止
   - `WIFI_PASSWORD`: 文字列、必須、8 文字以上を推奨

2. 既定値
   - `FRAME_WIDTH = 320`
   - `FRAME_HEIGHT = 240`
   - `FRAME_INTERVAL_MS = 500`
   - `PRONE_CONFIDENCE_TH = 0.70`
   - `PRONE_HOLD_SEC = 10`
   - `WIFI_RETRY_INTERVAL_SEC = 5`

3. 境界値
   - `FRAME_INTERVAL_MS`: 200 〜 1000
   - `PRONE_CONFIDENCE_TH`: 0.50 〜 0.95
   - `PRONE_HOLD_SEC`: 3 〜 30

## 3. HTTP 仕様

1. `GET /`
   - 役割: プレビュー画面を返す。
   - 応答: `text/html`
   - 内容: `<img src="/stream">` を含む最小ページ

2. `GET /stream`
   - 役割: MJPEG ストリーム配信
   - 応答: `multipart/x-mixed-replace; boundary=frame`
   - フレーム内容:
     - 通常時: 元画像
     - `ALERT` 時: 赤枠描画済み画像

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
  - `is_prone` (`true` / `false`)
  - `confidence` (0.0 〜 1.0)
  - `box` (`x`, `y`, `w`, `h`)
- 判定:
  - `confidence >= PRONE_CONFIDENCE_TH` かつ `is_prone == true` を検知候補とする。

## 5. 継続判定仕様

1. 検知開始
   - 初回検知候補で `prone_started_ms` を記録する。

2. 警告成立
   - `now_ms - prone_started_ms >= PRONE_HOLD_SEC * 1000` で `ALERT` へ遷移する。

3. 警告解除
   - 非検知状態が 3 秒継続で `MONITORING` に戻す。

4. 失敗時
   - 推論失敗フレームは継続判定に含めない。

## 6. 描画仕様

- 色: RGB888 `#FF0000`
- 線幅: 2px（疑似値。実装時は描画関数に合わせる）
- 描画条件: `ALERT` かつ `box` が有効範囲内
- 範囲外座標:
  - `x < 0` や `y < 0` は 0 に丸める。
  - `x + w`、`y + h` が画面外の場合は端で切り詰める。

## 7. 状態遷移仕様

- 初期状態: `BOOT`
- `BOOT -> WIFI_CONNECTING`
- `WIFI_CONNECTING -> READY`
- `READY -> MONITORING`
- `MONITORING -> ALERT`
- `ALERT -> MONITORING`
- `MONITORING/ALERT -> FAULT_CAMERA`
- `MONITORING/ALERT -> FAULT_INFERENCE`

禁止遷移:

- `BOOT -> ALERT`
- `FAULT_CAMERA -> ALERT`
- `FAULT_INFERENCE -> ALERT`

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
   - 条件: 推論失敗連続 10 回
   - 挙動: `FAULT_INFERENCE` へ遷移
   - 影響: 赤枠表示停止、映像配信のみ継続

## 9. 互換性と移行

- 既存外部 API は未公開のため互換性制約はない。
- 将来 HTTPS 化や認証追加時は `/health` の JSON 契約維持を優先する。
