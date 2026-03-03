# TODO: うつ伏せ検知システム (prone-guard)

## フェーズ 1: 基盤整備

- [ ] 既存コード (`main.c`, `prone_inference_bridge.cpp/.h`) を削除またはリネーム
- [ ] `idf_component.yml` の依存を更新 (`human_face_detect` → `coco_pose`)
- [ ] `partitions.csv` を新パーティション設計に更新
- [ ] YOLO11n-Pose `.espdl` モデルファイルを `main/models/` に配置
- [ ] `sdkconfig` に Wi-Fi SSID/パスワード、固定 IP 設定を追加

## フェーズ 2: 推論ブリッジ

- [ ] `pose_inference_bridge.h` を新規作成 (`pose_result_t`, `pose_inference_status_t` 定義)
- [ ] `pose_inference_bridge.cpp` を新規作成 (`COCOPose` ラッパー実装)
- [ ] `pose_inference_init()` 実装 (モデルロード)
- [ ] `pose_inference_run_jpeg()` 実装 (JPEG デコード → 推論 → 結果変換)
- [ ] `pose_inference_get_status()` 実装

## フェーズ 3: うつ伏せ判定

- [ ] `prone_detector.h` を新規作成 (`prone_status_t`, `prone_result_t` 定義)
- [ ] `prone_detector.c` を新規作成
- [ ] 条件 A (顔キーポイント不可視性) 実装
- [ ] 条件 B (耳と肩の位置関係) 実装
- [ ] 条件 C (肩と腰の Y 座標差) 実装
- [ ] 総合スコア算出とホールド・解除ロジック実装

## フェーズ 4: メインアプリケーション

- [ ] `main.c` を新規作成 (既存顔検出ロジックを全面書き換え)
- [ ] Wi-Fi STA 初期化 + 固定 IP 設定
- [ ] カメラ初期化
- [ ] 推論ブリッジ初期化
- [ ] HTTP サーバ (ポート 80) 実装
  - [ ] `GET /` ハンドラ (HTML + JS)
  - [ ] `GET /health` ハンドラ
  - [ ] `GET /keypoints` ハンドラ
- [ ] MJPEG ストリームサーバ (ポート 81) 実装
  - [ ] `GET /stream` ハンドラ
- [ ] ストリームハンドラ内の推論呼び出し + うつ伏せ判定統合
- [ ] 状態遷移管理 (BOOT → WIFI_CONNECTING → READY → MONITORING → ALERT)

## フェーズ 5: ビルド・検証

- [ ] `CMakeLists.txt` 更新
- [ ] `idf.py build` でコンパイル成功を確認
- [ ] `idf.py flash` で書き込み
- [ ] シリアルモニタで起動ログ確認
- [ ] ブラウザで `http://192.168.0.150/` にアクセスし MJPEG ストリーム表示確認
- [ ] `/health` API レスポンス確認
- [ ] `/keypoints` API レスポンス確認
- [ ] うつ伏せ姿勢での `ALERT` 状態遷移確認
- [ ] 通常姿勢への復帰で `MONITORING` 状態への復帰確認
