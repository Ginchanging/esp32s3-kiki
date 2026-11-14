#!/usr/bin/env bash
# Runs ON esp-hub (deploy.sh ha uploads and calls it). Idempotent:
# (re)creates the container only when the pinned image changed or it is missing.
set -euo pipefail
IMAGE=homeassistant/home-assistant:2026.10.4   # pinned; bump by hand after testing
NAME=homeassistant
CONF=/opt/homeassistant/config

docker image inspect "$IMAGE" >/dev/null 2>&1 || docker pull "$IMAGE"

current="$(docker inspect -f '{{.Config.Image}}' "$NAME" 2>/dev/null || true)"
if [ "$current" = "$IMAGE" ]; then
  docker restart "$NAME" >/dev/null
else
  docker rm -f "$NAME" >/dev/null 2>&1 || true
  docker run -d --name "$NAME" \
    --network host \
    --restart unless-stopped \
    --memory 700m --memory-swap 700m \
    -e TZ=Asia/Shanghai \
    -v "$CONF":/config \
    "$IMAGE" >/dev/null
fi
docker ps --filter "name=^${NAME}$" --format '{{.Names}} {{.Image}} {{.Status}}'
