"""OTA store, publish/claim races and authenticated streaming contract."""
import copy
import hashlib
import http.client
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
from http.server import ThreadingHTTPServer
import dashd
from ota import OTAStore, install_release, firmware_info, atomic_json


def image(elf="1" * 64, version="ota1-unit", chip=9):
    data = bytearray(384)
    data[0], data[3], data[23] = 0xE9, 0x40, 1
    struct.pack_into("<H", data, 12, chip)
    struct.pack_into("<I", data, 32, 0xABCD5432)
    data[48:48 + len(version)] = version.encode()
    data[80:89] = b"esp_dash\0"
    data[176:208] = bytes.fromhex(elf)
    return bytes(data) + hashlib.sha256(data).digest()


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / ".lock").touch()
        self.store = OTAStore(self.root, self.root / "state.json")
        self.device = {"device": "112233445566", "layout": "dual6-v1", "elf_sha256": "0" * 64,
                       "slot": "ota_0", "verified": True}
        self.store.check(self.device)
        self.data = image()
        self.path = self.root / "upload.bin"
        self.path.write_bytes(self.data)
        self.manifest = install_release(self.store, self.path)

    def tearDown(self):
        self.temp.cleanup()

    def publish(self):
        return self.store.publish(self.manifest["sha256"])

    def report(self, job, stage, device=None):
        return self.store.report({**(device or self.device), "job_id": job["job_id"], "stage": stage})

    def test_manifest_distinguishes_binary_and_elf_hashes(self):
        m = firmware_info(self.path)
        self.assertEqual(m["elf_sha256"], "1" * 64)
        self.assertEqual(m["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertNotEqual(m["sha256"], m["elf_sha256"])

    def test_legacy_chip_and_corruption_rejected(self):
        for data in (image(version="legacy"), image(chip=0), self.data[:-1] + b"x"):
            self.path.write_bytes(data)
            with self.assertRaises(ValueError):
                firmware_info(self.path)

    def test_test_release_requires_explicit_opt_in(self):
        self.path.write_bytes(image(version="ota1-test-unit"))
        with self.assertRaises(ValueError):
            firmware_info(self.path)
        self.assertTrue(firmware_info(self.path, True)["test"])

    def test_publish_offer_claim_and_fail_once(self):
        job = self.publish()
        offered = self.store.check(self.device)["update"]
        self.assertEqual(offered["job_id"], job["job_id"])
        self.assertTrue(offered["path"].endswith(self.manifest["sha256"] + ".bin"))
        self.report(job, "downloading")
        with self.assertRaises(ValueError):
            self.store.cancel()
        with self.assertRaises(ValueError):
            self.publish()
        self.report(job, "failed")
        self.assertIsNone(self.store.check(self.device)["update"])
        with self.assertRaises(LookupError):
            self.report(job, "downloading")
        self.assertEqual(self.store.status()["job"]["stage"], "failed")
        self.assertNotEqual(self.publish()["job_id"], job["job_id"])

    def test_cancelled_or_superseded_offer_cannot_claim(self):
        job = self.publish()
        self.store.cancel()
        self.assertEqual(self.store.status()["job"]["stage"], "cancelled")
        with self.assertRaises(LookupError):
            self.report(job, "downloading")
        self.assertIsNone(self.store.check(self.device)["update"])
        self.assertEqual(self.store.status()["job"]["stage"], "cancelled")
        newer = self.publish()
        with self.assertRaises(LookupError):
            self.report(job, "downloading")
        self.report(newer, "downloading")

    def test_pending_new_image_not_prematurely_confirmed(self):
        job = self.publish()
        self.report(job, "downloading")
        new = {**self.device, "slot": "ota_1", "elf_sha256": "1" * 64, "verified": False}
        self.assertIsNone(self.store.check(new)["update"])
        self.assertEqual(self.store.status()["job"]["stage"], "validating")
        with self.assertRaises(ValueError):
            self.report(job, "succeeded", new)
        new["verified"] = True
        self.report(job, "succeeded", new)
        self.assertEqual(self.store.status()["job"]["stage"], "succeeded")

    def test_same_image_is_noop(self):
        self.publish()
        self.assertIsNone(self.store.check({**self.device, "elf_sha256": "1" * 64})["update"])
        self.assertEqual(self.store.status()["job"]["stage"], "already_current")

    def test_expired_task_never_offered(self):
        job = self.publish()
        with patch("ota.time.time", return_value=job["expires"] + 1):
            self.assertIsNone(self.store.check(self.device)["update"])
        self.assertEqual(self.store.status()["job"]["stage"], "expired")

    def test_baseline_requires_confirmed_running_image(self):
        with self.assertRaises(ValueError):
            install_release(self.store, self.path, baseline=True)
        self.store.check({**self.device, "elf_sha256": "1" * 64})
        install_release(self.store, self.path, baseline=True)
        self.assertTrue((self.root / self.manifest["sha256"] / "baseline.json").exists())

    def test_reject_arbitrary_paths_wrong_devices_and_hashes(self):
        for value in ("../upload", "a" * 63, "F" * 64):
            with self.assertRaises(ValueError):
                self.store.release(value)
        with self.assertRaises(ValueError):
            self.store.check({**self.device, "layout": "old"})
        job = self.publish()
        with self.assertRaises(LookupError):
            self.report(job, "downloading", {**self.device, "device": "aabbccddeeff"})

    def test_rollback_chooses_only_confirmed_release(self):
        self.store.check({**self.device, "elf_sha256": "1" * 64})
        install_release(self.store, self.path, baseline=True)
        self.path.write_bytes(image("2" * 64))
        m2 = install_release(self.store, self.path)
        job = self.store.publish(m2["sha256"])
        self.store.check({**self.device, "elf_sha256": "2" * 64, "slot": "ota_1"})
        reverse = self.store.rollback()
        self.assertEqual(reverse["sha256"], self.manifest["sha256"])
        self.assertNotEqual(reverse["job_id"], job["job_id"])


class EndpointTest(StoreTest):
    def test_auth_streaming_and_invalid_requests(self):
        hub = dashd.Hub("x/y", [], None)
        hub.ota = self.store
        server = ThreadingHTTPServer(("127.0.0.1", 0), dashd.make_handler(hub, "test-token", {}))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            def call(method, path, body=None, token="test-token"):
                conn = http.client.HTTPConnection(*server.server_address, timeout=3)
                conn.request(method, path, body=json.dumps(body) if body is not None else None,
                             headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
                response = conn.getresponse()
                code, data, length = response.status, response.read(), response.getheader("Content-Length")
                conn.close()
                return code, data, length
            route = "/api/device/ota/images/" + self.manifest["sha256"] + ".bin"
            self.assertEqual(call("GET", route, token="wrong")[0], 401)
            code, data, length = call("GET", route)
            self.assertEqual(code, 200)
            self.assertEqual(data, self.data)
            self.assertEqual(int(length), len(data))
            self.assertEqual(call("GET", "/api/device/ota/images/../upload.bin")[0], 404)
            self.assertEqual(call("POST", "/api/device/ota/check", self.device, token="wrong")[0], 401)
            self.assertEqual(call("POST", "/api/device/ota/check", [])[0], 400)
            self.publish()
            code, raw, _ = call("POST", "/api/device/ota/check", self.device)
            self.assertEqual(code, 200)
            self.assertLess(len(raw), 2048)
            self.assertIsInstance(json.loads(raw)["update"], dict)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()
