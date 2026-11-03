#!/usr/bin/env python3
"""Send per-device credentials over USB; never print the provisioning payload."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import time


def private_file(path):
    if os.stat(path).st_mode & 0o077:
        raise ValueError(f"secret file must have mode 0600: {path}")
    return Path(path).read_text(encoding="utf-8")


def payload(path):
    path = Path(path)
    config = json.loads(private_file(path))
    if not re.fullmatch(r"[0-9a-f]{64}", config["admin_password"]):
        raise ValueError("admin_password must be 64 lowercase hexadecimal characters")
    provision = {"admin_password": config["admin_password"], "settings": config["settings"]}
    for field in ("server_certificate", "server_private_key", "edge_ca"):
        file = path.parent / config[field]
        provision[field] = private_file(file) if field == "server_private_key" else file.read_text(encoding="utf-8")
    data = (json.dumps({"provision": provision}, separators=(",", ":"), ensure_ascii=True) + "\n").encode()
    if len(data) > 32768:
        raise ValueError("provisioning data exceeds 32KiB")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    data = payload(args.config)
    import serial  # Provided by the ESP-IDF Python environment.
    with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=10, dsrdtr=False) as port:
        port.dtr = False
        port.rts = False
        port.reset_input_buffer()
        port.write(data)
        port.flush()
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            line = port.readline()
            if b"PROVISION_OK" in line:
                print("Provisioning succeeded; device is restarting.")
                return
            if b"PROVISION_FAILED" in line:
                raise RuntimeError("device rejected provisioning; check certificate/key and settings")
        raise TimeoutError("device did not confirm provisioning; check HMAC key and USB connection")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, TimeoutError) as error:
        print(f"Provisioning failed: {error}", file=sys.stderr)
        sys.exit(1)
