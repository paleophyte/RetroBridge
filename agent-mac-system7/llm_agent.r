/* Resource overrides for llm_agent.
 *
 * Retro68's default SIZE(-1) (from Retro68APPL.r) launches the app as a
 * normal foreground process. This SIZE resource instead marks the app
 * background-only: it is a headless TCP service with no windows or UI.
 * Its cooperative MacTCP polling loop calls WaitNextEvent and dispatches
 * high-level events, including Finder's Quit Application event.
 *
 * (A menu-bar status icon was also tried here and caused a fatal
 * double MMU fault that crashed QEMU itself -- see the NOTE in
 * llm_agent.c's main(). Not part of this build.)
 *
 * isHighLevelEventAware is required for PSKILL. The AppleEvent Manager
 * refuses to let an application *send* a high-level event unless its own
 * SIZE resource claims awareness of them: AESend returns -903 (noPortErr)
 * otherwise, which is what this flag being "not" aware used to produce. The
 * matching Quit handler and AEProcessAppleEvent dispatch are essential:
 * Finder waits for event-aware applications to quit before restarting or
 * shutting down. onlyLocalHLEvents keeps the scope to this machine.
 */

#include "Types.r"
#include "Processes.r"

resource 'SIZE' (-1) {
    reserved,
    ignoreSuspendResumeEvents,
    reserved,
    canBackground,
    needsActivateOnFGSwitch,
    onlyBackground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    1024 * 1024,
    1024 * 1024
};
