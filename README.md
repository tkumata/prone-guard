# prone-guard

Freenove ESP32-S3 WROOM CAM を使い、顔検知と `prone-like` 推定を監視して状態を公開するプロジェクトです。

現状は、`espressif/human_face_detect` コンポーネント (MSR + MNP) で顔検知と顔矩形のみを取得し、その時系列から `prone-like` をヒューリスティックで推定する実験コードです。
うつ伏せ検知モデルは未実装のため、判定はあくまで顔検知ベースの近似です。

## 目的

- ESP-IDF + ESP-DL でローカル推論を行う。
- Wi-Fi STA で既存アクセスポイントへ接続する。
- HTTP サーバでカメラ映像を配信し、`OK / UNKNOWN / NG (prone-like)` を公開する。

## 現在の状態

- プロジェクト雛形を作成済み。
- `main/main.c` に Wi-Fi STA 接続、`GET /`、`GET /health` の最小実装を追加済み。
- `GET /stream` は MJPEG 配信を実装済み（カメラ初期化失敗時のみ `503`）。
- 顔矩形の時系列から `missing_ms`、`pre_disappear_move_px`、`pre_disappear_area_drop` を算出し、`prone-like` を推定するロジックを実装済み。
- 顔検知成立時のみ、検知領域へ赤枠を重畳して `/stream` に配信する。
- Web UI は `OK: 顔検出`、`UNKNOWN: 顔未検出`、`NG: prone-like` を表示する。
- `main/prone_inference_bridge.cpp` で `human_face_detect_msr_s8_v1.espdl` と `human_face_detect_mnp_s8_v1.espdl` の2モデルを用いた推論実装を追加済み。
- ESP-DL と `esp32-camera` 依存は `main/idf_component.yml` に追加済み。

## prone-like 判定ロジック概要

`prone-like` は専用モデルではなく、顔検知結果の時系列からヒューリスティックに推定しています。大まかな流れは以下のとおりです。

1. 各フレームで顔検知を実行し、`is_face_detected == true` かつ `confidence >= 0.35` を生の顔検知成立とみなします。
2. 生の顔検知があった直近 `1200ms` は顔が見えているものとして扱い、その間は `OK` を返します。
3. 顔が見えている間は、最新 `6` 件までの顔矩形履歴から中心座標と面積を保存します。
4. 顔が見えなくなった瞬間に、直前 `1500ms` 以内の履歴を使って以下を計算します。
   - `pre_disappear_move_px`: 顔中心がどれだけ移動したか
   - `pre_disappear_area_drop`: 顔面積がどれだけ縮んだか
5. 顔が見えない時間 `missing_ms` と上記 2 指標から `prone_like_score` を加点式で算出します。

スコア加点条件は実装上、以下です。

- `missing_ms >= 2000` で `+0.55`
- `missing_ms >= 5000` で `+0.20`
- `pre_disappear_move_px >= 24px` で `+0.25`
- `pre_disappear_area_drop >= 0.20` で `+0.20`

最終的に `prone_like_score >= 0.70` なら `NG (prone-like)`、未満なら `UNKNOWN` です。顔が再び見えたら消失時間とスコアはリセットされ、状態は `OK` に戻ります。

## ドキュメント

- 要件定義: `docs/REQUIREMENTS.md`
- 設計書: `docs/DESIGN.md`
- 仕様書: `docs/SPECIFICATIONS.md`
- 実装 TODO: `docs/TODO.md`
- 環境構築手順: `docs/SETUP.md`
