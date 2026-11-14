#!/usr/bin/env bash
# macOS counterpart of esp.ps1.
#   ./esp.sh build                 compile the firmware (ESP-IDF v6.1)
#   ./esp.sh flash                 flash via campus.py (keeps NetID / token / UID in campus_nvs)
#   ./esp.sh monitor               serial log; Muse alerts are decoded to Chinese
#   ./esp.sh setup|status|scan|retry|clear     campus Wi-Fi tools (unchanged)
#   ./esp.sh dash                  dashboard status (token, MQTT, last fetch, heap)
#   ./esp.sh token [clear]         store the hub device token (reads server/secrets/device_token)
#   ./esp.sh bemfa [clear]         store the Bemfa UID for Muse alerts (hidden prompt)
#   ./esp.sh lang zh|en            switch the UI language
#   ./esp.sh demo ok|gh_failed|gh_running|server_down|off
#   ./esp.sh muse "文字"           show a local test alert on the board
#   ./esp.sh muse-send "文字"      send a real alert through Bemfa cloud (like Muse does)
#   ./esp.sh led 48|38|off         onboard RGB LED pin
#   ./esp.sh cat act 4|react 1|look fish|night on|time +3|pomo|auto   force Kiki's state
#   ./esp.sh agent done|attention|error [text]   inject an AI-agent push locally
#   ./esp.sh mi lamp on|off|bri 60   Mi Home control through the hub (same as tapping the 米家 page)
#   ./esp.sh knob cw|ccw [n]|hold cw [n]|press|long|rev   act like the EC11 knob (rev = flip direction)
#   ./esp.sh mirror                live window of the REAL board (frames over USB, click = touch)
#   ./esp.sh sim [fixture]         open the simulator window (fake data)
#   ./esp.sh shots                 render all simulator screenshots into shots/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
IDF="${IDF_PATH:-$HOME/esp/esp-idf-v6.1}"
cd "$ROOT"

idf_env() { . "$IDF/export.sh" >/dev/null 2>&1; }
campus() { idf_env; python -B campus.py "$@"; }
sim_build() {
  [ -f build-sim/build.ninja ] || cmake -S sim -B build-sim -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build-sim
}

action="${1:-}"; shift || true
case "$action" in
  build) idf_env; idf.py build ;;
  flash) campus flash "$@" ;;
  token)
    if [ "${1:-}" = clear ]; then campus token clear
    else campus token --value-file "${1:-server/secrets/device_token}"; fi ;;
  setup|status|scan|retry|clear|dash|bemfa|lang|demo|muse|led|monitor) campus "$action" "$@" ;;
  cat) campus cat "$*" ;;
  agent) campus agent "$*" ;;
  mi) campus mi "$*" ;;
  knob) campus knob "$*" ;;
  muse-send) python3 scripts/muse_send.py "$@" ;;
  mirror) idf_env; python -B scripts/mirror.py "$@" ;;
  sim) sim_build >/dev/null; ./build-sim/dash_sim "${1:-fixtures/1_ok.json}" ;;
  shots) sim_build >/dev/null; ./scripts/shots.sh ;;
  *) sed -n '2,20p' "$0"; exit 2 ;;
esac
