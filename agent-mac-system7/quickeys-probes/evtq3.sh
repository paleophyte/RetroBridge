#!/bin/bash
# EventQueue take 3. Fixes take 2's "Invalid argument syntax": gdb does NOT
# accept ';'-separated commands on one line -- each needs its own line.
#
# Take 2 showed qTail oscillating 0 <-> 0x1A340, so the EvQEl always lands at
# 0x1A340. Dumping that fixed address avoids dereferencing qTail when it is 0.
#
#   +0 qLink(4)  +4 qType(2)  +6 evtQWhat(2)  +8 evtQMessage(4)
#   +12 evtQWhen(4)  +16 evtQWhere(4)  +20 evtQModifiers(2)
# evtQWhat: 1=mouseDown 2=mouseUp 3=keyDown 4=keyUp 5=autoKey
cd /home/josh/mac-system7 || exit 1

echo "=== 1. clean restart ==="
ps -eo pid,comm | grep -i qemu | awk '{print $1}' | while read p; do kill "$p"; done
sleep 5
rm -f mon.sock qmp.sock
./launch_vm.sh
sleep 35
{ printf 'sendkey ret\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
sleep 30
echo -n "   agent: "; timeout 25 python3 agenttest.py PING 2>&1 | head -1

{ printf 'screendump "/tmp/eq3_before.ppm"\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
echo ""
echo "=== 2. hotkey at T+16 ==="
( sleep 16; { printf 'sendkey grave_accent\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1 ) &

emit_dump () {
  echo "printf \"\n=== $1 ===\n\""
  echo 'printf "qTail/qHead: "'
  echo 'x/2xw 0x14C'
  echo 'printf "EvQEl@1A340 (qLink qType what msg when where mods):\n"'
  echo 'x/22xb 0x1A340'
}

{
  echo 'set architecture m68k'
  echo 'set endian big'
  echo 'set confirm off'
  echo 'set pagination off'
  echo 'target remote 127.0.0.1:1234'
  echo 'printf "ENDIAN CHECK MTemp (want 000f000f): "'
  echo 'x/1xw 0x828'
  echo 'watch *(long *)0x150'
  echo 'continue'
  sleep 11
  emit_dump "IDLE (pre-hotkey)"
  echo 'continue'
  sleep 9
  for n in 1 2 3 4 5 6 7 8; do
    emit_dump "HIT $n"
    echo 'continue'
    sleep 2
  done
  echo 'quit'
} | timeout 100 gdb-multiarch 2>&1 | grep -viE "^warning:|No executable|Try using|determining executable|^Reading|Remote debugging|^\[" | grep -vE "^\s*$"

echo ""
echo "=== 3. macro fired? ==="
{ printf 'screendump "/tmp/eq3_after.ppm"\n'; sleep 3; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
cmp -s /tmp/eq3_before.ppm /tmp/eq3_after.ppm && echo "   NO -> INVALID" || echo "   YES ($(cmp -l /tmp/eq3_before.ppm /tmp/eq3_after.ppm | wc -l) bytes) -> VALID"
