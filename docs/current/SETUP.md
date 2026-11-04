# 環境構築と運用

## 商品向けの接続手順

商品では、お客様だけでの接続完結とセキュリティの両方を必須条件にする。
セットアップはエッジアプリ内のツールで行い、開発用スクリプトは使用しない。
商品用ツールは未実装である。詳細と受入条件は [接続設計](CONNECTION.md)、進捗は [実装計画](../plans/active/2026-10-28-issue-15-edge-sensors.md) を参照する。

1. 配布パッケージからエッジアプリを導入して起動する。
2. セットアップツールの「機器を追加」を開き、マイコンを USB で接続する。
3. 機器の登録ボタンを 3 秒押し、同梱カードの機器固有コードで認証する。
4. Wi-Fi、ベッド ID、設置情報を入力し、「接続する」を選ぶ。
5. アプリによる設定、Wi-Fi 上の相互の認証、登録の確定を待ち、「接続完了」の表示後に USB を外す。

相互の認証は、マイコンによるエッジの TLS 検証と、エッジによる機器別キーの検証を指す。
USB の登録自体も Security 2 で認証・暗号化する。
SDK、ビルド、eFuse 書込み、証明書生成、OS の CA 登録、固定 IP、秘密ファイル編集はお客様に要求しない。
出荷前の機器準備は製造側、証明書・機器別キーの発行と更新はアプリが担当する。

Wi-Fi 変更は USB 接続とアプリによる所有者認証で行う。
エッジを紛失した場合は、新エッジへ USB 接続し、登録ボタン 10 秒長押しとカードのコードで復旧する。
機器交換・登録解除では、エッジ側で旧機器のキーを失効させる。
カードと所有者の資格情報を両方失った場合に、認証を省略する復旧手順は設けない。
中断した場合はツールを再度開き、保存済みの状態に応じて登録を再開または取り消す。
再実行のために使用中の鍵や設定ファイルを削除しない。

## 開発者向けの現行試作手順

以下は現行コードのビルド・評価用であり、商品のお客様向け手順ではない。
現行 USB 設定は平文 JSON で、機器登録の相手認証・物理受付制限・世代管理は未実装である。
以下を実施しても商品向けの安全性や単独導入を確認済みとは扱わない。
既存の鍵や設定は再利用する。実機の flash と eFuse 書込みには、対象を示して個別の確認を求める。

通常の構築は [README の初期設定から動作確認まで](../../README.md#初期設定から動作確認まで) に従う。
開発ツールを導入して USB 接続した後、通常のターミナルでリポジトリルートから `python3 tools/setup.py` を実行する。
`export.sh` による環境有効化、設定生成、専用ビルド先でのビルド、確認付き HMAC 準備・flash、USB 初期設定、エッジ起動、HTTPS 接続確認をスクリプトが行う。
エッジと実機の Wi-Fi・時刻・センサー・送信の成功を確認するまでは完了表示を出さない。
起動したエッジは Ctrl-C まで動作し、失敗・中断時はスクリプトが起動したプロセスだけを停止する。
以下の第 3〜6 節は処理内容と個別診断用コマンドの説明である。通常の構築では、スクリプト実行後に各コマンドを重ねて実行する必要はない。

現在は、動作する範囲が異なる次の手順がある。

| 手順 | 準備するもの | 対応範囲 |
| --- | --- | --- |
| 第 3〜6 節: `python3 tools/setup.py` | `edge/local/` に設定を生成・再利用し、ビルド・実機書込み・USB 初期設定・エッジ起動・接続確認を実行 | 既存ファームウェアによるセンサー送信の試作 |
| 第 8 節: `python3 tools/setup.py --registration` | `edge/registration-local/` の設定を生成・再利用し、エッジをビルド・起動して HTTPS を確認 | 登録 DB、アプリ内の CA・証明書生成と更新、mDNS 広告の開発・検証 |
| 第 9 節: Security 2 検証用ファームウェア | 独立したテスト用ファームウェアと Rust の `pairing_probe` | USB 上の認証と初期時刻設定の相互運用検証 |

第 8 節と第 9 節は、商品用の初期登録を最後まで行う手順ではない。
エッジ側の登録状態・接続確認・失効の実装はあるが、商品用の USB 登録との統合と設定画面は未完了である。

## 0. コマンドの実行場所と変更先

本書のコマンドは、特記がなければリポジトリのルートで実行する。
リポジトリのルートは、clone 先の `README.md` があるディレクトリを指す。
`docs/current/` や `edge/` へ移動する必要はない。

clone を実行した親ディレクトリから、次のように移動する。
clone 時に別名を指定した場合は、`prone-guard` を指定した名前へ置換する。

```sh
# 実行場所: git clone を実行したディレクトリ (prone-guard の親)。
cd prone-guard
# README.md、main/、edge/、tools/ があることを確認する。
ls
```

以降は同じディレクトリで実行する。
新しいターミナルを使う場合も、clone 先のルートを開く。
`YOUR_PORT`、IP アドレス、`REPLACE_WITH_...` は設置先の値へ置換する。
置換前のコマンドは実行しない。

| 節 | 実行するコンピュータとディレクトリ | 変更先・影響 |
| --- | --- | --- |
| 1. 開発環境 | 開発用 Mac のリポジトリルート | 現在のシェルの環境変数を設定 |
| 2. 接続 | コマンドなし | 電源を切った基板の配線 |
| 3. 証明書と設定 | 開発用 Mac のリポジトリルート | `edge/local/` に鍵、証明書、設定をまとめて作成 |
| 4. HMAC キー | USB 接続した開発用 Mac のリポジトリルート | `edge/local/nvs-hmac.bin` を作成。`burn-key` は実機の eFuse を不可逆に変更 |
| 5. ファームウェア | USB 接続した開発用 Mac のリポジトリルート | ビルドはローカルファイルを生成。`flash` は実機のフラッシュを変更。初期設定ツールは実機の暗号化 NVS を変更 |
| 6. エッジアプリ | エッジとして使うコンピュータのリポジトリルート | ビルド成果物と SQLite DB を作成。アプリは終了まで動作 |
| 7. 検証 | 開発用コンピュータのリポジトリルート | テスト用のファイルとプロセスを作成。実機への書込みは行わない |
| 8. 登録管理エッジ | エッジとして使うコンピュータのリポジトリルート | `edge/registration-local/` に設定を作成。初回起動時に CA・秘密鍵・登録 DB・観測 DB を作成 |
| 9. Security 2 の検証 | USB 接続した開発用コンピュータのリポジトリルート | 独立した検証用ファームウェアをビルド・書込み。認証後に検証機の UTC を設定 |

開発用 Mac をエッジとして使用する場合は、全て同じルートで実行する。
通常のセットアップスクリプトは USB 接続したコンピュータをエッジにする。
別のコンピュータをエッジにする場合は、第 3〜6 節の個別手順で準備し、起動前にファイルを配置する。
転送対象は `config.json`、`edge.pem`、`edge-key.pem`、`ca.pem` とする。
転送先は、そのコンピュータのリポジトリの `edge/local/` とする。
このファイル転送手順は、第 3〜6 節の旧試作用である。
秘密鍵と設定ファイルの権限は `0600` を維持する。
CA 秘密鍵、デバイス秘密鍵、`device.json`、HMAC キーは転送しない。

設定生成は初回だけ行う。
セットアップスクリプトの再実行では、既存の `edge/local/config.json` と鍵を検証して再利用する。
設定がないのに生成先に別のファイルがある場合は、上書きせず停止する。
再実行ではビルド・確認付き実機書込み・接続確認を再度行う。通常のエッジ再起動だけなら第 6 節の起動コマンドを使う。
再実行のために、使用中の鍵や設定を削除しない。
`edge/local/` は Git の管理対象外だが、秘密情報として管理する。

## 1. 開発環境

- ファームウェア: ESP-IDF 6.0.3、ESP-DL 3.3.14、human_face_detect 0.5.1。
- エッジ: Rust 1.99.1、edition 2024。
- OS: macOS 27.x または Debian 13.7。
- USB 設定: ESP-IDF の Python と pyserial。
- 証明書と設定の生成: Python 3 と OpenSSL。

OpenSSL が必要なのは第 3 節の旧試作手順である。
第 8 節では Python 3 と Rust でエッジを構築し、証明書は Rust アプリが生成する。OpenSSL と ESP-IDF は不要である。

通常のセットアップでは、`setup.py` が ESP-IDF の環境を有効化するため、手動の source は不要である。
`IDF_PATH` があればその SDK を使用する。未設定なら EIM の標準配置 `~/.espressif/tools/activate_idf_v6.0.3.sh` から SDK・ツールの配置を取得する。
いずれもない場合は、`export.sh` がある SDK ディレクトリを対話入力する。
スクリプトは SDK の `export.sh` を読み込み、そこで選択された Python で自分自身を再起動する。
ビルド・eFuse・USB 設定はこの環境を引き継ぐ。読み込み失敗時や版の不一致時は実機へ書き込まず停止する。

以下の手動の環境有効化は、本書の個別診断や自動テストを直接実行するターミナル用である。`setup.py` を実行する前提手順ではない。
EIM の標準配置では次を使う。

```sh
source "$HOME/.espressif/tools/activate_idf_v6.0.3.sh"
```

手動導入の場合は、次の `/path/to/esp-idf` を SDK の配置先へ置き換える。

```sh
# 実行場所: 開発用 Mac のリポジトリルート。
export IDF_PATH="/path/to/esp-idf"
source "$IDF_PATH/export.sh"
```

有効化後に、使用する SDK の版を確認する。

```sh
# 実行場所: リポジトリルート。EIM と手動インストールの両方で確認する。
idf.py --version
# ESP-IDF v6.0.3 であることを確認する。
```

VS Code の SDK 選択とシリアルポートは、利用者の環境に合わせて設定する。
個人の絶対パスやポートを、共有のワークスペース設定へ保存しない。
C/C++ のコンパイル情報は、ビルドで生成する `compile_commands.json` を参照する。
Rust の版は `edge/rust-toolchain.toml` と Cargo.lock で固定する。
Debian のビルドには `build-essential`、`cmake`、`pkg-config`、`ca-certificates`、`libudev-dev` を用意する。
`libudev-dev` は Rust の `serialport` が Linux の USB ポートを列挙するために必要である。
SQLite と TLS ライブラリは Cargo の依存としてビルドする。

## 2. 接続

電源と USB を外し、基板の実際の端子表記を確認してから配線する。

### 2.1. 深度センサー

Switch Science の Conta VL53L5CX（商品番号 10281）を対象とする。
下表の番号は Conta コネクタのピン番号であり、Freenove の物理ピン番号ではない。実物のシルク印刷で端子を特定する。

| Conta 端子 | Freenove 側 |
| --- | --- |
| 5: SCL | GPIO21 |
| 4: SDA | GPIO14 |
| 3: IO4 / INT | GPIO47（省略可） |
| 2: GND | GND |
| 1: 3.3V | 3V3 |

IO4 は Conta 規格の端子名であり、Freenove の GPIO4 へ接続しない。
現行コードは `vl53l5cx_check_data_ready()` でポーリングし、GPIO47 は入力に設定するだけで読み取らないため、INT 配線は必須ではない。
Conta 回路図では INT は 47kΩ で IOVDD へプルアップされ、LPn は 47kΩ で High、I2C_RST は 47kΩ で Low に固定される。LPn / I2C_RST の追加配線は不要である。
IOVDD は R6（0Ω）を介して 3.3V と接続される標準実装を使用し、別電圧を印加しない。

カメラの SCCB は GPIO4 / GPIO5 の I2C0、新 I²C ドライバを使用する。
深度の I²C は I2C1、400kHz、7bit アドレス 0x29 とする。
データシートの 8bit 表記 `0x52` をそのまま ESP-IDF のアドレスへ指定しない。
GPIO が別でも、同じ I²C コントローラを別バスとして二重確保することはできない。
`sdkconfig.defaults` で `CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW=y` と `CONFIG_SCCB_HARDWARE_I2C_PORT0=y` を指定し、`main.c` でもビルド時に検査する。
既存の `sdkconfig` は defaults より優先される。再利用する場合は、対象ビルドの `idf.py menuconfig` の **Component config → Camera configuration** で **I2C driver selection for SCCB = New I2C driver**、**I2C peripheral to use for SCCB = I2C0** を選択する。
`build/setup/` を再利用する場合のコマンドは次のとおり。設定・鍵や NVS を消去する必要はない。

```sh
idf.py -B build/setup -DSDKCONFIG="$PWD/build/setup/sdkconfig" menuconfig
```

ULD は 8×8、5Hz、最も強い対象 1 個で測距する。
マイコンは最新の測距結果を毎秒送信する。
Conta は測距中に約 100mA を使用するため、電源の余裕を実測する。
実際の基板型式、端子、マップの向きを照合してから評価する。
ピン割当の根拠は [Freenove 公式ピン配置](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/ESP32S3_Pinout.png)、[Conta 製品資料](https://www.switch-science.com/products/10281)、[Conta 回路図](https://doc.switch-science.com/media/files/19d73541-d39a-4e3c-bf7a-69a2a56af50a.pdf) による。
GPIO14 / GPIO21 / GPIO47 は同ピン配置でカメラ、PSRAM（GPIO35〜37）、SD（GPIO38〜40）、USB（GPIO19 / GPIO20）、RGB LED（GPIO48）と重複しない。

Conta には SDA / SCL それぞれに 10kΩ のプルアップがある。
10kΩ だけで 400kHz の動作を保証しない。
配線容量に応じて 3.3V への外付け抵抗を選び、立ち上がりを実測する。
抵抗の選定は [ESP-IDF I²C ガイド](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/api-reference/peripherals/i2c.html) に従う。
内部プルアップだけに依存せず、5V へプルアップしない。

### 2.2. TFT ST7735

1.8-inch、128×160px、18bit の ST7735 モジュールを縦向きで使用する。
端子は GND / VCC / SCL / SDA / RES / DC / CS / BL とする。
専用資料がないため、汎用 ST7735 の初期条件で確認する。
下表は購入モジュールの保証仕様ではない。
USB と電源を外し、実物の端子名を照合してから配線する。

| TFT | Freenove | 接続方法 |
| --- | --- | --- |
| GND | GND | 共通にする |
| VCC | 3V3 | 3.3V で初期確認する。5V は印加しない |
| SCL | GPIO1 | SPI SCLK |
| SDA | GPIO2 | SPI MOSI |
| RES | GPIO42 | リセット |
| DC | GPIO41 | コマンドとデータの選択 |
| CS | GPIO38 | SPI チップ選択 |
| BL | 3V3 | 330Ω の直列抵抗を介して接続する |

SCL / SDA は SPI の信号名であり、深度センサーの I²C と並列接続しない。
SPI2_HOST、mode 0、8MHz の書込み専用接続を使用する。
GPIO38 は microSD の CMD と共用のため、カードを挿入しない。
GPIO2 には基板上 LED があるため、転送で点灯状態が変化する。
GPIO41 / GPIO42 の外部 JTAG は併用しない。
カメラ、PSRAM、USB、深度の GPIO は変更しない。

BL は LED の正極または High 有効の制御入力と仮定する。
GPIO から直接給電せず、初期接続では 330Ω の抵抗を省略しない。
バックライトの点灯と電流を実測する。
暗い場合も根拠なく抵抗を短絡しない。
カメラ・Wi-Fi・測距・バックライトの同時動作で電圧低下を確認する。

初期化はリセット後に 120ms、sleep out 後に 120ms 待機する。
RGB666、縦向き、BGR、表示反転オフを使用する。
列オフセットは +2、行オフセットは 0 とする。
論理上の横 128px をコントローラの RAM 列 2〜129 へ書き込む。
専用資料がないため、表示位置、向き、色順、反転は実機で確認する。
不一致がある場合は `main/tft_display.c` のパネル設定を調整する。
確認前に配線や実際の描画を合格扱いしない。

表示は [TFT 深度マップ仕様](SPECIFICATIONS.md#7-tft-深度マップ) に従う。
初期設定後、起動時に表示を開始する。
検出可能な初期化・転送失敗は `TFT update failed` として記録する。
再初期化は失敗から 5 秒以上空ける。
SPI の応答確認はなく、未接続でも転送成功になる場合がある。
SPI の成功ログだけでパネル接続を確認済みにしない。
実装と検証状態は [表示移行計画](../plans/active/2026-11-04-issue-17-st7735-depth-display.md) で管理する。

## 3. スクリプト内の証明書・設定生成

第 0 節で移動したリポジトリルートで実行する `python3 tools/setup.py` の設定生成段階を説明する。
この段階の後も、スクリプトはビルド・実機書込み・起動・接続確認へ進む。

スクリプトは、次の値を対話形式で入力する。

- エッジとデバイスの IPv4 アドレス。
- Wi-Fi の SSID とパスワード。パスワードは非表示で入力する。
- ベッド ID。空欄の場合は `bed-01` とする。
- カメラと深度マップの設置方向。空欄の場合は `未確認` とする。

IP アドレスは、ルーターの DHCP 予約などで固定する。
スクリプトは、デバイスや OS のネットワーク設定を変更しない。
設置方向は実測した内容を記入し、不明な方向は空欄のままにする。
Wi-Fi パスワードは、ファームウェアと同じ ASCII の 8〜63 文字に制限する。

生成先と処理内容を確認し、`y` を入力するとファイルを作成する。
空欄または `y` 以外を入力した場合は、作成せず終了する。
失敗した場合は、一時生成物を片付け、既存ファイルを維持する。
設定生成の段階では eFuse、フラッシュ、USB 初期設定、OS の信頼設定は変更しない。
後続の実機書込みには確認を求める。OS の CA 信頼設定は自動変更しない。

| 生成するファイル | 用途 |
| --- | --- |
| `ca.pem`、`ca-key.pem` | ローカル CA の証明書と秘密鍵 |
| `edge.pem`、`edge-key.pem` | エッジの HTTPS 証明書と秘密鍵 |
| `device.pem`、`device-key.pem` | デバイス管理画面の HTTPS 証明書と秘密鍵 |
| `config.json` | エッジ設定。デバイスキーの SHA-256 を保存 |
| `device.json` | USB 初期設定。Wi-Fi、デバイスキー、管理者パスワードを保存 |

全てリポジトリの `edge/local/` に、権限 `0600` で保存する。
生成先ディレクトリの権限は `0700` とする。
秘密の内容は、標準出力や OpenSSL のコマンド引数へ出さない。
サーバ証明書の SAN は、入力した IP アドレスに合わせる。
スクリプトは署名と IP の一致を検証してから、ファイルを配置する。
サーバ証明書は 365 日、CA 証明書は 3650 日有効とする。
既存環境の鍵は交換しない。再実行では証明書の厳格な検証も行い、期限・IP・署名・鍵の組を確認できなければ実機への書込み前に停止する。

ブラウザで管理画面を使う前に、`ca.pem` を信頼する CA として登録する。
証明書の検証を省略する設定や `curl -k` は使用しない。
CA の秘密鍵は実行中のアプリやマイコンへ配布しない。
管理画面は、ユーザ名 `admin` と `device.json` の `admin_password` を使用する。

デバイスキーはランダムな 64 桁の文字列とする。
エッジの hash は、その文字列の UTF-8 bytes の SHA-256 とする。
16 進表記を binary に戻した値の hash ではない。
保存期間の初期値は 6 時間とする。
空ベッドの深度は未測定として `null` を保存する。
実測後に `config.json` の設置情報を更新する。

残りのコマンドも、同じリポジトリルートで実行する。
スクリプトは、実行中の環境から求めた生成先を画面に表示する。
生成した秘密ファイルと個人の絶対パスを、公開する文書やログへ転載しない。

## 4. HMAC キーとパーティション

ファームウェアは、eFuse の既存 HMAC_UP キーを使用する。
キーを自動生成・書込みする処理は含めない。
未設定の場合はログにエラーを出して停止する。

初回の実機準備では、使用ブロックの状態を先に確認する。
次は作業者が実行する手順であり、今回の自動検証では実行していない。
eFuse の書込みは不可逆なので、既存のキー用途を確認する。

```sh
# 実行場所: USB 接続した開発用 Mac のリポジトリルート。
# ESP-IDF 環境を有効化してから実行する。
espefuse --chip esp32s3 --port YOUR_PORT summary
```

KEY0 に既存の `HMAC_UP` キーがある場合は、生成と書込みを省略する。
`summary` の出力を確認し、KEY0 が未使用の場合だけ次でキーを作成する。
既存のキーファイルがある場合は、上書きせず停止する。

```sh
# 実行場所: 開発用 Mac のリポジトリルート。ローカルファイルだけを作成。
umask 077
python3 -c 'import secrets; from pathlib import Path; Path("edge/local/nvs-hmac.bin").open("xb").write(secrets.token_bytes(32))'
```

次のコマンドは実機を不可逆に変更する。
確認が済むまでは実行しない。

```sh
# 実行場所: 上記と同じリポジトリルート。実機への不可逆な書込み。
espefuse --chip esp32s3 --port YOUR_PORT burn-key BLOCK_KEY0 edge/local/nvs-hmac.bin HMAC_UP
```

KEY0 が別の用途で使用中の場合は、別の空きブロックを選択する。
選んだ番号を `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID` に設定する。
HMAC キーのバックアップは、リポジトリ外の管理領域へ保管する。

`partitions.csv` は旧 nvs と factory の位置を維持する。
旧 storage の先頭 64KiB を、新しい secure_nvs に割り当てる。
独自に storage 領域を使っていた実機は、書込み前にデータを退避する。
このプロジェクトの旧実装は SPIFFS を使用していない。

## 5. ファームウェアのビルドと USB 初期設定

セットアップスクリプトは `build/setup/` とその中の専用 `sdkconfig` に `sdkconfig.defaults` を適用してビルドする。
ルートの既存 `sdkconfig` を引き継がず、ネイティブ USB のコンソールと HMAC 暗号化 NVS を使用する。
以下は個別に手動でビルドする場合のコマンドである。

```sh
# 実行場所: USB 接続した開発用 Mac のリポジトリルート。
# 同じターミナルで、第 1 節の ESP-IDF 環境を有効化する。
# set-target は既存のビルドディレクトリを再生成する。
idf.py set-target esp32s3
idf.py build
```

次は実機へ書き込む操作である。
HMAC キー、証明書、設定ファイルの準備後に実行する。
`YOUR_PORT` は接続した実機のシリアルポートへ置換する。

```sh
# 実行場所: 上記と同じリポジトリルート。実機への書込み。
idf.py -p YOUR_PORT flash
python3 tools/provision_device.py --port YOUR_PORT --config edge/local/device.json
```

初期設定ツールは、秘密をコマンド引数やログへ展開しない。
既定のコンソールは ESP32-S3 のネイティブ USB Serial/JTAG とする。
Freenove 基板の ESP32-S3 側 USB ポートへ接続する。
UART ブリッジを使用する場合は、主コンソールを UART に変更する。
JSON を USB へ送り、PROVISION_OK の確認後に終了する。
シリアルモニタと初期設定ツールで同じポートを同時使用しない。
初期設定後は、デバイスの HTTPS URL を開く。
Basic 認証はユーザ名 admin と device.json の admin_password を使用する。
接続不能になった場合も、USB から再設定できる。
設定変更で STA が変わった場合は、実機を再起動する。

## 6. エッジアプリ

```sh
# 実行場所: エッジとして使うコンピュータのリポジトリルート。
cargo build --manifest-path edge/Cargo.toml --locked
# JEV_API_KEY はシェルの秘密管理機能などで環境変数に設定する。
# 次のアプリは動作し続ける。Ctrl-C で終了する。
edge/target/debug/prone-guard-edge edge/local/config.json
```

起動状態の確認は、別のターミナルを開いて実行する。
別のターミナルでも、最初にリポジトリルートへ移動する。
以下の `192.168.1.50` は、セットアップ時に入力したエッジ IP へ置換する。
設定ファイル内の相対パスは、アプリ起動時の作業ディレクトリ基準である。

```sh
# 実行場所: エッジのリポジトリルート。アプリ起動中に別のターミナルで実行。
curl --cacert edge/local/ca.pem https://192.168.1.50:8443/health
```

JEV_API_KEY が未設定でも、観測の受付と保存は行う。
判定時には jev_not_configured を記録する。
API key が設定されている場合は、3 分分の観測が揃ってから送信する。
結果とデータ不足の理由は標準エラーへ JSON で記録する。
システムサービスで運用する場合は、ログの保存先とローテーションを設定する。
ログに生のセンサーデータや秘密を保存しない。

保存期間は config.json の retention_seconds で変更できる。
変更後はエッジアプリを再起動する。
100MB は DB、索引、rollback journal を含む合計予算である。
DB 本体は約 48MB に制限し、残りをジャーナル用に予約する。
容量不足時は古いデータが削除され、6 時間分を保持できない場合がある。

マイコンの NTP は起動時と 24 時間ごとに同期する。
エッジの時計は OS 標準の自動同期で維持する。
macOS の自動時刻設定、Debian の時刻同期状態を確認する。

## 7. 検証

```sh
# 実行場所: 開発用コンピュータのリポジトリルート。
# 第 1 節の ESP-IDF 環境を有効化し、ファームウェアをビルドしてから実行する。
./tests/run.sh
node tests/test_ui.cjs
python3 -m unittest discover -s tests -p 'test_provision.py'
python3 -m unittest discover -s tests -p 'test_setup.py'
cargo test --manifest-path edge/Cargo.toml --locked
cargo clippy --manifest-path edge/Cargo.toml --locked --all-targets -- -D warnings
python3 -m unittest discover -s tests -p 'test_edge_https.py'
```

別の ESP-IDF ビルド先を使う場合は、IDF_BUILD_DIR を指定する。
実機で、毎秒の受信、再接続、再起動後の NVS 復元を確認する。
64 セルの順序と、カメラ・深度の時間差も確認する。
TFT では、各セルの 16×16px 対応、64 階調、文字 2 行、無効セル、取得エラー、更新停止からの復帰を確認する。
同時接続で測距・送信周期を保てること、転送失敗の記録と取得・送信の継続を確認する。
TFT 未接続時の取得・送信継続、バックライト点灯時の電源も実機で確認する。
Jev の 180 秒入力、30 秒期限、姿勢ごとの識別能力は別に検証する。
合成データのテスト成功を、姿勢識別精度の確認として扱わない。

## 8. 登録管理エッジの開発用セットアップ

```sh
# 実行場所: エッジとして使うコンピュータのリポジトリルート。
python3 tools/setup.py --registration
```

スクリプトは作成前に確認し、`config.json` だけを権限 `0600`、ディレクトリを `0700` で配置する。
既存の `edge/local/` は変更しない。生成済みの登録管理設定と DB は検証して再利用する。
その後に Rust 1.99.1 でエッジをビルド・起動し、登録 DB の CA・名前を使って HTTPS の準備完了を確認する。
スクリプトは Ctrl-C までエッジを動作させる。失敗・中断時は、起動したエッジを停止して設定・DB を保持する。
IP アドレス、Wi-Fi、ベッド ID、機器別キーは、この設定作成では入力しない。
機器登録を行う商品用の画面と USB 統合が未完了のため、設定の作成やエッジの起動を「接続完了」とは扱わない。

生成する設定は次の形式である。パスはアプリ起動時の作業ディレクトリを基準に解決する。

```json
{
  "listen": "0.0.0.0:8443",
  "database": "edge/registration-local/sensors.sqlite",
  "registrations": "edge/registration-local/registrations.sqlite",
  "retention_seconds": 21600,
  "devices": []
}
```

- `registrations` を指定すると、登録 DB の有効な機器別キーで認証する。静的な `devices` は空にする。
- 初回起動時にエッジ ID、専用 CA、HTTPS 証明書と秘密鍵を生成し、登録 DB に保存する。
- 旧試作用の `tls_certificate` と `tls_private_key` の PEM ファイル指定は不要である。
- HTTPS 証明書の名前と mDNS 広告は `pg-edge-<edge-id>.local` とする。名前の探索とは別に TLS で認証する。
- エッジ証明書は 365 日、CA は 3650 日有効とする。エッジ証明書は残り 90 日で同じ鍵・名前を使って更新し、1 時間ごとの保守処理で確認する。
- 初期実装の AKI（発行者キー識別子）がないエッジ証明書は、次回起動時に同じ CA・鍵・名前で一度だけ再発行する。登録 DB の削除は不要である。
- 準備中キーは `/v1/pairing/check` の接続確認だけに使用でき、観測の保存は双方の確定後に許可する。初期状態では登録がないため観測を拒否する。

待受けポートは旧試作と同じ `8443` を使う。旧エッジを終了してから起動する。
この待受けは機器向けの HTTPS 受付であり、セットアップ画面は提供しない。
Jev の認証情報は従来と同じ `JEV_API_KEY` を使用する。

再起動時は生成済みの `config.json` を使い、セットアップスクリプトを再実行しない。
`registrations.sqlite` は CA 秘密鍵、エッジの TLS 秘密鍵、登録状態・世代・失効を保持する。
観測 DB と分けて保管し、6 時間の観測保持期限を理由に削除しない。別のエッジへ安易に複製しない。
旧 `device.json` の資格情報や第 5 節の平文 USB 設定は、この登録 DB の登録には使えない。
CA・証明書の期限切れやエッジ紛失からの商品用復旧は、USB 統合と製造保護を含む後続の実装・実機検証が必要である。

## 9. Security 2 と USB の相互運用検証

`tests/pairing_device/` は独立した検証用プロジェクトである。
公開された固定のテスト認証情報を使用し、出荷時の保護や商品用の機器登録は実装していない。
専用の開発用検証機を使用する。既存の試作機へ書き込むと、実行ファームウェアとパーティション構成が変わる。

```sh
# 実行場所: ESP-IDF 環境を有効化したリポジトリルート。ここではビルドだけを行う。
idf.py -C tests/pairing_device -B build/pairing-test \
  -DSDKCONFIG="$(pwd)/build/pairing-test/sdkconfig" -DIDF_TARGET=esp32s3 build
```

次は検証機へ書き込む操作である。ポートを確認し、専用の検証機に対してだけ実行する。

```sh
idf.py -C tests/pairing_device -B build/pairing-test \
  -DSDKCONFIG="$(pwd)/build/pairing-test/sdkconfig" -p YOUR_PORT flash
```

書込み後はシリアルモニタを閉じ、ESP32-S3 のネイティブ USB 側で GPIO0 のボタンを 3 秒押してから離す。
認証の受付は 120 秒、認証後の操作期限は 180 秒である。
ログは UART 側、認証フレームは USB Serial/JTAG 側を使う。

```sh
# 次のコードと機器 ID は、検証用ファームウェアだけの公開テスト値である。
PRONE_GUARD_TEST_CODE=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  cargo run --manifest-path edge/Cargo.toml --locked --example pairing_probe -- \
  YOUR_PORT 11111111111111111111111111111111
```

`pairing_probe` は Security 2 の相互認証後に機器 ID を照合し、エッジ OS の UTC を設定して応答を確認する。
Wi-Fi 設定、観測送信、登録の確定、製造保護の検証は行わない。
参照データとの照合、自動テスト、ビルド成功を、実機の相互運用や出荷保護との両立の成功には置き換えない。
