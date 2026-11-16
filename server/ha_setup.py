#!/usr/bin/env python3
"""Run ON esp-hub. HA admin operations use the token file, never command-line secrets.

The tiny client runs with aiohttp already installed inside HA's pinned container;
the dashboard backend itself remains Python-standard-library only.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from health import UNIQUE_IDS

CONFIG = Path("/opt/homeassistant/config")
TOKEN = Path("/etc/esp-dash/ha_token")
HTTP_CONFIG = {"server_host": ["127.0.0.1"], "server_port": 8123,
               "use_x_forwarded_for": True, "trusted_proxies": ["127.0.0.1/32"],
               "ip_ban_enabled": True, "login_attempts_threshold": 5}
CLIENT = r'''
import asyncio,json,sys,aiohttp
async def main():
    cfg=json.load(sys.stdin)
    async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=35)) as session:
        async with session.ws_connect('http://127.0.0.1:8123/api/websocket') as ws:
            if (await ws.receive_json()).get('type') != 'auth_required':
                raise RuntimeError('unexpected HA handshake')
            await ws.send_json({'type':'auth','access_token':cfg['token']})
            if (await ws.receive_json()).get('type') != 'auth_ok':
                raise RuntimeError('HA authentication failed')
            results=[]
            for i,command in enumerate(cfg['commands'],1):
                await ws.send_json({'id':i,**command})
                while True:
                    message=await ws.receive_json()
                    if message.get('id')==i and message.get('type')=='result': break
                if not message.get('success'):
                    raise RuntimeError('HA command failed: '+message.get('error',{}).get('code','unknown'))
                results.append(message.get('result'))
            print(json.dumps(results))
asyncio.run(main())
'''


def commands(*items):
    request = {"token": TOKEN.read_text().strip(), "commands": items}
    proc = subprocess.run(["docker", "exec", "-i", "homeassistant", "python3", "-c", CLIENT],
                          input=json.dumps(request), text=True, capture_output=True, timeout=45)
    if proc.returncode:
        # The child traceback contains no request/token, but keep diagnostics bounded regardless.
        raise RuntimeError("HA admin command failed; verify HA is ready and the token is an administrator")
    return json.loads(proc.stdout)


def http_config():
    return commands({"type": "http/config"})[0]


def check_http():
    current = http_config()
    stable = current.get("stable") or {}
    stable = stable.get("config", stable)
    if (current.get("active_config_type") != "stable" or current.get("pending") is not None
            or current.get("revert_at") or stable.get("error")
            or not all(stable.get(k) == v for k, v in HTTP_CONFIG.items())):
        raise RuntimeError("HA needs a confirmed loopback HTTP configuration before host networking")


def configure_http():
    deadline = time.monotonic()+180
    while True:
        try:
            current = http_config()
            break
        except (RuntimeError, OSError, subprocess.SubprocessError, ValueError):
            if time.monotonic() >= deadline:
                raise SystemExit("HA is not ready for network setup")
            time.sleep(2)
    desired = HTTP_CONFIG
    stable = current.get("stable") or current.get("default") or {}
    base = stable.get("config", stable)
    # Preserve existing TLS/CORS settings if they were explicitly configured.
    allowed = {"server_host", "server_port", "use_x_forwarded_for", "trusted_proxies", "ip_ban_enabled",
               "login_attempts_threshold", "ssl_certificate", "ssl_key", "ssl_peer_certificate",
               "cors_allowed_origins", "ssl_profile", "use_x_frame_options"}
    wanted = {k: v for k, v in base.items() if k in allowed}
    wanted.update(desired)
    if not all(base.get(k) == v for k, v in desired.items()) or current.get("pending"):
        while True:
            try:
                commands({"type": "http/config/configure", "config": wanted})
                break
            except (RuntimeError, OSError, subprocess.SubprocessError, ValueError):
                if time.monotonic() >= deadline:
                    raise SystemExit("HA did not finish startup for network setup")
                time.sleep(2)
        print("HA network configuration saved; waiting for restart", flush=True)
        deadline = time.monotonic()+180
        while True:
            time.sleep(2)
            try:
                state = http_config()
                pending = state.get("pending")
                if pending and pending.get("error"):
                    raise SystemExit("HA rejected the network trial; its automatic rollback is retained")
                if pending and state.get("active_config_type") == "pending":
                    commands({"type": "http/config/promote"})
                    break
                # If the restart has not happened yet, do not mistake the old server for the trial.
                active = state.get("stable") or {}
                active = active.get("config", active)
                if all(active.get(k) == v for k, v in desired.items()):
                    break
            except (RuntimeError, OSError, subprocess.SubprocessError, ValueError):
                pass
            if time.monotonic() >= deadline:
                raise SystemExit("HA did not confirm the network trial; automatic rollback remains enabled")
    check_http()
    print("HA network verified: localhost:8123, trusted local HTTPS proxy")


def discover(devices, entities):
    found = {}
    for entity in entities:
        if entity.get("platform") != "mobile_app" or entity.get("disabled_by"):
            continue
        uid = str(entity.get("unique_id", ""))
        for key, unique_id in UNIQUE_IDS.items():
            if uid == unique_id or uid.endswith("_"+unique_id):
                dev = entity.get("device_id")
                if dev:
                    found.setdefault(dev, {})[key] = entity["entity_id"]
    return [(d, found[d["id"]]) for d in devices if d.get("id") in found]


def atomic_json(path, value, mode=0o600):
    tmp = path.with_suffix(path.suffix+".tmp")
    fd = os.open(tmp, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, mode)
    with os.fdopen(fd, "w") as f:
        os.fchmod(f.fileno(), mode)
        json.dump(value, f, ensure_ascii=False, indent=2)
        f.write("\n")
    tmp.replace(path)


def registry():
    return commands({"type": "config/device_registry/list"}, {"type": "config/entity_registry/list"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "http", "http-check", "bind"))
    parser.add_argument("--device", help="exact HA device ID when multiple phones have Health sensors")
    args = parser.parse_args()
    if args.action == "http":
        configure_http()
        return
    if args.action == "http-check":
        check_http()
        return
    devices, entities = registry()
    phones = discover(devices, entities)
    if args.action == "inspect":
        print(json.dumps({"health_phones": [{"id": d["id"], "name": d.get("name_by_user") or d.get("name"),
                                           "metrics": list(metrics)} for d, metrics in phones]}, ensure_ascii=False))
        return
    selected = [(d, m) for d, m in phones if not args.device or d["id"] == args.device]
    if len(selected) != 1:
        raise SystemExit("Enable Apple Health sensors in the iPhone app first; use --device if multiple phones are listed")
    _, metrics = selected[0]
    atomic_json(Path("/etc/esp-dash/health.json"), {"enabled": True, "entities": metrics})
    # Included under recorder.exclude.entities. JSON is also valid YAML.
    atomic_json(CONFIG/"health_recorder.yaml", sorted(metrics.values()), 0o644)
    subprocess.run(["systemctl", "restart", "esp-dash-hub"], check=True)
    # Reloading recorder's configuration requires a HA restart; no storage files are edited.
    subprocess.run(["docker", "restart", "homeassistant"], check=True, stdout=subprocess.DEVNULL)
    print("Health mapping saved:", len(metrics), "selected metrics; no health values printed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
        sys.exit("HA setup failed; check container readiness and administrator access")
