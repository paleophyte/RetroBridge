/* RESTART.EXE -- tiny native Win16 helper used by llm_agent.c's UPDATE
   command to self-update without a full system REBOOT.

   Windows 3.1 keeps a module's code resident in memory by name until
   every instance of it has exited (GetModuleUsage() hits zero) --
   overwriting LLMAGENT.EXE on disk (PUT) while the old instance is
   still running does NOT take effect if you just WinExec() the same
   path again; that hands back another instance of whatever code is
   already loaded, not the new bytes on disk. This has to be a SEPARATE
   process that outlives the old agent: give it time to actually exit,
   then launch a fresh copy.

   This used to poll GetModuleHandle("LLMAGENT")/GetModuleUsage() in a
   tight Yield()-driven loop instead of a flat delay. Confirmed by
   direct testing that this was NOT reliable: the old instance's own
   exit sequence intermittently crashed, a different fault each time
   (a GPF inside the CRT's _exit_ once, an illegal instruction inside
   strpbrk_ another time) -- different addresses, different fault
   types, same general moment: right around when this helper's poll
   loop was actively querying the old task's module state. Win16's
   cooperative scheduler means only one task truly executes at a time,
   but polling KERNEL's module-tracking data *while* another task is
   mid-teardown still isn't necessarily safe if that task's own exit
   path yields control back (directly or via a KERNEL call that pumps
   messages internally) at a moment its own state is half torn down --
   this poll loop calling back into KERNEL right then is a plausible
   way to have corrupted something differently each time. A flat,
   hands-off delay removes that contention entirely: don't query the
   old task's state at all, just give it a generous, quiet window to
   finish exiting on its own before touching anything.

   Usage: RESTART.EXE <full path to LLMAGENT.EXE to relaunch> */

#include <windows.h>

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR cmdline, int cmdShow) {
    DWORD start;
    const DWORD wait_ms = 10000UL;
    char target[144];

    (void)hInst;
    (void)hPrevInst;
    (void)cmdShow;

    if (cmdline && cmdline[0]) {
        lstrcpyn(target, cmdline, sizeof(target));
    } else {
        lstrcpyn(target, "C:\\LLMWIN\\LLMAGENT.EXE", sizeof(target));
    }

    /* Deliberately does NOT call GetModuleHandle()/GetModuleUsage() at
       all during this wait -- see the comment above for why querying
       the old task's module state while it's mid-teardown is the
       leading suspect for the intermittent crashes this replaced.
       PeekMessage/Yield here only pump OUR OWN message queue (this
       helper never created a window and isn't waiting on anything
       Windows-related), so this stays cooperative without touching the
       old task at all. */
    start = GetTickCount();
    while (GetTickCount() - start < wait_ms) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Yield();
    }

    WinExec(target, SW_SHOWNORMAL);
    return 0;
}
