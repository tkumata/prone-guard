# 仕様: うつ伏せ検知システム (prone-guard)

## 1. キーポイント座標仕様

### 1.1 COCO Keypoints インデックス

| Index | Name           | Japanese       |
| ----- | -------------- | -------------- |
| 0     | nose           | nose           |
| 1     | left_eye       | left_eye       |
| 2     | right_eye      | right_eye      |
| 3     | left_ear       | left_ear       |
| 4     | right_ear      | right_ear      |
| 5     | left_shoulder  | left_shoulder  |
| 6     | right_shoulder | right_shoulder |
| 7     | left_elbow     | left_elbow     |
| 8     | right_elbow    | right_elbow    |
| 9     | left_wrist     | left_wrist     |
| 10    | right_wrist    | right_wrist    |
| 11    | left_hip       | left_hip       |
| 12    | right_hip      | right_hip      |
| 13    | left_knee      | left_knee      |
| 14    | right_knee     | right_knee     |
| 15    | left_ankle     | left_ankle     |
| 16    | right_ankle    | right_ankle    |

### 1.2 座標形式

- ESP-DL `COCOPose::run()` は `std::list<dl::detect::result_t>` を返す。
- 各 `result_t` には以下が含まれる:
  - `score`: 検出信頼度 (float, 0.0 ~ 1.0)
  - `box[4]`: バウンディングボックス [x1, y1, x2, y2] (int, ピクセル座標)
  - `keypoint[34]`: 17 キーポイント × 2 座標 (int, ピクセル座標)
    - `keypoint[2*i]` = x 座標
    - `keypoint[2*i+1]` = y 座標

## 2. うつ伏せ判定仕様

### 2.1 判定に使用するキーポイント

主に以下のキーポイントの相対位置関係を使用する:

- **顔系**: nose (0), left_eye (1), right_eye (2), left_ear (3), right_ear (4)
- **肩系**: left_shoulder (5), right_shoulder (6)
- **腰系**: left_hip (11), right_hip (12)

### 2.2 判定ロジック

以下の条件を組み合わせて「うつ伏せらしさスコア」を算出し、閾値で判定する。

#### 条件 A: 顔キーポイントの不可視性

うつ伏せ時はカメラから顔の主要キーポイント (鼻、両目) が見えにくくなる。

- 鼻・左目・右目のうち、座標が (0, 0) またはバウンディングボックス外にあるキーポイント数をカウントする。
- 2 個以上見えない場合: スコア +0.4

#### 条件 B: 耳と肩の位置関係

うつ伏せ時は耳が肩より背面 (カメラから見て肩に隠れる方向) に来やすい。

- 両耳の Y 座標が両肩の Y 座標の中点より上 (値が小さい ＝ カメラ上部) にある場合: スコア +0.3
- 片耳のみの場合: スコア +0.15

#### 条件 C: 肩と腰の Y 座標差

仰向けと比較して、うつ伏せ時は肩と腰の Y 座標差が小さくなる傾向がある (体が水平に近い)。

- 肩中点の Y 座標と腰中点の Y 座標の差が閾値 (画像高さの 15%) 以下の場合: スコア +0.3

### 2.3 総合判定

- うつ伏せスコア = 条件 A + 条件 B + 条件 C (0.0 ~ 1.0)
- 閾値: **0.6 以上**でうつ伏せと判定
- ホールド時間: 連続 **1500 ms** 以上うつ伏せ判定が続いた場合に `ALERT` 状態へ遷移
- 解除: うつ伏せスコアが **0.4 以下**に下がった状態が **1000 ms** 以上続いた場合に `MONITORING` 状態へ復帰

### 2.4 人物未検出時の挙動

- 推論結果が空 (人物未検出) の場合、うつ伏せ判定はスキップする。
- 人物未検出が **3000 ms** 以上続いた場合は `FAULT_INFERENCE` 状態へ遷移する。

## 3. 推論ブリッジ仕様

### 3.1 初期化

```c
esp_err_t pose_inference_init(void);
```

- `COCOPose` インスタンスを生成する。
- モデルは `coco_pose` コンポーネントが Flash パーティション (`models`) から自動ロードする。
- 戻り値: `ESP_OK` (成功), `ESP_ERR_NO_MEM` (メモリ不足), `ESP_FAIL` (その他)

### 3.2 推論実行

```c
typedef struct {
    int keypoints[34];   // 17 keypoints x 2 (x, y)
    int bbox[4];         // [x1, y1, x2, y2]
    float score;         // detection confidence
    bool detected;       // true if person detected
} pose_result_t;

esp_err_t pose_inference_run_jpeg(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    pose_result_t *result
);
```

- JPEG データを RGB888 にデコードし、`COCOPose::run()` を実行する。
- 複数人検出時は、`score` 最大の 1 人のみを返す。
- 戻り値: `ESP_OK` (成功), `ESP_ERR_INVALID_ARG` (引数不正), `ESP_FAIL` (デコード/推論失敗)

### 3.3 ステータス取得

```c
typedef enum {
    POSE_INFERENCE_STATUS_NOT_READY = 0,
    POSE_INFERENCE_STATUS_OK,
    POSE_INFERENCE_STATUS_MODEL_MISSING,
    POSE_INFERENCE_STATUS_FAULT,
} pose_inference_status_t;

pose_inference_status_t pose_inference_get_status(void);
```

## 4. うつ伏せ判定モジュール仕様

### 4.1 インターフェース

```c
typedef enum {
    PRONE_STATUS_UNKNOWN = 0,
    PRONE_STATUS_NOT_PRONE,
    PRONE_STATUS_PRONE,
} prone_status_t;

typedef struct {
    prone_status_t status;
    float score;          // 0.0 ~ 1.0
    int64_t held_ms;      // current hold duration (ms)
} prone_result_t;

prone_result_t prone_check(
    const int keypoints[34],
    const int bbox[4],
    int image_width,
    int image_height
);
```

### 4.2 定数

| Name                    | Value | Description                        |
| ----------------------- | ----- | ---------------------------------- |
| PRONE_SCORE_THRESHOLD   | 0.6   | prone threshold                    |
| PRONE_CLEAR_THRESHOLD   | 0.4   | clear threshold                    |
| PRONE_HOLD_MS           | 1500  | hold time for ALERT (ms)           |
| PRONE_CLEAR_MS          | 1000  | clear time for recovery (ms)       |
| PERSON_MISSING_FAULT_MS | 3000  | fault timeout when no person (ms)  |
| INFERENCE_INTERVAL_MS   | 500   | inference interval (ms)            |
| FACE_INVISIBLE_SCORE    | 0.4   | score for condition A              |
| EAR_SHOULDER_SCORE      | 0.3   | score for condition B (both ears)  |
| EAR_SHOULDER_HALF_SCORE | 0.15  | score for condition B (single ear) |
| SHOULDER_HIP_SCORE      | 0.3   | score for condition C              |
| SHOULDER_HIP_RATIO      | 0.15  | threshold ratio for condition C    |

## 5. HTTP API 仕様

### 5.1 `GET /`

HTML ページを返す。ページ内の JavaScript が以下を実行する:

- `<img>` タグで `http://<host>:81/stream` に接続し MJPEG ストリームを表示
- 定期的 (200 ms 間隔) に `/keypoints` をポーリングし、キーポイントと判定結果をオーバーレイ表示

### 5.2 `GET /health`

```json
{
  "state": "MONITORING",
  "wifi": "connected",
  "camera": "ok",
  "inference": "ok",
  "prone_detected": false,
  "prone_score": 0.0
}
```

### 5.3 `GET /keypoints`

```json
{
  "detected": true,
  "score": 0.85,
  "bbox": [50, 30, 280, 220],
  "keypoints": [
    {"name": "nose", "x": 160, "y": 80},
    {"name": "left_eye", "x": 150, "y": 75},
    ...
  ],
  "prone": {
    "status": "not_prone",
    "score": 0.15,
    "held_ms": 0
  }
}
```

### 5.4 `GET /stream` (port 81)

- Content-Type: `multipart/x-mixed-replace;boundary=frame`
- 各フレームは JPEG データ
- フレーム間隔: 約 30 ms (カメラ FPS 依存)

## 6. Wi-Fi 仕様

### 6.1 sdkconfig 設定項目

```text
CONFIG_WIFI_SSID="YOUR_SSID"
CONFIG_WIFI_PASSWORD="YOUR_PASSWORD"
CONFIG_STATIC_IP_ADDR="192.168.0.150"
CONFIG_STATIC_GW_ADDR="192.168.0.1"
CONFIG_STATIC_NETMASK_ADDR="255.255.255.0"
```

### 6.2 静的 IP 設定

- `esp_netif_dhcpc_stop()` で DHCP クライアントを停止してから `esp_netif_set_ip_info()` で固定 IP を設定する。
- `IP_EVENT_STA_GOT_IP` イベント発生後に HTTP サーバを起動する。

## 7. カメラ仕様

### 7.1 Freenove ESP32-S3-WROOM CAM ピンマッピング

| Signal     | GPIO        |
| ---------- | ----------- |
| XCLK       | 15          |
| SIOD (SDA) | 4           |
| SIOC (SCL) | 5           |
| D7         | 16          |
| D6         | 17          |
| D5         | 18          |
| D4         | 12          |
| D3         | 10          |
| D2         | 8           |
| D1         | 9           |
| D0         | 11          |
| VSYNC      | 6           |
| HREF       | 7           |
| PCLK       | 13          |
| PWDN       | -1 (unused) |
| RESET      | -1 (unused) |

### 7.2 カメラ設定

- `pixel_format`: `PIXFORMAT_JPEG`
- `frame_size`: `FRAMESIZE_QVGA` (320x240)
- `jpeg_quality`: 12
- `fb_count`: 2
- `fb_location`: `CAMERA_FB_IN_PSRAM`
- `grab_mode`: `CAMERA_GRAB_LATEST`
- `xclk_freq_hz`: 20000000 (20 MHz)

## 8. パーティション仕様

```csv
# Name,     Type,    SubType,  Offset,    Size
nvs,        data,    nvs,      0x9000,    0x6000
phy_init,   data,    phy,      0xf000,    0x1000
factory,    app,     factory,  0x10000,   0x600000
models,     data,    spiffs,   0x610000,  0x400000
storage,    data,    spiffs,   0xA10000,  0x5F0000
```

- 総 Flash サイズ: 16 MB (0x1000000)
- `models` パーティション: 4 MB — YOLO11n-Pose `.espdl` モデルファイルを格納
- `storage` パーティション: 約 5.9 MB — 将来拡張用
