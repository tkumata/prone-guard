"""Exercise the actual TLS listener and committed SQLite writes on loopback."""
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import sqlite3
import ssl
import struct
import subprocess
import tempfile
import time
import unittest
from unittest.mock import Mock, patch
import contextlib
import io

ROOT = Path(__file__).resolve().parents[1]


class EdgeHTTPS(unittest.TestCase):
    def test_setup_manages_real_registration_edge_and_verifies_tls_before_stopping(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "edge/target/debug").mkdir(parents=True)
            (root / "edge/target/debug/prone-guard-edge").symlink_to(ROOT / "edge/target/debug/prone-guard-edge")
            spec = importlib.util.spec_from_file_location("setup_runtime", ROOT / "tools/setup.py")
            setup = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(setup)
            with contextlib.redirect_stdout(io.StringIO()), patch("builtins.input", return_value="y"):
                setup.setup_registration(root)
            config_path = root / "edge/registration-local/config.json"
            config = json.loads(config_path.read_text())
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            config["listen"] = f"127.0.0.1:{port}"
            config_path.write_text(json.dumps(config))
            original_popen, original_health = subprocess.Popen, setup.get_health
            owned = []

            def spawn(*args, **kwargs):
                child = original_popen(*args, **kwargs)
                owned.append(child)
                managed = Mock(wraps=child)

                def wait(timeout=None):
                    if timeout is None:
                        raise KeyboardInterrupt
                    return child.wait(timeout=timeout)

                managed.wait.side_effect = wait
                return managed

            def health(address, context, **kwargs):
                self.assertEqual(address, ("127.0.0.1", 8443))
                return original_health(("127.0.0.1", port), context, **kwargs)

            output = io.StringIO()
            with contextlib.redirect_stdout(output), patch.dict(os.environ, {"JEV_API_KEY": "test-only"}), \
                    patch.object(setup.subprocess, "Popen", side_effect=spawn), \
                    patch.object(setup, "get_health", side_effect=health):
                setup.start_edge(root, config_path)
            self.assertEqual(len(owned), 1)
            self.assertIsNotNone(owned[0].poll())
            self.assertIn("登録管理エッジの HTTPS 起動を確認", output.getvalue())
            self.assertNotIn("セットアップ完了", output.getvalue())
            self.assertTrue((root / "edge/registration-local/registrations.sqlite").exists())

    def test_generated_registration_configuration_starts_with_native_certificates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tools").mkdir()
            (root / "edge").mkdir()
            script = root / "tools/setup.py"
            shutil.copyfile(ROOT / "tools/setup.py", script)
            spec = importlib.util.spec_from_file_location("registration_setup", script)
            setup = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(setup)
            with contextlib.redirect_stdout(io.StringIO()), patch("builtins.input", return_value="y"):
                setup.setup_registration(root)
            local = root / "edge/registration-local"
            config_path = local / "config.json"
            config = json.loads(config_path.read_text())
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            config["listen"] = f"127.0.0.1:{port}"
            config_path.write_text(json.dumps(config))
            environment = os.environ.copy()
            environment.pop("JEV_API_KEY", None)
            with open(root / "server.log", "wb") as log:
                process = subprocess.Popen([str(ROOT / "edge/target/debug/prone-guard-edge"), str(config_path)],
                                           cwd=root, stdout=log, stderr=log, env=environment)
                try:
                    last_error = None
                    for attempt in range(100):
                        connection = None
                        try:
                            with contextlib.closing(sqlite3.connect(
                                    (local / "registrations.sqlite").as_uri() + "?mode=ro",
                                    uri=True, timeout=0.1)) as database:
                                identity = json.loads(database.execute(
                                    "SELECT data FROM registration_metadata WHERE name='identity'"
                                ).fetchone()[0])
                            hostname = f"pg-edge-{identity['id']}.local"
                            context = ssl.create_default_context(cadata=identity["ca_certificate"])
                            connection = http.client.HTTPSConnection(hostname, port, context=context, timeout=3)
                            connection.sock = context.wrap_socket(
                                socket.create_connection(("127.0.0.1", port), timeout=3), server_hostname=hostname)
                            connection.request("GET", "/health")
                            response = connection.getresponse()
                            self.assertEqual(response.status, 200)
                            response.read()
                            break
                        except ssl.SSLCertVerificationError:
                            raise
                        except (OSError, sqlite3.OperationalError, TypeError) as error:
                            last_error = error
                            if process.poll() is not None:
                                self.fail((root / "server.log").read_text())
                            time.sleep(0.05)
                        finally:
                            if connection is not None:
                                connection.close()
                    else:
                        self.fail(f"Generated registration configuration did not become ready: {last_error}")
                    self.assertFalse((local / "edge-key.pem").exists())
                    self.assertEqual((local / "registrations.sqlite").stat().st_mode & 0o777, 0o600)
                    for secret in (identity["ca_key"], identity["server_key"]):
                        self.assertNotIn(secret, (root / "server.log").read_text())
                    with contextlib.closing(sqlite3.connect(local / "registrations.sqlite")) as database:
                        self.assertEqual(database.execute("SELECT count(*) FROM registrations").fetchone()[0], 0)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGINT)
                    process.wait(timeout=10)
                self.assertEqual(process.returncode, 0)
            self.assert_mdns_conflict_isolated(root, config_path, port, hostname, context, environment)

    def assert_mdns_conflict_isolated(self, root, config_path, port, hostname, context, environment):
        def dns_name(name):
            return b"".join(bytes([len(label)]) + label.encode("ascii")
                            for label in name.split(".")) + b"\0"

        # A same-name SRV response with a different port conflicts during probing.
        other_port = port - 1 if port == 65535 else port + 1
        instance = hostname.removesuffix(".local")
        srv = struct.pack("!HHH", 0, 0, other_port) + dns_name(hostname)
        packet = (struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0)
                  + dns_name(f"{instance}._prone-guard._tcp.local")
                  + struct.pack("!HHIH", 33, 0x8001, 120, len(srv)) + srv)
        log_path = root / "conflict.log"
        with log_path.open("wb") as log, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.bind(("127.0.0.1", 0))
            sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
            sender.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
            process = subprocess.Popen([str(ROOT / "edge/target/debug/prone-guard-edge"), str(config_path)],
                                       cwd=root, stdout=log, stderr=log, env=environment)
            try:
                deadline = time.monotonic() + 5
                while (time.monotonic() < deadline and process.poll() is None
                       and "mDNS name conflict" not in log_path.read_text()):
                    sender.sendto(packet, ("224.0.0.251", 5353))
                    time.sleep(0.02)
                self.assertIn('"event":"mdns_advertisement_failed"', log_path.read_text())
                self.assertIn("mDNS name conflict", log_path.read_text())
                self.assertIsNone(process.poll())

                connection = http.client.HTTPSConnection(hostname, port, context=context, timeout=3)
                try:
                    connection.sock = context.wrap_socket(
                        socket.create_connection(("127.0.0.1", port), timeout=3), server_hostname=hostname)
                    connection.request("GET", "/health")
                    with connection.getresponse() as response:
                        self.assertEqual(response.status, 200)
                        response.read()
                finally:
                    connection.close()

                with contextlib.closing(sqlite3.connect(root / "edge/registration-local/sensors.sqlite")) as db:
                    db.execute("INSERT INTO results VALUES ('test', 0, 0, '{}')")
                    db.commit()
                    deadline = time.monotonic() + 4
                    while db.execute("SELECT COUNT(*) FROM results").fetchone()[0]:
                        self.assertLess(time.monotonic(), deadline, "periodic expiry stopped")
                        time.sleep(0.05)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                process.wait(timeout=10)
            self.assertEqual(process.returncode, 0)

    def assert_request_deadlines(self, port, context, token, body):
        with contextlib.ExitStack() as stack:
            def connect():
                tcp = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=8))
                return stack.enter_context(context.wrap_socket(tcp, server_hostname="127.0.0.1"))

            idle = connect()
            preface = connect()
            preface.sendall(b"PRI ")
            incomplete = connect()
            incomplete.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")
            reused = connect()
            reused.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            with http.client.HTTPResponse(reused) as response:
                response.begin()
                self.assertEqual(response.status, 200)
                response.read()
            reused.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n")

            stalled_body = connect()
            stalled_body.sendall(
                b"POST /v1/measurements HTTP/1.1\r\nHost: localhost\r\n"
                b"Content-Type: application/x-ndjson\r\nContent-Length: 32\r\n\r\n{"
            )
            connection = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=3)
            stack.callback(connection.close)
            connection.request("POST", "/v1/measurements", body, {
                "Content-Type": "application/x-ndjson", "Authorization": f"Bearer {token}",
            })
            with connection.getresponse() as response:
                self.assertEqual(response.status, 204)
                response.read()

            for stream in (incomplete, idle, preface, reused):
                while stream.recv(4096):
                    pass
            with http.client.HTTPResponse(stalled_body) as response:
                response.begin()
                self.assertEqual(response.status, 408)
                response.read()
            self.assertEqual(stalled_body.recv(1), b"")

    def test_tls_authentication_ingest_and_body_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            key, cert = root / "server-key.pem", root / "server.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=127.0.0.1",
                "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True)
            key.chmod(0o600)
            token = secrets.token_hex(32)
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            config = {"listen": f"127.0.0.1:{port}", "database": str(root / "data.sqlite"),
                "tls_certificate": str(cert), "tls_private_key": str(key), "retention_seconds": 21600,
                "devices": [{"bed_id": "bed-01", "token_sha256": hashlib.sha256(token.encode()).hexdigest(), "installation": {}}]}
            config_path = root / "config.json"
            config_path.write_text(json.dumps(config))
            environment = os.environ.copy()
            environment.pop("JEV_API_KEY", None)
            with open(root / "server.log", "wb") as log:
                process = subprocess.Popen([str(ROOT / "edge/target/debug/prone-guard-edge"), str(config_path)],
                    stdout=log, stderr=log, env=environment)
                try:
                    context = ssl.create_default_context(cafile=str(cert))
                    for attempt in range(100):
                        try:
                            connection = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=3)
                            connection.request("GET", "/health")
                            response = connection.getresponse()
                            self.assertEqual(response.status, 200)
                            response.read()
                            connection.close()
                            break
                        except (OSError, ConnectionError):
                            if process.poll() is not None:
                                self.fail((root / "server.log").read_text())
                            time.sleep(0.05)
                    else:
                        self.fail("TLS server did not become ready")
                    with self.assertRaises(ssl.SSLCertVerificationError):
                        untrusted = http.client.HTTPSConnection("127.0.0.1", port, timeout=3)
                        try:
                            untrusted.request("GET", "/health")
                        finally:
                            untrusted.close()
                    measurement = json.loads((ROOT / "tests/fixtures/measurement.jsonl").read_text())
                    now = int(time.time())
                    measurement["timestamp"] = now
                    measurement["camera"]["timestamp"] = now
                    measurement["depth"]["timestamp"] = now
                    body = json.dumps(measurement).encode() + b"\n"
                    for auth, data, expected in [(None, body, 401), ("b" * 64, body, 401), (token, body, 204),
                        (token, body, 204), (token, b"x" * 16385, 413)]:
                        connection = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=3)
                        headers = {"Content-Type": "application/x-ndjson"}
                        if auth is not None:
                            headers["Authorization"] = "Bearer " + auth
                        connection.request("POST", "/v1/measurements", data, headers)
                        response = connection.getresponse()
                        self.assertEqual(response.status, expected)
                        response.read()
                        connection.close()
                    self.assert_request_deadlines(port, context, token, body)
                    with contextlib.closing(sqlite3.connect(root / "data.sqlite")) as database:
                        self.assertEqual(database.execute("SELECT count(*) FROM observations").fetchone()[0], 1)
                    if os.geteuid() != 0:
                        measurement["sequence"] += 1
                        body = json.dumps(measurement).encode() + b"\n"
                        root.chmod(0o500)
                        try:
                            connection = http.client.HTTPSConnection("127.0.0.1", port, context=context, timeout=3)
                            connection.request("POST", "/v1/measurements", body,
                                {"Content-Type": "application/x-ndjson", "Authorization": "Bearer " + token})
                            response = connection.getresponse()
                            self.assertEqual(response.status, 503)
                            response.read()
                            connection.close()
                        finally:
                            root.chmod(0o700)
                        with contextlib.closing(sqlite3.connect(root / "data.sqlite")) as database:
                            self.assertEqual(database.execute("SELECT count(*) FROM observations").fetchone()[0], 1)
                finally:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
