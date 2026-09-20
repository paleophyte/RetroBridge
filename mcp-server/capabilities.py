"""Advisory bridge coverage for this source tree, not wire-level negotiation."""


def profile_for(info: dict[str, str]) -> str:
    family = info.get("os_family", "")
    if family in ("9x", "nt"):
        return "win32"
    if family == "os2":
        return {"llm_agent-os2": "os2", "llm_agent-os2-13": "os2-13"}.get(info.get("agent", ""), "unknown")
    return family if family in ("dos", "win16", "netware", "mac68k") else "unknown"


COMMON = {
    "legacy_ping", "legacy_sysinfo", "legacy_capabilities", "legacy_upload",
    "legacy_download", "legacy_screenshot", "legacy_screenshot_file",
    "legacy_key", "legacy_type", "legacy_wait_for_agent",
}
GUI = {"legacy_click", "legacy_winlist", "legacy_ps", "legacy_kill"}
EXEC = {"legacy_exec", "legacy_exec_detach"}
JOBS = {"legacy_job_start", "legacy_job_status", "legacy_job_output", "legacy_job_cancel", "legacy_job_release"}
TOOLS = {
    "win32": COMMON | GUI | EXEC | {
        "legacy_reboot", "legacy_shutdown", "legacy_clipboard_set", "legacy_reg_get",
        "legacy_reg_set", "legacy_enable_autologon", "legacy_disable_autologon",
        "legacy_wait_for_desktop", "legacy_self_update"},
    "win16": COMMON | GUI | EXEC | {
        "legacy_reboot", "legacy_shutdown", "legacy_winmsg", "legacy_postmsg",
        "legacy_lbgettext", "legacy_win16_self_update"},
    "dos": COMMON | {"legacy_exec", "legacy_reboot"},
    "os2": COMMON | GUI | EXEC | {
        "legacy_winclose", "legacy_clipboard_set", "legacy_reboot", "legacy_self_update"},
    "os2-13": COMMON | GUI | EXEC | {
        "legacy_winclose", "legacy_reboot", "legacy_self_update"},
    "netware": COMMON | {"legacy_exec", "legacy_shutdown", "legacy_screens",
                         "legacy_autoexec", "legacy_debug", "legacy_netware_self_update"},
    "mac68k": COMMON | {"legacy_click", "legacy_double_click", "legacy_mouse_position",
                        "legacy_ps", "legacy_kill", "legacy_reboot", "legacy_shutdown",
                        "legacy_mac_self_update"},
    "unknown": {"legacy_sysinfo", "legacy_capabilities"},
}
NOTES = {
    "win32": ["Secure desktops are not controlled by synthetic input; screenshot access depends on the agent session."],
    "win16": ["WINLIST accepts parent_hwnd for immediate children.",
              "Journal mouse/key/type injection remains unreliable; prefer window/message controls.",
              "WINMSG is synchronous and can block on a modal dialog; POSTMSG only acknowledges queueing.",
              "LBGETTEXT requires a string-backed listbox; use Win16 message constants (some differ from Win32); pointer parameters are not marshalled.",
              "SHUTDOWN exits Windows to DOS; detached replies do not provide a real PID."],
    "dos": ["Screenshot is an 80x25 text rendering; BIOS keyboard input is limited.",
            "EXEC is synchronous and blocks other clients; no detach or built-in update."],
    "os2": ["WINCLOSE requests close/cancel for every exact title match, ignoring case; apps may prompt or refuse."],
    "os2-13": ["No clipboard support; process listing parses PSTAT.",
               "WINCLOSE requests close/cancel for every exact title match, ignoring case.",
               "Self-update has an observed intermittent rollback; inspect the verification result."],
    "netware": ["EXEC runs a console command without output capture or detach.",
                "DEBUG 1 truncates the agent log; AUTOEXEC may edit startup NCF with a retained backup.",
                "SHUTDOWN downs the server; it is not a hardware power-off command.",
                "Update stages fixed SYS:SYSTEM paths; verification requires startup identity and matching executable readback. Protocol-2 helpers retain backups and roll back exited candidates; unresolved or loaded/unready candidates require operator recovery."],
    "mac68k": ["Data-fork file transfer only; main-display screenshot; left mouse button and US keyboard layout.",
               "No shell, usable cross-process clipboard/window list, or completed drag support.",
               "PSKILL and power commands are cooperative requests and can be cancelled.",
               "MacBinary update requires the separately installed llm_updater; verification requires new startup identity and matching application forks, with system resource metadata normalized."],
    "unknown": ["No known profile for these SYSINFO fields; no platform operations inferred."],
}


def describe(info: dict[str, str]) -> dict:
    profile = profile_for(info)
    tools = TOOLS[profile].copy()
    if info.get("exec_jobs") == "1" and profile in {"win32", "win16", "os2", "os2-13"}:
        tools |= JOBS
        if info.get("job_cancel_scope") == "unsupported":
            tools.discard("legacy_job_cancel")
    if info.get("os_family") == "9x":
        tools -= {"legacy_enable_autologon", "legacy_disable_autologon"}
    return {
        "profile": profile,
        "basis": "Advisory source-tree profile inferred from SYSINFO; not negotiated or proof of installed-build support.",
        "identity": {key: info[key] for key in ("os_family", "agent", "agent_build", "agent_started") if key in info},
        "tools": sorted(tools),
        "limitations": NOTES[profile],
        "jobs": {key: info[key] for key in ("exec_jobs", "job_cancel_scope", "job_output_storage", "job_command_modes") if key in info},
        "text_encoding_policy": "Explicit per-machine codecs with strict conversion; default ASCII. TYPE/KEY remain ASCII-only; files stay raw bytes. See docs/TEXT_ENCODINGS.md.",
    }
