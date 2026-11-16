"""Prove phone reports without recording health values or restoring timestamps.

Pinned to HA 2026.10.4's mobile_app update signal. That signal is sent for actual
sensor webhooks, including unchanged values, but never for state restoration:
https://github.com/home-assistant/core/blob/2026.10.4/homeassistant/components/mobile_app/webhook.py
"""
from functools import partial

from homeassistant.components.mobile_app.const import SIGNAL_SENSOR_UPDATE
from homeassistant.const import EVENT_HOMEASSISTANT_STOP
from homeassistant.core import callback
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.dispatcher import async_dispatcher_connect

DOMAIN = "kiki_health_reports"
ENTITY = "sensor.kiki_health_reports"


async def async_setup(hass, config):
    entities = config[DOMAIN].get("entities", [])
    if (not isinstance(entities, list) or len(entities) > 10
            or any(not isinstance(e, str) or not e.startswith("sensor.") for e in entities)
            or len(set(entities)) != len(entities)):
        return False
    registry = er.async_get(hass)
    targets = {}
    for entity in entities:
        entry = registry.async_get(entity)
        if not entry or entry.platform != "mobile_app":
            return False
        targets[entity] = entry.unique_id

    # This state is deliberately NOT a RestoreEntity. Every HA boot starts empty.
    reports = {}
    hass.states.async_set(ENTITY, 0, {"reported_at": {}})

    @callback
    def record(entity, unique_id):
        entry = registry.async_get(entity)
        state = hass.states.get(entity)
        if (not entry or entry.platform != "mobile_app" or entry.unique_id != unique_id
                or not state or state.attributes.get("restored")):
            return
        reports[entity] = state.last_reported.timestamp()
        hass.states.async_set(ENTITY, len(reports), {"reported_at": dict(reports)})

    @callback
    def reported(entity, unique_id):
        # Let mobile_app finish applying the pending update, whichever listener
        # the dispatcher invokes first, then capture the exact state timestamp.
        hass.loop.call_soon(record, entity, unique_id)

    remove = [async_dispatcher_connect(
        hass, f"{SIGNAL_SENSOR_UPDATE}-sensor-{unique_id}", partial(reported, entity, unique_id))
        for entity, unique_id in targets.items()]

    @callback
    def stop(_event):
        for unsubscribe in remove:
            unsubscribe()

    hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, stop)
    return True
