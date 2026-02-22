# VS Code + ESP-IDF + ESP-DL 環境構築手順（Freenove ESP32-S3 WROOM CAM）

## 1. 事前準備

- 対応OS: macOS / Linux / Windows
- 推奨空き容量: 15GB 以上
- USB データ通信対応ケーブルを使用する（充電専用ケーブル不可）
- ボード: Freenove ESP32-S3 WROOM CAM

## 2. 必要ソフトウェア

1. VS Code
   - 公式配布版を導入する

2. ESP-IDF 拡張
   - VS Code 拡張 `Espressif IDF` を導入する

3. Python
   - ESP-IDF が要求する Python バージョンを使用する

4. CMake / Ninja / Git
   - ESP-IDF セットアップ時に不足がある場合は追加導入する

## 3. VS Code 初期セットアップ

1. コマンドパレットを開く
   - `Cmd + Shift + P`（macOS） / `Ctrl + Shift + P`（Windows/Linux）

2. `ESP-IDF: Open ESP-IDF Installation Manager` を実行
   - 拡張の導入後、インストーラを起動して ESP-IDF とツール群を導入する
   - 画面や拡張バージョンにより `ESP-IDF: Open ESP-IDF Install Manager` と表示される場合がある
   - 一般的な配置先はホームディレクトリ配下（例: `~/.espressif`）
   - 既定導入では保存先入力が出ず、自動配置される場合がある

3. 使用する ESP-IDF セットアップを選択
   - `ESP-IDF: Select Current ESP-IDF Version` を実行し、利用するセットアップを選択する

4. セットアップ確認
   - `ESP-IDF: Open ESP-IDF Terminal` を開き、そのターミナルで次を実行する

   ```bash
   idf.py --version
   python --version
   ```

   - `idf.py` が実行できれば基本導入は完了
   - 通常の zsh で `idf.py` が見つからない場合でも、ESP-IDF Terminal で実行できれば導入は正常
   - 例: `$HOME/.espressif/v5.5.3/esp-idf/export.sh` が存在する環境では、ESP-IDF 本体は `~/.espressif` 配下にある

## 4. 新規プロジェクト作成

1. プロジェクト作成
   - `ESP-IDF: New Project` を実行する
   - テンプレートは `hello_world` を選択する
   - 作成先を `$HOME/Developer/prone-guard` 配下に設定する

2. ターゲット設定
   - 次を実行する

   ```bash
   idf.py set-target esp32s3
   ```

3. 初回ビルド

   ```bash
   idf.py build
   ```

## 5. Freenove ESP32-S3 WROOM CAM 接続確認

1. USB 接続
   - ボードを USB で接続する
   - macOS の場合、USB-UART ポートに接続する

2. シリアルポート確認
   - macOS:

   ```bash
   ls /dev/cu.*
   ```

   - 接続前後で増えたデバイス名を確認する（例: `/dev/cu.usbmodemXXXX`）
   - macOS の場合: `/dev/cu.usbmodem5AB90112901` で認識されるはず

3. 書き込み確認

   ```bash
   idf.py -p /dev/cu.usbmodemXXXX flash monitor
   ```

   - モニタ終了は `Ctrl + ]`。

## 6. ESP-DL 導入

1. コンポーネント管理
   - プロジェクトルートで次を実行する。

   ```bash
   idf.py add-dependency "espressif/esp-dl=*"
   ```

2. 依存解決確認

   ```bash
   idf.py reconfigure
   idf.py build
   ```

3. 失敗時確認ポイント
   - ネットワーク到達性
   - Python 仮想環境の有効化状態
   - `managed_components` 配下の取得状態

## 7. カメラ有効化の基本設定

1. `menuconfig` を開く

   ```bash
   idf.py menuconfig
   ```

2. 設定項目
   - PSRAM 有効化
   - カメラ用ピン設定（Freenove 仕様に合わせる）
   - ログレベル設定（初期は `INFO` 推奨）

3. 保存後再ビルド

   ```bash
   idf.py build
   ```

## 8. 動作確認手順

1. 起動確認
   - 起動ログにクラッシュがないことを確認

2. フレーム取得確認
   - カメラ初期化成功ログを確認

3. 推論動作確認
   - テスト画像入力で姿勢ラベルが出力されることを確認

4. 通知確認
   - テスト用 Webhook へ `POST` されることを確認

## 9. 典型トラブルと対処

1. ポートが見えない
   - ケーブル交換、USB ハブ経由回避、ドライバ再確認を実施

2. `idf.py` が見つからない
   - ESP-IDF 拡張のセットアップ再実行、統合ターミナル再起動を実施

3. フラッシュ失敗
   - BOOT ボタン押下で書き込みモードに入れて再実行する

4. ESP-DL ビルド失敗
   - `idf.py fullclean` 後に再ビルドし、依存再取得を実行する

## 10. 再現性確保

- 使用した ESP-IDF バージョンを `README.md` または `docs` に固定記録する
- `sdkconfig.defaults` を管理し、設定差分を抑制する
- 依存コンポーネントのバージョンを明示固定する

## 11. この構成を採用する理由（3段階）

1. なぜ VS Code + ESP-IDF か
   - 公式拡張でセットアップ、ビルド、書き込み、モニタを一元化でき、初期障害を減らせるため

2. なぜ ESP-DL か
   - ESP32-S3 上で軽量推論を実行でき、クラウド依存を減らして通知遅延と停止リスクを下げられるため

3. なぜ Freenove ESP32-S3 WROOM CAM か
   - カメラ・PSRAM・Wi-Fi を単体ボードで扱え、試作速度と配線信頼性を両立しやすいため
