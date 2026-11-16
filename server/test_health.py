"""Health observations, privacy, persistence and wire budgets; entirely synthetic data."""
import copy
import datetime as dt
import json
from pathlib import Path
import stat
import tempfile
import unittest

from health import HealthHub, FIELDS, UNIQUE_IDS, TZ, normalized

NOW = int(dt.datetime(2026, 11, 16, 12, tzinfo=TZ).timestamp())
UNITS = {"steps": "steps", "distance_m": "km", "exercise_min": "min", "active_kcal": "kcal",
         "sleep_min": "min", "core_min": "min", "deep_min": "min", "rem_min": "min",
         "body_fat": "%", "weight_kg": "kg"}


class FakeHA:
    def __init__(self, now):
        self.now = now
        self.fail = False
        self.calls = []
        values = [8400, 5.6, 38, 290, 450, 230, 80, 140, 19.8, 68.125]
        self.data = {k: {"value": str(v), "unit": UNITS[k], "reported_at": now, "phone_reported_at": now}
                     for k, v in zip(UNIQUE_IDS, values)}

    def report(self, key, **changes):
        self.data[key].update(changes)
        self.data[key]["phone_reported_at"] = self.data[key]["reported_at"]

    def call(self, method, path, body=None):
        self.calls.append((method, path, body))
        if self.fail:
            raise OSError("synthetic transport failure")
        return copy.deepcopy(self.data)


class HealthTest(unittest.TestCase):
    def setUp(self):
        self.now = NOW
        self.ha = FakeHA(self.now)
        self.config = {"enabled": True, "entities": {k: "sensor.kiki_"+k for k in UNIQUE_IDS}}
        self.health = HealthHub(self.ha, self.config, clock=lambda: self.now)

    def poll(self):
        self.assertTrue(self.health.poll())
        return self.health.snapshot()

    def test_selected_entities_single_batch_and_zero_is_valid(self):
        self.ha.data["steps"]["value"] = "0"
        s = self.poll()
        self.assertEqual(s["values"]["steps"], 0)
        self.assertEqual(s["values"]["distance_m"], 5600)
        self.assertEqual(s["values"]["body_fat"], 198)
        self.assertEqual(len(self.ha.calls), 1)
        self.assertEqual(self.ha.calls[0][:2], ("POST", "/api/template"))
        for entity in self.config["entities"].values():
            self.assertIn(entity, self.ha.calls[0][2]["template"])

    def test_units_invalid_nonfinite_and_missing_permissions(self):
        self.assertEqual(normalized("distance_m", {"value": "2", "unit": "mi"}), 3219)
        self.assertEqual(normalized("sleep_min", {"value": "3600", "unit": "s"}), 60)
        self.assertAlmostEqual(normalized("weight_kg", {"value": "150", "unit": "lb"}), 68.039, 3)
        for key in FIELDS:
            for bad in ("unavailable", "unknown", "nan", "inf", "-1", True):
                self.ha.data[key]["value"] = bad
                self.assertIsNone(self.poll()["values"][key])
        self.ha.data["steps"]["value"] = "5"
        self.ha.data["steps"]["unit"] = "surprise"
        self.assertIsNone(self.poll()["values"]["steps"])

    def test_midnight_never_relabels_old_daily_totals(self):
        self.poll()
        self.now += 86400
        s = self.poll()  # phone still reports yesterday's readings
        self.assertTrue(all(s["values"][k] is None for k in FIELDS[:4]))
        self.assertEqual(s["activity"][-2], [8400, 38])
        self.assertEqual(s["activity"][-1], [None, None])
        self.ha.report("steps", value="25", reported_at=self.now)
        s = self.poll()
        self.assertEqual(s["values"]["steps"], 25)
        self.assertEqual(s["activity"][-1], [25, None])
        self.assertEqual(s["values"]["sleep_min"], 450)  # latest, not labelled last night

    def test_ha_restore_after_midnight_needs_a_phone_report_per_entity(self):
        self.poll()
        self.now += 86400
        for item in self.ha.data.values():
            # HA's numeric restores have new last_reported timestamps, without
            # a restored attribute. The non-restored report tracker starts empty.
            item.update(reported_at=self.now+.123456, phone_reported_at=0)
        s = self.poll()
        self.assertTrue(all(s["values"][k] is None for k in FIELDS[:4]))
        self.assertEqual(s["activity"][-2:], [[8400, 38], [None, None]])
        self.assertEqual(s["values"]["sleep_min"], 450)
        self.assertEqual(s["source_at"], [NOW]*len(FIELDS))
        self.assertEqual(s["weight_at"], NOW)
        self.assertNotIn("2026-11-17", self.health.days)

        # A new hub/first binding must also reject those same restored states.
        fresh = HealthHub(self.ha, self.config, clock=lambda: self.now)
        self.assertTrue(fresh.poll())
        self.assertTrue(all(v is None for v in fresh.snapshot()["values"].values()))
        self.assertEqual(fresh.days, {})

        # A genuine report may repeat the exact value. It only verifies its own
        # metric; another metric's restored total must remain unknown today.
        self.now += 60
        self.ha.report("steps", reported_at=self.now+.654321)
        s = self.poll()
        self.assertEqual(s["activity"][-1], [8400, None])
        self.assertEqual(s["values"]["steps"], 8400)
        self.assertIsNone(s["values"]["exercise_min"])

    def test_missing_or_stale_report_evidence_cannot_advance_observation(self):
        before = self.poll()
        self.now += 60
        self.ha.data["steps"].update(value="9999", reported_at=self.now)
        for proof in (None, 0, NOW, self.now-1, self.now+1, True, "nan", "inf"):
            self.ha.data["steps"]["phone_reported_at"] = proof
            s = self.poll()
            self.assertEqual(s["values"], before["values"])
            self.assertEqual(s["source_at"], before["source_at"])
        del self.ha.data["steps"]["phone_reported_at"]
        self.assertEqual(self.poll()["values"], before["values"])

    def test_weight_only_changes_on_new_value_and_wire_is_relative(self):
        s = self.poll()
        self.assertEqual(s["weight_hist"].count(0), 1)
        self.assertIsNone(s["weight_delta"])
        self.now += 86400
        for item in self.ha.data.values():
            item["reported_at"] = self.now
            item["phone_reported_at"] = self.now
        s = self.poll()
        self.assertIsNone(s["weight_hist"][-1])
        self.assertEqual(s["weight_at"], NOW)
        self.ha.data["weight_kg"]["value"] = "68.425"
        s = self.poll()
        self.assertEqual(s["weight_delta"], 3)
        self.assertEqual(s["weight_change"], 3)
        self.assertEqual(s["weight_span"], 2)
        self.assertEqual(s["weight_hist"][-2:], [0, 3])
        body = json.dumps(s)
        self.assertNotIn("weight_kg", body)
        self.assertNotIn("68.125", body)
        self.assertNotIn("68.425", body)

    def test_outage_preserves_data_but_unavailable_clears_live_value(self):
        before = self.poll()
        self.now += 60
        self.ha.fail = True
        self.assertFalse(self.health.poll())
        s = self.health.snapshot()
        self.assertFalse(s["ok"])
        self.assertEqual(s["values"], before["values"])
        self.assertEqual(s["fetched_at"], before["fetched_at"])
        self.ha.fail = False
        self.ha.report("steps", value="unavailable", reported_at=self.now)
        self.assertIsNone(self.poll()["values"]["steps"])

    def test_bad_response_is_atomic_and_older_reading_cannot_replace_newer(self):
        before = self.poll()
        self.ha.data["sleep_min"] = []
        self.assertFalse(self.health.poll())
        self.assertEqual(self.health.snapshot()["values"], before["values"])
        self.ha.data = FakeHA(NOW-120).data
        self.ha.data["steps"]["value"] = "99"
        self.assertEqual(self.poll()["values"]["steps"], 8400)

    def test_disabled_missing_entities_and_invalid_mapping(self):
        for config in ({}, {"enabled": True, "entities": {}}):
            h = HealthHub(self.ha, config, clock=lambda: self.now)
            self.assertFalse(h.poll())
            self.assertFalse(h.snapshot()["configured"])
        for entities in ({"weight_kg": "sensor.x');bad"}, {"other": "sensor.x"},
                         {"steps": "light.x"}, {"steps": "sensor.x", "sleep_min": "sensor.x"}):
            with self.assertRaises(ValueError):
                HealthHub(self.ha, {"enabled": True, "entities": entities})

    def test_observation_history_begins_at_onboarding_and_expires(self):
        for item in self.ha.data.values():
            item["reported_at"] -= 86400
            item["phone_reported_at"] = item["reported_at"]
        self.poll()
        self.assertNotIn("2026-11-15", self.health.days)
        for day in range(35):
            self.now = NOW+day*86400
            for item in self.ha.data.values():
                item["reported_at"] = self.now
                item["phone_reported_at"] = self.now
            self.ha.data["steps"]["value"] = str(day)
            self.poll()
        self.assertLessEqual(len(self.health.days), 30)
        self.assertNotIn("2026-11-16", self.health.days)

    def test_private_atomic_persistence_restart_and_rebinding(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"health.json"
            self.health = HealthHub(self.ha, self.config, path, clock=lambda: self.now)
            self.poll()
            self.assertTrue(path.exists())
            self.now += 300
            self.poll()
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            restored = HealthHub(self.ha, self.config, path, clock=lambda: self.now)
            self.assertEqual(restored.snapshot()["values"], self.health.snapshot()["values"])
            self.assertFalse(restored.snapshot()["ok"])
            config = copy.deepcopy(self.config)
            config["entities"]["steps"] = "sensor.another_phone_steps"
            new = HealthHub(self.ha, config, path, clock=lambda: self.now)
            self.assertTrue(all(v is None for v in new.snapshot()["values"].values()))
            path.write_text('{"schema": 1, "binding": "'+self.health.binding+'", "days": []}')
            self.assertFalse(HealthHub(self.ha, self.config, path).snapshot()["ok"])

    def test_cache_without_phone_report_provenance_is_not_reused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"health.json"
            self.poll()
            path.write_text(json.dumps({"schema": 1, "binding": self.health.binding,
                                        "since": NOW, "latest": self.health.latest,
                                        "days": self.health.days, "weights": self.health.weights}))
            restored = HealthHub(self.ha, self.config, path, clock=lambda: self.now)
            self.assertTrue(all(v is None for v in restored.snapshot()["values"].values()))
            self.assertEqual(restored.days, {})

    def test_snapshot_budget_even_at_largest_values_and_full_history(self):
        for day in range(30):
            self.now = NOW+day*86400
            for key, item in self.ha.data.items():
                item["reported_at"] = self.now
                item["phone_reported_at"] = self.now
                if key == "weight_kg":
                    item["value"] = str(60+day/10)
                elif key == "distance_m":
                    item["value"] = "1999"
                else:
                    item["value"] = "999"
            s = self.poll()
            self.assertLessEqual(len(json.dumps(s, separators=(",", ":")).encode()), 2048)


if __name__ == "__main__":
    unittest.main()
