/* Resource overrides for llm_agent.
 *
 * Retro68's default SIZE(-1) (from Retro68APPL.r) launches the app as a
 * normal foreground process. Since this agent never calls
 * WaitNextEvent/GetNextEvent in its main loop (its whole job is
 * MacTCP polling), a foreground launch permanently holds the frontmost
 * layer with no way for MultiFinder to hand input focus to anything
 * else -- the user's mouse/keyboard clicks just queue up forever. This
 * SIZE resource marks it background-only instead, which is what this
 * app actually is: a headless TCP service with no windows or UI.
 *
 * (A menu-bar status icon was also tried here and caused a fatal
 * double MMU fault that crashed QEMU itself -- see the NOTE in
 * llm_agent.c's main(). Not part of this build.)
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
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    1024 * 1024,
    1024 * 1024
};
