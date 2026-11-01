#!/usr/bin/env bash
# Serve tools/ble_setup.html for phone/desktop testing.
#
# Web Bluetooth needs a secure context. localhost counts as secure, so opening
# http://127.0.0.1:$PORT/ble_setup.html in desktop Chrome works as-is.
# From a phone on the same LAN, http:// is NOT secure: whitelist the origin in
#   chrome://flags/#unsafely-treat-insecure-origin-as-secure
# (paste http://<this-host-ip>:$PORT, then Relaunch). An https tunnel would
# also work, but cloudflared is blocked on the corporate network here.
set -euo pipefail
PORT="${1:-8765}"
DIR="$(cd "$(dirname "$0")" && pwd)"
IP="$(ipconfig getifaddr en0 2>/dev/null || echo 127.0.0.1)"

echo "desktop: http://127.0.0.1:$PORT/ble_setup.html"
echo "phone:   http://$IP:$PORT/ble_setup.html   (add this origin to the chrome flag below)"
echo "flag:    chrome://flags/#unsafely-treat-insecure-origin-as-secure"
echo
exec python3 -m http.server "$PORT" --directory "$DIR" --bind 0.0.0.0
