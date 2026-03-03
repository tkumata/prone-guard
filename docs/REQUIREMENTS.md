# 要求定義: うつ伏せ検知システム (prone-guard)

## 1. 目的

乳幼児のうつ伏せ寝による窒息リスクを低減するため、カメラ映像からリアルタイムに姿勢を推定し、うつ伏せ状態を検知するシステムを構築する。

## 2. 機能要求

### FR-01: 姿勢推定

- ESP-DL 公式 YOLO11n-Pose モデルを使用し、人体の 17 キーポイント座標を取得する。
- 取得するキーポイント (COCO Keypoints):
  - 0: nose (鼻)
  - 1: left_eye (左目)
  - 2: right_eye (右目)
  - 3: left_ear (左耳)
  - 4: right_ear (右耳)
  - 5: left_shoulder (左肩)
  - 6: right_shoulder (右肩)
  - 7: left_elbow (左肘)
  - 8: right_elbow (右肘)
  - 9: left_wrist (左手首)
  - 10: right_wrist (右手首)
  - 11: left_hip (左腰)
  - 12: right_hip (右腰)
  - 13: left_knee (左膝)
  - 14: right_knee (右膝)
  - 15: left_ankle (左足首)
  - 16: right_ankle (右足首)

### FR-02: うつ伏せ判定

- FR-01 で取得したキーポイント座標を用いて、うつ伏せ状態かどうかを判定する。
- 判定ロジックの詳細は `SPECIFICATIONS.md` で定義する。

### FR-03: 映像配信

- HTTP MJPEG ストリームでカメラ映像をブラウザに配信する。
- ユーザは `http://192.168.0.150/` にアクセスして映像を確認する。

### FR-04: ヘルスチェック API

- `/health` エンドポイントで、Wi-Fi 接続状態・カメラ状態・推論状態・検知状態を JSON で返す。

### FR-05: キーポイント API

- `/keypoints` エンドポイントで、最新の推論結果 (17 キーポイント座標、うつ伏せ判定結果、信頼度) を JSON で返す。

## 3. 非機能要求

### NFR-01: ハードウェア

- ボード: Freenove ESP32-S3-WROOM CAM
- SoC: ESP32-S3 (Dual-core Xtensa LX7, 240 MHz)
- PSRAM: 8 MB (Octal SPI)
- Flash: 16 MB
- カメラ: OV2640

### NFR-02: ネットワーク

- Wi-Fi STA モード (2.4 GHz)
- 固定 IP アドレス: `192.168.0.150`
- SSID / パスワード: sdkconfig に設定
- 切断時は自動再接続する (再接続間隔: 5 秒)

### NFR-03: モデル配置

- YOLO11n-Pose モデルファイル (`.espdl`, 約 3 MB) は Flash 内の専用パーティション `models` に配置する。
- `partitions.csv` で十分なサイズの `models` パーティションを確保する。

### NFR-04: 開発環境

- ESP-IDF v5.3 以上
- ESP-DL v3.2.0 以上 (`espressif/esp-dl`)
- ESP-DL 公式 `coco_pose` コンポーネント (`espressif/coco_pose`)
- VS Code + ESP-IDF 拡張

### NFR-05: 推論性能

- 推論間隔: 500 ms 以下を目標とする。
- カメラ解像度: QVGA (320x240)。
- JPEG フレームを RGB888 にデコードしてから推論に渡す。

### NFR-06: メモリ制約

- ESP-DL の静的メモリプランナーにより、内部 SRAM と PSRAM を効率的に使い分ける。
- カメラフレームバッファは PSRAM に配置する (`CAMERA_FB_IN_PSRAM`)。

## 4. 制約事項

- ESP32-S3 のリソース制約上、複数人の同時検知は対象外とする (検出された人物のうちスコア最大の 1 人のみ判定)。
- モデルの再学習や量子化は本プロジェクトのスコープ外とする (ESP-DL 公式の量子化済みモデルを使用)。
