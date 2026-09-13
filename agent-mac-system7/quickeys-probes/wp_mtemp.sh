#!/bin/bash
# Watchpoint on MTemp (0x828) across a real click, on a freshly restarted QEMU.
#
# Lesson from the last attempt: a gdb session killed by 'timeout' while it owns
# a halted VM leaves the QEMU process in a state that crashes the guest
# (Finder error type 11) and that loadvm does NOT clear. So: run ONE measurement,
# then restart QEMU unconditionally, whatever the outcome.
cd /home/josh/mac-system7 || exit 1

rm -f /tmp/wpm_status.log
( sleep 12; { printf 'sendkey grave_accent\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1 ) &
( sleep 22; { printf 'info status\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9]*[A-Za-z]//g' | grep -i "VM status" >> /tmp/wpm_status.log ) &

echo "=== watchpoint on MTemp 0x828 during a real click ==="
{
  echo 'set architecture m68k'
  echo 'set confirm off'
  echo 'set pagination off'
  echo 'target remote 127.0.0.1:1234'
  echo 'watch *(long *)0x828'
  echo 'continue'
  sleep 28
  echo 'quit'
} | timeout 45 gdb-multiarch 2>&1 | grep -iE "Hardware watchpoint|Old value|New value|Continuing|^0x0" | head -10

echo ""
echo "--- monitor at T+22s: $(cat /tmp/wpm_status.log 2>/dev/null || echo none) ---"
echo "    (paused = watchpoint FIRED, running = it did not)"
