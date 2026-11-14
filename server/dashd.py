#!/usr/bin/env python3
"""ESP32 desk dashboard backend. Standard library only, Python >= 3.10.

  dashd.py hub    HTTPS server on 8.138.138.222:8443
                  GET  /api/device/status   (device token)  -> JSON for the ESP32
                  POST /api/device/mi       (device token)  <- {"dev":"lamp","on":true,"bri":60}
                  POST /api/agent/report    (agent token)   <- metrics from other servers
                  polls GitHub Actions for one repository
                  polls Mi Home devices through the local Home Assistant (optional)
  dashd.py agent  runs on every other server; pushes local metrics to the hub
  dashd.py once   print this machine's metrics as JSON (for testing)

Secrets come from systemd credentials ($CREDENTIALS_DIRECTORY) or --cred-dir:
  hub:   hub.pem, hub.key, device_token, agent_tokens.json ({"name": "token"}), github_token (optional),
         ha_token + mihome.json (optional: Home Assistant long-lived token and the device list)
  agent: hub.pem, agent_token
Settings come from environment variables (see server/README.md).
"""
import argparse
import hmac
import http.client
import datetime
import json
import re
import os
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HUB_CN = "esp-dash.local"  # name in the pinned self-signed certificate
CPU_HIST = 60               # one sample per minute, last hour
CONTRIB_DAYS = 105          # 15 weeks for the heatmap
ONLINE_WINDOW_S = 90
MAX_BODY = 16 * 1024
CN_TZ = datetime.timezone(datetime.timedelta(hours=8))  # Kiki's days follow China time
LEDGER_DAYS = 7


def log(*parts):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *parts, flush=True)


def cn_date(ts=None):
    return datetime.datetime.fromtimestamp(time.time() if ts is None else ts, CN_TZ).date()


def state_path(name):
    """File in systemd's StateDirectory (/var/lib/esp-dash), or None when run by hand."""
    d = os.environ.get("STATE_DIRECTORY", "").split(":")[0]
    return Path(d) / name if d else None


def load_state(name):
    path = state_path(name)
    try:
        data = json.loads(path.read_text()) if path and path.exists() else {}
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError) as e:
        log("state: cannot read", name, e)
        return {}


def save_state(name, data):
    path = state_path(name)
    if not path:
        return
    try:
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, separators=(",", ":")))
        tmp.replace(path)
    except OSError as e:
        log("state: cannot write", name, e)


# ---------------------------------------------------------------- local metrics

class Metrics:
    """CPU needs two samples, so keep the previous /proc/stat reading."""

    def __init__(self, services_spec):
        self.prev = None
        self.services = parse_services(services_spec)

    def cpu_pct(self):
        with open("/proc/stat") as f:
            fields = [int(x) for x in f.readline().split()[1:]]
        idle = fields[3] + (fields[4] if len(fields) > 4 else 0)
        total = sum(fields[:8])
        prev, self.prev = self.prev, (idle, total)
        if not prev or total == prev[1]:
            return 0.0
        return round(100.0 * (1 - (idle - prev[0]) / (total - prev[1])), 1)

    def collect(self, name):
        mem = {}
        with open("/proc/meminfo") as f:
            for line in f:
                key, value = line.split(":", 1)
                mem[key] = int(value.split()[0])
        st = os.statvfs("/")
        used = (st.f_blocks - st.f_bfree) * st.f_frsize
        avail = st.f_bavail * st.f_frsize
        with open("/proc/uptime") as f:
            uptime = int(float(f.read().split()[0]))
        with open("/proc/loadavg") as f:
            load1 = float(f.read().split()[0])
        return {
            "name": name,
            "cpu_pct": self.cpu_pct(),
            "mem_pct": round(100.0 * (1 - mem.get("MemAvailable", 0) / max(mem.get("MemTotal", 1), 1)), 1),
            "disk_pct": round(100.0 * used / max(used + avail, 1), 1),
            "load1": load1,
            "uptime_s": uptime,
            "services": [{"name": n, "ok": check_service(kind, arg)} for n, kind, arg in self.services],
        }


def parse_services(spec):
    """'feishu-bot=tcp:127.0.0.1:3000,docker=systemd:docker,codex=proc:codex_monitor'"""
    out = []
    for item in filter(None, (x.strip() for x in (spec or "").split(","))):
        name, _, target = item.partition("=")
        kind, _, arg = target.partition(":")
        if kind in ("tcp", "systemd", "proc") and arg:
            out.append((name[:19], kind, arg))
    return out[:4]


def check_service(kind, arg):
    try:
        if kind == "tcp":
            host, _, port = arg.rpartition(":")
            with socket.create_connection((host, int(port)), timeout=2):
                return True
        if kind == "systemd":
            r = subprocess.run(["systemctl", "is-active", "--quiet", arg], timeout=5)
            return r.returncode == 0
        if kind == "proc":
            me = str(os.getpid())
            for pid in os.listdir("/proc"):
                if pid.isdigit() and pid != me:
                    try:
                        with open(f"/proc/{pid}/cmdline", "rb") as f:
                            if arg.encode() in f.read():
                                return True
                    except OSError:
                        pass
            return False
    except (OSError, ValueError, subprocess.SubprocessError):
        return False
    return False


# ---------------------------------------------------------------- credentials

def cred_dir(args):
    d = args.cred_dir or os.environ.get("CREDENTIALS_DIRECTORY")
    if not d:
        sys.exit("no credentials: run under systemd LoadCredential= or pass --cred-dir")
    return Path(d)


def read_cred(directory, name, required=True):
    p = directory / name
    if not p.exists():
        if required:
            sys.exit(f"missing credential {name}")
        return ""
    return p.read_text().strip()


# ---------------------------------------------------------------- hub

class Ledger:
    """Per-day counts (China dates) for Kiki's diary. Survives hub restarts."""
    EVENT_KEY = {"run_succeeded": "ci_ok", "run_failed": "ci_fail", "server_down": "down", "mi_done": "wash"}

    def __init__(self):
        self.days = {k: v for k, v in load_state("ledger.json").items() if isinstance(v, dict)}

    def count(self, kind):
        key = self.EVENT_KEY.get(kind)
        if not key:
            return
        day = self.days.setdefault(cn_date().isoformat(), {})
        day[key] = day.get(key, 0) + 1
        for old in sorted(self.days)[:-(LEDGER_DAYS + 1)]:
            del self.days[old]
        save_state("ledger.json", self.days)

    def snapshot(self, commits):
        today = cn_date()
        out = []
        for i in range(LEDGER_DAYS - 1, -1, -1):  # oldest first, today last
            d = today - datetime.timedelta(days=i)
            day = self.days.get(d.isoformat(), {})
            out.append({"d": d.strftime("%m-%d"), "wd": d.isoweekday() % 7,
                        **{k: day.get(k, 0) for k in ("ci_ok", "ci_fail", "wash", "down")},
                        "commits": commits.get(d.isoformat(), 0)})
        return out


class Hub:
    def __init__(self, repo, server_names, github_token):
        self.lock = threading.Lock()
        self.repo = repo
        self.github_token = github_token
        self.github = {"repo": repo, "fetched_at": 0, "runs": []}
        self.etag = None
        self.run_seen = {}  # run id -> (status, conclusion)
        self.github_baseline = False
        self.servers = {n: {"name": n, "reported_at": 0} for n in server_names}
        self.cpu_hist = {}  # name -> [(minute, cpu)]
        self.contrib = None
        self.commits = {}  # ISO date -> contributions that day (recent days only)
        self.ledger = Ledger()
        self.order = list(server_names)
        self.online = {}
        self.events = []
        self.last_event_id = 0
        self.mihome = None  # MiHome when Home Assistant is configured

    def add_event(self, kind, subject):
        # ids are epoch milliseconds so they keep increasing across hub restarts
        eid = max(int(time.time() * 1000), self.last_event_id + 1)
        self.last_event_id = eid
        self.events.append({"id": eid, "kind": kind, "subject": subject[:79]})
        self.events = self.events[-8:]
        self.ledger.count(kind)
        log("event", kind, subject)

    # --- GitHub ---
    def poll_github(self):
        url = f"https://api.github.com/repos/{self.repo}/actions/runs?per_page=4&exclude_pull_requests=true"
        req = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "esp-dash-hub",
            "X-GitHub-Api-Version": "2022-11-28",
        })
        if self.etag:
            req.add_header("If-None-Match", self.etag)
        if self.github_token:
            req.add_header("Authorization", f"Bearer {self.github_token}")
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                self.etag = resp.headers.get("ETag")
                data = json.load(resp)
        except urllib.error.HTTPError as e:
            if e.code == 304:
                with self.lock:
                    self.github["fetched_at"] = int(time.time())
                return
            log("github http", e.code, e.headers.get("x-ratelimit-remaining"))
            return
        except (urllib.error.URLError, OSError, ValueError) as e:
            log("github error", e)
            return
        now = int(time.time())
        runs = []
        for r in data.get("workflow_runs", [])[:4]:
            started = parse_time(r.get("run_started_at") or r.get("created_at"))
            updated = parse_time(r.get("updated_at"))
            end = updated if r.get("status") == "completed" else now
            runs.append({
                "id": r.get("id"),
                "workflow": (r.get("name") or "")[:47],
                "branch": (r.get("head_branch") or "")[:39],
                "run_number": r.get("run_number"),
                "status": r.get("status"),
                "conclusion": r.get("conclusion"),
                "duration_s": max(0, end - started) if started else 0,
                "updated_at": updated,
            })
        with self.lock:
            for r in runs:
                key = (r["status"], r["conclusion"])
                before = self.run_seen.get(r["id"])
                if self.github_baseline and before is None and r["status"] in ("in_progress", "queued"):
                    self.add_event("run_started", f'{r["workflow"]} #{r["run_number"]}')
                if self.github_baseline and before != key and r["status"] == "completed":
                    subject = f'{r["workflow"]} #{r["run_number"]}'
                    if r["conclusion"] == "success":
                        self.add_event("run_succeeded", subject)
                    elif r["conclusion"] in ("failure", "timed_out", "startup_failure"):
                        self.add_event("run_failed", subject)
                self.run_seen[r["id"]] = key
            self.github_baseline = True
            if len(self.run_seen) > 200:
                keep = {r["id"] for r in runs}
                self.run_seen = {k: v for k, v in self.run_seen.items() if k in keep}
            self.github = {"repo": self.repo, "fetched_at": now, "runs": runs}

    # --- servers ---
    def report(self, payload):
        name = payload["name"]
        entry = {k: payload[k] for k in ("cpu_pct", "mem_pct", "disk_pct", "load1", "uptime_s")
                 if isinstance(payload.get(k), (int, float))}
        entry["services"] = [
            {"name": str(s.get("name", ""))[:19], "ok": bool(s.get("ok"))}
            for s in payload.get("services", [])[:4] if isinstance(s, dict)]
        entry["name"] = name
        entry["reported_at"] = int(time.time())
        minute = entry["reported_at"] // 60
        with self.lock:
            hist = self.cpu_hist.setdefault(name, [])
            cpu = int(round(entry.get("cpu_pct", 0)))
            if hist and hist[-1][0] == minute:
                hist[-1] = (minute, max(hist[-1][1], cpu))  # keep the busiest sample of the minute
            else:
                hist.append((minute, cpu))
                del hist[:-CPU_HIST]
            if name not in self.servers:
                self.order.append(name)
            self.servers[name] = entry
            self.update_online()

    def update_online(self):
        now = time.time()
        for name, s in self.servers.items():
            online = s["reported_at"] > 0 and now - s["reported_at"] <= ONLINE_WINDOW_S
            before = self.online.get(name)
            if before is not None and before != online:
                self.add_event("server_up" if online else "server_down", name)
            if s["reported_at"] > 0:
                self.online[name] = online

    def snapshot(self):
        with self.lock:
            self.update_online()
            servers = []
            for name in self.order[:3]:
                s = dict(self.servers[name])
                s["online"] = self.online.get(name, False)
                s["cpu_hist"] = [c for _, c in self.cpu_hist.get(name, [])]
                servers.append(s)
            github = dict(self.github)
            if self.contrib:
                github["contrib"] = self.contrib
            snap = {
                "schema": 1,
                "generated_at": int(time.time()),
                "github": github,
                "servers": servers,
                "events": list(self.events),
                "days": self.ledger.snapshot(self.commits),
            }
        if self.mihome:
            snap["mihome"] = self.mihome.snapshot()
        return snap


# ---------------------------------------------------------------- Mi Home via Home Assistant
#
# Home Assistant (Docker, 127.0.0.1:8123) runs Xiaomi's official "Xiaomi Home"
# integration, which logs in with Xiaomi OAuth and gets device updates pushed over
# Xiaomi's cloud MQTT. We only talk to HA's local REST API with a long-lived token.

ENTITY_RE = re.compile(r"^[a-z_]+\.[a-z0-9_]+$")
MI_ID_RE = re.compile(r"^[a-z0-9_]{1,15}$")
MI_POLL_S = 5
MI_RUNNING = ("wash", "rinse", "spin", "dry", "pause")
# HA state text -> fixed washer state. First match wins ("洗涤完成" is done, not wash).
MI_STATE_WORDS = (
    ("done", ("完成", "结束", "done", "finish", "complete")),
    ("fault", ("故障", "错误", "异常", "fault", "error")),
    ("pause", ("暂停", "pause")),
    ("dry", ("烘干", "dry")),
    ("spin", ("脱水", "spin")),
    ("rinse", ("漂洗", "rinse")),
    ("idle", ("待机", "空闲", "预约", "idle", "standby", "wait", "delay")),
    ("off", ("关机", "关闭", "off")),
    ("wash", ("洗", "浸泡", "运行", "工作", "wash", "soak", "busy", "run", "working")),
)
MI_DISPENSER_WORDS = (
    ("fault", ("故障", "错误", "异常", "fault", "error")),
    ("clean", ("清洗", "clean")),
    ("heat", ("加热", "煮沸", "heat", "boil")),
    ("pour", ("出水", "取水", "制水", "pour", "dispens", "outlet")),
    ("idle", ("空闲", "待机", "idle", "standby")),
    ("off", ("关机", "关闭", "off")),
)
# kind -> the entity key it cannot work without
MI_REQUIRED = {"lamp": "entity", "switch": "entity", "washer": "status", "dispenser": "status",
               "scale": "weight_event"}
MI_ENTITY_KEYS = ("entity", "status", "stage", "left", "end_event", "temp", "tds", "weight_event", "offline_event")
# Event attributes that may carry a weighing (Xiaomi Home names arguments after the MIoT spec, zh-Hans)
MI_WEIGHT_ATTRS = ("称重过程数据", "离线数据上报")
SCALE_KEEP_DAYS = 60


class HAError(Exception):
    pass


class HomeAssistant:
    def __init__(self, token, host="127.0.0.1", port=8123):
        self.token, self.host, self.port = token, host, port

    def call(self, method, path, body=None):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=10)
        try:
            conn.request(method, path, body=None if body is None else json.dumps(body).encode(),
                         headers={"Authorization": f"Bearer {self.token}",
                                  "Content-Type": "application/json"})
            resp = conn.getresponse()
            data = resp.read()
        finally:
            conn.close()
        if resp.status != 200:
            raise HAError(f"HTTP {resp.status} for {path}")
        return json.loads(data) if data else None

    def state(self, entity):
        return self.call("GET", f"/api/states/{entity}")

    def service(self, domain, service, data):
        return self.call("POST", f"/api/services/{domain}/{service}", data)


def mi_washer_state(raw, overrides, words=MI_STATE_WORDS):
    if raw in overrides:
        return overrides[raw]
    low = raw.lower()
    for state, keys in words:
        if any(w in low for w in keys):
            return state
    return "other"


def mi_number(st):
    """Numeric sensor -> int, or None."""
    try:
        return int(round(float(st["state"])))
    except (TypeError, ValueError, KeyError):
        return None


def decode_s400(raw):
    """Payload of the Mi body-composition scale S400 (yunmai-ms107) weight events -> kg, or None.
    The format is not documented; filled in from a real sample."""
    return None


def decode_weight(attrs, d):
    """kg from a scale event's attributes, or None. Values are never logged."""
    for name in d.get("weight_attrs", MI_WEIGHT_ATTRS):
        raw = attrs.get(name)
        if raw is None:
            continue
        try:
            kg = float(raw) * float(d.get("weight_scale", 1))
        except (TypeError, ValueError):
            kg = decode_s400(raw)
        if kg is not None and 2 <= kg <= 300:
            return kg
    return None


def ha_time(stamp):
    """HA timestamp state ("2026-11-14T08:12:33.123456+00:00") -> epoch seconds, or None."""
    try:
        return datetime.datetime.fromisoformat(stamp).timestamp()
    except (TypeError, ValueError):
        return None


def mi_minutes(st):
    """Remaining-time sensor -> whole minutes (None if unknown)."""
    try:
        v = float(st["state"])
    except (TypeError, ValueError, KeyError):
        return None
    unit = (st.get("attributes", {}).get("unit_of_measurement") or "min").lower()
    if unit in ("s", "sec", "second", "seconds"):
        v /= 60
    elif unit in ("h", "hour", "hours"):
        v *= 60
    return max(0, int(round(v)))


class MiHome:
    """Polls the configured entities; turns a washer's running -> finished change into a hub event."""

    def __init__(self, hub, ha, config):
        self.hub, self.ha = hub, ha
        self.lock = threading.Lock()
        self.devices = []
        for d in config.get("devices", [])[:4]:
            kind = d.get("kind")
            filters = d.get("filters", [])
            ents = [d.get(k) for k in MI_ENTITY_KEYS if d.get(k)] + (filters if isinstance(filters, list) else [None])
            if (kind not in MI_REQUIRED or not MI_ID_RE.match(str(d.get("id", ""))) or not d.get(MI_REQUIRED[kind])
                    or not all(isinstance(e, str) and ENTITY_RE.match(e) for e in ents)):
                log("mihome: skipping device", d.get("id"))
                continue
            self.devices.append(d)
        self.by_id = {d["id"]: d for d in self.devices}
        self.view = {d["id"]: self.base(d) for d in self.devices}
        self.ok = False
        # washer id -> {"state": last state, "total": longest remaining time seen,
        #              "end": last wash-end event timestamp, "fired": when mi_done was last sent}
        self.runs = {}
        # scale id -> {"seen": {event entity: last event time}, "days": {ISO date: kg},
        #              "last": [ts, kg], "prev": [ts, kg]}. Absolute weights stay on the hub.
        self.scales = load_state("scale.json")

    @staticmethod
    def base(d):
        return {"id": d["id"], "kind": d["kind"], "name_zh": d.get("name_zh", d["id"])[:23],
                "name_en": d.get("name_en", d["id"])[:23], "online": False}

    def read(self, d):
        v = self.base(d)
        if d["kind"] in ("lamp", "switch"):
            st = self.ha.state(d["entity"])
            v["online"] = st["state"] in ("on", "off")
            v["on"] = st["state"] == "on"
            b = st.get("attributes", {}).get("brightness")
            if d["kind"] == "lamp" and isinstance(b, (int, float)):
                v["bri"] = max(1, round(b * 100 / 255))
            return v
        if d["kind"] == "dispenser":
            raw = str(self.ha.state(d["status"]).get("state", ""))
            v["online"] = raw not in ("unavailable", "unknown", "")
            v["state"] = mi_washer_state(raw, d.get("states", {}), MI_DISPENSER_WORDS) if v["online"] else "other"
            for key in ("temp", "tds"):
                if d.get(key):
                    v[key] = mi_number(self.ha.state(d[key]))
            v["filters"] = [mi_number(self.ha.state(e)) for e in d.get("filters", [])[:2]]
            return v
        if d["kind"] == "scale":
            st = self.ha.state(d["weight_event"])
            v["online"] = st.get("state") != "unavailable"
            self.track_weighing(d, st)
            if d.get("offline_event"):  # weighings stored while no gateway was around
                self.track_weighing(d, self.ha.state(d["offline_event"]))
            v.update(self.scale_view(d["id"]))
            return v
        st = self.ha.state(d["status"])
        raw = str(st.get("state", ""))
        v["online"] = raw not in ("unavailable", "unknown", "")
        state = mi_washer_state(raw, d.get("states", {})) if v["online"] else "other"
        if state == "wash" and d.get("stage"):
            stage = mi_washer_state(str(self.ha.state(d["stage"]).get("state", "")), d.get("states", {}))
            if stage in ("rinse", "spin", "dry"):
                state = stage
        v["state"] = state
        if d.get("left"):
            v["left_min"] = mi_minutes(self.ha.state(d["left"]))
        if d.get("end_event"):  # HA event entity: its state is the time of the last "wash finished"
            v["_end"] = str(self.ha.state(d["end_event"]).get("state", ""))
        return v

    def track_washer(self, d, v):
        run = self.runs.setdefault(d["id"], {"state": None, "total": 0, "end": None, "fired": 0})
        prev, state, left = run["state"], v.get("state"), v.get("left_min")
        end = v.pop("_end", None)
        if not v["online"]:
            return
        finished = False
        # 1) the device's own "wash finished" event (first reading is only a baseline)
        if end not in (None, "", "unknown", "unavailable"):
            finished = run["end"] is not None and end != run["end"]
            run["end"] = end
        elif run["end"] is None:
            run["end"] = end or ""
        # 2) running -> done (or -> idle/off with no time left, i.e. not cancelled half way)
        if state in MI_RUNNING:
            if prev not in MI_RUNNING:
                run["total"] = 0
            if left:
                run["total"] = max(run["total"], left)
            if run["total"] and left is not None:
                v["pct"] = max(0, min(100, round(100 * (1 - left / run["total"]))))
        elif prev in MI_RUNNING and (state == "done" or (state in ("idle", "off") and (left is None or left <= 1))):
            finished = True
        if finished and time.time() - run["fired"] > 600:  # both signals arrive: notify once
            run["fired"] = time.time()
            with self.hub.lock:
                self.hub.add_event("mi_done", d["id"])
        run["state"] = state

    def track_weighing(self, d, st):
        """A new event on a scale's event entity -> one weighing in the history."""
        ent, stamp = st.get("entity_id", ""), str(st.get("state", ""))
        rec = self.scales.setdefault(d["id"], {})
        seen = rec.setdefault("seen", {})
        if stamp in ("", "unknown", "unavailable") or seen.get(ent) == stamp:
            return
        seen[ent] = stamp
        kg = decode_weight(st.get("attributes", {}), d)
        if kg is None:
            log("mihome: scale event without a readable weight", d["id"])
        else:
            ts = ha_time(stamp) or time.time()
            days = rec.setdefault("days", {})
            day = cn_date(ts).isoformat()
            last = rec.get("last")
            if not last or ts >= last[0]:
                if last:
                    rec["prev"] = last
                rec["last"] = [ts, round(kg, 2)]
                days[day] = round(kg, 2)
            else:  # an older offline record: only fills a missing day
                days.setdefault(day, round(kg, 2))
            for old in sorted(days)[:-SCALE_KEEP_DAYS]:
                del days[old]
            log("mihome: weighing recorded", d["id"])
        save_state("scale.json", self.scales)

    def scale_view(self, dev_id):
        """Relative numbers only (0.1 kg units): the device never sees the weight itself."""
        rec = self.scales.get(dev_id, {})
        last, prev, days = rec.get("last"), rec.get("prev"), rec.get("days", {})
        if not last:
            return {}
        today = cn_date()
        vals = [days.get((today - datetime.timedelta(days=29 - i)).isoformat()) for i in range(30)]
        base = next((x for x in vals if x is not None), None)
        out = {"at": int(last[0])}
        if base is not None:
            out["hist"] = [None if x is None else round((x - base) * 10) for x in vals]
            out["d_30"] = round((last[1] - base) * 10)
        if prev:
            out["d_last"] = round((last[1] - prev[1]) * 10)
        return out

    def poll(self):
        view, ok = {}, True
        for d in self.devices:
            try:
                v = self.read(d)
                if d["kind"] == "washer":
                    self.track_washer(d, v)
            except (HAError, OSError, ValueError, KeyError, http.client.HTTPException) as e:
                ok = False
                v = self.base(d)
                if self.ok:
                    log("mihome: read failed", d["id"], type(e).__name__, e)
            view[d["id"]] = v
        with self.lock:
            if ok != self.ok:
                log("mihome:", "Home Assistant reachable" if ok else "Home Assistant unreachable")
            self.view, self.ok = view, ok

    def loop(self):
        while True:
            self.poll()
            time.sleep(MI_POLL_S)

    def snapshot(self):
        with self.lock:
            return {"ok": self.ok, "devices": [dict(self.view[d["id"]]) for d in self.devices]}

    def control(self, dev, on, bri):
        d = self.by_id.get(dev)
        if not d or d["kind"] not in ("lamp", "switch"):
            raise ValueError("unknown device")
        domain = d["entity"].split(".", 1)[0]
        data = {"entity_id": d["entity"]}
        if on is False:
            self.ha.service(domain, "turn_off", data)
        else:
            if bri is not None and domain == "light":
                data["brightness_pct"] = max(1, min(100, int(bri)))
            self.ha.service(domain, "turn_on", data)
        want = on is not False
        v = None
        for _ in range(8):  # the Mi cloud round trip usually takes about a second
            time.sleep(0.5)
            v = self.read(d)
            if v.get("on") == want and (bri is None or not want or abs(v.get("bri", bri) - bri) <= 2):
                break
        log("mihome: control", dev, "on" if want else "off", bri if bri is not None else "",
            "->", "on" if v.get("on") else "off")
        with self.lock:
            self.view[dev] = v
        return self.snapshot()


CELL_RE = re.compile(r'<td[^>]*?data-date="(\d{4}-\d\d-\d\d)"[^>]*?id="([^"]+)"[^>]*?data-level="(\d)"')
TIP_RE = re.compile(r'<tool-tip[^>]*?for="([^"]+)"[^>]*>([^<]*)</tool-tip>')


def fetch_contrib(user):
    """Public contributions calendar (no token).
    Returns (heatmap dict, {ISO date: count} for the last week) or None."""
    req = urllib.request.Request(f"https://github.com/users/{user}/contributions",
                                 headers={"User-Agent": "esp-dash-hub"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        html = resp.read().decode("utf-8", "replace")
    counts = {}
    for target, text in TIP_RE.findall(html):
        m = re.match(r"\s*(\d+) contribution", text)
        counts[target] = int(m.group(1)) if m else 0
    days = {}
    for date, cell_id, level in CELL_RE.findall(html):
        days[date] = (int(level), counts.get(cell_id, 0))
    if not days:
        return None
    last = max(days)
    today = datetime.date.fromisoformat(last)
    week_start = today - datetime.timedelta(days=(today.weekday() + 1) % 7)  # Sunday
    start = week_start - datetime.timedelta(weeks=CONTRIB_DAYS // 7 - 1)
    levels, week = [], 0
    for i in range(CONTRIB_DAYS):
        d = start + datetime.timedelta(days=i)
        lv, n = days.get(d.isoformat(), (0, 0))
        levels.append(str(lv) if d <= today else "0")
        if week_start <= d <= today:
            week += n
    since = (today - datetime.timedelta(days=LEDGER_DAYS)).isoformat()
    recent = {d: n for d, (_, n) in days.items() if d > since}
    return {"user": user, "levels": "".join(levels), "week": week, "today": days[last][1]}, recent


def parse_time(s):
    if not s:
        return 0
    return int(time.mktime(time.strptime(s, "%Y-%m-%dT%H:%M:%SZ")) - time.timezone)


class TLSServer(ThreadingHTTPServer):
    """TLS handshake runs in the per-connection thread with a timeout, so a
    stalled or scanning client can never block the accept loop."""
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, addr, handler, ctx):
        super().__init__(addr, handler)
        self.ctx = ctx
        self.slots = threading.BoundedSemaphore(32)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def finish_request(self, request, client_address):
        request.settimeout(10)
        try:
            tls = self.ctx.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return
        try:
            self.RequestHandlerClass(tls, client_address, self)
        finally:
            try:
                tls.close()
            except OSError:
                pass


def make_handler(hub, device_token, agent_tokens):
    class Handler(BaseHTTPRequestHandler):
        server_version = "esp-dash"
        sys_version = ""
        timeout = 10

        def log_message(self, fmt, *a):
            pass

        def bearer(self):
            h = self.headers.get("Authorization", "")
            return h[7:].strip() if h.startswith("Bearer ") else ""

        def send_json(self, code, obj):
            body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/healthz":
                return self.send_json(200, {"ok": True})
            if self.path != "/api/device/status":
                return self.send_json(404, {"error": "not found"})
            if not hmac.compare_digest(self.bearer().encode(), device_token.encode()):
                return self.send_json(401, {"error": "unauthorized"})
            self.send_json(200, hub.snapshot())

        def read_body(self):
            """JSON object body or None."""
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= MAX_BODY:
                    raise ValueError
                payload = json.loads(self.rfile.read(length))
                return payload if isinstance(payload, dict) else None
            except (ValueError, json.JSONDecodeError):
                return None

        def do_POST(self):
            if self.path == "/api/device/mi":
                return self.mi_control()
            if self.path != "/api/agent/report":
                return self.send_json(404, {"error": "not found"})
            token = self.bearer().encode()
            name = next((n for n, t in agent_tokens.items()
                         if hmac.compare_digest(token, t.encode())), None)
            if not name:
                return self.send_json(401, {"error": "unauthorized"})
            payload = self.read_body()
            if payload is None:
                return self.send_json(400, {"error": "bad request"})
            payload["name"] = name  # the token decides the name, not the body
            hub.report(payload)
            self.send_json(200, {"ok": True})

        def mi_control(self):
            if not hmac.compare_digest(self.bearer().encode(), device_token.encode()):
                return self.send_json(401, {"error": "unauthorized"})
            if not hub.mihome:
                return self.send_json(404, {"error": "mihome not configured"})
            body = self.read_body()
            on, bri = (body or {}).get("on"), (body or {}).get("bri")
            if (body is None or not isinstance(body.get("dev"), str)
                    or (on is not None and not isinstance(on, bool))
                    or (bri is not None and (isinstance(bri, bool) or not isinstance(bri, int)))
                    or (on is None and bri is None)):
                return self.send_json(400, {"error": "bad request"})
            try:
                snap = hub.mihome.control(body["dev"], on, bri)
            except ValueError:
                return self.send_json(400, {"error": "unknown device"})
            except (HAError, OSError, KeyError, http.client.HTTPException) as e:
                log("mihome: control failed", body["dev"], type(e).__name__, e)
                return self.send_json(502, {"error": "home assistant"})
            self.send_json(200, {"mihome": snap})

    return Handler


def run_hub(args):
    creds = cred_dir(args)
    device_token = read_cred(creds, "device_token")
    agent_tokens = json.loads(read_cred(creds, "agent_tokens.json"))
    github_token = read_cred(creds, "github_token", required=False)
    name = os.environ.get("HUB_NAME", "aliyun-hub")
    names = [n.strip() for n in os.environ.get("SERVERS", name).split(",") if n.strip()]
    if name not in names:
        names.insert(0, name)
    hub = Hub(os.environ.get("REPO", "ISBF-Lab/ISBFweb"), names, github_token)
    metrics = Metrics(os.environ.get("SERVICES", ""))
    metrics.collect(name)  # prime the CPU sample

    def github_loop():
        period = int(os.environ.get("GITHUB_PERIOD", "90"))
        user = os.environ.get("GITHUB_USER", "")
        next_contrib = 0
        while True:
            hub.poll_github()
            if user and time.time() >= next_contrib:
                try:
                    c = fetch_contrib(user)
                    if c:
                        with hub.lock:
                            hub.contrib, hub.commits = c
                    next_contrib = time.time() + 1800
                except (urllib.error.URLError, OSError, ValueError) as e:
                    log("contrib error", e)
                    next_contrib = time.time() + 300
            time.sleep(period)

    def self_loop():
        while True:
            time.sleep(30)
            try:
                hub.report(metrics.collect(name))
            except OSError as e:
                log("metrics error", e)

    hub.report(metrics.collect(name))
    threading.Thread(target=github_loop, daemon=True).start()
    threading.Thread(target=self_loop, daemon=True).start()

    ha_token = read_cred(creds, "ha_token", required=False)
    mi_conf = read_cred(creds, "mihome.json", required=False)
    if ha_token and mi_conf:
        try:
            hub.mihome = MiHome(hub, HomeAssistant(ha_token, port=int(os.environ.get("HA_PORT", "8123"))),
                                json.loads(mi_conf))
        except (ValueError, TypeError, AttributeError) as e:
            log("mihome: bad mihome.json", e)
        else:
            threading.Thread(target=hub.mihome.loop, daemon=True).start()
            log(f"mihome: {len(hub.mihome.devices)} device(s) via Home Assistant")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(creds / "hub.pem", creds / "hub.key")
    port = int(os.environ.get("PORT", "8443"))
    server = TLSServer((os.environ.get("BIND", "0.0.0.0"), port), make_handler(hub, device_token, agent_tokens), ctx)
    log(f"hub listening on {port}, repo {hub.repo}, servers {names}")
    server.serve_forever()


# ---------------------------------------------------------------- agent

def post_report(hub_url, ca_file, token, payload):
    host_port = hub_url.split("//", 1)[1].split("/", 1)[0]
    host, _, port = host_port.partition(":")
    ctx = ssl.create_default_context(cafile=str(ca_file))

    class Conn(http.client.HTTPSConnection):
        def connect(self):
            sock = socket.create_connection((host, int(port or 443)), timeout=10)
            self.sock = ctx.wrap_socket(sock, server_hostname=HUB_CN)  # verify the pinned name

    conn = Conn(host, int(port or 443), timeout=10)
    body = json.dumps(payload).encode()
    conn.request("POST", "/api/agent/report", body=body, headers={
        "Authorization": f"Bearer {token}", "Content-Type": "application/json"})
    resp = conn.getresponse()
    resp.read()
    conn.close()
    return resp.status


def run_agent(args):
    creds = cred_dir(args)
    token = read_cred(creds, "agent_token")
    hub_url = os.environ.get("HUB_URL", "https://8.138.138.222:8443")
    name = os.environ.get("AGENT_NAME", socket.gethostname())
    metrics = Metrics(os.environ.get("SERVICES", ""))
    metrics.collect(name)
    period = int(os.environ.get("PERIOD", "30"))
    log(f"agent {name} -> {hub_url} every {period}s")
    failures = 0
    while True:
        time.sleep(period if failures == 0 else min(period, 15))
        try:
            status = post_report(hub_url, creds / "hub.pem", token, metrics.collect(name))
            if status != 200:
                log("hub answered", status)
            failures = 0 if status == 200 else failures + 1
        except (OSError, ssl.SSLError, http.client.HTTPException) as e:
            failures += 1
            if failures in (1, 10) or failures % 100 == 0:
                log("report failed", type(e).__name__, e)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=("hub", "agent", "once"))
    p.add_argument("--cred-dir", help="directory with credentials (default: $CREDENTIALS_DIRECTORY)")
    args = p.parse_args()
    if args.mode == "once":
        m = Metrics(os.environ.get("SERVICES", ""))
        m.collect("x")
        time.sleep(0.5)
        print(json.dumps(m.collect(os.environ.get("AGENT_NAME", socket.gethostname())), indent=2))
        return
    (run_hub if args.mode == "hub" else run_agent)(args)


if __name__ == "__main__":
    main()
