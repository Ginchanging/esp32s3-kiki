import json
import http.client
from http.server import ThreadingHTTPServer
from pathlib import Path
import stat
import os
import subprocess
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import ha_setup
import dashd
from health import FIELDS, LIMITS


class HealthSetupTest(unittest.TestCase):
    def test_capacity_counters_are_bounded_and_endpoint_is_authenticated(self):
        hub = dashd.Hub("test/repo", [], "")
        headers = {"X-Dash-Heap-Free": "33000", "X-Dash-Heap-Min": "14000", "X-Dash-Uptime": "120"}
        hub.record_device_diagnostics(headers)
        before = dict(hub.device_diagnostics)
        for bad in ({}, {**headers, "X-Dash-Heap-Min": "34000"}, {**headers, "X-Dash-Uptime": "nan"}):
            hub.record_device_diagnostics(bad)
            self.assertEqual(hub.device_diagnostics, before)
        server = ThreadingHTTPServer(("127.0.0.1", 0), dashd.make_handler(hub, "test-token", {}))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for auth, expected in (({}, 401), ({"Authorization": "Bearer test-token"}, 200)):
                conn = http.client.HTTPConnection(*server.server_address, timeout=2)
                conn.request("GET", "/api/device/diagnostics", headers=auth)
                response = conn.getresponse()
                body = json.loads(response.read())
                self.assertEqual(response.status, expected)
                if expected == 200:
                    self.assertEqual(body["heap_min"], 14000)
                    self.assertEqual(set(body), {"received_at", "heap_free", "heap_min", "uptime_s"})
                conn.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_discovery_is_by_phone_and_official_metric_id(self):
        devices = [{"id": "phone-a"}, {"id": "phone-b"}, {"id": "watch"}]
        entities = [
            {"platform": "mobile_app", "device_id": "phone-a", "entity_id": "sensor.a_steps",
             "unique_id": "prefix_health_steps", "disabled_by": None},
            {"platform": "mobile_app", "device_id": "phone-a", "entity_id": "sensor.a_heart",
             "unique_id": "prefix_health_heart_rate", "disabled_by": None},
            {"platform": "mobile_app", "device_id": "phone-b", "entity_id": "sensor.b_weight",
             "unique_id": "prefix_health_body_mass", "disabled_by": None},
            {"platform": "mobile_app", "device_id": "watch", "entity_id": "sensor.watch_steps",
             "unique_id": "prefix_health_steps", "disabled_by": "user"},
            {"platform": "other", "device_id": "phone-a", "entity_id": "sensor.other_steps",
             "unique_id": "prefix_health_steps", "disabled_by": None},
        ]
        result = ha_setup.discover(devices, entities)
        self.assertEqual(result, [(devices[0], {"steps": "sensor.a_steps"}),
                                  (devices[1], {"weight_kg": "sensor.b_weight"})])

    def test_http_trial_is_promoted_only_after_restart(self):
        config = {"server_port": 8123}
        before = {"stable": config, "pending": None, "active_config_type": "stable"}
        pending = {"stable": config, "pending": {"config": {}}, "active_config_type": "stable"}
        running = {**pending, "active_config_type": "pending"}
        confirmed = {"stable": ha_setup.HTTP_CONFIG, "pending": None, "active_config_type": "stable"}
        with patch.object(ha_setup, "http_config", side_effect=[before, pending, running, confirmed]) as get:
            with patch.object(ha_setup, "commands", return_value=[{}]) as send:
                with patch.object(ha_setup.time, "sleep"):
                    ha_setup.configure_http()
                self.assertEqual(get.call_count, 4)
                self.assertEqual(send.call_args_list[0].args[0]["type"], "http/config/configure")
                self.assertEqual(send.call_args_list[1].args[0]["type"], "http/config/promote")
                self.assertEqual(send.call_count, 2)

    def test_host_network_requires_confirmed_private_http_config(self):
        confirmed = {"stable": ha_setup.HTTP_CONFIG, "pending": None, "active_config_type": "stable"}
        with patch.object(ha_setup, "http_config", return_value=confirmed):
            ha_setup.check_http()
        for unsafe in ({}, {**confirmed, "stable": {}},
                       {**confirmed, "stable": {**ha_setup.HTTP_CONFIG, "server_host": ["0.0.0.0"]}},
                       {**confirmed, "active_config_type": "default"},
                       {**confirmed, "active_config_type": "pending"},
                       {**confirmed, "pending": {"config": ha_setup.HTTP_CONFIG}},
                       {**confirmed, "pending": {}},
                       {**confirmed, "revert_at": "2026-11-16T01:00:00Z"},
                       {**confirmed, "stable": {**ha_setup.HTTP_CONFIG, "error": "apply_failed"}}):
            with self.subTest(unsafe=unsafe):
                with patch.object(ha_setup, "http_config", return_value=unsafe):
                    with self.assertRaises(RuntimeError):
                        ha_setup.check_http()

    def test_container_bootstrap_and_failed_host_verification(self):
        # Run the real startup script against command doubles, without Docker,
        # sockets, host config writes or administrator credentials.
        script = Path(__file__).parent/"homeassistant/run.sh"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root/"calls.jsonl"
            for name in ("docker", "python3"):
                command = root/name
                command.write_text(f"#!{sys.executable}\n"+'''
import json, os, sys
from pathlib import Path
with Path(os.environ['HA_TEST_LOG']).open('a') as f:
    f.write(json.dumps([Path(sys.argv[0]).name, *sys.argv[1:]])+'\\n')
if Path(sys.argv[0]).name == 'python3':
    sys.exit(int(os.environ['HA_TEST_VERIFY']))
''')
                command.chmod(0o755)
            env = {**os.environ, "PATH": str(root)+os.pathsep+os.environ["PATH"],
                   "HA_TEST_LOG": str(log), "HA_TEST_VERIFY": "0"}
            for args, verify in (([], "0"), (["--bootstrap"], "0"), (["--host"], "1"), (["--host"], "0")):
                log.unlink(missing_ok=True)
                proc = subprocess.run(["/bin/bash", str(script), *args], capture_output=True,
                                      text=True, env={**env, "HA_TEST_VERIFY": verify})
                calls = [json.loads(line) for line in log.read_text().splitlines()]
                if verify == "1":
                    self.assertNotEqual(proc.returncode, 0)
                    self.assertEqual(calls, [["python3", "/opt/esp-dash/ha_setup.py", "http-check"]])
                    continue
                self.assertEqual(proc.returncode, 0, proc.stderr)
                run, = [c for c in calls if c[:2] == ["docker", "run"]]
                if args == ["--host"]:
                    self.assertEqual(calls[0], ["python3", "/opt/esp-dash/ha_setup.py", "http-check"])
                    self.assertEqual(run[run.index("--network")+1], "host")
                else:
                    self.assertEqual(run[run.index("--network")+1], "bridge")
                    self.assertEqual(run[run.index("--publish")+1], "127.0.0.1:8123:8123")
                    self.assertNotIn("host", run)

    def test_private_mapping_atomic_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/"health.json"
            ha_setup.atomic_json(p, {"enabled": True, "entities": {"steps": "sensor.phone_steps"}})
            self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600)
            self.assertTrue(json.loads(p.read_text())["enabled"])
            self.assertFalse(p.with_suffix(".json.tmp").exists())

    def test_worst_case_status_stays_within_device_buffer(self):
        hub = dashd.Hub("😀"*47, ["😀"*19+str(i) for i in range(3)], "")
        hub.github = {"repo": "😀"*47, "fetched_at": 1900000000, "runs": [
            {"id": 99999999999, "workflow": "😀"*47, "branch": "😀"*39,
             "run_number": 99999999, "status": "completed", "conclusion": "failure",
             "duration_s": 999999, "updated_at": 1900000000} for _ in range(4)]}
        for s in hub.servers.values():
            s.update(services=[{"name": "😀"*19, "ok": False} for _ in range(4)])
        hub.cpu_hist = {n: [(i, 100) for i in range(60)] for n in hub.servers}
        hub.events = [{"id": 1900000000000+i, "kind": "run_succeeded", "subject": "😀"*79}
                      for i in range(8)]
        hub.contrib = {"user": "😀"*23, "levels": "4"*105, "week": 999, "today": 999}
        snapshot = hub.snapshot()
        for run in snapshot["github"]["runs"]:
            self.assertLessEqual(len(run["workflow"].encode()), 47)
        for event in snapshot["events"]:
            self.assertLessEqual(len(event["subject"].encode()), 79)
        # Reserve the full health budget, including a fully populated history.
        health_bytes = len(json.dumps(snapshot.pop("health"), separators=(",", ":")).encode())
        body_bytes = len(json.dumps(snapshot, ensure_ascii=False, separators=(",", ":")).encode())
        self.assertLessEqual(body_bytes+health_bytes, 8192)
        self.assertLessEqual(body_bytes+2048+11, 8192)
        # Add all four supported Mi Home cards and a maximum-size valid health summary.
        mi = {"ok": True, "devices": [{"id": "device_"+str(i), "kind": "scale", "name_zh": "X"*23,
                                      "name_en": "Y"*23, "online": True, "at": 4102444800,
                                      "d_last": -2980, "d_30": 2980, "hist": [0]+[2980]*29}
                                     for i in range(4)]}
        health = {"schema": 1, "configured": True, "ok": True, "fetched_at": 4102444800,
                  "since": 4100000000, "day": 47480, "values": dict(LIMITS),
                  "source_at": [4102444800]*len(FIELDS), "activity": [[1000000, 1440]]*7,
                  "weight_hist": [0]+[2980]*29, "weight_delta": -2980, "weight_change": 2980,
                  "weight_span": 30, "weight_at": 4102444800}
        hub.mihome = SimpleNamespace(snapshot=lambda: mi)
        hub.health = SimpleNamespace(snapshot=lambda: health)
        snapshot = hub.snapshot()
        self.assertLessEqual(len(json.dumps(snapshot, ensure_ascii=False, separators=(",", ":")).encode()), 8192)
        self.assertEqual(snapshot["events"][-1]["id"], hub.events[-1]["id"])
        self.assertEqual(len(hub.cpu_hist[next(iter(hub.servers))]), 60)


if __name__ == "__main__":
    unittest.main()
