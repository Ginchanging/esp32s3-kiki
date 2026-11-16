"""Kiki OTA store and SSH-only release administration. Standard library only.

Firmware/active.json are root-owned in /opt/esp-dash/firmware (hub reads only).
Device reports live in systemd's writable StateDirectory. No upload HTTP API.
"""
import argparse
import contextlib
try:
    import fcntl
except ImportError:  # Windows USB preflight also imports the image validator.
    fcntl = None
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import threading
import time
import uuid

LAYOUT = "dual6-v1"
SLOT_SIZE = 0x600000
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
DEVICE_ID = re.compile(r"[0-9a-f]{12}\Z")
JOB_ID = re.compile(r"[0-9a-f]{32}\Z")
TERMINAL = {"succeeded", "already_current", "failed", "rolled_back", "cancelled", "expired"}
STAGES = {"downloading", "verifying", "countdown", "rebooting", "validating"} | TERMINAL
_lock = threading.RLock()


def read_json(path, default=None):
    try:
        data = json.loads(Path(path).read_text())
        return data if isinstance(data, dict) else (default or {})
    except (OSError, ValueError):
        return default or {}


def atomic_json(path, data, mode=0o600):
    path = Path(path)
    fd, tmp = tempfile.mkstemp(prefix=".ota-", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "w") as f:
            json.dump(data, f, separators=(",", ":"))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


@contextlib.contextmanager
def store_lock(root):
    if fcntl is None:
        raise OSError("OTA store administration requires POSIX file locks")
    with _lock:
        # flock works on a read-only descriptor, including ProtectSystem=strict.
        try:
            f = (Path(root) / ".lock").open("rb")
        except FileNotFoundError:
            yield
            return
        with f:
            fcntl.flock(f, fcntl.LOCK_EX)
            yield


def firmware_info(path, allow_test=False):
    data = Path(path).read_bytes()
    if not 320 <= len(data) <= SLOT_SIZE or data[0] != 0xE9:
        raise ValueError("invalid firmware size/header")
    if struct.unpack_from("<H", data, 12)[0] != 9 or data[3] >> 4 != 4:
        raise ValueError("firmware must target ESP32-S3 / 16 MiB")
    if data[23] != 1 or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError("firmware image checksum mismatch")
    desc = data[32:288]
    if struct.unpack_from("<I", desc)[0] != 0xABCD5432:
        raise ValueError("missing app descriptor")
    version = desc[16:48].split(b"\0", 1)[0].decode("ascii")
    project = desc[48:80].split(b"\0", 1)[0].decode("ascii")
    if project != "esp_dash" or not version.startswith("ota1-"):
        raise ValueError("legacy/incompatible application")
    if version.startswith("ota1-test-") and not allow_test:
        raise ValueError("test image requires --allow-test")
    return {"schema": 1, "layout": LAYOUT, "chip": "esp32s3", "project": project,
            "version": version, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "elf_sha256": desc[144:176].hex(), "test": version.startswith("ota1-test-")}


def validate_manifest(m):
    if (m.get("schema") != 1 or m.get("layout") != LAYOUT or m.get("chip") != "esp32s3"
            or m.get("project") != "esp_dash" or not str(m.get("version", "")).startswith("ota1-")
            or not isinstance(m.get("size"), int) or isinstance(m["size"], bool)
            or not 320 <= m["size"] <= SLOT_SIZE
            or not HEX64.fullmatch(str(m.get("sha256", "")))
            or not HEX64.fullmatch(str(m.get("elf_sha256", "")))):
        raise ValueError("invalid OTA manifest")
    return m


class OTAStore:
    def __init__(self, root=None, state=None):
        self.root = Path(root or os.environ.get("FIRMWARE_DIRECTORY", "/opt/esp-dash/firmware"))
        state_dir = os.environ.get("STATE_DIRECTORY", "").split(":")[0]
        self.state_path = Path(state) if state else (Path(state_dir) / "ota.json" if state_dir else None)

    def state(self):
        s = read_json(self.state_path) if self.state_path else {}
        s.setdefault("devices", {})
        s.setdefault("jobs", {})
        return s

    def save(self, state):
        if self.state_path:
            atomic_json(self.state_path, state)

    def release(self, digest):
        if not HEX64.fullmatch(str(digest)):
            raise ValueError("invalid release")
        folder = self.root / digest
        m = validate_manifest(read_json(folder / "manifest.json"))
        if m["sha256"] != digest or (folder / "firmware.bin").stat().st_size != m["size"]:
            raise ValueError("release unavailable")
        return m, folder / "firmware.bin"

    def active(self):
        return read_json(self.root / "active.json")

    @staticmethod
    def device(body):
        if not isinstance(body, dict) or not DEVICE_ID.fullmatch(str(body.get("device", ""))):
            raise ValueError("invalid device")
        if (body.get("layout") != LAYOUT or not HEX64.fullmatch(str(body.get("elf_sha256", "")))
                or body.get("slot") not in ("ota_0", "ota_1") or not isinstance(body.get("verified"), bool)):
            raise ValueError("incompatible device")
        return {k: body[k] for k in ("device", "layout", "elf_sha256", "slot", "verified")}

    def check(self, body):
        device = self.device(body)
        now = int(time.time())
        with store_lock(self.root):
            s, active = self.state(), self.active()
            s["devices"][device["device"]] = {**device, "seen": now}
            result = {"schema": 1, "update": None}
            if active.get("cancelled") and active.get("device") == device["device"]:
                job_id = active.get("job_id", "")
                if JOB_ID.fullmatch(job_id):
                    job = s["jobs"].setdefault(job_id, dict(active))
                    job.update(stage="cancelled", updated=now)
            if active.get("device") == device["device"] and not active.get("cancelled"):
                job_id = active.get("job_id", "")
                if JOB_ID.fullmatch(job_id):
                    job = s["jobs"].setdefault(job_id, {**active, "stage": "pending"})
                    if job.get("stage") not in TERMINAL:
                        if now > active.get("expires", 0):
                            job.update(stage="expired", updated=now)
                        else:
                            try:
                                manifest, _ = self.release(active["sha256"])
                            except (OSError, ValueError, KeyError):
                                job.update(stage="failed", error="release_unavailable", updated=now)
                            else:
                                if device["elf_sha256"] == manifest["elf_sha256"]:
                                    if device["verified"]:
                                        job.update(stage="already_current" if job.get("stage") == "pending" else "succeeded", updated=now)
                                    else:
                                        job.update(stage="validating", updated=now)
                                else:
                                    result["update"] = {**manifest, "job_id": job_id,
                                                        "path": f'/api/device/ota/images/{manifest["sha256"]}.bin'}
            self.save(s)
            return result

    def report(self, body):
        device = self.device(body)
        job_id, stage = body.get("job_id", ""), body.get("stage", "")
        if not JOB_ID.fullmatch(str(job_id)) or stage not in STAGES:
            raise ValueError("invalid OTA report")
        now = int(time.time())
        with store_lock(self.root):
            s, active = self.state(), self.active()
            job = s["jobs"].get(job_id)
            if job is None:
                if active.get("job_id") != job_id or active.get("device") != device["device"]:
                    raise LookupError("unknown task")
                job = s["jobs"].setdefault(job_id, {**active, "stage": "pending"})
            if job.get("device") != device["device"]:
                raise LookupError("wrong device")
            if stage == "downloading" and (job.get("stage") in TERMINAL or active.get("job_id") != job_id or active.get("cancelled")
                                           or now > active.get("expires", 0)):
                raise LookupError("task cancelled/expired")
            if job.get("stage") not in TERMINAL:
                if stage in ("succeeded", "already_current"):
                    manifest, _ = self.release(job["sha256"])
                    if device["elf_sha256"] != manifest["elf_sha256"] or not device["verified"]:
                        raise ValueError("running image mismatch")
                job.update(stage=stage, updated=now, error=str(body.get("error", ""))[:48])
            s["devices"][device["device"]] = {**device, "seen": now}
            self.save(s)
            return {"ok": True}

    def status(self):
        with store_lock(self.root):
            s, a = self.state(), self.active()
            job = s["jobs"].get(a.get("job_id"), {})
            if a.get("cancelled") and a.get("job_id"):
                job = {**a, **job, "stage": "cancelled"}
            return {"devices": s["devices"], "active": a, "job": job or ({**a, "stage": "pending"} if a else {})}

    def publish(self, digest, device=None):
        manifest, _ = self.release(digest)
        with store_lock(self.root):
            s, old = self.state(), self.active()
            previous = s["jobs"].get(old.get("job_id"), {})
            if previous.get("stage") not in TERMINAL | {None, "pending"}:
                raise ValueError("an update is running; use status and wait")
            if not device:
                if len(s["devices"]) != 1:
                    raise ValueError("wait for the USB bootstrap device to register")
                device = next(iter(s["devices"]))
            if device not in s["devices"]:
                raise ValueError("unknown device")
            job = {"job_id": uuid.uuid4().hex, "device": device, "sha256": manifest["sha256"],
                   "created": int(time.time()), "expires": int(time.time()) + 900}
            atomic_json(self.root / "active.json", job, 0o644)
            return job

    def cancel(self):
        with store_lock(self.root):
            s, a = self.state(), self.active()
            job = s["jobs"].get(a.get("job_id"), {})
            if not a.get("job_id") or job.get("stage") in TERMINAL:
                return {"cancelled": False}
            if job.get("stage") not in TERMINAL | {None, "pending"}:
                raise ValueError("download already started; wait for its result")
            a["cancelled"] = True
            atomic_json(self.root / "active.json", a, 0o644)
            return {"cancelled": True}

    def rollback(self):
        s = self.state()
        if len(s["devices"]) != 1:
            raise ValueError("expected one device")
        d = next(iter(s["devices"].values()))
        candidates = []
        for folder in self.root.iterdir():
            if HEX64.fullmatch(folder.name):
                try:
                    m, _ = self.release(folder.name)
                    if m["elf_sha256"] != d["elf_sha256"] and not m.get("test"):
                        # Only images observed running successfully are rollback candidates.
                        confirmed = [j.get("updated", j.get("created", 0)) for j in s["jobs"].values()
                                     if j.get("sha256") == m["sha256"] and j.get("stage") in
                                     ("succeeded", "already_current")]
                        baseline = read_json(folder / "baseline.json")
                        if baseline.get("confirmed"):
                            confirmed.append(baseline.get("at", folder.stat().st_mtime))
                        if confirmed:
                            candidates.append((max(confirmed), m["sha256"]))
                except (ValueError, OSError):
                    pass
        if not candidates:
            raise ValueError("no previous confirmed release")
        return self.publish(max(candidates)[1], d["device"])


def install_release(store, image, allow_test=False, baseline=False):
    m = firmware_info(image, allow_test)
    folder = store.root / m["sha256"]
    with store_lock(store.root):
        if not folder.exists():
            stage = Path(tempfile.mkdtemp(prefix=".release-", dir=store.root))
            try:
                shutil.copyfile(image, stage / "firmware.bin")
                os.chmod(stage / "firmware.bin", 0o644)
                atomic_json(stage / "manifest.json", m, 0o644)
                os.chmod(stage, 0o755)
                os.replace(stage, folder)
            finally:
                if stage.exists():
                    shutil.rmtree(stage)
        elif firmware_info(folder / "firmware.bin", allow_test=True) != m:
            raise ValueError("immutable release mismatch")
        if baseline:
            devices = store.state()["devices"].values()
            if not any(d.get("elf_sha256") == m["elf_sha256"] and d.get("verified") for d in devices):
                raise ValueError("baseline is not running on the registered device")
            atomic_json(folder / "baseline.json", {"confirmed": True, "at": int(time.time())}, 0o644)
    return m


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=("install", "publish", "status", "cancel", "rollback"))
    p.add_argument("--image")
    p.add_argument("--sha256")
    p.add_argument("--allow-test", action="store_true")
    p.add_argument("--baseline", action="store_true")
    p.add_argument("--root", default="/opt/esp-dash/firmware")
    p.add_argument("--state", default="/var/lib/esp-dash/ota.json")
    a = p.parse_args()
    store = OTAStore(a.root, a.state)
    try:
        if a.action == "install":
            out = install_release(store, a.image, a.allow_test, a.baseline)
        elif a.action == "publish":
            out = store.publish(a.sha256)
        else:
            out = getattr(store, a.action)()
        print(json.dumps(out, separators=(",", ":")))
    except (ValueError, OSError) as e:
        print(str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
