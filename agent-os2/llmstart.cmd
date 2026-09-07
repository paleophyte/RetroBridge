/* llmstart.cmd -- registers LLMAGENT.EXE as a WPS Startup-folder object.
 *
 * CONFIG.SYS RUN= lines execute before the WPS/Session Manager finishes
 * initializing, so a process launched that way gets an incomplete session
 * context: its own DosStartSession calls fail forever after (breaks this
 * agent's EXEC/REBOOT/EXECDETACH/self-update). WPS Startup-folder objects
 * are launched by the shell itself, after WPS is fully up -- same session
 * lineage as double-clicking the icon by hand, which is known to work.
 *
 * Idempotent: OBJECTID + REPLACE means re-running this just updates the
 * existing entry instead of creating duplicates.
 */
call RxFuncAdd 'SysCreateObject', 'RexxUtil', 'SysCreateObject'

class    = 'WPProgram'
title    = 'LLM Agent'
location = '<WP_START>'
setup    = 'EXENAME=C:\LLMAGENT\LLMAGENT.EXE;' || ,
           'STARTUPDIR=C:\LLMAGENT;' || ,
           'PROGTYPE=WINDOWABLEVIO;' || ,
           'MINIMIZED=YES;' || ,
           'OBJECTID=<LLMAGENT_START>;'

result = SysCreateObject(class, title, location, setup, 'REPLACE')

if result then
    say 'SysCreateObject OK -- LLM Agent added to Startup folder'
else
    say 'SysCreateObject FAILED'
