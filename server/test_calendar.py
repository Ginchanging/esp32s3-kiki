"""Calendar aggregation and wire-contract checks; no iCloud credentials needed."""
import copy
import datetime as dt
import http.client
import json
import threading
import unittest
from http.server import ThreadingHTTPServer
from unittest.mock import patch
import dashd

TZ = dashd.CN_TZ
NOW = dt.datetime(2026, 11, 13, 12, 30, tzinfo=TZ).timestamp()


def event(start, end, title="日程", **kw):
    key = "dateTime" if "T" in start else "date"
    return {"start": {key: start}, "end": {key: end}, "summary": title, **kw}


class FakeHA:
    def __init__(self):
        self.entities = ["calendar.study", "calendar.life"]
        self.data = {"calendar.study": [], "calendar.life": []}
        self.calls = []
        self.fail = False

    def call(self, method, path, body=None):
        self.calls.append((method, path, body))
        if self.fail:
            raise dashd.HAError("unavailable")
        if path == "/api/template":
            return list(self.entities)
        if path == "/api/calendars":
            return [{"entity_id": e, "name": "课程" if e.endswith("study") else "生活"} for e in self.entities]
        return copy.deepcopy(self.data[path.split("/api/calendars/")[1].split("?")[0]])


class CalendarTest(unittest.TestCase):
    def setUp(self):
        self.now = NOW
        self.ha = FakeHA()
        self.cal = dashd.CalendarHub(self.ha, {}, clock=lambda: self.now)

    def poll(self):
        self.assertTrue(self.cal.poll())
        return self.cal.snapshot()

    def test_empty_is_successful_and_source_is_exact_entry(self):
        s = self.poll()
        self.assertTrue(s["configured"] and s["ok"])
        self.assertEqual(s["counts"], [0]*7)
        self.assertEqual(s["events"], [])
        self.assertIn('integration_entities("Kiki iCloud")', self.ha.calls[0][2]["template"])

    def test_all_day_exclusive_end_and_cross_midnight(self):
        self.ha.data["calendar.life"] = [event("2026-11-13", "2026-11-15", "旅行"),
            event("2026-11-12T23:00:00+08:00", "2026-11-13T01:00:00+08:00", "跨夜"),
            event("2026-11-12", "2026-11-13", "已结束")]
        s = self.poll()
        self.assertEqual(s["counts"], [2, 1, 0, 0, 0, 0, 0])
        self.assertTrue(s["events"][0]["all_day"])
        self.assertEqual(len(self.cal.snapshot("2026-11-14")["events"]), 1)
        self.assertEqual(len(self.cal.snapshot("2026-11-15")["events"]), 0)

    def test_multiple_sources_overlap_cancel_and_recurrence_instances(self):
        e = event("2026-11-13T08:00:00Z", "2026-11-13T09:00:00Z", uid="weekly")
        self.ha.data["calendar.study"] = [e, e, event("2026-11-14T08:00:00Z", "2026-11-14T09:00:00Z", uid="weekly"),
            event("2026-11-13", "2026-11-14", "取消", status="cancelled")]
        self.ha.data["calendar.life"] = [e]
        s = self.poll()
        self.assertEqual(s["counts"], [2, 1, 0, 0, 0, 0, 0])
        self.assertEqual({e["calendar"] for e in s["events"]}, {"生活", "课程"})
        self.assertEqual(s["events"][0]["start"], int(dt.datetime(2026, 11, 13, 16, tzinfo=TZ).timestamp()))

    def test_paging_and_worst_case_utf8_under_eight_kib(self):
        self.ha.data["calendar.study"] = [event("2026-11-13", "2026-11-14", "😀"*200+str(i)) for i in range(19)]
        s = self.poll()
        self.assertEqual(len(s["events"]), 8)
        self.assertEqual(s["total"], 19)
        self.assertEqual(len(self.cal.snapshot(offset=16)["events"]), 3)
        self.assertLessEqual(len(json.dumps(s, ensure_ascii=False).encode()), 8192)
        for e in s["events"]:
            self.assertLessEqual(len(e["title"].encode()), 96)
            self.assertTrue(e["title"].endswith("…"))

    def test_failure_and_missing_entry_preserve_previous_data(self):
        self.ha.data["calendar.study"] = [event("2026-11-13", "2026-11-14")]
        before = self.poll()
        self.ha.fail = True
        self.now += 901
        self.assertFalse(self.cal.poll())
        failed = self.cal.snapshot()
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["events"], before["events"])
        self.assertEqual(failed["fetched_at"], before["fetched_at"])
        self.ha.fail = False
        self.ha.entities = []
        self.assertFalse(self.cal.poll())
        self.assertTrue(self.cal.snapshot()["configured"])

    def test_atomic_poll_and_event_modification_deletion(self):
        self.ha.data["calendar.study"] = [event("2026-11-13", "2026-11-14", "旧标题")]
        before = self.poll()
        self.ha.data["calendar.life"] = [event("2026-11-13", "2026-11-12")]
        self.assertFalse(self.cal.poll())
        self.assertEqual(self.cal.snapshot()["events"], before["events"])
        self.ha.data["calendar.life"] = []
        self.ha.data["calendar.study"][0]["summary"] = "新标题"
        self.assertEqual(self.poll()["events"][0]["title"], "新标题")
        self.ha.data["calendar.study"] = []
        self.assertEqual(self.poll()["events"], [])

    def test_midnight_marks_window_stale_until_refetched(self):
        self.poll()
        self.now += 86400
        self.assertFalse(self.cal.snapshot()["ok"])
        self.assertTrue(self.poll()["ok"])

    def test_query_bounds(self):
        for day, offset in [("2026-11-12", 0), ("2026-11-20", 0), ("garbage", 0),
                            (None, -8), (None, 1), (None, 4104)]:
            with self.subTest(day=day, offset=offset), self.assertRaises(ValueError):
                self.cal.snapshot(day, offset)

    def test_first_failure_is_distinct_from_unconfigured(self):
        self.ha.fail = True
        self.assertFalse(self.cal.poll())
        self.assertTrue(self.cal.snapshot()["error"])
        self.ha.fail = False
        self.ha.entities = []
        self.assertFalse(self.cal.poll())
        s = self.cal.snapshot()
        self.assertFalse(s["configured"] or s["error"])

    def test_endpoint_auth_query_and_disabled(self):
        with patch.object(dashd, "load_state", return_value={}):
            hub = dashd.Hub("test/repo", [], "")
        hub.calendar = self.cal
        server = ThreadingHTTPServer(("127.0.0.1", 0), dashd.make_handler(hub, "test-token", {}))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

        def get(path, token="test-token"):
            c = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            try:
                c.request("GET", path, headers={"Authorization": "Bearer "+token})
                r = c.getresponse()
                return r.status, json.loads(r.read())
            finally:
                c.close()

        self.assertEqual(get("/api/device/calendar", "wrong")[0], 401)
        self.assertEqual(get("/api/device/calendar?day=bad")[0], 400)
        self.assertEqual(get("/api/device/calendar?offset=-8")[0], 400)
        self.assertEqual(get("/api/device/calendar?day=a&day=b")[0], 400)
        self.assertEqual(get("/api/device/calendar?extra=1")[0], 400)
        self.poll()
        status, data = get("/api/device/calendar?day=2026-11-13&offset=0")
        self.assertEqual(status, 200)
        self.assertTrue(data["configured"])
        hub.calendar = None
        self.assertFalse(get("/api/device/calendar")[1]["configured"])


if __name__ == "__main__":
    unittest.main()
