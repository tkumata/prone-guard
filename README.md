# prone-guard

Freenove ESP32-S3 WROOM CAM と YOLO11n-Pose を用いて姿勢監視からうつ伏せ状態を検知するシステムです。

## 目的

- ESP-IDF + ESP-DL でローカル推論 (YOLO11n-Pose) を行う。
- Wi-Fi STA で既存アクセスポイントへ接続する。
- HTTP サーバでカメラ映像を配信し、姿勢推定結果からうつ伏せ状態を判定してアラート状態へ遷移する。

## 現在の状態

- プロジェクト雛形を作成済み。
- `main/main.c` に Wi-Fi STA 接続、`GET /`、`GET /health`、`GET /keypoints` などの最小実装を追加済み。
- `GET /stream` は MJPEG 配信を実装済み（カメラ初期化失敗時のみ `503`）。
- 姿勢推定の状態遷移ロジック（うつ伏せ継続で `ALERT`、人物未検出の継続で `FAULT_INFERENCE`、検出再開で `MONITORING`）を実装済み。
- Webブラウザ上のJavaScriptで、配信映像に対してキーポイントやバウンディングボックスの描画と、うつ伏せ判定結果を重畳表示する。
- `main/pose_inference_bridge.cpp` で ESP-DL 公式の `coco_pose` (YOLO11n-Pose) を用いた推論実装を追加済み。
- `main/prone_detector.c` にて、17キーポイントの座標関係からうつ伏せ状態をスコア化して判定するロジックを追加済み。
- ESP-DL と `esp32-camera` 依存は `main/idf_component.yml` に追加済み。

## ドキュメント

- 要件定義: `docs/REQUIREMENTS.md`
- 設計書: `docs/DESIGN.md`
- 仕様書: `docs/SPECIFICATIONS.md`
- 実装 TODO: `docs/TODO.md`
- 環境構築手順: `docs/SETUP.md`
