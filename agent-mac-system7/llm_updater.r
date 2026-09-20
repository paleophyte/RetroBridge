/* Resource overrides for llm_updater -- same background-only SIZE(-1)
 * rationale as llm_agent.r. llm_updater yields during a teardown grace
 * period, then prepares/exchanges/launches the replacement. This resource
 * just keeps it from ever flashing a frontmost layer during that brief
 * window. */

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
    512 * 1024,
    512 * 1024
};
