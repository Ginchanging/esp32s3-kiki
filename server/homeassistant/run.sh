#!/usr/bin/env bash
# Runs ON esp-hub. Bootstrap publishes only to host loopback, even before HA has
# an account or HTTP configuration. Host networking requires a confirmed config.
set -euo pipefail
IMAGE=homeassistant/home-assistant:2026.10.4   # pinned; bump by hand after testing
NAME=homeassistant
CONF=/opt/homeassistant/config

case "${1:---bootstrap}" in
  --bootstrap) network=(--network bridge --publish 127.0.0.1:8123:8123) ;;
  --host)
    # Check the running bootstrap BEFORE removing it. Missing credentials,
    # unconfirmed trials and default/all-interface bindings all fail closed.
    python3 /opt/esp-dash/ha_setup.py http-check
    network=(--network host)
    ;;
  *) echo "usage: $0 [--bootstrap|--host]" >&2; exit 2 ;;
esac

docker image inspect "$IMAGE" >/dev/null 2>&1 || docker pull "$IMAGE"

# Recreate so an existing container cannot retain broader port publications.
# The config volume, including accounts and integrations, is retained.
docker stop "$NAME" >/dev/null 2>&1 || true
docker rm "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" \
  "${network[@]}" \
  --restart unless-stopped \
  --memory 700m --memory-swap 700m \
  -e TZ=Asia/Shanghai \
  -v "$CONF":/config \
  "$IMAGE" >/dev/null
docker ps --filter "name=^${NAME}$" --format '{{.Names}} {{.Image}} {{.Status}}'
