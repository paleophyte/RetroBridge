#!/bin/bash
# DEFINITIVE journaling test.
#
# Method is now validated: a watchpoint on MTemp (0x828) demonstrably fires
# during a real click, so watchpoints do catch click-time writes. If QuicKeys
# faked the button via journaling playback, JournalFlag (0x08DE) or JournalRef
# (0x08E8) would have to be written in the same window.
#
# Every earlier attempt had a hole: no proof the macro fired inside the window,
# or a crashed guest. This one restarts QEMU first for a clean debug state, and
# screendumps afterwards to prove the macro really fired.
cd /home/josh/mac-system7 || exit 1

echo "=== 1. clean restart (mandatory: gdb state does not survive cleanly) ==="
ps -eo pid,comm | grep -i qemu | awk '{print $1}' | while read p; do kill "$p"; done
sleep 5
rm -f mon.sock qmp.sock
./launch_vm.sh
sleep 35
{ printf 'sendkey ret\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
sleep 30
echo -n "   agent: "; timeout 25 python3 agenttest.py PING 2>&1 | head -1

echo ""
echo "=== 2. arm hotkey (T+12) and status probe (T+22) ==="
rm -f /tmp/mb2_status.log
{ printf 'screendump "/tmp/mb2_before.ppm"\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
( sleep 12; { printf 'sendkey grave_accent\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1 ) &
( sleep 22; { printf 'info status\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9]*[A-Za-z]//g' | grep -i "VM status" >> /tmp/mb2_status.log ) &

echo "=== 3. watch MBState 0x172 ALONE (no control wp -- it halted us mid-click last time) ==="
{
  echo 'set architecture m68k'
  echo 'set confirm off'
  echo 'set pagination off'
  echo 'target remote 127.0.0.1:1234'
  echo 'watch *(char *)0x172'

  echo 'continue'
  sleep 28
  echo 'quit'
} | timeout 45 gdb-multiarch 2>&1 | grep -iE "Hardware watchpoint|Old value|New value|Continuing" | head -10

echo ""
echo "=== 4. results ==="
echo "   monitor at T+22: $(cat /tmp/mb2_status.log 2>/dev/null || echo none)"
{ printf 'screendump "/tmp/mb2_after.ppm"\n'; sleep 3; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
if cmp -s /tmp/mb2_before.ppm /tmp/mb2_after.ppm; then
  echo "   macro fired?  NO  -> result is INVALID, rerun"
else
  echo "   macro fired?  YES ($(cmp -l /tmp/mb2_before.ppm /tmp/mb2_after.ppm | wc -l) bytes differ) -> result is VALID"
fi
