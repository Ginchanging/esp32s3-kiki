#!/usr/bin/env bash
# Store the Home Assistant long-lived access token on esp-hub (/etc/esp-dash/ha_token, 600).
# The token is read with a hidden prompt and sent over ssh stdin: it never lands in
# chat, shell history or a process list. It is checked against HA before it is kept.
set -euo pipefail
read -rsp "粘贴 Home Assistant 长期访问令牌（输入不会显示）: " tok
echo
[ -n "$tok" ] || { echo "没有输入，已取消" >&2; exit 1; }
printf '%s' "$tok" | ssh esp-hub 'set -e; umask 077
install -d -m 700 /etc/esp-dash
cat > /etc/esp-dash/ha_token.new
hdr=$(mktemp); { printf "Authorization: Bearer "; cat /etc/esp-dash/ha_token.new; } > "$hdr"
code=$(curl -s -o /dev/null -w "%{http_code}" -H @"$hdr" http://127.0.0.1:8123/api/ || true)
rm -f "$hdr"
if [ "$code" = 200 ]; then
  mv /etc/esp-dash/ha_token.new /etc/esp-dash/ha_token
  echo "OK：令牌有效，已保存到 hub"
else
  rm -f /etc/esp-dash/ha_token.new
  echo "令牌无效（HTTP $code），没有保存" >&2
  exit 1
fi'
unset tok
