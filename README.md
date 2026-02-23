# prone-guard

Freenove ESP32-S3 WROOM CAM を使い、乳児のうつ伏せ状態を検知してブラウザ映像上に赤枠を重畳表示するプロジェクトです。

現状は、うつ伏せ推論モデルがないので、テキトーなモデルにアクセスするだけのコード。

![sample](docs/assets/prone-guard-sample.png)

## 目的

- ESP-IDF + ESP-DL でローカル推論を行う。
- Wi-Fi STA で既存アクセスポイントへ接続する。
- HTTP サーバでカメラ映像を配信し、検知時に赤枠を表示する。

## 現在の状態

- プロジェクト雛形を作成済み。
- `main/main.c` に Wi-Fi STA 接続、`GET /`、`GET /health` の最小実装を追加済み。
- `GET /stream` は MJPEG 配信を実装済み（カメラ初期化失敗時のみ `503`）。
- うつ伏せ判定の状態遷移ロジック（10秒継続で `ALERT`、3秒解除で `MONITORING`）は実装済み。
- `main/models/prone.espdl` を埋め込み、推論ブリッジ経由で `is_prone` / `confidence` を更新する実装を追加済み。
- ESP-DL と `esp32-camera` 依存は `main/idf_component.yml` に追加済み。

## ドキュメント

- 要件定義: `docs/REQUIREMENTS.md`
- 設計書: `docs/DESIGN.md`
- 仕様書: `docs/SPECIFICATIONS.md`
- 実装 TODO: `docs/TODO.md`
- 環境構築手順: `docs/SETUP.md`
