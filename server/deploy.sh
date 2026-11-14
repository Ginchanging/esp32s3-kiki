#!/usr/bin/env bash
# Deploy the dashboard backend. Uses the SSH host aliases from ~/.ssh/config:
#   esp-hub (8.138.138.222, root) · gcp-isbf (34.68.253.253, ginm + sudo) · aliyun-codex (8.155.132.213, root)
# Only adds /opt/esp-dash, /etc/esp-dash and one systemd unit per host.
#   ./deploy.sh hub | gcp | codex | all | ha   (ha = Home Assistant for Mi Home, hub only)
set -euo pipefail
cd "$(dirname "$0")"
SEC=secrets

push() { # host unit-file credential-pairs...
  local host="$1" unit="$2"; shift 2
  local tmp; tmp="$(ssh "$host" mktemp -d)"
  scp -q dashd.py "$unit" "$host:$tmp/"
  local creds=()
  while [ $# -gt 0 ]; do scp -q "$1" "$host:$tmp/cred_$2"; creds+=("$2"); shift 2; done
  ssh "$host" "sudo -n bash -s" <<EOF
set -e
install -d -m 755 /opt/esp-dash
install -m 644 $tmp/dashd.py /opt/esp-dash/dashd.py
install -d -m 700 /etc/esp-dash
for c in ${creds[*]}; do install -m 600 -o root -g root $tmp/cred_\$c /etc/esp-dash/\$c; done
install -m 644 $tmp/$(basename "$unit") /etc/systemd/system/$(basename "$unit")
rm -rf $tmp
python3 -m py_compile /opt/esp-dash/dashd.py
systemctl daemon-reload
systemctl enable --now $(basename "$unit") >/dev/null
systemctl restart $(basename "$unit")
sleep 3
systemctl is-active $(basename "$unit")
EOF
}

agent_unit() { # name services -> path of a filled-in unit file
  local out; out="$(mktemp -d)/esp-dash-agent.service"
  sed -e "s|@AGENT_NAME@|$1|" -e "s|@SERVICES@|$2|" esp-dash-agent.service > "$out"
  echo "$out"
}

deploy_hub() {
  # ha_token is written on the hub by set_ha_token.sh; an empty one just disables Mi Home
  ssh esp-hub 'install -d -m 700 /etc/esp-dash; [ -e /etc/esp-dash/ha_token ] || install -m 600 /dev/null /etc/esp-dash/ha_token'
  push esp-hub esp-dash-hub.service \
    certs/hub.pem hub.pem certs/hub.key hub.key \
    "$SEC/device_token" device_token "$SEC/agent_tokens.json" agent_tokens.json \
    mihome.json mihome.json
}
deploy_gcp() {
  push gcp-isbf "$(agent_unit gcp-isbf 'isbf-app=tcp:127.0.0.1:3000,caddy=tcp:127.0.0.1:443,docker=proc:dockerd')" \
    certs/hub.pem hub.pem "$SEC/agent_gcp-isbf" agent_token
}
deploy_codex() {
  push aliyun-codex "$(agent_unit aliyun-codex 'isbf-one=tcp:127.0.0.1:443,dev=tcp:127.0.0.1:3000,codex=proc:app-server-daemon')" \
    certs/hub.pem hub.pem "$SEC/agent_aliyun-codex" agent_token
}

# Home Assistant + the official Xiaomi Home integration on esp-hub (localhost:8123 only).
XH_VER=v0.5.0
XH_SHA=fa093245846a1118c4b1d751dbe1e6298ceeccc66734ac830c062091e09496e3
deploy_ha() {
  local tmp; tmp="$(ssh esp-hub mktemp -d)"
  scp -q homeassistant/configuration.yaml homeassistant/run.sh "esp-hub:$tmp/"
  ssh esp-hub "bash -s" <<EOF
set -e
cc=/opt/homeassistant/config/custom_components/xiaomi_home
install -d -m 755 /opt/homeassistant/config/custom_components
install -m 644 $tmp/configuration.yaml /opt/homeassistant/config/configuration.yaml
install -m 755 $tmp/run.sh /opt/homeassistant/run.sh
if [ "\$(cat \$cc/.kiki_version 2>/dev/null)" != "$XH_VER" ]; then
  curl -sSL --retry 3 -o $tmp/xh.zip https://github.com/XiaoMi/ha_xiaomi_home/releases/download/$XH_VER/xiaomi_home.zip
  echo "$XH_SHA  $tmp/xh.zip" | sha256sum -c --quiet
  rm -rf \$cc && mkdir -p \$cc && python3 -m zipfile -e $tmp/xh.zip \$cc
  echo $XH_VER > \$cc/.kiki_version
fi
rm -rf $tmp
/opt/homeassistant/run.sh
EOF
}

case "${1:-all}" in
  hub) deploy_hub ;;
  ha) deploy_ha ;;
  gcp) deploy_gcp ;;
  codex) deploy_codex ;;
  all) deploy_hub; deploy_gcp; deploy_codex ;;
  *) echo "usage: $0 hub|gcp|codex|all|ha" >&2; exit 2 ;;
esac
