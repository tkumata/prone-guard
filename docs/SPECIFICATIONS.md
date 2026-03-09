# prone-like 監視デバイス 仕様書

## 1. 対象機能

- ESP32-S3 上の HTTP 映像配信
- ESP-DL による顔検知推論
- 顔矩形の時系列からの `prone-like` 推定

## 2. 設定仕様

1. 固定設定
   - `CONFIG_WIFI_SSID`: 文字列、必須、空文字禁止 (`menuconfig` 定義)
   - `CONFIG_WIFI_PASSWORD`: 文字列、必須、8 文字以上 (`menuconfig` 定義)

2. 既定値
   - `FRAME_WIDTH = 320`
   - `FRAME_HEIGHT = 240`
   - `FRAME_INTERVAL_MS = 500`
   - `FACE_CONFIDENCE_TH = 0.35`
   - `PRONE_LIKE_MISSING_MS_TH = 2000`
   - `PRONE_LIKE_LONG_MISSING_MS_TH = 5000`
   - `PRONE_LIKE_PRE_DISAPPEAR_MOVE_TH = 24.0`
   - `PRONE_LIKE_AREA_DROP_TH = 0.20`
   - `PRONE_LIKE_SCORE_TH = 0.70`
   - `LANDMARK_MIN_DIST = 3`
   - `YA_ROTATION_TH = 0.30`
   - `VP_DEVIATION_TH = 0.12`
   - `FR_FORESHORTEN_TH = 0.40`
   - `W_MISSING = 0.35`
   - `W_MOVE = 0.15`
   - `W_AREA = 0.15`
   - `W_YAW = 0.25`
   - `W_VP = 0.15`
   - `W_FR = 0.15`
   - `WIFI_RETRY_INTERVAL_SEC = 5`

3. 境界値
   - `FRAME_INTERVAL_MS`: 200 〜 1000
   - `FACE_CONFIDENCE_TH`: 0.25 〜 0.95
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
  - `landmarks` (5点ランドマーク: 左目・右目・鼻・左口角・右口角、各 x,y 座標)
- 判定:
  - `confidence >= FACE_CONFIDENCE_TH` かつ `is_face_detected == true` を顔検知成立とする。
  - ランドマーク座標から方向メトリクスを算出する。
  - `missing_ms`、`pre_disappear_move_px`、`pre_disappear_area_drop`、ランドマーク回転傾向 から `prone_like_score` を算出する。

## 4.1 方向メトリクス仕様

- ヨー非対称度 (YA): 鼻から左目/右目までのユークリッド距離比。範囲 [0,1]。
- 縦比率 (VP): 目〜鼻 / 目〜口 の縦位置比。範囲 [0,1]。正面顔で約 0.35〜0.45。
- 短縮比 (FR): 目の間隔 / 目〜口の縦幅。正面顔で約 0.8〜1.2。
- ランドマーク間距離が `LANDMARK_MIN_DIST` 未満の場合、当該メトリクスは無効値とする。

## 4.2 prone_like_score 算出仕様

- 6成分の重み付き線形結合 (重み合計 1.2):
  - `f_missing`: 消失時間の線形補間 [T_min, T_max] -> [0, 1]
  - `f_move`: 消失前移動量 / `PRONE_LIKE_PRE_DISAPPEAR_MOVE_TH`
  - `f_area`: 消失前面積縮小率 / `PRONE_LIKE_AREA_DROP_TH`
  - `f_yaw`: 消失前ヨー変化量（絶対値） / `YA_ROTATION_TH`
  - `f_vp`: 消失前縦比率変化量（絶対値） / `VP_DEVIATION_TH`
  - `f_fr`: 消失前短縮比変化量（絶対値） / `FR_FORESHORTEN_TH`
- 各成分は [0, 1] にクランプ後、対応する重みを乗じて合算する。
- ランドマークが無効な場合、回転由来の成分 (f_yaw, f_vp, f_fr) は 0 にフォールバックする。

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
