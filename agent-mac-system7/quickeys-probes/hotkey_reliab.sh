#!/bin/bash
# Is the QuicKeys hotkey reliable after loadvm, or does snapshot restore break
# ADB key delivery? This decides whether the snapshot workflow can be used for
# any click experiment at all.
cd /home/josh/mac-system7 || exit 1

mon () { { printf "$1\n"; sleep 2; } | socat - UNIX-CONNECT:./mon.sock 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9]*[A-Za-z]//g'; }

try_hotkey () {
  local TAG="$1"
  mon "screendump \"/tmp/hk_a.ppm\"" >/dev/null; sleep 1
  mon 'sendkey grave_accent' >/dev/null
  sleep 5
  mon "screendump \"/tmp/hk_b.ppm\"" >/dev/null; sleep 1
  if cmp -s /tmp/hk_a.ppm /tmp/hk_b.ppm; then echo "  $TAG: NO EFFECT"; else echo "  $TAG: FIRED"; fi
}

echo "=== A. current state (several loadvms deep) ==="
try_hotkey "attempt 1"
try_hotkey "attempt 2"
try_hotkey "attempt 3"

echo ""
echo "=== B. after a fresh loadvm ==="
mon 'loadvm baseline' >/dev/null; sleep 5
try_hotkey "attempt 1"
try_hotkey "attempt 2"

echo ""
echo "=== C. does any other key reach the guest? (caps lock toggles a visible LED-free state; use cmd-shift-3 screenshot instead) ==="
mon 'sendkey ctrl' >/dev/null; sleep 1
echo "  (sent a bare modifier; no visual check, just confirming no crash)"
mon 'info status' | grep -i "VM status"
