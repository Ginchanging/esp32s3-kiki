"""Synthetic HA dispatcher tests; no HA installation or health values needed."""
import asyncio
import datetime as dt
import importlib.util
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch


class HealthReportsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.entity = "sensor.phone_steps"
        self.uid = "phone_health_steps"
        self.now = dt.datetime(2026, 11, 16, tzinfo=dt.timezone.utc)
        self.states, self.listeners, self.stops = {}, {}, []
        self.entries = {self.entity: SimpleNamespace(platform="mobile_app", unique_id=self.uid)}

        def set_state(entity, value, attributes):
            self.states[entity] = SimpleNamespace(state=str(value), attributes=attributes,
                                                   last_reported=self.now)

        def connect(_hass, signal, target):
            self.listeners[signal] = target
            return lambda: self.listeners.pop(signal)

        self.hass = SimpleNamespace(
            loop=asyncio.get_running_loop(),
            states=SimpleNamespace(get=self.states.get, async_set=set_state),
            bus=SimpleNamespace(async_listen_once=lambda event, fn: self.stops.append(fn)))
        modules = {}
        for name, attrs in {
            "homeassistant": {}, "homeassistant.components": {},
            "homeassistant.components.mobile_app": {},
            "homeassistant.components.mobile_app.const": {"SIGNAL_SENSOR_UPDATE": "mobile_app_sensor_update"},
            "homeassistant.const": {"EVENT_HOMEASSISTANT_STOP": "homeassistant_stop"},
            "homeassistant.core": {"callback": lambda fn: fn},
            "homeassistant.helpers": {},
            "homeassistant.helpers.entity_registry": {
                "async_get": lambda hass: SimpleNamespace(async_get=self.entries.get)},
            "homeassistant.helpers.dispatcher": {"async_dispatcher_connect": connect},
        }.items():
            module = ModuleType(name)
            module.__dict__.update(attrs)
            modules[name] = module
        path = Path(__file__).parent/"homeassistant/custom_components/kiki_health_reports/__init__.py"
        spec = importlib.util.spec_from_file_location("kiki_health_reports", path)
        self.component = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, modules):
            spec.loader.exec_module(self.component)
        self.config = {self.component.DOMAIN: {"entities": [self.entity]}}

    def source_write(self, **attributes):
        self.states[self.entity] = SimpleNamespace(last_reported=self.now, attributes=attributes)

    def reports(self):
        return self.states[self.component.ENTITY].attributes["reported_at"]

    async def test_restore_does_not_prove_report_and_reboot_clears_markers(self):
        self.source_write()  # official mobile_app restores have no restored flag
        self.assertTrue(await self.component.async_setup(self.hass, self.config))
        self.assertEqual(self.reports(), {})
        self.now += dt.timedelta(minutes=1)
        self.source_write()  # generic HA state writes also do not prove a report
        await asyncio.sleep(0)
        self.assertEqual(self.reports(), {})
        self.listeners[f"mobile_app_sensor_update-sensor-{self.uid}"]()
        await asyncio.sleep(0)
        self.assertEqual(self.reports(), {self.entity: self.now.timestamp()})
        self.stops.pop()(None)
        self.assertEqual(self.listeners, {})
        # Even if some other mechanism tried to restore the marker sensor, setup
        # always clears it. No previous HA process can vouch for this boot's state.
        self.now += dt.timedelta(days=1)
        self.source_write()
        self.assertTrue(await self.component.async_setup(self.hass, self.config))
        self.assertEqual(self.reports(), {})

    async def test_phone_reports_capture_exact_timestamp_in_either_listener_order(self):
        self.assertTrue(await self.component.async_setup(self.hass, self.config))
        signal, = self.listeners
        self.assertEqual(signal, f"mobile_app_sensor_update-sensor-{self.uid}")
        for tracker_first in (True, False):
            self.now += dt.timedelta(seconds=60, microseconds=123456)
            if tracker_first:
                self.listeners[signal]()
                self.source_write()
            else:
                self.source_write()
                self.listeners[signal]()
            await asyncio.sleep(0)
            self.assertEqual(self.reports(), {self.entity: self.now.timestamp()})
        # Unchanged values still get a fresh timestamp through the same signal.
        self.now += dt.timedelta(seconds=60)
        self.source_write()
        self.listeners[signal]()
        await asyncio.sleep(0)
        marker = self.states[self.component.ENTITY]
        self.assertEqual(marker.attributes, {"reported_at": {self.entity: self.now.timestamp()}})
        self.assertEqual(marker.state, "1")

    async def test_missing_placeholder_or_rebound_entity_cannot_prove_report(self):
        self.assertTrue(await self.component.async_setup(self.hass, self.config))
        signal, = self.listeners
        self.listeners[signal]()
        await asyncio.sleep(0)
        self.assertEqual(self.reports(), {})
        self.source_write(restored=True)
        self.listeners[signal]()
        await asyncio.sleep(0)
        self.assertEqual(self.reports(), {})
        self.source_write()
        self.entries[self.entity].unique_id = "different_phone_health_steps"
        self.listeners[signal]()
        await asyncio.sleep(0)
        self.assertEqual(self.reports(), {})

    async def test_tracker_is_limited_to_explicit_mobile_app_entities(self):
        self.assertTrue(await self.component.async_setup(self.hass, {self.component.DOMAIN: {"entities": []}}))
        self.assertEqual(self.listeners, {})
        for entities in ({}, "sensor.phone_steps", [self.entity]*2, [self.entity]*11,
                         ["sensor.unknown"], ["light.phone"], [True]):
            self.assertFalse(await self.component.async_setup(
                self.hass, {self.component.DOMAIN: {"entities": entities}}))
        self.entries[self.entity].platform = "other"
        self.assertFalse(await self.component.async_setup(self.hass, self.config))


if __name__ == "__main__":
    unittest.main()
