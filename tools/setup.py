#!/usr/bin/env python3
"""開発用の設定生成、ビルド、USB 初期設定、エッジ起動と接続確認を行う。"""
import argparse
import base64
from contextlib import closing
import getpass
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit
import warnings


def check_destination(destination):
    if destination.is_symlink() or (
        destination.exists()
        and (not destination.is_dir() or any(destination.iterdir()))
    ):
        raise ValueError(f"{destination} に既存ファイルがあります。上書きせず停止します。")


def read_ip(prompt):
    try:
        address = ipaddress.IPv4Address(input(prompt).strip())
    except ipaddress.AddressValueError:
        raise ValueError("IPv4 アドレスを指定してください。") from None
    if (address.is_unspecified or address.is_loopback
            or address.is_multicast or address.is_reserved):
        raise ValueError("設置先 LAN で使用する IPv4 アドレスを指定してください。")
    return str(address)


def read_settings():
    edge_ip = read_ip("エッジの IPv4 アドレス: ")
    device_ip = read_ip("デバイスの IPv4 アドレス (DHCP 予約先): ")
    if edge_ip == device_ip:
        raise ValueError("エッジとデバイスには別々の IP アドレスが必要です。")
    ssid = input("Wi-Fi SSID: ")
    if (not 1 <= len(ssid.encode("utf-8")) <= 32
            or any(ord(c) < 32 for c in ssid)):
        raise ValueError("SSID は制御文字を含まない UTF-8 の 1〜32 bytes で指定してください。")
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        password = getpass.getpass("Wi-Fi パスワード (非表示): ")
    if (not 8 <= len(password) <= 63
            or any(not 32 <= ord(c) <= 126 for c in password)):
        raise ValueError("Wi-Fi パスワードは ASCII の 8〜63 文字で指定してください。")
    bed_id = input("ベッド ID [bed-01]: ").strip() or "bed-01"
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", bed_id):
        raise ValueError("ベッド ID は英数字と _ . - の 1〜64 文字で指定してください。")
    camera_view = input("カメラの設置方向 [未確認]: ").strip() or "未確認"
    depth_order = input("深度マップの向き [未確認]: ").strip() or "未確認"
    for direction in (camera_view, depth_order):
        if (len(direction.encode("utf-8")) > 512
                or any(ord(c) < 32 for c in direction)):
            raise ValueError("設置方向は制御文字を含まない UTF-8 の 512 bytes 以下で指定してください。")
    return edge_ip, device_ip, {
        "wifi_ssid": ssid,
        "wifi_password": password,
        "bed_id": bed_id,
        "destination_url": f"https://{edge_ip}:8443/v1/measurements",
        "device_key": secrets.token_hex(32),
        "clear_device_key": False,
    }, {
        "camera_view": camera_view,
        "depth_map_order": depth_order,
        "empty_bed_distance_mm": None,
    }


def openssl_run(openssl, directory, *arguments):
    try:
        subprocess.run(
            [openssl, *arguments], cwd=directory, check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        # 子プロセスの出力を転記せず、秘密が診断へ混入する経路を避ける。
        raise RuntimeError(
            f"OpenSSL の {arguments[0]} が失敗しました。生成物は配置していません。"
        ) from None


def generate(root, openssl, edge_ip, device_ip, settings, installation):
    destination = root / "edge/local"
    check_destination(destination)
    edge = json.loads(
        (root / "edge/config.example.json").read_text(encoding="utf-8")
    )
    edge["devices"] = [{
        "bed_id": settings["bed_id"],
        "token_sha256": hashlib.sha256(
            settings["device_key"].encode("utf-8")
        ).hexdigest(),
        "installation": installation,
    }]
    device = {
        "server_certificate": "device.pem",
        "server_private_key": "device-key.pem",
        "edge_ca": "ca.pem",
        "admin_password": secrets.token_hex(32),
        "settings": settings,
    }
    # 一時ディレクトリは 0700。全ファイルの生成成功後に一括で配置する。
    with tempfile.TemporaryDirectory(
        prefix=".setup-", dir=root / "edge"
    ) as temporary:
        stage = Path(temporary)
        openssl_run(openssl, stage, "ecparam", "-name", "prime256v1",
                    "-genkey", "-noout", "-out", "ca-key.pem")
        openssl_run(openssl, stage, "req", "-new", "-x509", "-key",
                    "ca-key.pem", "-out", "ca.pem",
                    "-days", "3650", "-subj", "/CN=prone-guard-local-ca",
                    "-addext", "basicConstraints=critical,CA:TRUE",
                    "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        for name, address in (("edge", edge_ip), ("device", device_ip)):
            openssl_run(openssl, stage, "ecparam", "-name", "prime256v1",
                        "-genkey", "-noout", "-out", f"{name}-key.pem")
            openssl_run(openssl, stage, "req", "-new", "-key",
                        f"{name}-key.pem", "-out", f"{name}.csr",
                        "-subj", f"/CN=prone-guard-{name}")
            (stage / f"{name}.ext").write_text(
                f"subjectAltName=IP:{address}\n"
                "basicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature\n"
                "extendedKeyUsage=serverAuth\n"
                "subjectKeyIdentifier=hash\n"
                "authorityKeyIdentifier=keyid,issuer\n", encoding="ascii",
            )
            openssl_run(openssl, stage, "x509", "-req", "-in", f"{name}.csr",
                        "-CA", "ca.pem", "-CAkey", "ca-key.pem", "-set_serial",
                        str(secrets.randbits(159) | 1), "-out", f"{name}.pem",
                        "-days", "365", "-extfile", f"{name}.ext")
            openssl_run(openssl, stage, "verify", "-CAfile", "ca.pem",
                        "-purpose", "sslserver",
                        "-verify_ip", address, f"{name}.pem")
            (stage / f"{name}.csr").unlink()
            (stage / f"{name}.ext").unlink()
        for name, config in (("config.json", edge), ("device.json", device)):
            (stage / name).write_text(
                json.dumps(config, indent=2, ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
        for path in stage.iterdir():
            path.chmod(0o600)
        check_destination(destination)
        os.rename(stage, destination)


def setup_registration(root):
    destination = root / "edge/registration-local"
    check_destination(destination)
    print(f"リポジトリ: {root}\n生成先: {destination}")
    print("登録管理エッジの開発用設定を作成します。商品用の USB 登録と設定画面は未実装です。")
    print("作成するのは config.json だけです。CA・証明書・秘密鍵はエッジの初回起動時に生成されます。")
    print("この設定生成では既存の edge/local/、実機、OS の設定を変更しません。")
    if input("上記のファイルを作成しますか？ [y/N]: ").strip().lower() != "y":
        print("キャンセルしました。ファイルは作成していません。")
        return False
    config = {
        "listen": "0.0.0.0:8443",
        "database": "edge/registration-local/sensors.sqlite",
        "registrations": "edge/registration-local/registrations.sqlite",
        "retention_seconds": 21600,
        "devices": [],
    }
    with tempfile.TemporaryDirectory(
        prefix=".setup-", dir=root / "edge"
    ) as temporary:
        stage = Path(temporary)
        path = stage / "config.json"
        path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
        path.chmod(0o600)
        check_destination(destination)
        os.rename(stage, destination)
    print(f"作成しました: {destination / 'config.json'}")
    print("続いてビルドとエッジの起動確認を行います。商品用の USB 登録は行いません。")
    return True


def create_configuration(root):
    check_destination(root / "edge/local")
    openssl = shutil.which("openssl")
    if openssl is None:
        raise RuntimeError("OpenSSL が見つかりません。インストール後に再実行してください。")
    print(f"リポジトリ: {root}\n生成先: {root / 'edge/local'}")
    print("旧センサー試作用です。平文 USB 設定を使い、商品用の機器登録には使用しません。")
    print("設定生成は初回だけ行います。IP は DHCP 予約などで固定してください。")
    edge_ip, device_ip, settings, installation = read_settings()
    print("作成: ca.pem, ca-key.pem, edge.pem, edge-key.pem, device.pem, "
          "device-key.pem, config.json, device.json")
    print("秘密ファイルは 0600 で保存します。秘密の内容は表示しません。")
    print("まず設定を生成します。その後にビルド、確認付き実機書込み、エッジ起動・接続確認を行います。")
    if input("上記のファイルを作成しますか？ [y/N]: ").strip().lower() != "y":
        print("キャンセルしました。ファイルは作成していません。")
        return False
    generate(root, openssl, edge_ip, device_ip, settings, installation)
    print(f"作成しました: {root / 'edge/local'}")
    print("管理画面: https://" + device_ip
          + " (ユーザ名 admin、パスワードは device.json に保存)")
    return True


def run(root, arguments, *, capture=False, timeout=1800):
    try:
        return subprocess.run(
            arguments, cwd=root, check=True, timeout=timeout,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None, text=True,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        # 捕捉した出力には eFuse や秘密情報が含まれ得るため転記しない。
        raise RuntimeError(
            f"処理が失敗またはタイムアウトしました: {shlex.join(arguments)}"
        ) from None


def activate_idf(root):
    if (os.environ.get("IDF_PATH")
            and os.environ.get("_PRONE_GUARD_IDF_ACTIVE")
            == os.environ["IDF_PATH"]):
        return
    environment = os.environ.copy()
    sdk = environment.get("IDF_PATH")
    activation = Path.home() / ".espressif/tools/activate_idf_v6.0.3.sh"
    if sdk:
        location = Path(sdk).expanduser().resolve()
    elif activation.is_file():
        exported = run(root, ["bash", str(activation), "-e"],
                       capture=True, timeout=30).stdout
        values = dict(line.split("=", 1)
                      for line in exported.splitlines() if "=" in line)
        environment.update({name: values[name] for name in (
            "IDF_PATH", "IDF_TOOLS_PATH", "IDF_PYTHON_ENV_PATH",
        )})
        location = Path(environment["IDF_PATH"])
    else:
        location = Path(input(
            "ESP-IDF 6.0.3 のディレクトリ（export.sh がある場所）: "
        ).strip()).expanduser().resolve()
    if not (location / "export.sh").is_file():
        raise ValueError(f"{location} に export.sh が見つかりません。")
    tools = Path(environment.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    # EIM は tools 自体を指すが、SDK の export.sh は tools を含む親ディレクトリを使う。
    if tools.name == "tools" and (tools / "esp-rom-elfs").is_dir():
        environment["IDF_TOOLS_PATH"] = str(tools.parent)
    if environment.get("IDF_PYTHON_ENV_PATH"):
        environment["PATH"] = (
            str(Path(environment["IDF_PYTHON_ENV_PATH"]) / "bin")
            + os.pathsep + environment.get("PATH", "")
        )
    print("ESP-IDF の export.sh を読み込み、SDK の Python 環境でセットアップを続行します。",
          flush=True)
    # source の環境と Python を引き継ぐため、シェル内で読み込んでから同じスクリプトを exec する。
    script = '''set -e
export IDF_PATH="$1"
source "$1/export.sh"
export _PRONE_GUARD_IDF_ACTIVE="$IDF_PATH"
shift
exec python3 "$@"
'''
    os.execvpe("bash", ["bash", "-c", script, "bash", str(location),
                        str(root / "tools/setup.py"), *sys.argv[1:]],
               environment)


def preflight(root, registration):
    if not registration:
        if " " in str(root):
            raise ValueError("ESP-IDF のビルドには空白を含まないリポジトリパスを使用してください。")
        activate_idf(root)
    if shutil.which("cargo") is None:
        raise RuntimeError("cargo が見つかりません。Rust と rustup を導入してください。")
    version = run(root, ["rustc", "--version"],
                  capture=True, timeout=30).stdout
    if not version.startswith("rustc 1.99.1 "):
        raise RuntimeError("Rust 1.99.1 を選択してください。rustup で導入した場合は "
                           "RUSTUP_TOOLCHAIN=1.99.1 を設定して再実行できます。")
    run(root, ["cargo", "--version"], capture=True, timeout=30)
    if registration:
        return None
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise RuntimeError(
            "export.sh の実行後に IDF_PATH が設定されていません。SDK の導入状態を確認してください。"
        )
    idf = [sys.executable, str(Path(idf_path) / "tools/idf.py")]
    version = run(root, [*idf, "--version"],
                  capture=True, timeout=30).stdout.strip()
    if not re.fullmatch(r"ESP-IDF v6\.0\.3(?:-.*)?", version):
        raise RuntimeError("ESP-IDF 6.0.3 の環境が必要です。")
    run(root, [sys.executable, "-c", "import serial, esptool, espefuse"],
        capture=True, timeout=30)
    if shutil.which("openssl") is None:
        raise RuntimeError("OpenSSL が見つかりません。")
    return idf


def choose_port(port=None):
    from serial.tools.list_ports import comports
    ports = list(comports())
    if not ports:
        raise RuntimeError("USB ポートが見つかりません。データ通信対応ケーブルで接続してください。")
    for item in ports:
        print(f"  {item.device}: {item.description}")
    selected = port or input("接続した ESP32-S3 の USB ポート: ").strip()
    for item in ports:
        if item.device == selected:
            return item
    raise ValueError("表示された USB ポートを指定してください。")


def wait_port(original, timeout=30):
    from serial.tools.list_ports import comports
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for item in comports():
            if ((original.serial_number
                 and item.serial_number == original.serial_number
                 and item.vid == original.vid and item.pid == original.pid)
                    or (not original.serial_number
                        and item.device == original.device)):
                return item.device
        time.sleep(0.5)
    raise TimeoutError("同じ実機の USB ポートが戻りません。接続とポートを確認して再実行してください。")


def efuse_summary(root, port):
    with tempfile.TemporaryDirectory(prefix="prone-efuse-") as directory:
        path = Path(directory) / "summary.json"
        run(root, [sys.executable, "-m", "espefuse", "--chip", "esp32s3",
                   "--port", port, "summary", "--format", "json", "--file",
                   str(path)], capture=True, timeout=60)
        return json.loads(path.read_text())


def hmac_state(summary):
    purpose, block = summary["KEY_PURPOSE_0"], summary["BLOCK_KEY0"]
    if purpose["value"] == "HMAC_UP":
        if block["readable"] and not any(bytes.fromhex(block["value"])):
            raise ValueError("KEY0 の用途は HMAC_UP ですがキーが空です。実機の状態を確認してください。")
        return "ready"
    if (purpose["value"] == "USER" and purpose["writeable"] is True
            and block["readable"] is True and block["writeable"] is True
            and not any(bytes.fromhex(block["value"]))):
        return "empty"
    # shortcut: 既定の KEY0 だけを扱う。別ブロック対応時はビルド設定との対応も追加する。
    raise ValueError("KEY0 は未使用ではありません。別用途のキーを変更せず停止します。")


def prepare_hmac(root, port):
    summary = efuse_summary(root, port)
    mac = summary["MAC"]["value"]
    state = hmac_state(summary)
    print(f"対象: ESP32-S3 / USB {port} / MAC {mac} / KEY0: {state}")
    if state == "ready":
        return
    print("KEY0 に HMAC_UP キーを書き込みます。この変更は不可逆です。")
    print("キーは edge/local/nvs-hmac.bin に保存し、既存ファイルがあれば再利用します。")
    confirmation = f"BURN {mac}"
    if input(f"実行する場合は {confirmation} と入力: ").strip() != confirmation:
        raise RuntimeError("eFuse 書込みをキャンセルしました。実機へは書き込んでいません。")
    current = efuse_summary(root, port)
    if current["MAC"]["value"] != mac or hmac_state(current) != "empty":
        raise RuntimeError("確認中に対象または eFuse の状態が変わりました。書き込まず停止します。")
    key = root / "edge/local/nvs-hmac.bin"
    if key.is_symlink():
        raise ValueError("HMAC キーにシンボリックリンクは使用できません。")
    if key.exists():
        if key.stat().st_mode & 0o077 or len(key.read_bytes()) != 32:
            raise ValueError("既存の HMAC キーは 32 bytes、権限 0600 である必要があります。")
    else:
        with key.open("xb") as file:
            os.fchmod(file.fileno(), 0o600)
            file.write(secrets.token_bytes(32))
    run(root, [sys.executable, "-m", "espefuse", "--chip", "esp32s3",
               "--port", port, "--do-not-confirm", "burn-key", "BLOCK_KEY0",
               str(key), "HMAC_UP"],
        capture=True, timeout=120)
    verified = efuse_summary(root, port)
    if verified["MAC"]["value"] != mac or hmac_state(verified) != "ready":
        raise RuntimeError(
            "eFuse の書込みを確認できません。再書込みせず実機の状態を確認してください。"
        )
    print("HMAC_UP キーを確認しました。nvs-hmac.bin のバックアップを保管してください。")


def load_configuration(root):
    local = root / "edge/local"
    if local.is_symlink() or local.stat().st_mode & 0o077:
        raise ValueError("edge/local/ は権限 0700 の実ディレクトリである必要があります。")
    for name in ("config.json", "device.json", "ca.pem", "edge.pem",
                 "edge-key.pem", "device.pem", "device-key.pem"):
        path = local / name
        if path.is_symlink() or path.stat().st_mode & 0o077:
            raise ValueError(f"{path} は権限 0600 の実ファイルである必要があります。")
    config = json.loads((local / "config.json").read_text())
    device = json.loads((local / "device.json").read_text())
    if (device["server_certificate"] != "device.pem"
            or device["server_private_key"] != "device-key.pem"
            or device["edge_ca"] != "ca.pem"
            or not re.fullmatch(r"[0-9a-f]{64}", device["admin_password"])
            or not re.fullmatch(
                r"[0-9a-f]{64}", device["settings"]["device_key"])):
        raise ValueError("device.json の証明書参照または認証情報が不正です。")
    target = urlsplit(device["settings"]["destination_url"])
    edge_ip = str(ipaddress.IPv4Address(target.hostname))
    if (target.scheme != "https" or target.port != 8443
            or target.path != "/v1/measurements"):
        raise ValueError(
            "送信先はこのエッジの https://IPv4:8443/v1/measurements にしてください。"
        )
    if (config.get("registrations")
            or config["listen"] != "0.0.0.0:8443"
            or config["tls_certificate"] != "edge/local/edge.pem"
            or config["tls_private_key"] != "edge/local/edge-key.pem"):
        raise ValueError("edge/local/config.json は通常セットアップ用の設定ではありません。")
    expected = hashlib.sha256(
        device["settings"]["device_key"].encode()
    ).hexdigest()
    if not any(item["bed_id"] == device["settings"]["bed_id"]
               and item["token_sha256"] == expected
               for item in config["devices"]):
        raise ValueError("エッジとデバイスのベッド ID または機器別キーが一致しません。")
    san = run(root, ["openssl", "x509", "-in", str(local / "device.pem"),
                     "-noout", "-ext", "subjectAltName"],
              capture=True, timeout=30).stdout
    addresses = re.findall(r"IP Address:([0-9.]+)", san)
    if len(addresses) != 1:
        raise ValueError("デバイス証明書には IPv4 アドレスを 1 個指定してください。")
    device_ip = str(ipaddress.IPv4Address(addresses[0]))
    for name, address in (("edge", edge_ip), ("device", device_ip)):
        openssl_run("openssl", local, "verify", "-x509_strict", "-CAfile",
                    "ca.pem", "-purpose", "sslserver",
                    "-verify_ip", address, f"{name}.pem")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(local / f"{name}.pem",
                                local / f"{name}-key.pem")
    with socket.socket() as listener:
        listener.bind((edge_ip, 8443))
    return device, edge_ip, device_ip


def get_health(address, context, authorization=None, hostname=None):
    host, port = address
    connection = http.client.HTTPSConnection(
        host, port, context=context, timeout=3,
    )
    try:
        if hostname:
            connection.sock = context.wrap_socket(
                socket.create_connection(address, timeout=3),
                server_hostname=hostname,
            )
        connection.request("GET", "/health",
                           headers={"Authorization": authorization}
                           if authorization else {})
        response = connection.getresponse()
        body = response.read(16385)
        if response.status != 200 or len(body) > 16384:
            raise RuntimeError("HTTPS の正常応答を確認できません。")
        health = json.loads(body)
        if not isinstance(health, dict):
            raise ValueError("HTTPS の状態応答がオブジェクトではありません。")
        return health
    finally:
        connection.close()


def verify_build(build):
    required = {"CONFIG_IDF_TARGET=\"esp32s3\"", "CONFIG_NVS_ENCRYPTION=y",
                "CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y",
                "CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0",
                "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
                "CONFIG_PARTITION_TABLE_CUSTOM=y",
                "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions.csv\""}
    actual = set((build / "sdkconfig").read_text().splitlines())
    if not required.issubset(actual):
        raise ValueError("build/setup/sdkconfig の USB・暗号化 NVS・パーティション設定が異なります。"
                         "実機へ書き込まず停止します。")


def wait_health(process, probe, ready, timeout):
    deadline = time.monotonic() + timeout
    last = "応答なし"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"エッジが接続確認前に終了しました（終了コード {process.returncode}）。"
            )
        try:
            health = probe()
            if ready(health):
                return
            last = "状態が準備完了になっていません"
        except (OSError, ValueError, RuntimeError, sqlite3.Error,
                http.client.HTTPException):
            last = "HTTPS 接続または応答を確認できません"
        time.sleep(1)
    raise TimeoutError(
        f"接続確認がタイムアウトしました: {last}。"
        "USB、Wi-Fi、IP、時刻、センサー、TCP 8443 を確認してください。"
    )


def start_edge(root, configuration, device=None, edge_ip=None, device_ip=None):
    if not os.environ.get("JEV_API_KEY"):
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            key = getpass.getpass("Jev API key（非表示、観測の保存だけなら空欄）: ")
        if key:
            if len(key) > 512 or any(not 33 <= ord(c) <= 126 for c in key):
                raise ValueError(
                    "Jev API key は ASCII の表示文字 1〜512 文字で指定してください。"
                )
            os.environ["JEV_API_KEY"] = key
    print("エッジを起動します。Ctrl-C でエッジも終了します。")
    process = subprocess.Popen(
        [str(root / "edge/target/debug/prone-guard-edge"), str(configuration)],
        cwd=root,
    )
    try:
        if device is None:
            def probe():
                with closing(sqlite3.connect(
                    (root / "edge/registration-local/registrations.sqlite")
                    .as_uri() + "?mode=ro", uri=True,
                )) as database:
                    row = database.execute(
                        "SELECT data FROM registration_metadata "
                        "WHERE name='identity'"
                    ).fetchone()
                if row is None:
                    raise RuntimeError("登録管理エッジが証明書を準備中です。")
                identity = json.loads(row[0])
                return get_health(("127.0.0.1", 8443),
                                  ssl.create_default_context(
                                      cadata=identity["ca_certificate"]),
                                  hostname=f"pg-edge-{identity['id']}.local")
            wait_health(process, probe,
                        lambda health: health.get("receiver") == "ready", 30)
            print("登録管理エッジの HTTPS 起動を確認しました。商品用 USB 登録は未実装です。")
        else:
            context = ssl.create_default_context(
                cafile=str(root / "edge/local/ca.pem")
            )
            wait_health(process, lambda: get_health((edge_ip, 8443), context),
                        lambda health: health.get("receiver") == "ready", 30)
            credential = base64.b64encode(
                ("admin:" + device["admin_password"]).encode()
            ).decode()
            print("実機の Wi-Fi・時刻同期・センサー・観測送信を確認しています（最大 120 秒）。")
            expected = {"wifi": "connected", "clock": "synchronized",
                        "camera": "ok", "depth": "ok", "sender": "ok"}
            wait_health(process, lambda: get_health(
                (device_ip, 443), context, "Basic " + credential),
                lambda health: all(health.get(k) == v
                                   for k, v in expected.items()), 120)
            print("セットアップ完了: エッジの HTTPS 受付と実機のセンサー送信成功を確認しました。")
            print(f"管理画面: https://{device_ip}/ "
                  "（admin、パスワードは edge/local/device.json）")
            print("Jev の実 API 応答と識別精度は、この接続確認には含みません。")
        code = process.wait()
        if code:
            raise RuntimeError(f"エッジが異常終了しました（終了コード {code}）。")
    except KeyboardInterrupt:
        print("\nエッジを停止します。設定と鍵は保存されています。")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main(registration=False, port=None):
    root = Path(__file__).resolve().parents[1]
    idf = preflight(root, registration)
    if registration:
        destination = root / "edge/registration-local"
        if destination.is_symlink():
            raise ValueError("登録管理の設定先にシンボリックリンクは使用できません。")
        if not (destination / "config.json").exists():
            if not setup_registration(root):
                return
        else:
            print("既存の登録管理設定と DB を再利用します。")
        if (destination.stat().st_mode & 0o077
                or (destination / "config.json").is_symlink()
                or (destination / "config.json").stat().st_mode & 0o077):
            raise ValueError("登録管理設定は権限 0700 のディレクトリと 0600 の実ファイルで保存してください。")
        config = json.loads((destination / "config.json").read_text())
        if (config["listen"] != "0.0.0.0:8443"
                or config["registrations"]
                != "edge/registration-local/registrations.sqlite"):
            raise ValueError("登録管理設定の待受けまたは登録 DB の配置が異なります。")
        with socket.socket() as listener:
            listener.bind(("0.0.0.0", 8443))
        run(root, ["cargo", "build", "--manifest-path", "edge/Cargo.toml",
                   "--locked"])
        start_edge(root, destination / "config.json")
        return
    local = root / "edge/local"
    if local.is_symlink():
        raise ValueError("edge/local/ にシンボリックリンクは使用できません。")
    selected = choose_port(port)
    if (local / "config.json").exists():
        print("既存の鍵と設定を再利用します。")
    elif not create_configuration(root):
        return
    device, edge_ip, device_ip = load_configuration(root)
    build = root / "build/setup"
    build.mkdir(parents=True, exist_ok=True)
    idf = [*idf, "-B", str(build), f"-DSDKCONFIG={build / 'sdkconfig'}",
           f"-DSDKCONFIG_DEFAULTS={root / 'sdkconfig.defaults'}",
           "-DIDF_TARGET=esp32s3"]
    print("ファームウェアとエッジをビルドします。既存の build/ と sdkconfig は再生成しません。")
    run(root, [*idf, "build"])
    verify_build(build)
    run(root, ["cargo", "build", "--manifest-path", "edge/Cargo.toml",
               "--locked"])
    print(f"{selected.device} のファームウェアと USB 設定を書き換えます。")
    print("独自に旧 storage 領域を使っていた場合は先に退避してください。")
    if input("この実機へ書き込みますか？ [y/N]: ").strip().lower() != "y":
        raise RuntimeError("flash をキャンセルしました。設定と鍵は保持しています。")
    prepare_hmac(root, selected.device)
    run(root, [*idf, "-p", selected.device, "flash"])
    actual_port = wait_port(selected)
    time.sleep(2)
    run(root, [sys.executable, str(root / "tools/provision_device.py"),
               "--port", actual_port, "--config", str(local / "device.json")],
        capture=True, timeout=60)
    start_edge(root, local / "config.json", device, edge_ip, device_ip)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registration", action="store_true",
                        help="登録管理エッジを設定・ビルド・起動する（商品用 USB 登録は未実装）")
    parser.add_argument("--port", help="ESP32-S3 の USB ポート。省略時は一覧から対話入力する")
    args = parser.parse_args()
    try:
        if args.registration and args.port:
            raise ValueError("--registration は USB 登録を行わないため --port は指定できません。")
        main(registration=args.registration, port=args.port)
    except getpass.GetPassWarning:
        print("セットアップ失敗: パスワードを非表示で入力できるターミナルから実行してください。",
              file=sys.stderr)
        sys.exit(1)
    except (OSError, ValueError, KeyError, RuntimeError,
            sqlite3.Error) as error:
        print(f"セットアップ失敗: {error}", file=sys.stderr)
        sys.exit(1)
    except (EOFError, KeyboardInterrupt):
        print("\n入力を中断しました。", file=sys.stderr)
        sys.exit(1)
