# prone-like 監視デバイス 仕様書

## 1. 対象機能

- ESP32-S3 上の HTTP 映像配信
- ESP-DL による顔検知推論
- 顔矩形の時系列からの `prone-like` 推定
- Jev 1.14 の画像判定と Web 管理画面での設定

## 2. 設定仕様

1. 固定設定
   - `CONFIG_WIFI_SSID`: 文字列、必須、空文字禁止 (`menuconfig` 定義)
   - `CONFIG_WIFI_PASSWORD`: 文字列、必須、8 文字以上 (`menuconfig` 定義)

2. 既定値
   - 撮影サイズ: 640x480
   - ローカル推論サイズ: 320x240
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

## 10. Jev モードと設定 (Issue #10)

### 設定 API

- `GET /settings` は `mode`、`threshold`、`key_configured` を返す。
- `mode` は `local` または `jev` とする。
- `threshold` は 0〜1 の有限数とし、初期値は 0.5 とする。
- `POST /settings` は `Content-Type: application/json` を要求する。
- 本文の上限は 2047 bytes とする。
- 本文は `mode`、`threshold`、`api_key` をすべて含む。
- `api_key` は空文字または 512 文字以下の ASCII 可視文字とする。
- 空文字の場合は、保存済み API key を変更しない。
- 保存成功時は、NVS に commit してから設定を即時反映する。
- 判定方式、閾値、API key を再起動後も維持する。
- 入力不正は HTTP 400、保存失敗は HTTP 500 とする。
- API key 自体は取得応答とログへ出力しない。
- NVS 初期化に失敗しても、資格情報を自動消去しない。

### 画像と判定

- 撮影はブラウザの接続状態から独立したタスクで実行する。
- 最新 JPEG は 1 件だけ保持する。バッファ上限は 256KiB とする。
- 配信と推論はそれぞれ画像をコピーし、処理中に共有ロックを保持しない。
- ローカル推論は 320x240 に縮小し、既存の画素閾値を維持する。
- Jev へは VGA の JPEG を multipart で送る。
- API は `https://api.typesafe.ai/v1/systemone` とする。
- モデルは `jev-1.14.0` に固定する。
- HTTPS のサーバ証明書とホスト名を検証する。
- プロジェクト内の自己署名 GTS Root R4 を `cert_pem` に指定する。
- Jev 接続では `crt_bundle_attach` を指定しない。
- SNTP の時刻同期前は `clock_not_ready` とし、画像を送らない。
- 送信は 10 秒周期、通信タイムアウトは送信開始から 8 秒とする。
- タイムアウト後の再送は行わず、次の周期の画像を送る。
- 並行送信と遅延分のまとめ送りは行わない。
- Choice で `prone`、`not_prone`、`unjudgeable` を分類する。
- 質問は、顔の向きではなく胴体と支持面の関係を対象にする。
- `unjudgeable` は判別不能エラーとし、非うつ伏せへ変換しない。
- 判別可能な場合は、`probabilities.prone >= threshold` を `true` とする。
- `true` は `NG`、`false` は `OK`、エラーは `UNKNOWN` とする。
- 設定変更前に開始した処理の結果は適用しない。
- API のリダイレクトと認証の自動再試行は行わない。
- 応答本文の上限は 8192 bytes とする。
- 不正な JSON、型、選択肢、確率はエラーとする。

### 状態取得

`GET /health` に次の項目を追加する。

| 項目 | 値 |
| --- | --- |
| `mode` | `local` または `jev` |
| `jev_status` | `ok`、`pending`、`not_configured`、`network_error`、`timeout`、`http_error`、`invalid_response`、`unjudgeable`、`camera_error`、`clock_not_ready` |
| `jev_probability` | 判定成功時はうつ伏せ確率、それ以外は -1 |
| `jev_is_prone` | Jev 判定成功時は bool、それ以外は null |

Jev モードでは顔検知の枠を表示しない。
管理画面は Jev の `OK` を「うつ伏せ以外」と表示する。

### 検証の限界

- 自動テストは周期、応答検証、閾値、設定入力を確認する。
- 実 API、実機の連続配信、NVS 復元、識別精度は別途検証する。
- Jev の画像入力は実験機能であり、識別精度は未確認である。
