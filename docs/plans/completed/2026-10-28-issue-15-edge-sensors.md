# センサー送信・エッジ判定と安全なセットアップの実装計画

## Status

- Phase: implementation_and_validation_pending。
- 本書を実装計画の正本とする。
- センサー送信・保存・Jev 判定は実装済み。実機・実 API・Debian の検証は未完了。
- 接続方式の設計方針は概ね了承済み。セットアップツールと認証付き登録は未実装。
- センサー構成の実装承認は有効とする。未選定の依存追加や設計変更へ流用しない。
- Task ごとに実装状態と未検証範囲を管理し、計画は active に置く。

## Goal

マイコンで顔検知と深度を取得し、エッジで保存・品質判定・Jev 判定を行う。
お客様がセットアップツールだけで機器の接続・設定変更・交換・復旧を完結できるようにする。
導入の簡素化とセキュリティの確保を、両方とも必須の受入条件にする。

## Scope

- Freenove ESP32-S3 WROOM CAM、ESP-DL、Conta VL53L5CX を使用する。
- 同じリポジトリのルートを ESP-IDF、`edge/` を Rust の開発単位にする。
- マイコンは最新の観測を毎秒送り、エッジは SQLite に保存する。
- エッジはベッドごとに直近 3 分の観測を Jev へ渡す。
- 初期登録と復旧は認証付き USB、通常通信は Wi-Fi STA と HTTPS を使う。
- 製造時の機器固有情報と保護設定、署名付き更新・復旧、出荷検査を実装する。
- エッジアプリにセットアップツールを組み込み、証明書・キー・登録状態を管理する。
- macOS と Debian の配布パッケージ、USB 権限設定、ローカル設定画面を用意する。
- 実装、境界条件、実機での動作、お客様による単独操作を検証する。

| 開発・実行環境 | 対象 |
| --- | --- |
| マイコン | ESP-IDF 6.0.3、ESP-DL 3.3.14、human_face_detect 0.5.1 |
| エッジ | Rust 1.99.1 / edition 2024、macOS 27.x、Debian 13.7 |
| 依存 | 採用時点の最新版と対象環境の互換性を確認し、解決結果を lockfile に固定する |

## Non-goals

- 映像配信、画像の外部送信、マイコン内の姿勢判定。
- 判定方式の切替、bool への変換、うつ伏せ確率の閾値設定。
- 通知アプリ、登録専用クラウド、スマートフォン必須の導入、モデル学習。
- 顔の個人認証、医療用途の認証、人的監視の代替、未測定の精度保証。
- 証明書検証を省略する復旧、未認証の設定 AP、USB の平文設定による商品登録。

## Required Behavior

### センサーと観測

- MUST: VGA (640×480) で撮影し、顔検知と顔矩形の座標系を 320×240 にそろえる。
- MUST: 既存の ESP-DL 顔検知モデルとピクセル基準を再利用する。
- MUST: 撮影・取得・送信をブラウザの接続状態から独立させる。
- MUST: 顔の有無と面積、8×8 の深度と target_status を毎秒 JSONL で送る。
- MUST: 起動 ID、シーケンス番号、ベッド ID、UTC 秒と各センサーの観測時刻を付ける。
- MUST: 正常な未検出と取得エラーを区別し、取得エラーは状態と `null` で表す。
- MUST: ULD の配列順序と status を維持し、品質が不足する値を正常値へ置き換えない。
- MUST: 最新の観測だけを RAM で共有し、マイコンへセンサーデータを永続保存しない。
- MUST: 送信待ちの観測を蓄積せず、期限切れ・設定変更前の結果を現在の状態へ適用しない。
- MUST: JPEG をタスク間で共有する処理を残す場合は、共有バッファ上限 96KiB を守る。

| センサー接続 | 割当・条件 |
| --- | --- |
| SCL / SDA | GPIO21 / GPIO14、I2C1、400kHz、7bit アドレス 0x29 |
| IO4 / INT | GPIO47（省略可）。現行は I²C ポーリング。カメラの GPIO4 / GPIO5 と混同しない |
| 電源 | 3V3 / GND。測距時約 100mA とカメラ・Wi-Fi 同時動作の余裕を実測する |
| 測距 | 8×8、5Hz、最も強い対象 1 個。最新結果を毎秒送る |
| status | `5` を有効とし、`6/9` とその他の状態を区別する。`255` は対象未検出 |

実物の基板型式、端子、カメラと深度マップの向きは実機で照合する。
カメラの SCCB は GPIO4 / GPIO5、新 I²C ドライバの I2C0 に固定し、深度の I2C1 と分離する。競合する既存設定はビルド時に拒否する。移行手順は [SETUP](../../current/SETUP.md#21-深度センサー) に従う。
完全な JSONL と入力制約は [仕様](../../current/SPECIFICATIONS.md) に従う。

### 受付・保存・判定

- MUST: `/v1/measurements` は `application/x-ndjson`、末尾改行付き、最大 16KiB とする。
- MUST: 機器別キー、許可ベッド、入力の型・配列・値・時刻を保存前に検証する。
- MUST: 同じベッド・起動 ID・番号の同一観測は重複保存しない。内容の競合は拒否する。
- MUST: DB 保存成功後にだけ受領成功を返し、保存失敗を成功にしない。
- MUST: 保存期間は初期値 6 時間、全ベッド合計は 100,000,000 bytes 以下とする。
- MUST: DB 本体・索引・ジャーナルを合算し、容量不足では古い観測から削除する。
- MUST: 期限切れを判定から除外し、1 秒周期で保守する。時計巻戻しで保持時間を延ばさない。
- MUST: SQLite は単一 writer、rollback journal、`synchronous=EXTRA` を使う。
- MUST: DB 本体は予算の 48% 以下とし、ジャーナルの余裕を確保する。
- MUST: 機器数は 1〜64 とし、Jev 待機中に DB mutex を保持しない。
- MUST: Jev の未設定・障害中も観測の受付・保存と他ベッドの処理を継続する。
- MUST: Jev の質問は期間末尾の胴体と支持面の関係を対象とし、顔の有無だけで姿勢を決めない。
- MUST: `prone / not_prone / unjudgeable` の 3 状態で扱う。
- MUST: confidence 0.9 未満、欠測、通信失敗、不正・古い応答を非うつ伏せへ変換しない。
- MUST: 判定不能を、うつ伏せ確率が低いという理由だけで決めない。
- MUST: 品質不足では理由を記録し、Jev を呼ばない。後続周期で品質を満たせば再開する。
- MUST: Jev の認証情報は `JEV_API_KEY` から取得し、入力からベッド ID・起動 ID・秘密を除く。
- MUST: 判定記録は SQLite とログで期間・判定時刻を明示し、過去値を現在の状態として扱わない。

| 項目 | 値・動作 |
| --- | --- |
| マイコンの送信周期 / 全体期限 | 1 秒 / DNS・接続・送受信を含めて 5 秒、同時送信 1 件 |
| 時刻設定 | 初期登録は認証済み USB でエッジの OS 時刻を取得する。通常運用は起動時と 86,400 秒ごとに NTP 同期する |
| Jev 周期 / 対象期間 / 全体期限 | 180 秒 / 直近 180 秒 / 30 秒。同じ周期では再試行しない |
| 判定窓 | `[window_end-180, window_end)`。観測秒ごとに最大 1 件 |
| 有効観測 | 180 区間中 162 区間以上。深度に少なくとも 1 セルの status `5` が必要 |
| 連続欠測 / 最新観測の経過 | ともに 5 秒以下 |
| 到着遅延・未来方向の時刻ずれ | 5 秒以下 |
| カメラと深度の観測時刻差 | 1 秒以下 |
| Jev API / モデル / 質問の版 | `/v1/systemone` / `jev-1.14.0` / `sensor-posture-v1` |
| Jev 入力 | 数値 JSONL を JSON の `state` へ格納。最大 200,000 bytes |
| 長文ヘッダ | `TypeSafe-Danger-Zone-Long-Context: true` |

Jev の回答は型、選択肢、確率の範囲と総和、モデルとの対応を検証する。
入力上限の超過時は間引きや切捨てを行わず、理由を記録する。
confidence と実際の識別精度は区別する。品質値は実測前の初期値である。

### 安全な接続と設定

- MUST: お客様だけで初回導入、設定変更、登録解除、機器交換、復旧を完了できる。
- MUST: 製造側で固有 ID・登録コード・SRP verifier、eFuse と出荷時の保護を設定・検査する。
- MUST: Secure Boot v2、Flash Encryption の Release 設定、暗号化 NVS、JTAG と不要な ROM 書込み経路の制限を出荷前に有効化する。
- MUST: 署名付き更新、更新失敗時の復旧、脆弱な版への巻戻し防止を実装し、出荷時の保護を有効にして検証する。
- MUST: USB の物理受付と固有コードで Security 2 (SRP6a / AES-GCM) を確立する。
- MUST: 初期登録では Security 2 の認証後にエッジの OS 時刻を渡し、TLS による接続確認より前に機器の初期時刻を設定する。
- MUST: 初期登録の時刻設定に NTP の成功を要求しない。通常運用の NTP 同期失敗も成功扱いしない。
- MUST: 未認証の時刻情報で信頼先を変更しない。時刻不正で証明書を検証できない場合は接続を停止し、認証済み USB による復旧へ案内する。
- MUST: 登録済み機器は所有者認証で管理し、初期コードだけで上書きできないようにする。
- MUST: 登録は未確定・接続確認・確定を区別し、電源断後も同じ取引を安全に再開できる。
- MUST: 通常通信は TLS 1.2 以上と機器別 Bearer token を使う。
- MUST: CA、ホスト名、期限、登録状態・世代、許可ベッドを検証する。
- MUST: HTTP への切替、証明書検証の省略、自動リダイレクトを禁止する。
- MUST: mDNS は探索にだけ使い、IP 変更後も同じ相手を認証する。
- MUST: CA 秘密鍵をエッジ内だけに保管し、機器の管理 TLS 秘密鍵は機器内で生成する。
- MUST: 秘密を暗号化 NVS または実行ユーザ限定の領域へ保存し、ログ・HTML・取得 API へ返さない。
- MUST: エッジのローカル設定画面と機器側の管理 API を認証・入力検証で保護する。
- MUST: ローカル Web 画面を使う場合は loopback bind、Host・Origin 検証、CSRF 対策を行う。
- MUST: 証明書を自動更新し、キーの更新・失効、期限切れ、エッジ紛失を認証済みの手順で扱う。
- MUST: 旧 NVS を一括消去せず、新設定の保存後に不要な旧 Jev 設定だけを削除する。
- MUST: NVS 初期化失敗で自動消去せず、暗号化できなければ平文保存へ切り替えない。
- MUST: 秘密の空欄保存は既存値を維持し、明示削除では該当する通信を停止する。

受付期限、失敗回数、登録状態、更新・復旧手順は [接続設計](../../current/CONNECTION.md) に従う。
対応ネットワークは DHCP・mDNS と機器間通信が可能な同一 LAN とする。

### セットアップツール

セットアップツールはエッジアプリの機能として配布する。
設定処理と登録処理を共用し、別の鍵生成アプリやお客様向け CLI を増やさない。

| 画面・処理 | 必須動作 |
| --- | --- |
| 起動 | OS のアプリ起動操作から設定画面を開く。SDK・Python・Rust・OpenSSL の別途導入を要求しない |
| 機器選択 | USB 機器を検出し、物理操作とコード認証後に機器 ID を照合する |
| 入力 | Wi-Fi、ベッド ID、設置情報だけを入力する。パスワードとコードを露出させない |
| 自動設定 | CA・証明書・機器別キー・保存先・送信先を用意し、認証済み USB で設定する |
| 接続確認 | USB、Wi-Fi、探索、時刻、TLS、エッジ認証、登録確定を段階ごとに確認する |
| 完了 | 双方の永続化と確定を確認してから「接続完了」を表示する |
| 再実行 | 登録済み状態を読み、追加・変更・復旧を選べる。既存の鍵・観測・無関係な機器設定を上書きしない |
| 中断・失敗 | セッション鍵を破棄し、保存済みの登録状態から再開・取消しを案内する。途中の成功を全体成功と表示しない |
| 運用 | Wi-Fi 変更、証明書更新の案内、失効、交換、登録解除、カードを使った復旧を同じ画面で扱う |

お客様に固定 IP、送信先 URL、証明書ファイル、認証キー、eFuse 操作を要求しない。
製造時の不可逆操作をツールの通常処理へ含めない。
既存の `tools/setup.py` と `tools/provision_device.py` は開発者向け試作用とし、商品用の登録経路には使わない。
設置情報が未測定の場合は未確認として保存し、測定済みに補完しない。

## Tasks

### Task 1: 共有契約と責務の確定

- Status: センサー構成の契約は定義済み。接続とツールの詳細は本書と接続設計に定義。
- Objective: 送受信・保存・判定・登録の境界をそろえる。
- Scope: 要件、仕様、設計、ADR、共通 JSONL 例。
- Acceptance Criteria: 正常・欠測・不正入力と、認証・登録・保存・判定の責務が一致する。
- Verification: 本書と正本文書、C が生成した共通例、Rust の入力検証を照合する。

### Task 2: エッジの受付と保存

- Status: 実装・自動検証済み。Debian・実機・長時間運転は未検証。
- Objective: 許可した機器の観測だけを保存する。
- Scope: `edge/src/config.rs`、`lib.rs`、`store.rs`、`measurement.rs`、関連テスト。
- Acceptance Criteria: 認証とベッド分離、入力検証、保存成功後の受領、重複・競合、6 時間・100MB を満たす。
- Verification: 不正認証・過大本文・保存失敗・容量境界・時計巻戻し・再起動を検証する。

### Task 3: マイコンの取得・設定・送信

- Status: 実装・自動検証済み。配線・実機周期・NVS 復元・24 時間同期は未検証。
- Objective: 最新の顔検知と深度を毎秒送る。
- Scope: `main/main.c`、`prone_inference_bridge.*`、`depth_sensor.*`、`telemetry.*`、`monitor_settings*`、`edge_sender.*`。
- Acceptance Criteria: VGA 撮影、320×240 の顔矩形、欠測表現、暗号化保存、5 秒期限、設定世代の整合を満たす。
- Verification: C の境界テスト、ESP-IDF ビルドと、実機の配線・電源・方向・切断復旧・再起動を確認する。

### Task 4: エッジの Jev 判定

- Status: 実装・自動検証済み。実 API の入力長・期限・識別精度は未検証。
- Objective: 品質を満たす直近 3 分の観測から、検証可能な 3 状態の結果を得る。
- Scope: `edge/src/jev.rs`、`measurement.rs`、`lib.rs`、関連テスト。
- Acceptance Criteria: 不足時の停止・再開、30 秒期限、確率・回答検証、他ベッドの継続、古い応答の不適用を満たす。
- Verification: 制御可能な時刻と応答で境界を検証し、180 秒分の入力を実 API でも確認する。

### Task 5: センサー構成の結合評価

- Status: ローカル HTTPS 結合検証済み。実機・実 API・Debian・精度評価は未検証。
- Objective: 見逃し、誤警報、判定不能、遅延と運用限界を測る。
- Scope: 実機の連続運転、姿勢・設置・遮蔽条件ごとの正解付き評価。
- Acceptance Criteria: 自動検証と実測を区別し、未測定の精度を合格にしない。
- Verification:
  - うつ伏せ・仰向け・横向き・不在、暗所、不鮮明、遮蔽、寝返り途中を比較する。
  - 顔が横向きのうつ伏せと、顔が隠れた仰向けを含める。
  - 質問調整用と最終評価用のセッションを分ける。
  - ブラウザを閉じた状態、Wi-Fi・センサー・DB・Jev の障害、6 時間・100MB の連続運転を確認する。

### Task 6: 登録・製造・配布の技術確認

- Status: 未実施。
- Objective: 必須の保護を維持したまま、両 OS で安全な登録を成立させる。
- Scope: Security 2 クライアント、USB transport、証明書生成、mDNS、出荷保護、署名付き更新、配布方法。
- Acceptance Criteria:
  - ESP-IDF 6.0.3 の公開 API と、保守されている暗号・I/O 実装を使用する。
  - USB Serial/JTAG と出荷時の JTAG・ROM 書込み制限が両立する。
  - 標準 console の UART・256-byte 制約に依存しない。
  - 採用依存の版と理由、機器固有の秘密の製造管理、更新・復旧の必要条件を確定する。
- Verification: 公式実装との相互運用と、出荷保護を有効にした実機の USB 操作を確認する。
- Stop condition: 保護と USB が両立しない場合は、安全性を弱めず設計を改訂する。

### Task 7: 製造準備と出荷時の保護

- Status: 未実装。Task 6 で確定した機器保護と USB の両立条件に従う。
- Objective: お客様が不可逆操作や秘密の書込みをせずに使える機器を製造する。
- Scope: 製造用の機器準備ツール、eFuse・ブート・フラッシュ・NVS の保護設定、登録カードの生成、出荷検査。
- Acceptance Criteria:
  - 機器固有の ID・登録コード・salt・SRP verifier を生成し、対応を検査する。コード原文を機器や製造ログに保存しない。
  - HMAC eFuse、暗号化 NVS、Flash Encryption の Release 設定、Secure Boot v2、JTAG と不要な ROM 書込み経路の制限を設定・検査する。
  - eFuse 書込み前に既存状態と用途を検査し、出荷準備をお客様用セットアップツールから実行できないようにする。
  - 製造時の ID・認証情報・保護設定は、登録解除やお客様の復旧で削除しない。
  - 出荷検査は保護設定の読戻しと認証付き USB 登録を含み、未検査・不合格の機器を出荷可能にしない。
- Verification: 出荷時の保護を有効にした実機で、固有情報の対応、秘密の非露出、USB 登録、デバッグ・不要な書込み経路の拒否を確認する。

### Task 8: 署名付きファームウェア更新と復旧

- Status: 未実装。Task 6 で確定した更新方式とパーティション構成に従う。
- Objective: 出荷後も安全に修正を配布し、失敗時に復旧できるようにする。
- Scope: ファームウェアの署名・配布、更新処理、起動確認、復旧、セキュリティ版による巻戻し防止。
- Acceptance Criteria:
  - 正当な署名を持つ更新だけを受け付け、改ざん・不正署名・禁止した旧セキュリティ版を拒否する。
  - 更新の中断・電源断・起動失敗から、許可された動作可能な版または復旧経路へ戻れる。
  - 復旧でも署名検証と巻戻し防止を省略せず、機器固有情報・登録状態・Wi-Fi 設定を無断で削除しない。
  - 署名用秘密鍵を機器、お客様用ツール、配布パッケージへ含めない。
- Verification: Task 7 の保護を有効にした実機で、正規更新、不正署名、改ざん、禁止版、更新中の電源断、起動失敗、復旧後の登録維持を確認する。

### Task 9: 認証付き登録と資格情報管理

- Status: 未実装。Task 6 の技術確認と依存の確定が先行する。
- Objective: 無断登録を拒否し、途中失敗から安全に復旧できる登録処理を作る。
- Scope: `device_security.*`、USB と物理受付、登録の永続化、エッジ認証、証明書更新 API、双方の mDNS。
- Acceptance Criteria:
  - 物理受付、Security 2、取引 ID、要求番号、登録世代、接続確認・確定・取消しを実装する。
  - 認証済み USB でエッジの OS 時刻を設定してから TLS 接続を確認し、未認証の時刻設定要求は拒否する。
  - 準備中キーは `/v1/pairing/check` だけで使え、観測保存と管理では拒否する。
  - 電源断・切断後も確定状態を照合でき、既存設定を無断で消去しない。
  - 証明書・キーの更新、即時の失効、交換・解除・復旧を扱う。
  - 商品用ビルドから旧 USB 平文 JSON を除去する。
- Verification: 誤コード、受付時間外、改ざん、再送、偽 mDNS、不正証明書、保存失敗、確定前後の電源断、再起動後の失効を検証する。NTP 未到達でも認証済み USB の時刻で初期登録できることと、証明書を検証できない時刻では接続を停止することを確認する。

### Task 10: セットアップツールと配布パッケージ

- Status: 未実装。現行の証明書生成スクリプトでは代替しない。
- Objective: お客様の導入と運用変更を、同じツールの画面だけで完結させる。
- Scope: エッジのローカル設定画面、Task 9 の登録処理との接続、macOS / Debian の配布・USB 権限設定・サービス導入。
- Acceptance Criteria:
  - Required Behavior のセットアップツールの全項目を満たす。
  - USB 機器の選択、コード認証、入力、進捗、接続確認、再開・取消し、完了を操作できる。
  - SDK・言語処理系・OpenSSL・固定 IP・秘密ファイル編集・OS の CA 登録を要求しない。
  - 再実行しても使用中のキーや無関係な設定を破壊しない。
  - ローカル管理を LAN へ公開せず、画面と診断へ秘密を返さない。
- Verification: 両 OS のクリーンな環境で導入し、初回成功・中断・誤入力・再実行・変更・交換・復旧を操作する。

### Task 11: お客様の単独操作とセキュリティの受入れ

- Status: 未実施。Task 7〜10 の実装・検証後に受け入れる。
- Objective: 開発者の代理操作なしで、安全な接続と復旧が完結することを確認する。
- Scope: [接続設計の受入条件](../../current/CONNECTION.md#7-保護する範囲と受入条件)全体。
- Acceptance Criteria: 初めて使うお客様が導入・設定変更・交換・期限切れやエッジ紛失からの復旧を完了できる。
- Verification: 自動テスト、出荷保護を有効にした実機の攻撃・障害試験、お客様の操作試験を分けて記録する。

## Open Questions

ユーザの追加回答を必要とする質問はない。
技術選定と評価は Task 5〜6 で実装側が担当する。
精度の実用上の合格基準は未設定であり、評価の実施だけで実用上の合格としない。
必要な追加センサー、画像送信、新しい依存、重大な設計変更が判明した場合は、変更範囲を具体化する。

## Validation

| 対象 | 確認状態 |
| --- | --- |
| ESP-IDF 6.0.3 ビルド | 成功済み |
| C の設定・URL・顔面積・JSONL・欠測 | ASan / UBSan を含む自動検証済み |
| 管理画面の設定とエラー表示 | 自動検証済み。実機ブラウザ操作は未確認 |
| Rust の認証・品質・重複・保存・時計巻戻し | macOS 27.2 / Rust 1.99.1 で自動検証済み |
| HTTPS リスナー | 認証、16KiB 制限、保存成功と保存失敗時の拒否を確認済み |
| 保存容量 | 縮小した 256KiB の予算で検証済み。実物の 100MB での連続運転は未確認 |
| Rust lint | `cargo clippy --locked --all-targets -- -D warnings` 成功済み |
| 開発者用セットアップスクリプト | `export.sh` による環境有効化から設定生成、ビルド・確認付き eFuse 準備・flash・USB 初期設定・エッジ起動・HTTPS 接続確認まで実行するよう更新。証明書・権限・既存ファイル保護、失敗時の停止、SDK 仮想 eFuse、実エッジの HTTPS 起動と停止、環境読み込みと Python の引継ぎを含む Python 35 テスト成功。SDK 未有効化の環境から実際の `export.sh` の読み込みと SDK Python・6.0.3・USB モジュールを確認済み。`build/setup/` の ESP-IDF 6.0.3 ビルド、USB/HMAC/パーティション設定、エッジビルド成功。実機の書込み・初期設定・送信と Debian は未確認 |
| Security 2 と USB transport | SDK の API と標準 console の制約を確認済み。実装・相互運用は未確認 |
| 製造準備・出荷保護・署名付き更新と復旧 | 未実装・未検証 |
| 認証済み USB による初期時刻設定 | 未実装・未検証 |
| 実機・実 API・Debian | 未確認。NVS 復元、配線、周期、通信期限、長時間運転、識別精度を含む |
| 商品用セットアップツールと単独導入 | 未実装・未検証 |

検証の成功は確認済みの対象に限定し、旧構成の結果を商品用登録の証拠にしない。
同一差分で成功済みの検証は、新しい変更や未解決の懸念がない限り繰り返さない。

## References

- [要件](../../current/REQUIREMENTS.md)
- [仕様](../../current/SPECIFICATIONS.md)
- [設計](../../current/DESIGN.md)
- [接続・登録設計](../../current/CONNECTION.md)
- [セットアップ手順](../../current/SETUP.md)
- [ADR-0001](../../adr/0001-edge-sensor-processing.md)
- [ADR-0002](../../adr/0002-customer-device-pairing.md)
- [共通 JSONL 例](../../../tests/fixtures/measurement.jsonl)
- [Issue #15](https://github.com/tkumata/prone-guard/issues/15)
- [Freenove 公式ピン図](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/ESP32S3_Pinout.png)
- [Conta VL53L5CX](https://www.switch-science.com/products/10281)
- [Conta 回路図](https://doc.switch-science.com/media/files/19d73541-d39a-4e3c-bf7a-69a2a56af50a.pdf)
- [ST ULD マニュアル](https://www.st.com/resource/en/user_manual/dm00797144-a-guide-to-using-the-vl53l5cx-multizone-timeofflight-ranging-sensor-with-wide-field-of-view-ultra-lite-driver-uld-stmicroelectronics.pdf)
- [TypeSafe System One API](https://docs.typesafe.ai/api/system-one)
- [TypeSafe Choice](https://docs.typesafe.ai/primitives/choice)
- [TypeSafe 入力長](https://docs.typesafe.ai/models)
- [ESP-IDF ESP-TLS](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/api-reference/protocols/esp_tls.html)
- [SQLite](https://www.sqlite.org/pragma.html)
