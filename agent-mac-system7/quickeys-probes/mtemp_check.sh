#!/bin/bash
# Does MTemp/RawMouse/Mouse actually change across a click? Measured with the
# monitor's own physical-memory reads, so this does not depend on watchpoints,
# gdb, or the guest agent at all.
cd /home/josh/mac-system7 || exit 1

mon () { { printf "$1\n"; sleep 2; } | socat - UNIX-CONNECT:./mon.sock 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9]*[A-Za-z]//g'; }

echo "=== reset ==="
mon 'loadvm baseline' >/dev/null; sleep 3
mon 'info status' | grep -i "VM status"

echo ""
echo "=== BEFORE ==="
mon 'xp /6xh 0x828' | grep -E "^0x|0000" | head -3
mon 'screendump "/tmp/mt_before.ppm"' >/dev/null; sleep 1

echo ""
echo "=== fire hotkey ==="
mon 'sendkey grave_accent' >/dev/null
sleep 5

echo "=== AFTER ==="
mon 'xp /6xh 0x828' | grep -E "^0x|0000" | head -3
mon 'screendump "/tmp/mt_after.ppm"' >/dev/null; sleep 1

echo ""
echo "=== did the macro actually fire? ==="
if cmp -s /tmp/mt_before.ppm /tmp/mt_after.ppm; then
  echo "SCREEN IDENTICAL -> macro did NOT fire"
else
  echo "SCREEN CHANGED -> macro fired ($(cmp -l /tmp/mt_before.ppm /tmp/mt_after.ppm 2>/dev/null | wc -l) bytes differ)"
fi
mon 'info status' | grep -i "VM status"
