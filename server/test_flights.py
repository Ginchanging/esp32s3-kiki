"""Deterministic flight provider, geometry, dedupe and endpoint contract checks.

Run: python3 -m unittest discover -s server -p 'test_*.py'
No upstream network calls or persistent state writes.
"""
import copy
import json
import math
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from unittest.mock import patch
import dashd


class FlightsTest(unittest.TestCase):
    def setUp(self):
        self.t = 1794544200.0
        self.state = patch.object(dashd, "load_state", return_value={})
        self.state.start()
        self.addCleanup(self.state.stop)
        self.write = patch.object(dashd, "save_state")
        self.saved = self.write.start()
        self.addCleanup(self.write.stop)
        self.r = dashd.FlightRadar({"lon": 113.5773054, "lat": 22.35591073, "radius_m": 20000,
                                    "alert_radius_m": 5000}, clock=lambda: self.t)
        self.r.meta["CSN8798"] = (self.t+10000, {"flight": "CZ8798", "airline": "China Southern",
                                               "origin": "TNA", "destination": "SZX"})

    def plane(self, metres=3000, **kw):
        a = {"hex": "780e08", "flight": "CSN8798 ", "r": "B-8076", "t": "A320", "category": "A3",
             "lat": self.r.lat, "lon": self.r.lon+math.degrees(metres/(6371008.8*math.cos(math.radians(self.r.lat)))),
             "alt_baro": 10000, "track": 90, "seen_pos": .5}
        a.update(kw)
        return a

    def ingest(self, *aircraft, stamp=None):
        self.r.ingest({"now": self.t*1000 if stamp is None else stamp, "ac": list(aircraft)})

    def test_baidu_coordinate_and_geometry(self):
        lon, lat = dashd.flight_wgs84(113.588935, 22.358541, "bd09")
        self.assertAlmostEqual(lon, 113.5773054, places=6)
        self.assertAlmostEqual(lat, 22.35591073, places=6)
        self.assertEqual(dashd.flight_geometry(lon, lat, lon, lat)[0], 0)
        d, b = dashd.flight_geometry(lon, lat, lon+.01, lat)
        self.assertTrue(1000 < d < 1100)
        self.assertEqual(b, 90)
        for a, b, system in [(True, lat, "wgs84"), (lon, float('nan'), "bd09"), (lon, lat, "typo")]:
            with self.assertRaises(ValueError): dashd.flight_wgs84(a, b, system)

    def test_units_timestamps_and_eligibility(self):
        self.ingest(self.plane())
        f = self.r.snapshot()["aircraft"][0]
        self.assertEqual(f["altitude_m"], 3048)
        self.assertEqual(f["observed_at"], int(self.t-.5))
        self.assertTrue(f["eligible"])
        self.assertEqual(f["heading"], 90)
        self.ingest(self.plane(), stamp=self.t)
        self.assertEqual(self.r.snapshot()["fetched_at"], int(self.t))
        with self.assertRaises(ValueError): self.ingest(self.plane(), stamp=(self.t-31)*1000)

    def test_filter_ground_military_invalid_and_old_positions(self):
        self.ingest(self.plane(alt_baro="ground"), self.plane(dbFlags=1), self.plane(category="A7"),
                    self.plane(seen_pos=31), self.plane(lon=float('nan')), self.plane(hex="~12345"))
        self.assertEqual(self.r.snapshot()["aircraft"], [])
        self.ingest(self.plane(t="C172", flight="B1234"))
        self.assertFalse(self.r.snapshot()["aircraft"][0]["eligible"])

    def test_neo_max_and_widebody_type_designators(self):
        for model in ('A20N', 'A21N', 'A19N', 'B38M', 'B77W', 'B78X', 'B748', 'B763', 'C919'):
            self.ingest(self.plane(t=model))
            self.assertTrue(self.r.snapshot()["aircraft"][0]["eligible"], model)

    def test_entry_boundary_baseline_and_no_replay(self):
        self.ingest(self.plane(3000))
        self.assertEqual(self.r.events, [])  # already overhead when the hub boots
        self.ingest(self.plane(9000))
        self.t += 121
        self.ingest(self.plane(9000))
        self.t += 1
        self.ingest(self.plane(5001))
        self.assertEqual(self.r.events, [])
        self.ingest(self.plane(4999))
        self.assertEqual(len(self.r.events), 1)
        self.ingest(self.plane(3000))
        self.assertEqual(len(self.r.events), 1)
        self.assertTrue(self.saved.called)
        # An empty/failed observation does not rearm a previously notified aircraft.
        self.ingest()
        self.t += 20
        self.ingest(self.plane(3000))
        self.assertEqual(len(self.r.events), 1)

    def test_reentry_requires_both_far_time_and_cooldown(self):
        self.ingest(self.plane(6000))
        self.t += 10
        self.ingest(self.plane(3000))
        self.assertEqual(len(self.r.events), 1)
        self.ingest(self.plane(9000))
        self.t += 121
        self.ingest(self.plane(9000))
        self.ingest(self.plane(3000))
        self.assertEqual(len(self.r.events), 1)
        self.ingest(self.plane(9000))
        self.t += 1800
        self.ingest(self.plane(9000))
        self.ingest(self.plane(3000))
        self.assertEqual(len(self.r.events), 2)

    def test_stale_position_does_not_alert_and_event_expires(self):
        self.ingest(self.plane(6000))
        self.t += 10
        self.ingest(self.plane(3000, seen_pos=16))
        self.assertEqual(self.r.events, [])
        self.ingest(self.plane(3000))
        self.assertEqual(len(self.r.events), 1)
        self.t += 61
        self.assertEqual(self.r.snapshot()["events"], [])

    def test_enrichment_and_atc_callsign(self):
        def get(url):
            return {"response": {"flightroute": {"callsign_iata": "AY27W", "airline": {"name": "Finnair"},
                    "origin": {"iata_code": "HEL", "municipality": "Helsinki"},
                    "destination": {"iata_code": "LHR", "municipality": "London"}}}}
        self.r.get_json = get
        f = self.r.enrich("FIN27W")
        self.assertEqual(f["flight"], "")  # don't label an ATC alias as a public flight number
        self.assertEqual(f["destination"], "LHR")
        self.r.get_json = lambda url: {"response": {"flightroute": {
            "callsign_iata": "3331", "airline": {"name": "Hebei Airlines"}}}}
        self.assertEqual(self.r.enrich("HBH3331")["flight"], "")

    def test_bounded_utf8_reply_and_independent_dashboard(self):
        self.ingest(self.plane())
        f = self.r.snapshot()["aircraft"][0]
        f.update(origin_city="城"*32, destination_city="城"*32, airline="航"*32)
        self.r.aircraft = [dict(f, hex=f'{i:06x}') for i in range(8)]
        self.r.events = [{"id": i+1, "at": int(self.t), "flight": copy.deepcopy(f)} for i in range(3)]
        self.assertLessEqual(len(json.dumps(self.r.snapshot(), ensure_ascii=False, separators=(',', ':')).encode()), 8000)
        hub = dashd.Hub("test/repo", [], "")
        hub.flights = self.r
        server = ThreadingHTTPServer(('127.0.0.1', 0), dashd.make_handler(hub, 'test-token', {}))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f'http://127.0.0.1:{server.server_port}'
            with self.assertRaises(urllib.error.HTTPError) as e:
                urllib.request.urlopen(url+'/api/device/flights')
            self.assertEqual(e.exception.code, 401)
            for path in ('flights', 'status'):
                req = urllib.request.Request(url+'/api/device/'+path, headers={'Authorization':'Bearer test-token'})
                with urllib.request.urlopen(req) as r: body = json.load(r)
                self.assertEqual(body['schema'], 1)
                self.assertEqual('aircraft' in body, path == 'flights')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
