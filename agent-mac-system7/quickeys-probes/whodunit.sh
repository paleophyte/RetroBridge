#!/bin/bash
# WHO writes MTemp, and what happens immediately afterwards?
#
# The MTemp (0x828) watchpoint is a reliable trigger that lands us inside the
# click sequence. Once halted there, $pc identifies the module, and single
# stepping forward walks into whatever does the BUTTON half -- the part no
# trap sweep, journaling probe or MBState watch has been able to see.
#
# Module map (addresses verified stable this session):
#   CDRV_1        0x00160200 .. 0x0016861A
#   CDRV_0        0x0016EC00 .. 0x00186A00   (the .QuicKeys driver)
#   CODE_1        0x0071FF30 .. 0x00728B28   (QuicKeys Toolbox main segment)
#   ROM           0x40800000 ..
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

echo ""
echo "=== 2. hotkey at T+14; watchpoint on MTemp catches the click ==="
( sleep 14; { printf 'sendkey grave_accent\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1 ) &

{
  echo 'set architecture m68k'
  echo 'set endian big'
  echo 'set confirm off'
  echo 'set pagination off'
  echo 'target remote 127.0.0.1:1234'
  echo 'watch *(long *)0x828'
  echo 'continue'
  sleep 20
  echo 'printf "\n===== HALTED INSIDE THE CLICK =====\n"'
  echo 'printf "PC = %x\n", $pc'
  echo 'printf "A0=%x A1=%x D0=%x D1=%x\n", $a0, $a1, $d0, $d1'
  echo 'printf "\n--- code at PC ---\n"'
  echo 'x/12i $pc-24'
  echo 'printf "\n--- stack ---\n"'
  echo 'x/8xw $sp'
  echo 'printf "\n--- PC TRAIL (next 40 instructions) ---\n"'
  for i in $(seq 1 40); do echo 'printf "%x  ", $pc'; echo 'stepi'; done
  echo 'printf "\n\n--- where we ended up ---\n"'
  echo 'printf "PC = %x\n", $pc'
  echo 'x/10i $pc'
  echo 'printf "\nMBState now = %x   MTemp now = %x\n", *(unsigned char*)0x172, *(unsigned long*)0x828'
  echo 'quit'
} | timeout 90 gdb-multiarch 2>&1 | grep -viE "^warning:|No executable|Try using|determining executable|^Reading|Remote debugging|^\[" | grep -vE "^\s*$"
