#!/bin/bash
# Launch the System 7.5.3 guest.
#
# Adds three things over the original hand-typed command line:
#   -s                 gdbstub on localhost:1234 (VM still runs free; -S would halt it)
#   -qmp  qmp.sock     JSON control socket -- reliable for scripting, unlike the
#                      human monitor whose readline echo mangles scraped output
#   -monitor mon.sock  kept as-is for screendump / sendkey / mouse_move / savevm
#
# NOTE: deliberately one long line, no backslash continuations -- those get eaten
# when this file is written through a shell heredoc.
cd /home/josh/mac-system7 || exit 1

LOG="qemu_$(date +%Y%m%d_%H%M%S).log"

setsid nohup qemu-system-m68k -M q800 -bios quadra800.rom -drive file=hdd753.qcow2,format=qcow2,if=none,id=hd0,cache=writethrough -device scsi-hd,drive=hd0 -drive file=visexfer.iso,format=raw,if=none,id=cd0,media=cdrom -device scsi-cd,drive=cd0 -nic tap,model=dp83932,mac=52:54:00:75:03:01,ifname=tap-mac1,script=no,downscript=no -audio none -display vnc=:1 -monitor unix:/home/josh/mac-system7/mon.sock,server,nowait -qmp unix:/home/josh/mac-system7/qmp.sock,server,nowait -gdb tcp:127.0.0.1:1234 -m 256 </dev/null >"$LOG" 2>&1 &

echo "launched, log=$LOG"
