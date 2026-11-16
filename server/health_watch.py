#!/usr/bin/env python3
"""Read-only 24-hour capacity check on esp-hub. Records counters, never health values."""
import argparse
import http.client
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import time


def command(*args):
    result = subprocess.run(args, text=True, capture_output=True, check=True, timeout=15)
    return result.stdout.strip()


def collect():
    now = int(time.time())
    out = {"at": now}
    memory = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, rest = line.split(":", 1)
        memory[key] = int(rest.split()[0])*1024
    out["host_available"] = memory["MemAvailable"]
    stats = json.loads(command("docker", "stats", "--no-stream", "--format", "{{json .}}", "homeassistant"))
    out["ha_cpu_pct"] = float(stats["CPUPerc"].rstrip("%"))
    amount = stats["MemUsage"].split("/")[0].strip()
    match = re.fullmatch(r"([\d.]+)\s*(B|KiB|MiB|GiB|kB|MB|GB)", amount)
    if match:
        out["ha_memory"] = int(float(match[1])*{"B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3,
                                                "kB": 1000, "MB": 1000**2, "GB": 1000**3}[match[2]])
    info = json.loads(command("docker", "inspect", "--format", "{{json .State}}", "homeassistant"))
    out["ha_oom"] = bool(info.get("OOMKilled"))
    out["ha_restarts"] = int(command("docker", "inspect", "--format", "{{.RestartCount}}", "homeassistant"))
    hub = dict(line.split("=", 1) for line in command("systemctl", "show", "esp-dash-hub",
                                                    "-p", "MemoryCurrent", "-p", "NRestarts").splitlines())
    out["hub_memory"] = int(hub["MemoryCurrent"])
    out["hub_restarts"] = int(hub["NRestarts"])
    token = Path("/etc/esp-dash/device_token").read_text().strip()
    ctx = ssl.create_default_context(cafile="/etc/esp-dash/hub.pem")
    # Local loopback does not match the device certificate's DNS name; its CA remains verified.
    ctx.check_hostname = False
    for endpoint in ("status", "diagnostics"):
        conn = http.client.HTTPSConnection("127.0.0.1", 8443, timeout=10, context=ctx)
        try:
            conn.request("GET", "/api/device/"+endpoint, headers={"Authorization": "Bearer "+token})
            response = conn.getresponse()
            body = response.read(8193)
            if response.status != 200 or len(body) > 8192:
                raise ValueError("invalid device response")
            data = json.loads(body)
            if endpoint == "status":
                health = data.get("health", {})
                out.update(status_bytes=len(body), health_configured=bool(health.get("configured")),
                           health_ok=bool(health.get("ok")),
                           health_values=sum(v is not None for v in health.get("values", {}).values()),
                           health_age=now-health["fetched_at"] if health.get("fetched_at") else -1)
                stamps = health.get("source_at", [])
                out["phone_age"] = now-max(stamps) if stamps and max(stamps) else -1
            else:
                out["device"] = data
        finally:
            conn.close()
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--hours", type=float, default=24)
    parser.add_argument("--interval", type=int, default=300)
    parser.add_argument("--out", default="/var/lib/esp-dash/health_watch.jsonl")
    args = parser.parse_args()
    if args.once:
        print(json.dumps(collect(), separators=(",", ":")))
        return
    deadline = time.monotonic()+max(1, args.hours*3600)
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "a", buffering=1) as f:
        f.write(json.dumps({"event": "start", "at": int(time.time()), "hours": args.hours})+"\n")
        while True:
            try:
                sample = collect()
            except Exception as error:
                sample = {"at": int(time.time()), "error": type(error).__name__}
            f.write(json.dumps(sample, separators=(",", ":"))+"\n")
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(max(10, args.interval), remaining))
        f.write(json.dumps({"event": "complete", "at": int(time.time())})+"\n")


if __name__ == "__main__":
    main()
