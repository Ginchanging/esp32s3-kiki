#!/usr/bin/env bash
# Run ON esp-hub, after ha_setup.py http has confirmed its network trial.
set -euo pipefail
if ! command -v caddy >/dev/null; then
  # The base Ubuntu repository on esp-hub does not provide Caddy. Use its
  # official signed stable repository, as documented at caddyserver.com/docs/install.
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y debian-keyring debian-archive-keyring apt-transport-https curl gnupg
  curl --connect-timeout 10 --max-time 60 -fsSL https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
    | gpg --batch --yes --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl --connect-timeout 10 --max-time 60 -fsSL https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt \
    -o /etc/apt/sources.list.d/caddy-stable.list
  chmod 644 /usr/share/keyrings/caddy-stable-archive-keyring.gpg /etc/apt/sources.list.d/caddy-stable.list
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y caddy
fi
install -d -m 755 /etc/caddy
install -m 644 /opt/esp-dash/ha.Caddyfile /etc/caddy/kiki-ha.caddy
python3 - <<'PY'
from pathlib import Path
p=Path('/etc/caddy/Caddyfile')
text=p.read_text() if p.exists() else ''
line='import /etc/caddy/kiki-ha.caddy'
if line not in text.splitlines():
    if p.exists() and not p.with_suffix('.pre-kiki').exists():
        p.with_suffix('.pre-kiki').write_text(text)
    p.write_text(text.rstrip()+'\n\n'+line+'\n')
PY
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
systemctl enable --now caddy >/dev/null
systemctl reload caddy
systemctl is-active caddy
