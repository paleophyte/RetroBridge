# Place a copy of the guest's 16-bit TCP/IP DLL here before building:

  TCPIPDLL.DLL

build.bat will create tcpipdll.lib via wlib.

## Finding the right DLL on the OS/2 1.3 guest

`os2sock.h` in this directory binds to entry points named `_sock_init`,
`_socket`, `_bind`, ... — the classic IBM 16-bit Berkeley-sockets-workalike
export set. This is what IBM's TCP/IP for OS/2 has shipped as `TCPIPDLL.DLL`
across the 1.x/2.x line (the `../agent-os2` 32-bit port still ships a copy
of this exact header, unused there now, left from when that agent was also
16-bit — see its git history).

If the guest's TCP/IP stack DLL turns out to be named differently, or
exports these under different names, you have two options:

1. Rename the guest DLL's copy here to `TCPIPDLL.DLL` before running
   build.bat, if the exports match but the filename doesn't.
2. Edit the `#pragma aux ... "_name"` lines in `os2sock.h` to match
   whatever the guest DLL actually exports (check with a resource/export
   dumper, or IBM's TCP/IP for OS/2 documentation for the installed
   version) if the export names themselves differ.

Do not commit IBM TCP/IP DLLs to this repo.
