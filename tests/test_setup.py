import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


PROJECT = Path(__file__).resolve().parents[1]


class Setup(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="prone setup ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo with spaces"
        (self.root / "tools").mkdir(parents=True)
        (self.root / "edge").mkdir()
        for name in ("tools/setup.py", "edge/config.example.json"):
            shutil.copyfile(PROJECT / name, self.root / name)
        spec = importlib.util.spec_from_file_location("setup_tool", self.root / "tools/setup.py")
        self.setup = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.setup)
        self.foreign = Path(self.temporary.name) / "unrelated"
        self.foreign.mkdir()

    def run_setup(self, confirm="y", camera="", depth=""):
        output = io.StringIO()
        answers = ["192.168.1.50", "192.168.1.51", "test Wi-Fi", "bed-02", camera, depth, confirm]
        with contextlib.chdir(self.foreign), contextlib.redirect_stdout(output), \
                patch("builtins.input", side_effect=answers), \
                patch.object(self.setup.getpass, "getpass", return_value="not-a-real-WiFi-secret"):
            self.setup.create_configuration(self.root)
        return output.getvalue()

    def test_generation_from_another_directory_and_existing_usb_format(self):
        with patch.object(self.setup.subprocess, "run", wraps=subprocess.run) as processes:
            output = self.run_setup(camera="ベッドの上方", depth="未確認")
        local = self.root / "edge/local"
        edge = json.loads((local / "config.json").read_text())
        device = json.loads((local / "device.json").read_text())
        settings = device["settings"]
        self.assertEqual(settings["destination_url"], "https://192.168.1.50:8443/v1/measurements")
        self.assertEqual(edge["devices"][0]["bed_id"], settings["bed_id"])
        self.assertEqual(edge["devices"][0]["token_sha256"],
                         self.setup.hashlib.sha256(settings["device_key"].encode()).hexdigest())
        self.assertEqual(edge["devices"][0]["installation"]["camera_view"], "ベッドの上方")
        self.assertIsNone(edge["devices"][0]["installation"]["empty_bed_distance_mm"])
        self.assertEqual(edge["retention_seconds"], 21600)
        self.assertEqual(edge["database"], "edge/local/sensors.sqlite")
        self.assertRegex(settings["device_key"], r"^[0-9a-f]{64}$")
        self.assertRegex(device["admin_password"], r"^[0-9a-f]{64}$")
        self.assertNotEqual(settings["device_key"], device["admin_password"])
        self.assertEqual(local.stat().st_mode & 0o777, 0o700)
        self.assertEqual({path.name for path in local.iterdir()}, {
            "ca.pem", "ca-key.pem", "edge.pem", "edge-key.pem", "device.pem", "device-key.pem",
            "config.json", "device.json",
        })
        for path in local.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(self.foreign.iterdir()), [])
        self.assertIn(str(local), output)
        arguments = repr(processes.call_args_list)
        for secret in (settings["wifi_password"], settings["device_key"], device["admin_password"],
                       (local / "ca-key.pem").read_text()):
            self.assertNotIn(secret, output)
            self.assertNotIn(secret, arguments)

        spec = importlib.util.spec_from_file_location("provision_tool", PROJECT / "tools/provision_device.py")
        provision = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(provision)
        payload = json.loads(provision.payload(local / "device.json"))["provision"]
        self.assertEqual(payload["settings"], settings)
        self.assertEqual(payload["server_private_key"], (local / "device-key.pem").read_text())
        for name, address in (("edge", "192.168.1.50"), ("device", "192.168.1.51")):
            verified = subprocess.run([
                "openssl", "verify", "-CAfile", str(local / "ca.pem"), "-purpose", "sslserver",
                "-verify_ip", address, str(local / f"{name}.pem"),
            ], capture_output=True)
            self.assertEqual(verified.returncode, 0, verified.stderr)
            mismatch = subprocess.run([
                "openssl", "verify", "-CAfile", str(local / "ca.pem"),
                "-verify_ip", "192.168.1.99", str(local / f"{name}.pem"),
            ], capture_output=True)
            self.assertNotEqual(mismatch.returncode, 0)
            certificate_key = subprocess.check_output([
                "openssl", "x509", "-in", str(local / f"{name}.pem"), "-pubkey", "-noout",
            ])
            private_key_public = subprocess.check_output([
                "openssl", "pkey", "-in", str(local / f"{name}-key.pem"), "-pubout",
            ])
            self.assertEqual(certificate_key, private_key_public)
        before = {path.name: path.read_bytes() for path in local.iterdir()}
        with self.assertRaisesRegex(ValueError, "既存ファイル"), patch("builtins.input") as prompt:
            self.setup.create_configuration(self.root)
        prompt.assert_not_called()
        self.assertEqual(before, {path.name: path.read_bytes() for path in local.iterdir()})

    def test_cancel_does_not_create_files(self):
        output = self.run_setup(confirm="")
        self.assertIn("キャンセル", output)
        self.assertEqual({p.name for p in (self.root / "edge").iterdir()}, {"config.example.json"})

    def test_registration_generation_keeps_existing_sensor_configuration(self):
        local = self.root / "edge/local"
        local.mkdir()
        marker = local / "keep.txt"
        marker.write_text("existing prototype credentials")
        with contextlib.redirect_stdout(io.StringIO()), patch("builtins.input", return_value="y"):
            self.assertTrue(self.setup.setup_registration(self.root))
        destination = self.root / "edge/registration-local"
        self.assertEqual({p.name for p in destination.iterdir()}, {"config.json"})
        config = json.loads((destination / "config.json").read_text())
        self.assertEqual(config, {
            "listen": "0.0.0.0:8443", "database": "edge/registration-local/sensors.sqlite",
            "registrations": "edge/registration-local/registrations.sqlite",
            "retention_seconds": 21600, "devices": [],
        })
        self.assertEqual(destination.stat().st_mode & 0o777, 0o700)
        self.assertEqual((destination / "config.json").stat().st_mode & 0o777, 0o600)
        self.assertEqual(marker.read_text(), "existing prototype credentials")
        self.assertEqual(list(self.foreign.iterdir()), [])
        before = (destination / "config.json").read_bytes()
        with self.assertRaisesRegex(ValueError, "既存ファイル"):
            self.setup.setup_registration(self.root)
        self.assertEqual((destination / "config.json").read_bytes(), before)

    def test_registration_cancel_and_symlink_leave_existing_files_untouched(self):
        destination = self.root / "edge/registration-local"
        with contextlib.redirect_stdout(io.StringIO()), patch("builtins.input", return_value=""):
            self.setup.setup_registration(self.root)
        self.assertFalse(destination.exists())
        destination.symlink_to(self.foreign, target_is_directory=True)
        with self.assertRaises(ValueError), patch("builtins.input") as prompt:
            self.setup.setup_registration(self.root)
        prompt.assert_not_called()
        self.assertEqual(list(self.foreign.iterdir()), [])

    def test_registration_partial_failure_does_not_publish_configuration(self):
        destination = self.root / "edge/registration-local"
        with contextlib.redirect_stdout(io.StringIO()), patch("builtins.input", return_value="y"), \
                patch.object(self.setup.os, "rename", side_effect=OSError("injected rename failure")), \
                self.assertRaisesRegex(OSError, "rename failure"):
            self.setup.setup_registration(self.root)
        self.assertFalse(destination.exists())
        self.assertEqual({p.name for p in (self.root / "edge").iterdir()}, {"config.example.json"})

    def test_partial_failure_cleans_only_own_files(self):
        marker = self.root / "edge/keep.txt"
        marker.write_text("unrelated")
        local = self.root / "edge/local"
        local.mkdir()
        original = self.setup.openssl_run

        def fail(openssl, directory, *arguments):
            if arguments[0] == "x509":
                raise RuntimeError("injected signing failure")
            return original(openssl, directory, *arguments)

        with patch.object(self.setup, "openssl_run", side_effect=fail), \
                self.assertRaisesRegex(RuntimeError, "signing failure"):
            self.run_setup()
        self.assertEqual(list(local.iterdir()), [])
        self.assertEqual(marker.read_text(), "unrelated")
        self.assertEqual({p.name for p in (self.root / "edge").iterdir()},
                         {"config.example.json", "local", "keep.txt"})

    def test_destination_created_during_generation_is_preserved(self):
        local = self.root / "edge/local"
        original = self.setup.openssl_run

        def create_existing(openssl, directory, *arguments):
            if not local.exists():
                local.mkdir()
                (local / "keep.txt").write_text("created by another process")
            return original(openssl, directory, *arguments)

        with patch.object(self.setup, "openssl_run", side_effect=create_existing), \
                self.assertRaisesRegex(ValueError, "既存ファイル"):
            self.run_setup()
        self.assertEqual((local / "keep.txt").read_text(), "created by another process")
        self.assertEqual({p.name for p in (self.root / "edge").iterdir()}, {"config.example.json", "local"})

    def test_symlink_is_not_followed(self):
        local = self.root / "edge/local"
        local.symlink_to(self.foreign, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.setup.create_configuration(self.root)
        self.assertTrue(local.is_symlink())
        self.assertEqual(list(self.foreign.iterdir()), [])

    def test_input_validation_stops_before_writing(self):
        cases = [
            (["invalid"], "valid-password"),
            (["127.0.0.1"], "valid-password"),
            (["192.168.1.50", "192.168.1.50"], "valid-password"),
            (["192.168.1.50", "192.168.1.51", "あ" * 11], "valid-password"),
            (["192.168.1.50", "192.168.1.51", "SSID"], "short"),
            (["192.168.1.50", "192.168.1.51", "SSID"], "あ" * 8),
            (["192.168.1.50", "192.168.1.51", "SSID", "bad/id"], "valid-password"),
        ]
        for answers, password in cases:
            with self.subTest(answers=answers), contextlib.redirect_stdout(io.StringIO()), \
                    patch("builtins.input", side_effect=answers), \
                    patch.object(self.setup.getpass, "getpass", return_value=password), \
                    self.assertRaises(ValueError):
                self.setup.create_configuration(self.root)
            self.assertFalse((self.root / "edge/local").exists())

    def test_openssl_error_output_is_not_exposed(self):
        secret = "SECRET_IN_CHILD_OUTPUT"
        with patch.object(self.setup.subprocess, "run", side_effect=subprocess.CalledProcessError(
            1, ["openssl"], stderr=secret.encode(),
        )), self.assertRaises(RuntimeError) as error:
            self.setup.openssl_run("openssl", self.foreign, "req")
        self.assertNotIn(secret, str(error.exception))

    def test_password_echo_fallback_is_rejected(self):
        def unavailable(prompt):
            self.setup.warnings.warn("no password echo control", self.setup.getpass.GetPassWarning)

        with patch("builtins.input", side_effect=["192.168.1.50", "192.168.1.51", "SSID"]), \
                patch.object(self.setup.getpass, "getpass", side_effect=unavailable), \
                self.assertRaises(self.setup.getpass.GetPassWarning):
            self.setup.read_settings()
        self.assertFalse((self.root / "edge/local").exists())

    def test_setup_runs_build_flash_provision_and_start_without_replacing_credentials(self):
        self.run_setup()
        local = self.root / "edge/local"
        before = {p.name: p.read_bytes() for p in local.iterdir()}
        events = []

        def command(root, arguments, **kwargs):
            if arguments[-1] == "build":
                events.append("firmware build")
            elif arguments[0] == "cargo":
                events.append("edge build")
            elif arguments[-1] == "flash":
                events.append("flash")
            elif "--config" in arguments:
                events.append("USB settings")
            else:
                raise AssertionError(arguments)
            self.assertEqual(root, self.root.resolve())

        selected = SimpleNamespace(device="test-usb")
        with contextlib.redirect_stdout(io.StringIO()), \
                patch.object(self.setup, "preflight", return_value=["idf.py"]), \
                patch.object(self.setup, "choose_port", return_value=selected), \
                patch.object(self.setup, "load_configuration", return_value=({}, "edge", "device")), \
                patch.object(self.setup, "run", side_effect=command), \
                patch.object(self.setup, "verify_build"), \
                patch.object(self.setup, "prepare_hmac", side_effect=lambda *_: events.append("HMAC check")), \
                patch.object(self.setup, "wait_port", return_value="reconnected-usb"), \
                patch.object(self.setup.time, "sleep"), \
                patch.object(self.setup, "start_edge", side_effect=lambda *_: events.append("start and verify")), \
                patch("builtins.input", return_value="y"):
            self.setup.main(port="test-usb")
        self.assertEqual(events, ["firmware build", "edge build", "HMAC check", "flash",
                                  "USB settings", "start and verify"])
        self.assertEqual(before, {p.name: p.read_bytes() for p in local.iterdir()})

    def test_build_or_usb_failure_stops_before_starting_edge(self):
        self.run_setup()
        for stage in ("build", "USB"):
            with self.subTest(stage=stage):
                def command(root, arguments, **kwargs):
                    if (stage == "build" and arguments[-1] == "build"
                            or stage == "USB" and "--config" in arguments):
                        raise RuntimeError(f"{stage} failed")

                with contextlib.redirect_stdout(io.StringIO()), \
                        patch.object(self.setup, "preflight", return_value=["idf.py"]), \
                        patch.object(self.setup, "choose_port", return_value=SimpleNamespace(device="usb")), \
                        patch.object(self.setup, "load_configuration", return_value=({}, "edge", "device")), \
                        patch.object(self.setup, "run", side_effect=command), \
                        patch.object(self.setup, "verify_build"), \
                        patch.object(self.setup, "prepare_hmac") as hmac, \
                        patch.object(self.setup, "wait_port", return_value="usb"), \
                        patch.object(self.setup.time, "sleep"), \
                        patch.object(self.setup, "start_edge") as start, \
                        patch("builtins.input", return_value="y"), \
                        self.assertRaisesRegex(RuntimeError, f"{stage} failed"):
                    self.setup.main()
                start.assert_not_called()
                if stage == "build":
                    hmac.assert_not_called()

    def test_declining_flash_does_not_burn_or_write_usb(self):
        self.run_setup()
        with contextlib.redirect_stdout(io.StringIO()), \
                patch.object(self.setup, "preflight", return_value=["idf.py"]), \
                patch.object(self.setup, "choose_port", return_value=SimpleNamespace(device="usb")), \
                patch.object(self.setup, "load_configuration", return_value=({}, "edge", "device")), \
                patch.object(self.setup, "run") as commands, \
                patch.object(self.setup, "verify_build"), \
                patch.object(self.setup, "prepare_hmac") as hmac, \
                patch.object(self.setup, "start_edge") as start, \
                patch("builtins.input", return_value=""), \
                self.assertRaisesRegex(RuntimeError, "flash をキャンセル"):
            self.setup.main()
        self.assertFalse(any(call.args[1][-1] == "flash" for call in commands.call_args_list))
        hmac.assert_not_called()
        start.assert_not_called()

    def test_missing_tools_fail_before_configuration_changes(self):
        result = subprocess.run([sys.executable, str(self.root / "tools/setup.py"), "--registration"],
                                input="y\n", capture_output=True, text=True, cwd=self.foreign,
                                env={**os.environ, "PATH": ""})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cargo が見つかりません", result.stderr)
        self.assertFalse((self.root / "edge/registration-local").exists())

    def bootstrap(self, *, eim=False, explicit=False, fail=False):
        sdk = self.root / 'sdk with spaces and $literal'
        sdk.mkdir()
        binary = sdk / "bin"
        binary.mkdir()
        (binary / "python3").write_text(
            '#!/bin/sh\nexport ACTIVATED_PYTHON=yes\n'
            f'exec {shlex.quote(sys.executable)} "$@"\n')
        (binary / "python3").chmod(0o755)
        trace = self.root / "activation.txt"
        (sdk / "export.sh").write_text(
            f'printf "export\\n" >> {shlex.quote(str(trace))}\n'
            + ('return 9\n' if fail else
               f'export PATH={shlex.quote(str(binary))}:"$PATH"\nexport EXPORT_RAN=yes\n'))
        home = self.root / "home"
        if eim:
            activation = home / ".espressif/tools/activate_idf_v6.0.3.sh"
            activation.parent.mkdir(parents=True)
            (activation.parent / "esp-rom-elfs").mkdir()
            values = [f"IDF_PATH={sdk}", f"IDF_TOOLS_PATH={activation.parent}", f"IDF_PYTHON_ENV_PATH={sdk}"]
            activation.write_text('[ "$1" = -e ] || exit 1\n'
                                  + "printf '%s\\n' " + " ".join(shlex.quote(v) for v in values) + "\n"
                                  + f'printf "eim\\n" >> {shlex.quote(str(trace))}\n')
        probe = self.root / "tools/setup.py"
        probe.write_text('''import json, os, sys
print(json.dumps({"sdk": os.environ["IDF_PATH"], "loaded": os.environ.get("EXPORT_RAN"),
                  "marker": os.environ.get("_PRONE_GUARD_IDF_ACTIVE"),
                  "interpreter": os.environ.get("ACTIVATED_PYTHON"), "arguments": sys.argv[1:]}))
''')
        driver = '''import importlib.util, sys
from pathlib import Path
source, root = sys.argv[1:3]
arguments = sys.argv[3:]
spec = importlib.util.spec_from_file_location("setup_bootstrap", source)
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)
sys.argv = [str(Path(root) / "tools/setup.py"), *arguments]
setup.activate_idf(Path(root))
'''
        environment = {k: v for k, v in os.environ.items()
                       if k not in ("IDF_PATH", "_PRONE_GUARD_IDF_ACTIVE")}
        environment["HOME"] = str(home)
        if explicit:
            environment["IDF_PATH"] = str(sdk)
        result = subprocess.run([sys.executable, "-c", driver, str(PROJECT / "tools/setup.py"),
                                 str(self.root), "--port", "/dev/test-usb"],
                                input=str(sdk) + "\n", text=True, capture_output=True,
                                cwd=self.foreign, env=environment, timeout=10)
        return result, sdk, trace

    def test_eim_bootstrap_sources_export_and_uses_activated_python_without_manual_source(self):
        result, sdk, trace = self.bootstrap(eim=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout.splitlines()[-1])
        self.assertEqual(Path(data["sdk"]).resolve(), sdk.resolve())
        self.assertEqual(Path(data["marker"]).resolve(), sdk.resolve())
        self.assertEqual(data["loaded"], "yes")
        self.assertEqual(data["interpreter"], "yes")
        self.assertEqual(data["arguments"], ["--port", "/dev/test-usb"])
        self.assertEqual(trace.read_text().splitlines(), ["eim", "export"])

    def test_explicit_sdk_also_sources_export(self):
        result, sdk, trace = self.bootstrap(explicit=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout.splitlines()[-1])["loaded"], "yes")
        self.assertEqual(trace.read_text().splitlines(), ["export"])

    def test_custom_installation_can_be_entered_without_source_commands(self):
        result, sdk, trace = self.bootstrap()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(json.loads(result.stdout.splitlines()[-1])["sdk"]).resolve(), sdk.resolve())
        self.assertEqual(trace.read_text().splitlines(), ["export"])

    def test_failed_export_stops_before_restarting_setup(self):
        result, _, trace = self.bootstrap(explicit=True, fail=True)
        self.assertEqual(result.returncode, 9)
        self.assertNotIn('"loaded"', result.stdout)
        self.assertEqual(trace.read_text().splitlines(), ["export"])

    def test_activated_setup_does_not_reexec_and_registration_needs_no_sdk(self):
        with patch.dict(os.environ, {"IDF_PATH": "/sdk", "_PRONE_GUARD_IDF_ACTIVE": "/sdk"}), \
                patch.object(self.setup.os, "execvpe") as execute:
            self.setup.activate_idf(self.root)
        execute.assert_not_called()
        with patch.object(self.setup, "activate_idf") as activate, \
                patch.object(self.setup.shutil, "which", return_value="/cargo"), \
                patch.object(self.setup, "run", return_value=SimpleNamespace(stdout="rustc 1.99.1 (test)")):
            self.assertIsNone(self.setup.preflight(self.root, True))
        activate.assert_not_called()

    def test_generated_build_settings_must_keep_usb_and_hmac_contract(self):
        build = self.root / "build/setup"
        build.mkdir(parents=True)
        defaults = (PROJECT / "sdkconfig.defaults").read_text()
        config = defaults + '\nCONFIG_IDF_TARGET="esp32s3"\n'
        (build / "sdkconfig").write_text(config)
        self.setup.verify_build(build)
        for expected, wrong in (("CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0", "CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=1"),
                                ("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", "CONFIG_ESP_CONSOLE_UART_DEFAULT=y")):
            with self.subTest(expected=expected):
                (build / "sdkconfig").write_text(config.replace(expected, wrong))
                with self.assertRaisesRegex(ValueError, "実機へ書き込まず"):
                    self.setup.verify_build(build)

    def test_registration_setup_builds_and_starts_with_existing_database_preserved(self):
        with contextlib.redirect_stdout(io.StringIO()), patch("builtins.input", return_value="y"):
            self.setup.setup_registration(self.root)
        database = self.root / "edge/registration-local/registrations.sqlite"
        database.write_bytes(b"existing identity and registrations")
        with contextlib.redirect_stdout(io.StringIO()), \
                patch.object(self.setup, "preflight"), patch.object(self.setup.socket, "socket"), \
                patch.object(self.setup, "run") as build, patch.object(self.setup, "start_edge") as start, \
                patch.object(self.setup, "choose_port") as usb, patch("builtins.input") as prompt:
            self.setup.main(registration=True)
        self.assertIn("build", build.call_args.args[1])
        self.assertEqual(start.call_args.args[1], database.parent.resolve() / "config.json")
        usb.assert_not_called()
        prompt.assert_not_called()
        self.assertEqual(database.read_bytes(), b"existing identity and registrations")

    def efuses(self, purpose="USER", key=None):
        return {"MAC": {"value": "12:34:56:78:9a:bc"},
                "KEY_PURPOSE_0": {"value": purpose, "writeable": True},
                "BLOCK_KEY0": {"value": key or "00 " * 32, "readable": True, "writeable": True}}

    def test_hmac_rejects_used_protected_empty_or_unknown_blocks(self):
        cases = [self.efuses("XTS_AES_128_KEY"), self.efuses(key="01 " * 32),
                 self.efuses("HMAC_UP"), self.efuses(key="not hex")]
        protected = self.efuses()
        protected["BLOCK_KEY0"]["readable"] = False
        cases.append(protected)
        locked = self.efuses()
        locked["KEY_PURPOSE_0"]["writeable"] = False
        cases.append(locked)
        for summary in cases:
            with self.subTest(summary=summary), self.assertRaises(ValueError):
                self.setup.hmac_state(summary)

    def test_hmac_never_burns_on_cancel_existing_key_or_changed_device(self):
        ready = self.efuses("HMAC_UP", "01 " * 32)
        changed = self.efuses()
        changed["MAC"]["value"] = "different device"
        cases = [(ready, ready, "", None), (self.efuses(), self.efuses(), "", RuntimeError),
                 (self.efuses(), changed, "BURN 12:34:56:78:9a:bc", RuntimeError)]
        for first, second, answer, failure in cases:
            with self.subTest(answer=answer, first=first), contextlib.redirect_stdout(io.StringIO()), \
                    patch.object(self.setup, "efuse_summary", side_effect=[first, second]), \
                    patch.object(self.setup, "run") as burn, patch("builtins.input", return_value=answer):
                if failure:
                    with self.assertRaises(failure):
                        self.setup.prepare_hmac(self.root, "usb")
                else:
                    self.setup.prepare_hmac(self.root, "usb")
                burn.assert_not_called()
                self.assertFalse((self.root / "edge/local/nvs-hmac.bin").exists())

    def test_hmac_burn_reuses_private_backup_and_requires_verified_readback(self):
        local = self.root / "edge/local"
        local.mkdir()
        key = local / "nvs-hmac.bin"
        original = bytes(range(32))
        key.write_bytes(original)
        key.chmod(0o600)
        for final, failure in ((self.efuses("HMAC_UP", "01 " * 32), False), (self.efuses(), True)):
            with self.subTest(failure=failure), contextlib.redirect_stdout(io.StringIO()), \
                    patch.object(self.setup, "efuse_summary", side_effect=[self.efuses(), self.efuses(), final]), \
                    patch.object(self.setup, "run") as burn, \
                    patch("builtins.input", return_value="BURN 12:34:56:78:9a:bc"):
                if failure:
                    with self.assertRaisesRegex(RuntimeError, "書込みを確認できません"):
                        self.setup.prepare_hmac(self.root, "usb")
                else:
                    self.setup.prepare_hmac(self.root, "usb")
                self.assertIn("burn-key", burn.call_args.args[1])
                self.assertEqual(key.read_bytes(), original)

    @unittest.skipUnless(importlib.util.find_spec("espefuse"), "ESP-IDF の espefuse が必要")
    def test_hmac_workflow_with_sdk_virtual_efuse_and_no_physical_device(self):
        (self.root / "edge/local").mkdir(mode=0o700)
        virtual = self.root / "virtual-efuse.bin"
        original_run = self.setup.run

        def virtual_run(root, arguments, **kwargs):
            self.assertEqual(arguments[:3], [sys.executable, "-m", "espefuse"])
            return original_run(root, [*arguments[:3], "--virt", "--path-efuse-file", str(virtual),
                                       *arguments[3:]], **kwargs)

        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.object(self.setup, "run", side_effect=virtual_run), \
                patch("builtins.input", return_value="BURN 00:00:00:00:00:00 (OK)"):
            self.setup.prepare_hmac(self.root, "unused-virtual-port")
        key = self.root / "edge/local/nvs-hmac.bin"
        saved = key.read_bytes()
        self.assertEqual(len(saved), 32)
        self.assertEqual(key.stat().st_mode & 0o777, 0o600)
        self.assertNotIn(saved.hex(), output.getvalue())
        with contextlib.redirect_stdout(io.StringIO()), patch.object(self.setup, "run", side_effect=virtual_run), \
                patch("builtins.input") as confirm:
            self.setup.prepare_hmac(self.root, "unused-virtual-port")
        confirm.assert_not_called()
        self.assertEqual(key.read_bytes(), saved)

    def test_existing_configuration_requires_matching_credentials_and_private_files(self):
        self.run_setup()
        local = self.root / "edge/local"
        with patch.object(self.setup.socket, "socket"):
            device, edge, address = self.setup.load_configuration(self.root)
        self.assertEqual((edge, address), ("192.168.1.50", "192.168.1.51"))
        device["settings"]["device_key"] = "a" * 64
        (local / "device.json").write_text(json.dumps(device))
        with self.assertRaisesRegex(ValueError, "一致しません"):
            self.setup.load_configuration(self.root)
        (local / "device.json").chmod(0o644)
        with self.assertRaisesRegex(ValueError, "0600"):
            self.setup.load_configuration(self.root)

    def test_https_health_checks_certificate_identity_and_authentication(self):
        self.run_setup()
        seen = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append((self.path, self.headers.get("Authorization")))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"receiver":"ready"}')

            def log_message(self, *_):
                pass

        server = HTTPServer(("127.0.0.1", 0), Handler)
        context = self.setup.ssl.SSLContext(self.setup.ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.root / "edge/local/edge.pem", self.root / "edge/local/edge-key.pem")
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            trusted = self.setup.ssl.create_default_context(cafile=str(self.root / "edge/local/ca.pem"))
            health = self.setup.get_health(server.server_address, trusted, "Basic test", "192.168.1.50")
            self.assertEqual(health, {"receiver": "ready"})
            self.assertEqual(seen, [("/health", "Basic test")])
            with self.assertRaises(self.setup.ssl.SSLCertVerificationError):
                self.setup.get_health(server.server_address, trusted, hostname="192.168.1.99")
            with self.assertRaises(self.setup.ssl.SSLCertVerificationError):
                self.setup.get_health(server.server_address, self.setup.ssl.create_default_context(),
                                      hostname="192.168.1.50")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_wait_health_retries_database_startup_and_stops_on_child_exit_or_timeout(self):
        process = Mock()
        process.poll.return_value = None
        probe = Mock(side_effect=[self.setup.sqlite3.OperationalError("not ready"), {"receiver": "ready"}])
        with patch.object(self.setup.time, "sleep"):
            self.setup.wait_health(process, probe, lambda h: h.get("receiver") == "ready", 5)
        process.poll.return_value = 1
        process.returncode = 1
        with self.assertRaisesRegex(RuntimeError, "終了コード 1"):
            self.setup.wait_health(process, probe, lambda _: True, 5)
        process.poll.return_value = None
        with patch.object(self.setup.time, "monotonic", side_effect=[0, 2]), self.assertRaises(TimeoutError):
            self.setup.wait_health(process, probe, lambda _: True, 1)

    def test_failed_device_check_stops_owned_edge_without_reporting_success(self):
        self.run_setup()
        process = Mock()
        process.poll.return_value = None
        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.dict(os.environ, {"JEV_API_KEY": "test-key"}), \
                patch.object(self.setup.subprocess, "Popen", return_value=process), \
                patch.object(self.setup, "wait_health", side_effect=[None, TimeoutError("device failed")]), \
                self.assertRaisesRegex(TimeoutError, "device failed"):
            self.setup.start_edge(self.root, self.root / "edge/local/config.json",
                                  {"admin_password": "a" * 64}, "192.168.1.50", "192.168.1.51")
        process.terminate.assert_called_once()
        process.wait.assert_called_with(timeout=10)
        self.assertNotIn("セットアップ完了", output.getvalue())

    def test_setup_reports_success_only_after_both_health_checks(self):
        self.run_setup()
        process = Mock()
        process.poll.return_value = 0
        process.wait.return_value = 0
        checked = []

        def check(child, probe, ready, timeout):
            checked.append(ready({"receiver": "ready"}))
            if timeout == 120:
                checked.append(ready({"wifi": "connected", "clock": "synchronized", "camera": "ok",
                                      "depth": "ok", "sender": "ok"}))
            self.assertNotIn("セットアップ完了", output.getvalue())

        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.dict(os.environ, {"JEV_API_KEY": "test-key"}), \
                patch.object(self.setup.subprocess, "Popen", return_value=process), \
                patch.object(self.setup, "wait_health", side_effect=check):
            self.setup.start_edge(self.root, self.root / "edge/local/config.json",
                                  {"admin_password": "a" * 64}, "192.168.1.50", "192.168.1.51")
        self.assertEqual(checked, [True, False, True])
        self.assertIn("セットアップ完了", output.getvalue())


if __name__ == "__main__":
    unittest.main()
