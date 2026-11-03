import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("provision_device", Path(__file__).parents[1] / "tools/provision_device.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class Provisioning(unittest.TestCase):
    def test_private_files_and_usb_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("certificate.pem", "key.pem", "ca.pem"):
                (root / name).write_text("test-only-material")
                (root / name).chmod(0o600)
            config = root / "device.json"
            config.write_text(json.dumps({"admin_password": "a" * 64,
                "server_certificate": "certificate.pem", "server_private_key": "key.pem", "edge_ca": "ca.pem",
                "settings": {"bed_id": "bed-01"}}))
            config.chmod(0o600)
            encoded = provision.payload(config)
            self.assertTrue(encoded.endswith(b"\n"))
            decoded = json.loads(encoded)["provision"]
            self.assertEqual(decoded["settings"]["bed_id"], "bed-01")
            self.assertEqual(decoded["server_private_key"], "test-only-material")
            (root / "key.pem").chmod(0o644)
            with self.assertRaises(ValueError):
                provision.payload(config)


if __name__ == "__main__":
    unittest.main()
