"""Small Apple Health snapshots through HA. No HealthKit credentials or sample exports.

HA's phone sensors expose values and state/report timestamps, not measurement dates.
The dates below are observation dates; an unchanged weight is never a new weighing.
Absolute weights are private to this module's mode-0600 state file.
"""
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import threading
import time

TZ = dt.timezone(dt.timedelta(hours=8))
POLL_S = 60
SAVE_S = 300
HISTORY_DAYS = 30
WIRE_MAX = 2048
STATE_SCHEMA = 2  # v1 accepted HA restoration writes without phone-report evidence.
REPORTS_ENTITY = "sensor.kiki_health_reports"
ENTITY_RE = re.compile(r"^sensor\.[a-z0-9_]+$")
# Order is shared with dash_health_metric_t. Values use integers on the device.
FIELDS = ("steps", "distance_m", "exercise_min", "active_kcal", "sleep_min",
          "core_min", "deep_min", "rem_min", "body_fat")
DAILY = FIELDS[:4]
LIMITS = dict(zip(FIELDS, (1000000, 2000000, 1440, 50000, 2880, 2880, 2880, 2880, 1000)))
# HA's unique IDs are more stable than translated/generated entity names.
UNIQUE_IDS = {
    "steps": "health_steps", "distance_m": "health_distance_walking_running",
    "exercise_min": "health_apple_exercise_time", "active_kcal": "health_active_energy_burned",
    "sleep_min": "health_sleep_duration", "core_min": "health_sleep_core",
    "deep_min": "health_sleep_deep", "rem_min": "health_sleep_rem",
    "body_fat": "health_body_fat_percentage", "weight_kg": "health_body_mass",
}


def date(ts):
    return dt.datetime.fromtimestamp(ts, TZ).date()


def number(value):
    if isinstance(value, bool):
        return None
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except (TypeError, ValueError, OverflowError):
        return None


def normalized(key, state):
    value = number(state.get("value"))
    if value is None:
        return None
    unit = str(state.get("unit", "")).strip().lower()
    units = ({"steps": 1, "step": 1, "": 1} if key == "steps" else
             {"m": 1, "km": 1000, "mi": 1609.344} if key == "distance_m" else
             {"kcal": 1, "kj": 1/4.184} if key == "active_kcal" else
             {"%": 10} if key == "body_fat" else
             {"kg": 1, "g": .001, "lb": .45359237, "lbs": .45359237} if key == "weight_kg" else
             {"min": 1, "minutes": 1, "s": 1/60, "h": 60, "hours": 60})
    if unit not in units:
        return None
    value *= units[unit]
    if key == "weight_kg":
        return round(value, 3) if 2 <= value <= 300 else None
    return int(round(value)) if 0 <= value <= LIMITS[key] else None


class HealthHub:
    def __init__(self, ha, config, path=None, clock=time.time):
        self.ha, self.clock = ha, clock
        self.path = Path(path) if path else None
        self.lock = threading.Lock()
        self.entities = config.get("entities", {})
        if (not isinstance(self.entities, dict) or len(self.entities) > len(UNIQUE_IDS) or
                any(k not in UNIQUE_IDS or not isinstance(v, str) or not ENTITY_RE.fullmatch(v)
                    for k, v in self.entities.items()) or
                len(set(self.entities.values())) != len(self.entities)):
            raise ValueError("invalid health entity mapping")
        self.configured = bool(config.get("enabled", False) and self.entities and ha)
        self.binding = hashlib.sha256(json.dumps(self.entities, sort_keys=True).encode()).hexdigest()
        self.since = int(clock())
        self.latest, self.days, self.weights = {}, {}, {}
        self.ok, self.fetched_at = False, 0
        # Persist onboarding/the first baseline immediately; later changes are batched.
        self.dirty, self.saved_at = False, self.since-SAVE_S
        if self.configured:
            self._load()

    def _load(self):
        try:
            if not self.path or not self.path.exists() or self.path.stat().st_size > 262144:
                return
            data = json.loads(self.path.read_text())
            if data.get("schema") != STATE_SCHEMA or data.get("binding") != self.binding:
                return
            # Only our small canonical state is restored, never arbitrary HA attributes.
            now = int(self.clock())
            since = number(data.get("since"))
            if since is not None and 0 < since <= now:
                self.since = int(since)
            for key, item in data.get("latest", {}).items():
                if key not in UNIQUE_IDS or not isinstance(item, dict):
                    continue
                v, stamp = item.get("v"), number(item.get("t"))
                if stamp is None or not 0 < stamp <= now+60:
                    continue
                if v is not None and not self._canonical(key, v):
                    continue
                self.latest[key] = {"v": v, "t": int(stamp)}
            for day, row in data.get("days", {}).items():
                try:
                    dt.date.fromisoformat(day)
                except (ValueError, TypeError):
                    continue
                if isinstance(row, dict):
                    self.days[day] = {k: v for k, v in row.items()
                                      if k in UNIQUE_IDS and self._canonical(k, v)}
            for key in ("last", "prev"):
                item = data.get("weights", {}).get(key)
                if (isinstance(item, list) and len(item) == 2 and number(item[0]) is not None
                        and 0 < item[0] <= now+60 and self._canonical("weight_kg", item[1])):
                    self.weights[key] = item
            self._prune(now)
        except (OSError, ValueError, TypeError, AttributeError, OverflowError):
            # Do not put health values or a JSON exception's source into logs.
            self.latest, self.days, self.weights = {}, {}, {}

    @staticmethod
    def _canonical(key, v):
        n = number(v)
        return n is not None and (2 <= n <= 300 if key == "weight_kg" else
                                 0 <= n <= LIMITS[key] and int(n) == n)

    def _prune(self, now):
        lo = max(date(now)-dt.timedelta(days=HISTORY_DAYS-1), date(self.since)).isoformat()
        hi = date(now).isoformat()
        old = [d for d in self.days if not lo <= d <= hi]
        for day in old:
            del self.days[day]
        if old:
            self.dirty = True
        if self.weights.get("prev", [now])[0] < now-HISTORY_DAYS*86400:
            del self.weights["prev"]
            self.dirty = True

    def _save(self, now):
        if not self.dirty or not self.path or now-self.saved_at < SAVE_S:
            return
        data = {"schema": STATE_SCHEMA, "binding": self.binding, "since": self.since,
                "latest": self.latest, "days": self.days, "weights": self.weights}
        tmp = self.path.with_suffix(".tmp")
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                os.fchmod(f.fileno(), 0o600)
                json.dump(data, f, separators=(",", ":"), allow_nan=False)
            tmp.replace(self.path)
        except (OSError, ValueError):
            return  # retry later; never log the payload
        self.dirty, self.saved_at = False, now

    def _template(self):
        entries = []
        reports = json.dumps(REPORTS_ENTITY)
        for key, entity in self.entities.items():
            name = json.dumps(entity)
            entries.append(json.dumps(key)+": {'value': states("+name+"), "
                           "'unit': state_attr("+name+", 'unit_of_measurement') or '', "
                           "'reported_at': as_timestamp(states["+name+"].last_reported, 0) "
                           "if states["+name+"] else 0, "
                           "'phone_reported_at': (state_attr("+reports+", 'reported_at') or {}).get("
                           +name+", 0)}")
        return "{{ {"+", ".join(entries)+"} | tojson }}"

    def poll(self):
        if not self.configured:
            return False
        now = int(self.clock())
        try:
            data = self.ha.call("POST", "/api/template", {"template": self._template()})
            if not isinstance(data, dict) or any(k not in data for k in self.entities):
                raise ValueError("incomplete health response")
            samples = {}
            for key in self.entities:
                item = data[key]
                if not isinstance(item, dict):
                    raise ValueError("invalid health response")
                stamp = number(item.get("reported_at"))
                # mobile_app also writes numeric restored states on startup. Only
                # its update signal proves a phone report; a later HA state write
                # invalidates that proof. Keep the last verified observation/date.
                if (stamp is None or not 0 < stamp <= now+60
                        or number(item.get("phone_reported_at")) != stamp):
                    continue
                samples[key] = {"v": normalized(key, item), "t": int(stamp)}
        except Exception:
            # Transport, HA and template errors all preserve the last good snapshot.
            with self.lock:
                self.ok = False
                self._prune(now)
                self._save(now)
            return False
        with self.lock:
            for key, item in samples.items():
                prev = self.latest.get(key)
                if prev and item["t"] and item["t"] < prev["t"]:
                    continue
                if prev != item:
                    self.latest[key] = item
                    self.dirty = True
                if item["v"] is None:
                    continue
                observed = date(item["t"])
                if key == "weight_kg":
                    last = self.weights.get("last")
                    if last is None or last[1] != item["v"]:
                        if last:
                            self.weights["prev"] = last
                        self.weights["last"] = [now, item["v"]]
                        self.days.setdefault(date(now).isoformat(), {})[key] = item["v"]
                        self.dirty = True
                elif date(self.since) <= observed <= date(now):
                    row = self.days.setdefault(observed.isoformat(), {})
                    if row.get(key) != item["v"]:
                        row[key] = item["v"]
                        self.dirty = True
            self.ok, self.fetched_at = True, now
            self._prune(now)
            self._save(now)
        return True

    def snapshot(self):
        now = int(self.clock())
        today = date(now)
        with self.lock:
            self._prune(now)
            values, stamps = {}, []
            for key in FIELDS:
                item = self.latest.get(key, {})
                value, stamp = item.get("v"), item.get("t", 0)
                if key in DAILY and (not stamp or date(stamp) != today):
                    value = None
                values[key] = value
                stamps.append(stamp)
            activity = [[self.days.get((today-dt.timedelta(days=6-i)).isoformat(), {}).get(k)
                         for k in ("steps", "exercise_min")] for i in range(7)]
            weights = [(i, self.days.get((today-dt.timedelta(days=29-i)).isoformat(), {}).get("weight_kg"))
                       for i in range(30)]
            known = [(i, v) for i, v in weights if v is not None]
            base = known[0][1] if known else 0
            hist = [round((v-base)*10) if v is not None else None for _, v in weights]
            last, prev = self.weights.get("last"), self.weights.get("prev")
            snap = {"schema": 1, "configured": self.configured, "ok": self.ok,
                    "fetched_at": self.fetched_at, "since": self.since if self.configured else 0,
                    "day": (today-dt.date(1970, 1, 1)).days, "values": values,
                    "source_at": stamps, "activity": activity, "weight_hist": hist,
                    "weight_delta": round((last[1]-prev[1])*10) if last and prev else None,
                    "weight_change": round((known[-1][1]-base)*10) if len(known) > 1 else None,
                    "weight_span": known[-1][0]-known[0][0]+1 if known else 0,
                    "weight_at": last[0] if last else 0}
            if len(json.dumps(snap, separators=(",", ":")).encode()) > WIRE_MAX:
                raise ValueError("health wire budget exceeded")
            return snap

    def loop(self):
        while True:
            self.poll()
            time.sleep(POLL_S)
