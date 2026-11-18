import http.client
import io
import json
import threading
import unittest
from http.server import ThreadingHTTPServer
from unittest.mock import patch

import dashd


def response(**payload):
    reply = io.BytesIO(json.dumps(payload).encode())
    reply.headers = {"ETag": '"runs"'}
    return reply


class ActionsTest(unittest.TestCase):
    def setUp(self):
        self.hub = dashd.Hub("example/desk", [], "github-test-token")

    def test_status_keeps_five_runs_with_attempts(self):
        runs = [{"id": i, "run_number": i, "run_attempt": 2 if i == 12 else 1,
                 "status": "completed", "conclusion": "success"} for i in range(12, 6, -1)]
        with patch("dashd.urllib.request.urlopen", return_value=response(workflow_runs=runs)) as request:
            self.hub.poll_github()
        self.assertIn("per_page=5", request.call_args.args[0].full_url)
        self.assertEqual(request.call_count, 1)
        wire = self.hub.snapshot()
        self.assertEqual([(r["id"], r["attempt"]) for r in wire["github"]["runs"]],
                         [(12, 2), (11, 1), (10, 1), (9, 1), (8, 1)])
        self.assertNotIn("jobs", wire["github"]["runs"][0])

    def test_step_pages_share_the_attempt_job_page_and_keep_timing(self):
        steps = [
            {"number": i, "name": "测试" * 80 + "\nName", "status": "completed", "conclusion": "failure",
             "started_at": "2026-11-18T00:00:05.000+08:00", "completed_at": "2026-11-17T16:00:47Z"}
            for i in range(1, 9)
        ]
        job = {"id": 9007199254740990, "run_id": 12, "name": "Frontend tests",
               "status": "completed", "conclusion": "failure", "steps": steps}
        with patch("dashd.urllib.request.urlopen", return_value=response(total_count=7, jobs=[job])) as request:
            jobs = self.hub.action_snapshot(12, 2, 2, 0, 0)
            first = self.hub.action_snapshot(12, 2, 2, job["id"], 1)
            second = self.hub.action_snapshot(12, 2, 2, job["id"], 2)
            with self.assertRaises(LookupError):
                self.hub.action_snapshot(12, 2, 2, job["id"] - 1, 1)
        self.assertEqual(request.call_count, 1)
        req = request.call_args.args[0]
        self.assertTrue(req.full_url.endswith("/actions/runs/12/attempts/2/jobs?per_page=6&page=2"))
        self.assertEqual(req.get_header("Authorization"), "Bearer github-test-token")
        self.assertEqual(jobs["total"], 7)
        self.assertEqual(jobs["rows"][0]["duration_s"], -1)
        self.assertEqual([r["id"] for r in first["rows"] + second["rows"]], list(range(1, 9)))
        self.assertEqual(first["rows"][0]["duration_s"], 42)
        self.assertLessEqual(len(first["rows"][0]["name"].encode()), 95)
        self.assertLess(len(json.dumps(first, ensure_ascii=False).encode()), 8192)

    def test_device_route_auth_validation_and_upstream_failure(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), dashd.make_handler(self.hub, "device-token", {}))
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            conn = http.client.HTTPConnection(*server.server_address, timeout=2)
            try:
                def get(path, token=None):
                    headers = {"Authorization": f"Bearer {token}"} if token else {}
                    conn.request("GET", path, headers=headers)
                    reply = conn.getresponse()
                    return reply.status, json.loads(reply.read())

                path = "/api/device/actions?run_id=12&attempt=2&jobs_page=1"
                with patch("dashd.urllib.request.urlopen", side_effect=OSError("offline")) as request:
                    self.assertEqual(get(path)[0], 401)
                    self.assertEqual(get(path + "&run_id=13", "device-token")[0], 400)
                    self.assertEqual(get(path + "&job_id=91", "device-token")[0], 400)
                    request.assert_not_called()
                    self.assertEqual(get(path, "device-token")[0], 502)
                    self.assertEqual(get("/api/device/status", "device-token")[0], 200)
                with patch("dashd.urllib.request.urlopen", return_value=response(total_count=0, jobs=[])):
                    status, page = get(path, "device-token")
                    self.assertEqual((status, page["rows"], page["total"]), (200, [], 0))
            finally:
                conn.close()
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
