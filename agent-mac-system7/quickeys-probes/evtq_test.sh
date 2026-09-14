#!/bin/bash
# Does QuicKeys enqueue a mouse event into the low-level event queue?
#
# EventQueue is a QHdr at 0x014A: qFlags(2) qHead(4)@0x14C qTail(4)@0x150.
# Any Enqueue() changes qTail, so we watch qTail and inspect each newly queued
# EvQEl as it arrives.
#
# EvQEl layout from the queue element pointer:
#   +0 qLink(4)  +4 qType(2)  +6 evtQWhat(2)  +8 evtQMessage(4)
#   +12 evtQWhen(4)  +16 evtQWhere(4, Point)  +20 evtQModifiers(2)
# evtQWhat: 1=mouseDown 2=mouseUp 3=keyDown 4=keyUp 5=autoKey
#
# CONFOUND: the grave_accent hotkey itself generates a keyDown, so the first
# hit is expected to be evtQWhat=3. The question is whether a 1 or 2 follows.
#
# gdb commands are fed on stdin with sleeps BETWEEN them: 'continue' returns
# asynchronously, so a command sent immediately after it dies with "target is
# running". Sleeping lets the watchpoint actually hit first.
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

{ printf 'screendump "/tmp/eq_before.ppm"\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
echo ""
echo "=== 2. hotkey fires at T+14 (gdb starts now) ==="
( sleep 14; { printf 'sendkey grave_accent\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1 ) &

DUMP='printf "  qTail=%x qHead=%x\n", *(unsigned long*)0x150, *(unsigned long*)0x14C'
ELEM='printf "  evtQWhat=%d msg=%x where=%x\n", *(unsigned short*)(*(unsigned long*)0x150+6), *(unsigned long*)(*(unsigned long*)0x150+8), *(unsigned long*)(*(unsigned long*)0x150+16)'

{
  echo 'set architecture m68k'
  echo 'set confirm off'
  echo 'set pagination off'
  echo 'target remote 127.0.0.1:1234'
  echo 'printf "IDLE STATE:\n"'
  echo "$DUMP"
  echo 'watch *(long *)0x150'
  echo 'continue'
  sleep 8
  echo 'printf "\n--- after 8s idle (before hotkey) ---\n"'
  echo "$DUMP"
  echo 'continue'
  sleep 12
  echo 'printf "\n--- HIT A ---\n"'
  echo "$DUMP"; echo "$ELEM"
  echo 'continue'
  sleep 4
  echo 'printf "\n--- HIT B ---\n"'
  echo "$DUMP"; echo "$ELEM"
  echo 'continue'
  sleep 4
  echo 'printf "\n--- HIT C ---\n"'
  echo "$DUMP"; echo "$ELEM"
  echo 'continue'
  sleep 4
  echo 'printf "\n--- HIT D ---\n"'
  echo "$DUMP"; echo "$ELEM"
  echo 'quit'
} | timeout 70 gdb-multiarch 2>&1 | grep -viE "^warning:|No executable|Try using|determining executable|^Reading|Remote debugging|^\[" | grep -vE "^\s*$"

echo ""
echo "=== 3. did the macro fire? ==="
{ printf 'screendump "/tmp/eq_after.ppm"\n'; sleep 3; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
cmp -s /tmp/eq_before.ppm /tmp/eq_after.ppm && echo "   NO -> INVALID" || echo "   YES ($(cmp -l /tmp/eq_before.ppm /tmp/eq_after.ppm | wc -l) bytes) -> VALID"
