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
import math
import queue
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
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))  # trusted sibling module, also under python -I
from ota import OTAStore, HEX64

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
        self.flights = None  # FlightRadar; independent from dashboard freshness/events
        self.calendar = None  # iCloud via HA; independent from dashboard freshness
        self.ota = OTAStore()

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


CALENDAR_BATCH = 8
CALENDAR_MAX_EVENTS = 4096


def calendar_day(date):
    return (date - datetime.date(1970, 1, 1)).days


def calendar_text(value, limit):
    """Bound UTF-8 bytes, including the ellipsis, without splitting a character."""
    value = " ".join(value.split()) if isinstance(value, str) else ""
    raw = value.encode("utf-8")
    return value if len(raw) <= limit else raw[:limit - 3].decode("utf-8", "ignore") + "…"


class CalendarHub:
    """Read a single HA CalDAV config entry, then serve small in-memory snapshots.

    Calendar events never enter Hub.add_event(), the ledger, or persistent logs.
    Only a complete successful poll replaces the cache, so a failed calendar
    cannot turn a real day's appointments into a misleading empty list.
    """

    def __init__(self, ha, config, clock=time.time):
        title = config.get("entry_title", "Kiki iCloud")
        if not isinstance(title, str) or not title.strip() or len(title) > 100:
            raise ValueError("invalid calendar entry title")
        self.ha, self.title, self.clock = ha, title, clock
        self.lock = threading.Lock()
        self.events = []
        self.configured = self.ok = self.error = False
        self.fetched_at = 0
        self.window_start = self.window_end = 0

    @staticmethod
    def event_time(value):
        if not isinstance(value, dict):
            raise ValueError("invalid calendar time")
        if "date" in value:
            d = datetime.date.fromisoformat(value["date"])
            return int(datetime.datetime.combine(d, datetime.time(), CN_TZ).timestamp()), True
        t = datetime.datetime.fromisoformat(value["dateTime"].replace("Z", "+00:00"))
        if t.tzinfo is None:
            t = t.replace(tzinfo=CN_TZ)
        return int(t.timestamp()), False

    def poll(self):
        today = cn_date(self.clock())
        start = datetime.datetime.combine(today, datetime.time(), CN_TZ)
        end = start + datetime.timedelta(days=7)
        # integration_entities accepts a config-entry title. Do not accidentally
        # aggregate unrelated calendars (or another CalDAV account) in HA.
        template = ("{{ integration_entities(" + json.dumps(self.title) + ")"
                    " | select('match', '^calendar[.]') | list | tojson }}")
        try:
            entities = self.ha.call("POST", "/api/template", {"template": template})
            if not isinstance(entities, list) or not all(
                    isinstance(e, str) and ENTITY_RE.fullmatch(e) and e.startswith("calendar.")
                    for e in entities):
                raise ValueError("invalid calendar entities")
            if not entities:
                with self.lock:
                    # A temporarily missing/reloading entry must not destroy
                    # a previously working snapshot.
                    self.ok = False
                    self.error = self.configured
                return False
            with self.lock:
                self.configured = True
            names = self.ha.call("GET", "/api/calendars")
            if not isinstance(names, list):
                raise ValueError("invalid calendar list")
            names = {e["entity_id"]: e.get("name", e["entity_id"]) for e in names}
            query = urllib.parse.urlencode({"start": start.isoformat(), "end": end.isoformat()})
            events, seen = [], set()
            for entity in sorted(set(entities)):
                data = self.ha.call("GET", f"/api/calendars/{entity}?{query}")
                if not isinstance(data, list):
                    raise ValueError("invalid event list")
                for event in data:
                    if (event.get("status") or "").lower() in ("cancelled", "canceled"):
                        continue
                    a, all_day = self.event_time(event["start"])
                    b, end_all_day = self.event_time(event["end"])
                    if b <= a or all_day != end_all_day:
                        raise ValueError("invalid event interval")
                    if b <= int(start.timestamp()) or a >= int(end.timestamp()):
                        continue
                    if uid := event.get("uid"):
                        key = (entity, uid, a)
                        if key in seen:
                            continue
                        seen.add(key)
                    events.append({"start": a, "end": b, "all_day": all_day,
                                   "title": calendar_text(event.get("summary"), 96),
                                   "calendar": calendar_text(names.get(entity, entity), 48)})
                    if len(events) > CALENDAR_MAX_EVENTS:
                        raise ValueError("calendar capacity exceeded")
            events.sort(key=lambda e: (not e["all_day"], e["start"], e["end"], e["calendar"], e["title"]))
        except (HAError, OSError, ValueError, TypeError, KeyError, AttributeError):
            with self.lock:
                self.ok = False
                self.error = True
            log("calendar: update failed")
            return False
        with self.lock:
            self.events = events
            self.configured = self.ok = True
            self.error = False
            self.fetched_at = int(self.clock())
            self.window_start, self.window_end = int(start.timestamp()), int(end.timestamp())
        log("calendar: updated", len(entities), "calendars,", len(events), "events")
        return True

    @staticmethod
    def query(day, offset, now):
        today = cn_date(now)
        selected = today if day is None else datetime.date.fromisoformat(day)
        if not today <= selected < today + datetime.timedelta(days=7):
            raise ValueError("day outside seven-day window")
        if not 0 <= offset <= CALENDAR_MAX_EVENTS or offset % CALENDAR_BATCH:
            raise ValueError("invalid calendar offset")
        return today, selected

    def snapshot(self, day=None, offset=0):
        now = self.clock()
        today, selected = self.query(day, offset, now)
        lo = int(datetime.datetime.combine(today, datetime.time(), CN_TZ).timestamp())
        chosen = int(datetime.datetime.combine(selected, datetime.time(), CN_TZ).timestamp())
        with self.lock:
            counts = [sum(e["start"] < lo + (i+1)*86400 and e["end"] > lo+i*86400
                          for e in self.events) for i in range(7)]
            events = [e for e in self.events if e["start"] < chosen+86400 and e["end"] > chosen]
            return {"schema": 1, "configured": self.configured,
                    "ok": self.ok and self.window_start <= lo and self.window_end >= lo+7*86400,
                    "error": self.error,
                    "fetched_at": self.fetched_at, "week_start": calendar_day(today),
                    "counts": counts, "day": calendar_day(selected), "offset": offset,
                    "total": len(events), "events": events[offset:offset+CALENDAR_BATCH]}

    def loop(self):
        while True:
            ok = self.poll()
            now = self.clock()
            midnight = datetime.datetime.combine(cn_date(now)+datetime.timedelta(days=1),
                                                 datetime.time(), CN_TZ).timestamp()
            time.sleep(max(1, min(900 if ok else 60, midnight-now)))


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
            st = self.ha.state(d["left"])
            v["left_min"] = mi_minutes(st)
            # when that value was reported: the washer goes quiet for most of a cycle (it
            # stopped at "71 min" for 80 minutes), so the finish time is counted from here
            v["_left_at"] = ha_time(st.get("last_changed"))
        if d.get("end_event"):  # HA event entity: its state is the time of the last "wash finished"
            v["_end"] = str(self.ha.state(d["end_event"]).get("state", ""))
        return v

    def track_washer(self, d, v):
        run = self.runs.setdefault(d["id"], {"state": None, "total": 0, "end": None, "fired": 0})
        prev, state, left = run["state"], v.get("state"), v.get("left_min")
        end, left_at = v.pop("_end", None), v.pop("_left_at", None)
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
            if left is not None and left_at and state != "pause":
                v["eta"] = int(left_at + left * 60)  # expected finish (epoch s); the card shows this
                left = max(0.0, (v["eta"] - time.time()) / 60)
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


# ---------------------------------------------------------------- overhead flights (personal, non-commercial)
# Position data: https://github.com/adsbfi/opendata (attribution required).
# Reference routes: https://www.adsbdb.com/ . Routes are not a live flight plan.

def flight_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def flight_wgs84(lon, lat, coord_type):
    """Normalize a configured point. BD-09 inverse is approximate, ample for a 5 km circle.

    Conversion formula adapted from wandergis/coordtransform (MIT).
    https://github.com/wandergis/coordtransform
    """
    if not flight_number(lon) or not flight_number(lat) or not (-180 <= lon <= 180 and -90 <= lat <= 90):
        raise ValueError("invalid location")
    if coord_type == "wgs84":
        return float(lon), float(lat)
    if coord_type not in ("bd09", "gcj02"):
        raise ValueError("coord_type must be wgs84, bd09 or gcj02")
    if coord_type == "bd09":
        x, y = lon - .0065, lat - .006
        z = math.hypot(x, y) - .00002 * math.sin(y * math.pi * 3000 / 180)
        theta = math.atan2(y, x) - .000003 * math.cos(x * math.pi * 3000 / 180)
        lon, lat = z * math.cos(theta), z * math.sin(theta)
    if not (73.66 < lon < 135.05 and 3.86 < lat < 53.55):
        return lon, lat

    def offset(xlon, xlat):
        x, y = xlon - 105, xlat - 35
        da = -100 + 2*x + 3*y + .2*y*y + .1*x*y + .2*math.sqrt(abs(x))
        da += (20*math.sin(6*x*math.pi) + 20*math.sin(2*x*math.pi))*2/3
        da += (20*math.sin(y*math.pi) + 40*math.sin(y*math.pi/3))*2/3
        da += (160*math.sin(y*math.pi/12) + 320*math.sin(y*math.pi/30))*2/3
        db = 300 + x + 2*y + .1*x*x + .1*x*y + .1*math.sqrt(abs(x))
        db += (20*math.sin(6*x*math.pi) + 20*math.sin(2*x*math.pi))*2/3
        db += (20*math.sin(x*math.pi) + 40*math.sin(x*math.pi/3))*2/3
        db += (150*math.sin(x*math.pi/12) + 300*math.sin(x*math.pi/30))*2/3
        r = math.radians(xlat)
        m = 1 - .00669342162296594323 * math.sin(r)**2
        s = math.sqrt(m)
        return (db*180/(6378245/s*math.cos(r)*math.pi),
                da*180/((6378245*(1-.00669342162296594323))/(m*s)*math.pi))

    wlon, wlat = lon, lat
    for _ in range(8):
        dx, dy = offset(wlon, wlat)
        wlon -= wlon + dx - lon
        wlat -= wlat + dy - lat
    return wlon, wlat


def flight_geometry(lon0, lat0, lon, lat):
    p, q, dl = math.radians(lat0), math.radians(lat), math.radians(lon-lon0)
    h = math.sin((q-p)/2)**2 + math.cos(p)*math.cos(q)*math.sin(dl/2)**2
    distance = 6371008.8 * 2 * math.asin(min(1, math.sqrt(max(0, h))))
    bearing = math.degrees(math.atan2(math.sin(dl)*math.cos(q),
                                     math.cos(p)*math.sin(q)-math.sin(p)*math.cos(q)*math.cos(dl))) % 360
    return int(round(distance)), int(round(bearing)) % 360


def flight_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Kiki-desk-dashboard/1.0 (personal)",
                                              "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=6) as resp:
        # A bounded upstream body, never passed raw to the microcontroller.
        raw = resp.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("flight response too large")
    return json.loads(raw)


class FlightRadar:
    """One position worker + one metadata worker; requests only read cached snapshots."""
    AIRLINER = re.compile(r"^(?:A[23456]\d\d|A1[89]N|A2[01]N|B[378]\d\d|B7\d[A-Z]|B38M|B39M|B3XM|C919|C909|ARJ|E1[4579]\d|E2\d\d|CRJ|AT[47]|DH8|MD[189]|DC10|IL96|BLCF|A3ST)")
    CALLSIGN = re.compile(r"^[A-Z]{3}[0-9][A-Z0-9]{0,4}$")

    def __init__(self, config, get_json=flight_json, clock=time.time):
        self.lon, self.lat = flight_wgs84(config["lon"], config["lat"], config.get("coord_type", "wgs84"))
        self.radius = config.get("radius_m", 20000)
        self.alert_radius = config.get("alert_radius_m", 5000)
        if (not flight_number(self.radius) or not flight_number(self.alert_radius)
                or not 1000 <= self.alert_radius <= self.radius <= 100000):
            raise ValueError("invalid flight radii")
        self.place = str(config.get("place", "Desk"))[:16]
        self.get_json, self.clock = get_json, clock
        self.lock = threading.Lock()
        self.jobs = queue.Queue(maxsize=32)
        self.pending, self.meta = set(), {}
        self.aircraft, self.events = [], []
        saved = load_state("flights.json")
        self.tracks = {k: v for k, v in saved.get("tracks", {}).items() if isinstance(v, dict)}
        self.last_id = int(saved.get("last_id", 0))
        self.baseline = False
        self.fetched_at = 0
        self.ok = False
        self.retry_s = 10

    def metadata(self, callsign):
        """Called under lock. Missing metadata never stalls a position poll."""
        if not self.CALLSIGN.fullmatch(callsign):
            return {}
        rec = self.meta.get(callsign)
        if (not rec or rec[0] <= self.clock()) and callsign not in self.pending:
            try:
                self.jobs.put_nowait(callsign)
                self.pending.add(callsign)
            except queue.Full:
                pass
        return dict(rec[1]) if rec else {}

    def enrich(self, callsign):
        route, airline = {}, {}
        try:
            response = self.get_json("https://api.adsbdb.com/v0/callsign/" + callsign).get("response", {})
            route = response.get("flightroute") or {}
            airline = route.get("airline") or {}
        except urllib.error.HTTPError as e:
            if e.code != 404:
                raise
        if not airline:
            response = self.get_json("https://api.adsbdb.com/v0/airline/" + callsign[:3]).get("response", [])
            airline = response[0] if isinstance(response, list) and response else {}
        out = {"airline": str(airline.get("name") or "")[:32]}
        flight = str(route.get("callsign_iata") or "")
        # Alphanumeric ATC callsigns are not necessarily public flight numbers.
        out["flight"] = flight if re.fullmatch(r"(?:[A-Z][A-Z0-9]|[0-9][A-Z])\d{1,4}", flight) else ""
        for end in ("origin", "destination"):
            airport = route.get(end) or {}
            out[end] = str(airport.get("iata_code") or airport.get("icao_code") or "")[:4]
            out[end + "_city"] = str(airport.get("municipality") or airport.get("name") or "")[:32]
        return out

    def enrich_loop(self):
        while True:
            callsign = self.jobs.get()
            try:
                out = self.enrich(callsign)
                ttl = 21600 if out.get("origin") and out.get("destination") else 900
            except (OSError, ValueError, TypeError, AttributeError, http.client.HTTPException):
                out, ttl = {}, 60
            with self.lock:
                self.meta[callsign] = (self.clock() + ttl, out)
                self.pending.discard(callsign)
                if len(self.meta) > 512:
                    oldest = min(self.meta, key=lambda k: self.meta[k][0])
                    del self.meta[oldest]
            self.jobs.task_done()

    def ingest(self, data):
        now = self.clock()
        stamp = data.get("now")
        if not flight_number(stamp) or not isinstance(data.get("ac"), list):
            raise ValueError("invalid position response")
        stamp = stamp / 1000 if stamp > 100000000000 else stamp
        if not -5 <= now - stamp <= 30:
            raise ValueError("stale position response")
        aircraft = []
        for a in data["ac"]:
            if not isinstance(a, dict):
                continue
            lon, lat, age = a.get("lon"), a.get("lat"), a.get("seen_pos")
            hexid, model = str(a.get("hex", "")).lower(), str(a.get("t") or "")[:8]
            flags = a.get("dbFlags", 0)
            category = str(a.get("category", ""))
            if (not all(flight_number(v) for v in (lon, lat, age)) or not -180 <= lon <= 180
                    or not -90 <= lat <= 90 or age < 0 or now - stamp + age > 30
                    or not re.fullmatch(r"[0-9a-f]{6}", hexid)
                    or a.get("alt_baro") == "ground" or category in ("A7", "B1", "B2", "B3", "B4", "B6", "C1", "C2", "C3")
                    or (isinstance(flags, int) and flags & 1)):
                continue
            distance, bearing = flight_geometry(self.lon, self.lat, lon, lat)
            if distance > self.radius:
                continue
            callsign = str(a.get("flight") or "").strip().upper()[:8]
            altitude, heading = a.get("alt_baro"), a.get("track")
            f = {"hex": hexid, "callsign": callsign, "registration": str(a.get("r") or "")[:12],
                 "model": model, "observed_at": int(stamp-age), "distance_m": distance,
                 "bearing": bearing, "heading": int(heading) % 360 if flight_number(heading) else -1,
                 "altitude_m": int(round(altitude*.3048)) if flight_number(altitude) else -32768}
            with self.lock:
                f.update(self.metadata(callsign))
            f["eligible"] = bool(self.CALLSIGN.fullmatch(callsign) and self.AIRLINER.match(model)
                                 and f.get("airline") and flight_number(altitude))
            aircraft.append(f)
        aircraft.sort(key=lambda f: (f["distance_m"], f["hex"]))
        changed = False
        with self.lock:
            for f in aircraft:
                key = f["hex"] + ":" + f["callsign"]
                inside = f["distance_m"] <= self.alert_radius
                rec = self.tracks.setdefault(key, {"armed": not (inside and not self.baseline),
                                                   "last_alert": 0, "far_since": 0})
                rec["seen"] = now
                if f["distance_m"] > self.alert_radius + 2000:
                    if not rec.get("far_since"):
                        rec["far_since"] = now
                    if now - rec["far_since"] >= 120:
                        rec["armed"] = True
                else:
                    rec["far_since"] = 0
                if (self.baseline and inside and f["eligible"] and now-f["observed_at"] <= 15
                        and rec.get("armed") and now-rec.get("last_alert", 0) >= 1800):
                    self.last_id = max(self.last_id+1, int(now*1000))
                    self.events.append({"id": self.last_id, "at": int(now), "flight": dict(f)})
                    self.events = self.events[-3:]
                    rec.update(armed=False, last_alert=now)
                    changed = True
                    log("flight: nearby", f["flight"] if f.get("flight") else f["callsign"], f["distance_m"], "m")
            self.tracks = {k: v for k, v in self.tracks.items() if now-v.get("seen", 0) < 86400}
            self.baseline, self.aircraft, self.fetched_at, self.ok = True, aircraft[:8], int(stamp), True
            if changed:
                save_state("flights.json", {"last_id": self.last_id, "tracks": self.tracks})

    def poll(self):
        url = (f"https://opendata.adsb.fi/api/v3/lat/{self.lat:.6f}/lon/{self.lon:.6f}"
               f"/dist/{math.ceil(self.radius/1852)}")
        self.ingest(self.get_json(url))

    def loop(self):
        failures = 0
        while True:
            delay = 10
            try:
                self.poll()
                failures = 0
            except (OSError, ValueError, TypeError, AttributeError, http.client.HTTPException) as e:
                failures += 1
                delay = min(300, 30 * 2**min(failures-1, 4))
                if isinstance(e, urllib.error.HTTPError) and e.code == 429:
                    retry = e.headers.get("Retry-After", "")
                    if retry.isdigit():
                        delay = max(delay, min(3600, int(retry)))
                with self.lock:
                    self.ok = False
                if failures == 1 or failures % 10 == 0:
                    log("flights: position fetch failed", type(e).__name__)
            time.sleep(delay)

    def snapshot(self):
        with self.lock:
            out = {"schema": 1, "configured": True, "ok": self.ok, "source": "adsb.fi",
                   "place": self.place, "radius_m": int(self.radius), "alert_radius_m": int(self.alert_radius),
                   "fetched_at": self.fetched_at, "aircraft": [dict(f) for f in self.aircraft],
                   "events": [dict(e, flight=dict(e["flight"])) for e in self.events if self.clock()-e["at"] <= 60]}
        # Keep the firmware's 8 KiB buffer invariant even for worst-case UTF-8 city names.
        while len(json.dumps(out, ensure_ascii=False, separators=(",", ":")).encode()) > 8000:
            if out["aircraft"]:
                out["aircraft"].pop()
            elif out["events"]:
                out["events"].pop(0)
            else:
                break
        return out


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
            path = urllib.parse.urlsplit(self.path)
            if path.path.startswith("/api/device/ota/images/"):
                return self.ota_image(path.path)
            is_calendar = path.path == "/api/device/calendar"
            if not is_calendar and self.path not in ("/api/device/status", "/api/device/flights"):
                return self.send_json(404, {"error": "not found"})
            if not hmac.compare_digest(self.bearer().encode(), device_token.encode()):
                return self.send_json(401, {"error": "unauthorized"})
            if is_calendar:
                try:
                    # Python 3.10 rejects an empty query under these flags;
                    # newer Python versions return {}. Support both hosts.
                    query = urllib.parse.parse_qs(path.query, strict_parsing=True, keep_blank_values=True) if path.query else {}
                    if any(k not in ("day", "offset") or len(v) != 1 for k, v in query.items()):
                        raise ValueError("invalid query")
                    day = query.get("day", [None])[0]
                    offset = int(query.get("offset", ["0"])[0])
                    calendar = hub.calendar or CalendarHub(None, {})
                    return self.send_json(200, calendar.snapshot(day, offset))
                except (ValueError, OverflowError):
                    return self.send_json(400, {"error": "invalid calendar query"})
            if self.path == "/api/device/flights":
                return self.send_json(200, hub.flights.snapshot() if hub.flights else
                                      {"schema": 1, "configured": False})
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
            if self.path in ("/api/device/ota/check", "/api/device/ota/report"):
                if not hmac.compare_digest(self.bearer().encode(), device_token.encode()):
                    return self.send_json(401, {"error": "unauthorized"})
                body = self.read_body()
                try:
                    method = hub.ota.check if self.path.endswith("/check") else hub.ota.report
                    result = method(body)
                except (ValueError, TypeError, KeyError):
                    return self.send_json(400, {"error": "invalid OTA request"})
                except LookupError:
                    return self.send_json(409, {"error": "OTA task unavailable"})
                except OSError:
                    return self.send_json(503, {"error": "OTA store unavailable"})
                return self.send_json(200, result)
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

        def ota_image(self, path):
            if not hmac.compare_digest(self.bearer().encode(), device_token.encode()):
                return self.send_json(401, {"error": "unauthorized"})
            name = path.removeprefix("/api/device/ota/images/")
            digest = name[:-4] if name.endswith(".bin") else ""
            if not HEX64.fullmatch(digest):
                return self.send_json(404, {"error": "release not found"})
            try:
                manifest, image = hub.ota.release(digest)
                f = image.open("rb")
            except (ValueError, OSError):
                return self.send_json(404, {"error": "release not found"})
            with f:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(manifest["size"]))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                try:
                    while chunk := f.read(32768):
                        self.wfile.write(chunk)
                except (OSError, ssl.SSLError):
                    pass  # an interrupted download never changes the running device slot

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
    calendar_conf = read_cred(creds, "calendar.json", required=False)
    if ha_token and calendar_conf:
        try:
            config = json.loads(calendar_conf)
            if config.get("enabled", True):
                hub.calendar = CalendarHub(HomeAssistant(ha_token, port=int(os.environ.get("HA_PORT", "8123"))), config)
                threading.Thread(target=hub.calendar.loop, daemon=True).start()
                log("calendar: enabled via Home Assistant")
        except (ValueError, TypeError, AttributeError):
            log("calendar: bad configuration")
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

    flight_conf = read_cred(creds, "flights.json", required=False)
    if flight_conf:
        try:
            config = json.loads(flight_conf)
            if config.get("enabled", True):
                hub.flights = FlightRadar(config)
                threading.Thread(target=hub.flights.loop, daemon=True).start()
                threading.Thread(target=hub.flights.enrich_loop, daemon=True).start()
                log("flights: enabled (adsb.fi + adsbdb)")
        except (ValueError, TypeError, KeyError, AttributeError) as e:
            log("flights: bad configuration", type(e).__name__)

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
