#!/bin/bash
# POSITIVE CONTROL for the watchpoint method.
#
# A "watchpoint never fired" result is worthless unless watchpoints demonstrably
# fire on this stub. Control A watches Ticks (0x16A), which the OS bumps 60x a
# second, so it must halt almost immediately. If the VM halts, watchpoints work.
cd /home/josh/mac-system7 || exit 1

run_wp () {
  local NAME="$1"; local EXPR="$2"; local FIREKEY="$3"
  echo ""
  echo "########## $NAME : watch $EXPR ##########"
  { printf 'loadvm baseline\n'; sleep 3; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
  sleep 2
  rm -f /tmp/wp_status.log
  if [ "$FIREKEY" = "yes" ]; then
    ( sleep 10; { printf 'sendkey grave_accent\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1 ) &
  fi
  ( sleep 18; { printf 'info status\n'; sleep 2; } | socat - UNIX-CONNECT:./mon.sock 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9]*[A-Za-z]//g' | grep -i "VM status" >> /tmp/wp_status.log ) &
  {
    echo 'set architecture m68k'
    echo 'set confirm off'
    echo 'set pagination off'
    echo 'target remote 127.0.0.1:1234'
    echo "watch $EXPR"
    echo 'continue'
    sleep 25
    echo 'quit'
  } | timeout 45 gdb-multiarch 2>&1 | grep -iE "Hardware watchpoint|Old value|New value|^0x|Continuing|watchpoint [0-9]" | head -12
  echo "--- monitor says: $(cat /tmp/wp_status.log 2>/dev/null || echo none) ---"
  # always leave the VM running again for the next test
  { printf 'cont\n'; sleep 1; } | socat - UNIX-CONNECT:./mon.sock >/dev/null 2>&1
}

run_wp "CONTROL A: Ticks 0x16A (OS bumps 60Hz, MUST fire)" '*(long *)0x16A' no
run_wp "CONTROL B: MTemp 0x828 (Mechanism 1 writes on click)" '*(long *)0x828' yes
