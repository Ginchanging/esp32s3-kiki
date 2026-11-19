import unittest
from unittest.mock import patch

from dashd import HAError, MiHome


class TestMiHomeMeter(unittest.TestCase):
    def setUp(self):
        self.plug = {"id": "plug", "kind": "switch", "entity": "switch.plug", "protect_off": True,
                     "power_w": "sensor.power", "energy_kwh": "sensor.energy"}
        self.states = {
            "switch.plug": {"state": "on"},
            "sensor.power": {"state": "98.7", "attributes": {"unit_of_measurement": "W"}},
            "sensor.energy": {"state": "0.01", "attributes": {"unit_of_measurement": "kWh"}},
        }
        self.reads = []
        self.services = []
        with patch("dashd.load_state", return_value={}):
            self.mi = MiHome(None, self, {"devices": [self.plug]})

    def state(self, entity):
        self.reads.append(entity)
        if entity not in self.states:
            raise HAError("missing sensor")
        return self.states[entity]

    def service(self, domain, service, data):
        self.services.append((domain, service, data))
        self.states["switch.plug"]["state"] = "on" if service == "turn_on" else "off"

    def poll(self):
        self.mi.poll()
        snapshot = self.mi.snapshot()
        self.assertTrue(snapshot["ok"])
        return snapshot["devices"][0]

    def test_reads_meter_without_switching(self):
        plug = self.poll()
        self.assertTrue(plug["protect_off"] and plug["on"])
        self.assertEqual((plug["power_w"], plug["energy_kwh"]), (98.7, 0.01))
        self.assertEqual(self.services, [])

    def test_missing_or_invalid_reading_keeps_the_switch_online(self):
        del self.states["sensor.power"]
        for state, unit, expected in (("0", "kWh", 0), ("unknown", "kWh", None),
                                      ("inf", "kWh", None), ("12", "W", None)):
            with self.subTest(state=state, unit=unit):
                self.states["sensor.energy"] = {"state": state, "attributes": {"unit_of_measurement": unit}}
                plug = self.poll()
                self.assertTrue(plug["online"] and plug["on"])
                self.assertIsNone(plug["power_w"])
                self.assertEqual(plug["energy_kwh"], expected)

    def test_control_does_not_wait_for_sensors(self):
        self.poll()
        self.reads.clear()
        with patch("dashd.time.sleep"):
            plug = self.mi.control("plug", False, None)["devices"][0]
        self.assertEqual(self.services, [("switch", "turn_off", {"entity_id": "switch.plug"})])
        self.assertEqual(self.reads, ["switch.plug"])
        self.assertFalse(plug["on"])
        self.assertIsNone(plug["power_w"])
        self.assertEqual(plug["energy_kwh"], 0.01)
