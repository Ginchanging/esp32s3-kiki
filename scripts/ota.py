#!/usr/bin/env python3
"""Build/publish/observe Kiki OTA through the existing SSH hub alias."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
from ota import firmware_info, TERMINAL, LAYOUT


def remote(action, *args):
    command = shlex.join(["python3", "-I", "/opt/esp-dash/ota.py", action, *args])
    r = subprocess.run(["ssh", "esp-hub", command], text=True, stdout=subprocess.PIPE, check=True)
    return json.loads(r.stdout)


def verify_build():
    sys.path.insert(0, str(ROOT))
    from flash_layout import parse_table, NEW_LAYOUT
    if parse_table((ROOT / "build/partition_table/partition-table.bin").read_bytes()) != NEW_LAYOUT:
        raise ValueError("build has an incompatible partition layout")
    config = (ROOT / "sdkconfig").read_text()
    if "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" not in config or 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"' not in config:
        raise ValueError("build must enable 16 MiB flash and boot rollback")


def install(image, allow_test=False, baseline=False):
    firmware_info(image, allow_test)
    staging = subprocess.check_output(["ssh", "esp-hub", "mktemp -d /tmp/kiki-ota.XXXXXXXX"], text=True).strip()
    if not staging.startswith("/tmp/kiki-ota.") or any(c not in "/.-_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" for c in staging):
        raise ValueError("invalid remote staging directory")
    try:
        subprocess.run(["scp", "-q", str(image), f"esp-hub:{staging}/firmware.bin"], check=True)
        args = ["--image", staging + "/firmware.bin"]
        if allow_test:
            args.append("--allow-test")
        if baseline:
            args.append("--baseline")
        return remote("install", *args)
    finally:
        subprocess.run(["ssh", "esp-hub", shlex.join(["rm", "-rf", "--", staging])], check=False)


def display(status):
    job = status.get("job", {})
    phase = job.get("stage", "idle")
    reason = job.get("error", "")
    devices = status.get("devices", {})
    current = next(iter(devices.values()), {}) if len(devices) == 1 else {}
    print(f"OTA {phase} · {current.get('slot', '?')} · {current.get('elf_sha256', '?')[:12]}"
          + (f" · {reason}" if reason else ""), flush=True)
    return phase


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", nargs="?", default="publish", choices=("publish", "status", "cancel", "rollback", "baseline"))
    p.add_argument("--image", type=Path, help="publish an already-built image")
    p.add_argument("--allow-test", action="store_true", help="explicit rollback-test release")
    p.add_argument("--timeout", type=int, default=600)
    a = p.parse_args()
    if a.timeout < 1:
        p.error("timeout must be positive")
    if a.action == "status":
        display(remote("status"))
        return 0
    if a.action == "cancel":
        print(json.dumps(remote("cancel")))
        return 0
    if a.action == "rollback":
        job = remote("rollback")
    else:
        if not a.image and a.action != "baseline":
            subprocess.run([str(ROOT / "esp.sh"), "build"], cwd=ROOT, check=True)
        verify_build()
        m = install(a.image or ROOT / "build/esp_dash.bin", a.allow_test, a.action == "baseline")
        print(f"发布 {m['version']} · {m['size']} bytes · {m['sha256'][:12]}", flush=True)
        if a.action == "baseline":
            return 0
        job = remote("publish", "--sha256", m["sha256"])
    print(f"任务 {job['job_id']} · 等待板子主动获取（无需 USB）", flush=True)
    deadline, last = time.monotonic() + a.timeout, None
    while time.monotonic() < deadline:
        s = remote("status")
        if s.get("active", {}).get("job_id") != job["job_id"]:
            raise ValueError("task was superseded; inspect ota status")
        signature = json.dumps(s, sort_keys=True)
        if signature != last:
            phase = display(s)
            last = signature
            if phase in TERMINAL:
                return 0 if phase in ("succeeded", "already_current") else 1
        time.sleep(3)
    print("等待超时，结果尚未确定；运行 ./esp.sh ota status 继续查询。", file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        print(f"OTA: {e}", file=sys.stderr)
        sys.exit(1)
