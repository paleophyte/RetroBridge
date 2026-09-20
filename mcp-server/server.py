"""MCP bridge/server exposing shell exec + file transfer + screenshot +
input injection on legacy Windows boxes (WFW 3.11, 95/98/ME/NT4/2000/XP),
FreeDOS, OS/2 (1.3 and 2.11), NetWare, and classic Mac System 7 to an LLM client.

Supports multiple legacy machines from one bridge process: each is a
section in machines.ini (see machines.ini.example), and every tool takes
a `machine` argument naming which one to target. Call legacy_list_machines
to discover what's configured.

Everything goes through one target-agent channel per machine: llm_agent
(agent-win32/llm_agent.c on Windows, agent-dos/llm_agent.c on FreeDOS,
agent-os2/llm_agent.c on OS/2 2.11, agent-os2-13/llm_agent.c on OS/2 1.3 -
a separate 16-bit build, since 1.3 predates the 32-bit kernel entirely), a
tiny token-authed TCP service, assumed to be on an isolated lab/host-only
network (see docs/ARCHITECTURE.md for the trust model). On Windows,
screenshot/click/key/type are built into the agent itself (GDI capture +
mouse_event/keybd_event). On FreeDOS, screenshot is a text-mode render and
key/type stuff the BIOS keyboard buffer; on OS/2 2.11, most GUI/window/
process tools work over 32-bit PM, with registry/shutdown returning ERR;
OS/2 1.3 supports the same set minus CLIPSET (no 16-bit equivalent
found/validated yet); self-update works there but has one intermittent
failure that rolls back safely, so check the result rather than looping on
it - see agent-os2-13/README.md - see docs/ARCHITECTURE.md / agent-dos /
agent-os2 / agent-os2-13.

LEGACY_MACHINES_FILE points at the ini file; defaults to machines.ini next
to this script.
"""

from __future__ import annotations

import hashlib
import io
import json
import ntpath
import os
import tempfile
import time
from pathlib import Path

from mcp.server.mcpserver import Image, MCPServer
from PIL import Image as PILImage

from agent_client import AgentAuthError, AgentClient, AgentProtocolError
from capabilities import describe as describe_capabilities, profile_for
from machines import MachineConfig, MachineConfigError, load_machines
from text_codec import normalize_encoding, decode_text

MACHINES_FILE = Path(
    os.environ.get("LEGACY_MACHINES_FILE") or (Path(__file__).parent / "machines.ini")
)

_machines_cache: dict[str, MachineConfig] | None = None


def _machines() -> dict[str, MachineConfig]:
    global _machines_cache
    if _machines_cache is None:
        _machines_cache = load_machines(MACHINES_FILE)
    return _machines_cache


def _machine(name: str) -> MachineConfig:
    machines = _machines()
    if name not in machines:
        available = ", ".join(sorted(machines)) or "(none configured)"
        raise MachineConfigError(f"unknown machine {name!r}. Configured machines: {available}")
    return machines[name]


srv = MCPServer(
    "retro-ssh-server",
    instructions=(
        "MCP tools for driving legacy machines through small target agents "
        "on an isolated lab network: "
        "Windows 95/98/ME/NT4/2000/XP, Windows for Workgroups 3.11 "
        "(agent-win16), FreeDOS (agent-dos), OS/2 2.x "
        "(agent-os2), OS/2 1.3 (agent-os2-13, a separate 16-bit build), "
        "NetWare 3.12+ (agent-netware), and classic Mac System 7 (agent-mac-system7). "
        "Run shell commands, transfer files, take screenshots, "
        "and send keyboard input. Every tool takes a `machine` argument "
        "naming which configured machine to target - call "
        "legacy_list_machines first if you don't already know the name. "
        "Call legacy_capabilities for SYSINFO-based platform tools and limitations; "
        "this is advisory, not negotiated support for an arbitrary installed build. "
        "Use legacy_screenshot before legacy_click/legacy_key when you "
        "don't already know current on-screen coordinates. FreeDOS agents "
        "support ping/exec/upload/download/sysinfo/reboot plus text-mode "
        "screenshot and key/type; OS/2 2.11 agents support ping/exec/"
        "exec_detach/upload/download/sysinfo/screenshot/click/key/type/"
        "winlist/pslist/pskill/reboot/clipboard/self-update; OS/2 1.3 "
        "agents support the same set except clipboard (no 16-bit equivalent "
        "available/validated yet - returns an error) and self-update "
        "(works, but with one intermittent failure that rolls back "
        "safely - check legacy_self_update's result rather than assuming "
        "it succeeded). "
        "FreeDOS clipboard/registry/shutdown and OS/2 registry/shutdown "
        "still return errors. FreeDOS "
        "screenshots are "
        "rendered "
        "from the 80x25 text screen, not a GUI framebuffer. Note: a "
        "synthetic ctrl-alt-del will not unlock a locked/secure-desktop "
        "Windows screen - that's an OS security measure, not a bug. "
        "legacy_reboot/legacy_shutdown require confirm=True and may "
        "immediately interrupt work. Mac requests go through Finder and "
        "can prompt to save or be cancelled; an acknowledgment does not "
        "prove completion."
    ),
)


def _agent(machine: str) -> AgentClient:
    m = _machine(machine)
    if not m.agent_enabled:
        raise MachineConfigError(
            f"machine {machine!r} is inventory-only (agent_enabled=false); "
            "agent tools require llm_agent. Use a separate SSH client for SSH hosts."
        )
    return AgentClient(m.host, m.exec_port, m.exec_token,
                       max_command_bytes=m.max_command_bytes,
                       max_response_bytes=m.max_response_bytes,
                       text_encoding=m.text_encoding, exec_encoding=m.exec_encoding,
                       exec_command_encoding=m.exec_command_encoding, file_encoding=m.file_encoding)


@srv.tool()
def legacy_list_machines() -> str:
    """List machines configured in machines.ini, without credentials.
    Inventory-only entries (agent_enabled=false) are listed explicitly;
    they cannot be used with agent tools. Call this first to find a target."""
    try:
        machines = _machines()
    except MachineConfigError as e:
        return f"[config error] {e}"
    if not machines:
        return "no machines configured"
    lines = []
    for m in machines.values():
        suffix = f" vm_name={m.vm_name}" if m.vm_name else ""
        if m.agent_enabled:
            lines.append(f"{m.name}: host={m.host} exec_port={m.exec_port}{suffix}")
        else:
            lines.append(f"{m.name}: host={m.host} agent_enabled=false (inventory-only){suffix}")
    return "\n".join(lines)


@srv.tool()
def legacy_exec(machine: str, command: str, output_encoding: str | None = None) -> str:
    """Run a command using the target platform's shell/console command handler.
    Returns captured output and exit status where supported; NetWare provides
    no output capture, and Mac has no shell. One-shot per call with no persisted
    shell state. Synchronous commands can occupy a single-client agent.
    Output uses the machine's exec_encoding, or a per-call output_encoding
    override (e.g. utf-16-le for an explicitly Unicode-producing program).
    This changes decoding only, not the guest console/code page."""
    try:
        override = normalize_encoding(output_encoding, output=True) if output_encoding is not None else None
        agent = _agent(machine)
        result = agent.exec(command)
    except ValueError as e:
        return f"[protocol/input error] {e}"
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    try:
        text = decode_text(result.output, override or agent.exec_encoding, "EXEC output")
    except ValueError as e:
        return f"[output decoding error] {e}\n[command already executed; exit code: {result.exit_code}]"
    return f"{text}\n[exit code: {result.exit_code}]"


@srv.tool()
def legacy_exec_detach(machine: str, command: str) -> str:
    """Launch a program on the named legacy machine and return
    immediately without waiting for it to exit. Runs the program
    directly, not through cmd.exe - no &&, %VAR% expansion, redirection,
    or built-ins like `dir`/`start`; use legacy_exec for those. The PID
    in the reply is the actual launched program (safe to pass to
    legacy_kill or look for in legacy_ps). Use this for GUI apps, browser
    launches, installers that keep running, or helper scripts that write
    their own log file."""
    try:
        result = _agent(machine).exec_detach(command)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"launched detached on {machine}: {result.reply}"


@srv.tool()
def legacy_ping(machine: str) -> str:
    """Check that the llm_agent exec service on the named legacy machine
    is reachable and the configured token is accepted."""
    try:
        ok = _agent(machine).ping()
    except (MachineConfigError, AgentAuthError) as e:
        return f"unreachable/auth failed: {e}"
    except AgentProtocolError as e:
        return f"[protocol/input error] {e}"
    except OSError as e:
        return f"unreachable: {e}"
    return "ok" if ok else "unexpected response"


@srv.tool()
def legacy_upload(machine: str, local_path: str, remote_path: str) -> str:
    """Copy a file from this control machine to the named legacy machine."""
    try:
        n = _agent(machine).put(local_path, remote_path)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"
    return f"uploaded {n} bytes to {machine}: {local_path} -> {remote_path}"


@srv.tool()
def legacy_download(machine: str, remote_path: str, local_path: str) -> str:
    """Copy a file from the named legacy machine to this control machine."""
    try:
        n = _agent(machine).get(remote_path, local_path)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"
    return f"downloaded {n} bytes from {machine}: {remote_path} -> {local_path}"


@srv.tool()
def legacy_screenshot(machine: str) -> Image:
    """Capture the current screen of the named legacy machine. Requires
    the agent to be running as an interactive service (see
    docs/ARCHITECTURE.md) - if it isn't, this may return a blank/black
    image instead of erroring, since capturing a disconnected window
    station is a valid (just useless) result."""
    try:
        bmp = _agent(machine).screenshot()
    except (MachineConfigError, AgentAuthError) as e:
        raise RuntimeError(f"auth/config error: {e}") from e
    except AgentProtocolError as e:
        raise RuntimeError(f"protocol error: {e}") from e
    except OSError as e:
        raise RuntimeError(f"connection error: {e}") from e
    png_buf = io.BytesIO()
    PILImage.open(io.BytesIO(bmp)).save(png_buf, format="PNG")
    return Image(data=png_buf.getvalue(), format="png")


@srv.tool()
def legacy_screenshot_file(machine: str, local_path: str, image_format: str = "png") -> str:
    """Capture the current screen and save it on this control machine.
    Use this when a raw screenshot would be too large/noisy for the tool
    transcript. image_format is "png" or "bmp"; png is the default."""
    fmt = image_format.lower()
    if fmt not in ("png", "bmp"):
        return '[bad input] image_format must be "png" or "bmp"'
    try:
        bmp = _agent(machine).screenshot()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"

    path = Path(local_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if fmt == "bmp":
        path.write_bytes(bmp)
    else:
        PILImage.open(io.BytesIO(bmp)).save(path, format="PNG")
    return f"saved {fmt} screenshot from {machine} to {path}"


@srv.tool()
def legacy_click(machine: str, x: int, y: int, button: int = 1) -> str:
    """Move the mouse to (x, y) in screen coordinates on the named legacy
    machine and click. button: 1=left, 2=middle, 3=right."""
    try:
        _agent(machine).click(x, y, button)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"clicked ({x}, {y}) button {button} on {machine}"


@srv.tool()
def legacy_key(machine: str, key: str) -> str:
    """Press a single key or key combo on the named legacy machine, e.g.
    'enter', 'esc', 'tab', 'ctrl-alt-del', 'alt-tab', 'a', 'shift-a'.
    Key names/characters must be ASCII; native keyboard layout limits apply.
    A synthetic ctrl-alt-del will not unlock a locked/secure-desktop
    screen - that's intentional OS behavior, not a bug here."""
    try:
        _agent(machine).key(key)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"pressed {key} on {machine}"


@srv.tool()
def legacy_type(machine: str, text: str) -> str:
    """Type a string of ASCII text (no newlines - use legacy_key('enter')
    for that) on the named legacy machine, one keystroke per character.
    Non-ASCII is rejected before sending; use clipboard text where supported
    or transfer a file. Native keyboard layout/injection limits still apply.
    For special keys/combos use legacy_key."""
    try:
        _agent(machine).type_text(text)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    except ValueError as e:
        return f"[bad input] {e}"
    return f"typed {len(text)} characters on {machine}"


@srv.tool()
def legacy_ps(machine: str) -> str:
    """List running processes (PID and image name) on the named legacy
    machine."""
    try:
        procs = _agent(machine).pslist()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    if not procs:
        return "no processes returned"
    return "\n".join(f"{pid}\t{name}" for pid, name in procs)


@srv.tool()
def legacy_kill(machine: str, pid: int) -> str:
    """Forcibly terminate a process by PID on the named legacy machine.
    No protection against killing critical processes (including the
    agent's own) - same trust model as legacy_exec."""
    try:
        _agent(machine).pskill(pid)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"terminated pid {pid} on {machine}"


@srv.tool()
def legacy_sysinfo(machine: str) -> str:
    """OS family/version/service pack, computer name, memory, and C:
    disk space for the named legacy machine. Use this to detect what
    you're actually talking to instead of guessing from context."""
    try:
        info = _agent(machine).sysinfo()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return "\n".join(f"{k}={v}" for k, v in info.items())


@srv.tool()
def legacy_wait_for_agent(machine: str, timeout_seconds: int = 180, interval_seconds: int = 5) -> str:
    """Poll until the named machine's agent accepts connections. Useful
    immediately after legacy_reboot. Returns as soon as ping succeeds."""
    deadline = time.time() + max(1, timeout_seconds)
    interval = max(1, interval_seconds)
    last_error = ""
    attempts = 0
    while time.time() < deadline:
        attempts += 1
        try:
            if _agent(machine).ping():
                return f"agent reachable on {machine} after {attempts} attempt(s)"
            last_error = "unexpected ping response"
        except MachineConfigError as e:
            return f"[config error] {e}"
        except (AgentAuthError, OSError) as e:
            last_error = str(e)
        time.sleep(interval)
    return f"timed out waiting for agent on {machine}; last error: {last_error}"


@srv.tool()
def legacy_wait_for_desktop(machine: str, timeout_seconds: int = 180, interval_seconds: int = 5) -> str:
    """Poll until the interactive desktop looks logged in. This checks
    for Explorer or visible top-level windows, which is a better
    post-reboot readiness signal than agent ping alone."""
    deadline = time.time() + max(1, timeout_seconds)
    interval = max(1, interval_seconds)
    last = ""
    attempts = 0
    while time.time() < deadline:
        attempts += 1
        try:
            agent = _agent(machine)
            procs = agent.pslist()
            names = {name.lower() for _, name in procs}
            # userinit.exe deliberately excluded: it's the transient
            # process that launches the shell and then exits, so seeing
            # it running is more a "still logging in" signal than "ready" -
            # including it risked a premature/false-positive readiness
            # report in that narrow window.
            #
            # Suffix match, not membership: pslist() reports the FULL PATH
            # on Windows 9x ("c:\windows\explorer.exe"), not the bare
            # filename, so a plain `"explorer.exe" in names` check can
            # never match there. Masked for a long time by the window-list
            # fallback below always finding something else first (the
            # agent's own console window, or a login dialog) - only
            # surfaced once both of those stopped being reliably present
            # (agent console hidden via FreeConsole, login dialog removed
            # via Windows Logon) and this became the only signal left.
            if any(n.endswith("explorer.exe") for n in names):
                return f"desktop appears ready on {machine}: login shell process found after {attempts} attempt(s)"
            windows = agent.winlist()
            interesting = [w.title for w in windows if w.title and "program manager" not in w.title.lower()]
            if interesting:
                return f"desktop appears ready on {machine}: visible window {interesting[0]!r} after {attempts} attempt(s)"
            last = f"{len(procs)} processes, {len(windows)} visible windows"
        except MachineConfigError as e:
            return f"[config error] {e}"
        except (AgentAuthError, AgentProtocolError, OSError) as e:
            last = str(e)
        time.sleep(interval)
    return f"timed out waiting for desktop on {machine}; last observation: {last}"


@srv.tool()
def legacy_reboot(machine: str, confirm: bool = False) -> str:
    """Request a reboot of the named legacy machine; requires confirm=True.
    May immediately interrupt work. On Mac this asks Finder, which can
    prompt to save or be cancelled; success acknowledges delivery, not a
    completed reboot. Verify machine state before assuming it restarted."""
    if not confirm:
        return "not executed: pass confirm=True to actually reboot the machine"
    try:
        _agent(machine).reboot()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"reboot initiated on {machine}"


@srv.tool()
def legacy_shutdown(machine: str, confirm: bool = False) -> str:
    """Request shutdown of the named legacy machine; requires confirm=True.
    May immediately interrupt work; actual power-off depends on the platform.
    On Mac, Finder can prompt to save or cancel. Success acknowledges
    delivery, not completed shutdown; verify machine state separately."""
    if not confirm:
        return "not executed: pass confirm=True to actually shut down the machine"
    try:
        _agent(machine).shutdown()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"shutdown initiated on {machine}"


@srv.tool()
def legacy_winlist(machine: str, parent_hwnd: int | None = None) -> str:
    """List visible top-level windows (handle, position/size, class,
    title) on the named legacy machine. Use this to find dialogs/buttons
    by title instead of screenshotting and guessing pixel coordinates.
    Win16 only: parent_hwnd lists immediate child controls, with titles
    formatted as '<control id>:<text>'. Omit it for top-level windows."""
    try:
        agent = _agent(machine)
        if parent_hwnd is not None:
            _require_profile(agent, ("win16",))
            windows = agent.winlist(parent_hwnd)
        else:
            windows = agent.winlist()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    if not windows:
        return "no windows returned"
    return "\n".join(
        f"hwnd={w.hwnd} pos=({w.x},{w.y}) size={w.width}x{w.height} "
        f"class={w.class_name!r} title={w.title!r}"
        for w in windows
    )


@srv.tool()
def legacy_clipboard_set(machine: str, text: str) -> str:
    """Set the clipboard on the named legacy machine to plain text (no
    newlines). Follow with legacy_key(machine, 'ctrl-v') to paste it -
    more reliable than legacy_type for exact strings like product keys
    or paths, since it sidesteps keyboard-layout character mapping."""
    try:
        _agent(machine).clipboard_set(text)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    except ValueError as e:
        return f"[bad input] {e}"
    return f"clipboard set on {machine}"


@srv.tool()
def legacy_reg_get(machine: str, root: str, subkey: str, value_name: str) -> str:
    """Read a registry value on the named legacy machine. root is one of
    HKLM/HKCU/HKCR/HKU/HKCC. Only REG_SZ/REG_EXPAND_SZ/REG_DWORD are
    supported. Use this instead of legacy_exec + reg.exe - reg.exe
    doesn't exist by default before Windows XP."""
    try:
        value = _agent(machine).reg_get(root, subkey, value_name)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return str(value)


@srv.tool()
def legacy_reg_set(machine: str, root: str, subkey: str, value_name: str, value_type: str, data: str) -> str:
    """Write a registry value on the named legacy machine. root is one of
    HKLM/HKCU/HKCR/HKU/HKCC. value_type is "SZ" or "DWORD" (the only two
    supported). Creates the key if it doesn't already exist."""
    try:
        _agent(machine).reg_set(root, subkey, value_name, value_type, data)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    except ValueError as e:
        return f"[bad input] {e}"
    return f"set {root}\\{subkey}\\{value_name} on {machine}"


@srv.tool()
def legacy_enable_autologon(machine: str, username: str, password: str, domain: str = "", force: bool = True) -> str:
    """Configure NT-family Winlogon autologon on the named machine.
    Use before rebooting when the agent needs a logged-in interactive
    desktop after startup. The password is stored by Windows in plaintext
    under the Winlogon key, so only use this on isolated lab machines."""
    subkey = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
    try:
        agent = _agent(machine)
    except MachineConfigError as e:
        return f"[auth/config error] {e}"
    if not domain:
        try:
            domain = agent.sysinfo().get("computer_name", "")
        except (AgentAuthError, AgentProtocolError, OSError):
            pass  # fall back to domain="" (local account) below

    values = [
        ("AutoAdminLogon", "SZ", "1"),
        ("DefaultUserName", "SZ", username),
        ("DefaultPassword", "SZ", password),
        ("DefaultDomainName", "SZ", domain),
        ("ForceAutoLogon", "SZ", "1" if force else "0"),
        ("DisableCAD", "DWORD", "1"),
    ]
    try:
        for name, typ, data in values:
            agent.reg_set("HKLM", subkey, name, typ, data)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"enabled Winlogon autologon for {username!r} on {machine}"


@srv.tool()
def legacy_disable_autologon(machine: str) -> str:
    """Disable Winlogon autologon and clear the stored plaintext
    DefaultPassword value. Leaves DefaultUserName/DefaultDomainName alone."""
    subkey = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
    values = [
        ("AutoAdminLogon", "SZ", "0"),
        ("ForceAutoLogon", "SZ", "0"),
        ("DefaultPassword", "SZ", ""),
    ]
    try:
        agent = _agent(machine)
        for name, typ, data in values:
            agent.reg_set("HKLM", subkey, name, typ, data)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    return f"disabled Winlogon autologon on {machine}"


def _update_paths(remote_dir: str, *names: str) -> list[str]:
    """Reject traversal and obvious filename collisions before any PUT."""
    drive, tail = ntpath.splitdrive(remote_dir)
    if (len(drive) != 2 or drive[1] != ":" or not tail.startswith("\\")
            or any(c in tail for c in '/:*?"<>|\r\n\0\t')):
        raise ValueError("update directory must be an absolute drive path")
    if any(part in (".", "..") or part.endswith((".", " "))
           for part in tail.split("\\") if part):
        raise ValueError("update directory contains an ambiguous component")
    for name in names:
        if (not name or name in (".", "..") or name.endswith((".", " "))
                or any(c in name for c in '\\/:*?"<>|\r\n\0')):
            raise ValueError("update filenames must be plain, unambiguous filenames")
    paths = [ntpath.join(remote_dir, name) for name in names]
    if len({ntpath.normcase(p) for p in paths}) != len(paths):
        raise ValueError("staged agent, helper, and target must be different files")
    return paths


def _check_update_target(info: dict[str, str], target: str) -> None:
    actual = info.get("agent_exe")
    if actual and ntpath.normcase(ntpath.normpath(actual)) != ntpath.normcase(ntpath.normpath(target)):
        raise ValueError("target path differs from the responding agent's executable")


def _stage_verified(agent: AgentClient, local: Path, remote: str, scratch: Path) -> int:
    if not 0 < local.stat().st_size <= 64 * 1024 * 1024:
        raise ValueError("update files must be nonempty and at most 64 MiB")
    count = agent.put(local, remote)
    agent.get(remote, scratch)
    if scratch.read_bytes() != local.read_bytes():
        raise AgentProtocolError("staged file readback differs from the local snapshot")
    return count


def _wait_for_replaced_agent(
    machine: str,
    remote_target: str,
    old_started: str | None,
    expected_sha256: str,
    timeout_seconds: int = 120,
    interval_seconds: int = 5,
    settle_seconds: int = 15,
) -> str:
    """Verify a new startup identity, its startup hash, and installed bytes.

    Quiet settling is essential on single-client OS/2 agents: probing while
    the updater sends SELFEXIT has previously disrupted the swap. Never
    substitute PING or the on-disk AGENT.PID file for these checks.
    """
    time.sleep(max(0, settle_seconds))
    deadline = time.monotonic() + max(1, timeout_seconds)
    last = "no response yet"
    with tempfile.TemporaryDirectory() as td:
        readback = Path(td) / "installed.exe"
        while time.monotonic() < deadline:
            try:
                agent = _agent(machine)
                info = agent.sysinfo()
                started = info.get("agent_started")
                if not started or started == old_started:
                    last = "outgoing agent still responding or startup identity unavailable"
                elif info.get("agent_sha256") != expected_sha256:
                    last_update = info.get("update_last", "").split(" ", 2)
                    if (info.get("os_family") == "netware" and info.get("update_state") == "idle"
                            and len(last_update) == 3 and last_update[:2] == ["rolled-back", expected_sha256]):
                        return (f"[update NOT verified] NetWare restored the previous executable on {machine}; "
                                f"recovery records: {last_update[2]}")
                    last = "startup executable hash differs (wrong build or rollback)"
                elif not info.get("agent_exe"):
                    last = "startup executable path unavailable"
                else:
                    _check_update_target(info, remote_target)
                    agent.get(remote_target, readback)
                    if hashlib.sha256(readback.read_bytes()).hexdigest() != expected_sha256:
                        last = "installed executable readback differs"
                    else:
                        confirm = agent.sysinfo()
                        if any(confirm.get(k) != info.get(k) for k in
                               ("agent_started", "agent_sha256", "agent_exe")):
                            last = "agent changed during verification"
                        else:
                            return f"update verified on {machine}: new startup identity and installed executable match SHA-256 {expected_sha256}"
            except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError, ValueError) as e:
                last = str(e)
            time.sleep(max(1, interval_seconds))
    return f"[update NOT verified] timed out on {machine}; last: {last}"


def _check_update_jobs(agent: AgentClient, info: dict[str, str]) -> None:
    if info.get("exec_jobs") == "1" and agent.job_list():
        raise ValueError("retained jobs exist; collect output and release completed jobs before updating")


@srv.tool()
def legacy_self_update(
    machine: str,
    new_agent_local_path: str,
    update_exe_local_path: str,
    remote_dir: str,
    wait_for_agent: bool = True,
    new_agent_name: str = "llm_agent_new.exe",
    update_exe_name: str = "update.exe",
    target_agent_name: str = "llm_agent.exe",
) -> str:
    """Update the agent on the named legacy machine in place: uploads a
    new agent binary and update helper, then launches the helper detached
    to stop the running agent, swap the binary, and restart it.

    On Windows, build both with `make` in agent-win32/. On OS/2, build with
    agent-os2/build.bat (produces llm_agent.exe + update.exe). remote_dir
    is the absolute directory the agent is currently deployed in (e.g.
    C:\\llmagent). Agents with startup identity reject a different target path.

    Optional *_name args set the remote filenames (defaults match the
    Windows layout). For OS/2 8.3 deploys use e.g. new_agent_name=
    'LLMNEW.EXE', update_exe_name='UPDATE.EXE', target_agent_name=
    'LLMAGENT.EXE'.

    The connection carrying this call completes and closes cleanly before
    the old agent process actually stops. If wait_for_agent is True, polls
    after a quiet settling period for a new startup identity, matching startup
    SHA-256, and matching installed executable readback. Older replacement
    agents without startup identity cannot be verified; PING is insufficient."""
    try:
        remote_new_agent, remote_update_exe, remote_target_agent = _update_paths(
            remote_dir, new_agent_name, update_exe_name, target_agent_name)
        agent = _agent(machine)
        before = agent.sysinfo()
        _check_update_jobs(agent, before)
        _check_update_target(before, remote_target_agent)
        # Freeze both inputs before uploading either one. A rebuild during the
        # update must not change which bytes we later call verified.
        with tempfile.TemporaryDirectory() as td:
            binary, helper, scratch = (Path(td) / n for n in ("agent.exe", "helper.exe", "readback"))
            binary.write_bytes(Path(new_agent_local_path).read_bytes())
            helper.write_bytes(Path(update_exe_local_path).read_bytes())
            expected = hashlib.sha256(binary.read_bytes()).hexdigest()
            _stage_verified(agent, binary, remote_new_agent, scratch)
            _stage_verified(agent, helper, remote_update_exe, scratch)
            try:
                result = agent.exec_detach(
                    f'"{remote_update_exe}" "{remote_new_agent}" "{remote_target_agent}"'
                )
                launch = f"update launched on {machine}: {result.reply}"
            except (AgentProtocolError, OSError) as e:
                # The updater can stop the old process before its reply arrives.
                # Do not relaunch it or infer success; verify the postconditions.
                launch = f"update launch outcome unknown on {machine}: {e}"
    except ValueError as e:
        return f"[update preflight error] {e}"
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"

    msg = launch + " (not yet verified)"
    if wait_for_agent:
        msg += "\n" + _wait_for_replaced_agent(
            machine, remote_target_agent, before.get("agent_started"), expected)
    return msg


@srv.tool()
def legacy_win16_self_update(
    machine: str,
    new_agent_local_path: str,
    restart_exe_local_path: str,
    remote_dir: str,
    wait_for_agent: bool = True,
) -> str:
    """Update a Windows for Workgroups 3.11 Win16 agent safely.

    Unlike the Win32/OS2 updater, this stages the new agent as
    LLMNEW.EXE and then sends the agent's UPDATE command. RESTART.EXE
    does the swap only after the old Win16 task exits, preserving the
    previous binary as LLMAGENT.OLD for local recovery.
    Both uploads are read back before UPDATE. Waiting requires a new startup
    identity, matching startup SHA-256 and installed bytes; older replacement
    builds without identity fields remain unverified. Use a short absolute
    directory path without whitespace.
    """
    try:
        remote_new_agent, remote_restart, remote_target = _update_paths(
            remote_dir, "LLMNEW.EXE", "RESTART.EXE", "LLMAGENT.EXE")
        if any(c.isspace() for c in remote_dir) or len(remote_dir) > 126:
            raise ValueError("Win16 update requires a short directory path without whitespace")
        agent = _agent(machine)
        before = agent.sysinfo()
        _check_update_jobs(agent, before)
        _check_update_target(before, remote_target)
        with tempfile.TemporaryDirectory() as td:
            binary, helper, scratch = (Path(td) / n for n in ("agent.exe", "helper.exe", "readback"))
            binary.write_bytes(Path(new_agent_local_path).read_bytes())
            helper.write_bytes(Path(restart_exe_local_path).read_bytes())
            expected = hashlib.sha256(binary.read_bytes()).hexdigest()
            n_agent = _stage_verified(agent, binary, remote_new_agent, scratch)
            n_restart = _stage_verified(agent, helper, remote_restart, scratch)
            try:
                agent.update()
                launch = "UPDATE accepted"
            except (AgentProtocolError, OSError) as e:
                launch = f"UPDATE outcome unknown: {e}"
    except ValueError as e:
        return f"[update preflight error] {e}"
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"

    msg = (
        f"Win16 update staged on {machine}: "
        f"{n_agent} bytes to {remote_new_agent}, "
        f"{n_restart} bytes to {remote_restart}; {launch} (not yet verified)"
    )
    if wait_for_agent:
        msg += "\n" + _wait_for_replaced_agent(
            machine, remote_target, before.get("agent_started"), expected)
    return msg


def _require_profile(agent: AgentClient, profiles: tuple[str, ...]) -> dict[str, str]:
    info = agent.sysinfo()
    profile = profile_for(info)
    if profile not in profiles:
        raise AgentProtocolError(
            f"tool requires {' / '.join(profiles)}; SYSINFO identifies {profile}; command not sent")
    return info


def _platform_command(machine: str, profiles: tuple[str, ...], method: str, *args) -> str:
    """Keep platform guards/error reporting consistent for extension tools."""
    try:
        agent = _agent(machine)
        _require_profile(agent, profiles)
        result = getattr(agent, method)(*args)
        return "OK (request accepted)" if result is None else json.dumps(result, ensure_ascii=True)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol/input error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"


@srv.tool()
def legacy_capabilities(machine: str) -> str:
    """Read SYSINFO and report advisory tool coverage and platform limits.
    Does not probe mutating commands. Profiles describe this source tree;
    older/custom builds may differ. Unknown platforms get no inferred support."""
    try:
        agent = _agent(machine)
        report = describe_capabilities(agent.sysinfo())
        report["text_encodings"] = {
            "text_encoding": agent.text_encoding,
            "exec_command_encoding": agent.exec_command_encoding,
            "file_encoding": agent.file_encoding,
            "exec_encoding": agent.exec_encoding,
        }
        return json.dumps(report, indent=2)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"


@srv.tool()
def legacy_winmsg(machine: str, hwnd: int, msg: int, wparam: int = 0, lparam: int = 0) -> str:
    """Win16: synchronous SendMessage; returns numeric LRESULT.
    Use handles from legacy_winlist. Only scalar parameters are transferred;
    strings/buffers/pointers are not marshalled. Use Win16 message constants;
    control-message numbers can differ from Win32. A modal dialog can block the
    agent; use legacy_postmsg for actions that may open one."""
    return _platform_command(machine, ("win16",), "winmsg", hwnd, msg, wparam, lparam)


@srv.tool()
def legacy_postmsg(machine: str, hwnd: int, msg: int, wparam: int = 0, lparam: int = 0) -> str:
    """Win16: queue PostMessage without waiting for the UI to process it.
    Win16 message constants and scalar parameters only; no pointer/buffer
    marshalling. Suitable for actions that may open modal dialogs. OK means
    queued, not that the UI action finished."""
    return _platform_command(machine, ("win16",), "postmsg", hwnd, msg, wparam, lparam)


@srv.tool()
def legacy_lbgettext(machine: str, hwnd: int, index: int) -> str:
    """Win16: read one zero-based item from a string-backed LISTBOX.
    Use WINLIST children to find the control. Does not select an item.
    Owner-drawn controls without LBS_HASSTRINGS and text over 32767 bytes
    are rejected by the agent."""
    return _platform_command(machine, ("win16",), "lbgettext", hwnd, index)


@srv.tool()
def legacy_winclose(machine: str, title: str) -> str:
    """OS/2 1.3/2.x: request close/cancel for ALL exact title matches,
    ignoring case. Use legacy_winlist first. Apps can prompt or refuse;
    OK acknowledges posting the requests, not completed closure."""
    return _platform_command(machine, ("os2", "os2-13"), "winclose", title)


@srv.tool()
def legacy_screens(machine: str) -> str:
    """NetWare: list CLIB screens as [id, displayed, name] rows.
    This lists screens; it does not switch the active console screen."""
    return _platform_command(machine, ("netware",), "screens")


@srv.tool()
def legacy_autoexec(machine: str) -> str:
    """NetWare: check/add CLIBAUX and LLMAGENT startup loads in AUTOEXEC.NCF.
    May edit SYS:SYSTEM\\AUTOEXEC.NCF, retaining LLMAUTO.BAK. Ambiguous or
    reversed entries fail without editing. Does not interpret other NCF files
    or prove a successful boot. Existing recovery files can block a new edit."""
    return _platform_command(machine, ("netware",), "autoexec")


@srv.tool()
def legacy_debug(machine: str, enabled: bool | None = None) -> str:
    """NetWare: query logging (omit enabled), enable it, or disable it.
    Enabling TRUNCATES SYS:SYSTEM\\LLMAGENT.LOG. Download an existing log
    before enabling if needed; download the new log before disabling."""
    return _platform_command(machine, ("netware",), "debug", enabled)


@srv.tool()
def legacy_double_click(machine: str, x: int, y: int, button: int = 1) -> str:
    """Mac: double-click at global screen coordinates; only button=1 exists.
    Inspect a current screenshot first. Uses the agent's DBLCLICK command."""
    return _platform_command(machine, ("mac68k",), "double_click", x, y, button)


@srv.tool()
def legacy_mouse_position(machine: str) -> str:
    """Mac: read global x/y coordinates and button state (0=released, 1=down)."""
    return _platform_command(machine, ("mac68k",), "mouse_position")


def _mac_resource_sha256(resource: bytes) -> str:
    """Exclude only Resource Manager directory metadata, not application bytes."""
    if len(resource) < 256:
        raise ValueError("resource fork too short")
    data, mapping, data_size, map_size = (int.from_bytes(resource[n:n + 4], "big") for n in (0, 4, 8, 12))
    if (not 256 <= data <= len(resource) or not 256 <= mapping <= len(resource)
            or data_size > len(resource) - data or map_size > len(resource) - mapping):
        raise ValueError("resource fork header points outside its data/map areas")
    return hashlib.sha256(resource[:16] + bytes(112) + resource[128:]).hexdigest()


def _wait_for_mac_replacement(machine: str, before: dict[str, str],
                              data_sha: str, resource_sha: str,
                              timeout_seconds: int = 120, interval_seconds: int = 5,
                              settle_seconds: int = 15) -> str:
    """Compare startup and fresh installed-fork hashes on a stable new instance."""
    time.sleep(max(0, settle_seconds))
    deadline = time.monotonic() + max(1, timeout_seconds)
    last = "no response yet"
    expected = {"agent_data_sha256": data_sha, "agent_resource_sha256": resource_sha,
                "disk_data_sha256": data_sha, "disk_resource_sha256": resource_sha,
                "agent_resource_hash_mode": "sha256-zero-system-16-127-v1"}
    while time.monotonic() < deadline:
        try:
            agent = _agent(machine)
            info = agent.sysinfo()
            if info.get("os_family") != "mac68k":
                last = "responding agent is not a Mac"
            elif not info.get("agent_started") or info["agent_started"] == before.get("agent_started"):
                last = "outgoing agent still responding or startup identity unavailable"
            elif not info.get("agent_location") or (before.get("agent_location") and
                                                     info["agent_location"] != before["agent_location"]):
                last = "application location unavailable or changed"
            elif any(info.get(key) != value for key, value in expected.items()):
                last = "startup or installed application fork differs (wrong build, rollback, or unreadable fork)"
            else:
                confirm = agent.sysinfo()
                if any(confirm.get(key) != info.get(key) for key in
                       (*expected, "agent_started", "agent_location", "os_family")):
                    last = "application identity or installed forks changed during verification"
                else:
                    return (f"update verified on {machine}: new startup identity and installed application "
                            f"forks match SHA-256 data={data_sha} resource={resource_sha} "
                            "(resource system metadata bytes 16..127 normalized)")
        except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError, ValueError) as e:
            last = str(e)
        time.sleep(max(1, interval_seconds))
    return f"[update NOT verified] timed out on {machine}; last: {last}"


@srv.tool()
def legacy_mac_self_update(machine: str, new_agent_local_path: str, wait_for_agent: bool = True) -> str:
    """Mac: send a complete Retro68 MacBinary .bin via UPDATE <size>.
    Requires llm_agent and an already-installed llm_updater in the same folder.
    The companion updater must be upgraded separately. Validates the container
    before transfer. By default, waits for a new startup identity and matching
    startup/fresh installed SHA-256 values for BOTH forks at the same location.
    Resource fingerprints normalize only system-owned metadata bytes 16..127.
    Older replacement builds lacking identity remain unverified. Disabling the
    wait reports acceptance only. No automatic retry of the update handoff."""
    try:
        agent = _agent(machine)
        before = _require_profile(agent, ("mac68k",))
        with tempfile.TemporaryDirectory() as td:
            snapshot = Path(td) / "agent.bin"
            source = Path(new_agent_local_path)
            if not 128 <= source.stat().st_size <= 64 * 1024 * 1024:
                raise ValueError("MacBinary update size outside configured limit")
            payload = source.read_bytes()
            data, resource = AgentClient._macbinary_forks(payload)
            data_sha, resource_sha = hashlib.sha256(data).hexdigest(), _mac_resource_sha256(resource)
            snapshot.write_bytes(payload)
            try:
                size = agent.mac_update(snapshot)
                result = f"Mac update accepted on {machine}: {size} MacBinary bytes"
            except (AgentProtocolError, OSError) as e:
                result = f"Mac update handoff outcome unknown on {machine}: {e}; do not blindly retry"
            if wait_for_agent:
                return result + "; " + _wait_for_mac_replacement(machine, before, data_sha, resource_sha)
            return result + "; replacement NOT verified (waiting disabled)"
    except ValueError as e:
        return f"[update preflight error] {e}"
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol/input error] {e}; replacement NOT verified"
    except OSError as e:
        return f"[connection/file error] {e}; replacement NOT verified; do not blindly retry an uncertain handoff"


@srv.tool()
def legacy_netware_self_update(machine: str, new_agent_local_path: str, update_nlm_local_path: str,
                              wait_for_agent: bool = True) -> str:
    """NetWare: stage LLMAGENT.NEW and UPDATE.NLM in SYS:SYSTEM, read both
    back byte-for-byte, then send UPDATE so the agent self-exits.
    Does not use console UNLOAD (which has abended on NetWare 3.12).
    By default, waits for a new startup identity, matching startup SHA-256,
    and installed executable readback. Older replacement builds lacking
    identity remain unverified. Acceptance alone does not verify helper launch.
    Requires a protocol-2 helper; unresolved recovery blocks staging. Failed
    startup rolls back only after the candidate exits; a loaded/unready NLM
    requires operator recovery. Backups are retained. No handoff is retried."""
    try:
        agent = _agent(machine)
        before = _require_profile(agent, ("netware",))
        if before.get("update_protocol") == "2" and before.get("update_state") != "idle":
            raise ValueError("NetWare updater busy or recovery unresolved; inspect SYS:SYSTEM\\LLMUPD before staging")
        target = r"SYS:SYSTEM\LLMAGENT.NLM"
        _check_update_target(before, target)
        with tempfile.TemporaryDirectory() as td:
            binary, helper, scratch = (Path(td) / n for n in ("agent.nlm", "helper.nlm", "readback"))
            binary.write_bytes(Path(new_agent_local_path).read_bytes())
            helper.write_bytes(Path(update_nlm_local_path).read_bytes())
            if any(not 0 < p.stat().st_size <= 64 * 1024 * 1024 for p in (binary, helper)):
                raise ValueError("NetWare update inputs must be nonempty and at most 64 MiB")
            if b"RETRO_NW_UPDATE_PROTOCOL_2" not in helper.read_bytes():
                raise ValueError("NetWare updates require a protocol-2 UPDATE.NLM helper")
            _stage_verified(agent, binary, r"SYS:SYSTEM\LLMAGENT.NEW", scratch)
            _stage_verified(agent, helper, r"SYS:SYSTEM\UPDATE.NLM", scratch)
            expected = hashlib.sha256(binary.read_bytes()).hexdigest()
            try:
                agent.update()
                result = f"NetWare update accepted on {machine}"
            except (AgentProtocolError, OSError) as e:
                result = f"NetWare update handoff outcome unknown on {machine}: {e}; do not blindly retry"
            if wait_for_agent:
                verification = _wait_for_replaced_agent(machine, target, before.get("agent_started"), expected)
                if "update verified on" in verification:
                    info = {}
                    try:
                        for _ in range(10):
                            info = _agent(machine).sysinfo()
                            if info.get("update_state") != "busy":
                                break
                            time.sleep(1)
                    except (AgentProtocolError, OSError):
                        info = {}
                    if info.get("update_protocol") != "2" or info.get("update_state") != "idle":
                        verification += "; updater recovery NOT cleared; inspect SYS:SYSTEM\\LLMUPD before retrying"
                    else:
                        verification += "; recovery records: " + info.get("update_last", "unavailable")
                return result + "; " + verification
            return result + "; replacement NOT verified (waiting disabled)"
    except ValueError as e:
        return f"[update preflight error] {e}"
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol/input error] {e}; replacement NOT verified"
    except OSError as e:
        return f"[connection/file error] {e}; replacement NOT verified; do not blindly retry an uncertain handoff"


def _job_agent(machine: str) -> AgentClient:
    agent = _agent(machine)
    info = agent.sysinfo()
    if info.get("exec_jobs") != "1":
        raise AgentProtocolError("installed agent does not advertise background jobs; command not sent")
    return agent


@srv.tool()
def legacy_job_start(machine: str, command: str, shell: bool = True) -> str:
    """Start a background job and return a job ID without waiting for completion.
    shell=True uses the platform's shell; False launches a program directly.
    Jobs survive client disconnects, but results are lost on agent restart.
    Cancellation scope is reported by the agent; process means descendants
    are NOT guaranteed to stop. Never automatically repeat an uncertain start.
    Win16/OS2 support shell jobs only and no cancellation. Their output uses
    uncapped disk files (only retrieval is capped). Win16 runs one active
    foreground DOS box and cannot report the inner command's exit status.
    Use legacy_job_status/output and release completed results explicitly."""
    import uuid
    job_id = uuid.uuid4().hex
    attempted = False
    try:
        agent = _job_agent(machine)
        # Validate the entire encoded request before marking launch uncertain.
        if type(shell) is not bool:
            raise ValueError("shell must be a boolean")
        agent._encode_command(f"JOBSTART {job_id} {'S' if shell else 'D'} {command}",
                              encoding=agent.exec_command_encoding if shell else agent.text_encoding)
        attempted = True
        return json.dumps(agent.job_start(job_id, command, shell=shell))
    except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError, ValueError) as e:
        return json.dumps({"job_id": job_id, "error": str(e),
                           "launch_uncertain": attempted,
                           "guidance": "Query this ID/list jobs; do not rerun blindly." if attempted else "Start was not sent."})


@srv.tool()
def legacy_job_status(machine: str, job_id: str | None = None) -> str:
    """Read one job's state or list retained jobs when job_id is omitted.
    A null exit_code means unavailable, not success. Completion/cancellation
    follows the reported scope. No command strings are included in listings."""
    try:
        if job_id is not None:
            AgentClient._job_id(job_id)
        agent = _job_agent(machine)
        return json.dumps(agent.job_status(job_id) if job_id is not None else
                          [agent.job_status(j) for j in agent.job_list()])
    except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError) as e:
        return f"[job error] {e}"


@srv.tool()
def legacy_job_output(machine: str, job_id: str, offset: int = 0,
                      max_bytes: int = 65536, output_encoding: str | None = None) -> str:
    """Read retained job output by byte offset, up to 65536 bytes per call.
    Returns exact base64 bytes plus strictly decoded text where possible.
    Offsets are byte offsets; multibyte characters may span chunks. Empty
    output is not proof of completion: inspect legacy_job_status. This does
    not consume output or rerun the job. Check status for truncation/errors."""
    import base64
    try:
        AgentClient._job_id(job_id)
        if type(offset) is not int or not 0 <= offset <= 4294967295:
            raise ValueError("invalid byte offset")
        if type(max_bytes) is not int or not 1 <= max_bytes <= 65536:
            raise ValueError("max_bytes must be between 1 and 65536")
        codec = normalize_encoding(output_encoding, output=True) if output_encoding is not None else None
        agent = _job_agent(machine)
        codec = codec or agent.exec_encoding
        data = agent.job_read(job_id, offset, max_bytes)
        result = {"job_id": job_id, "offset": offset, "next_offset": offset + len(data),
                  "data_base64": base64.b64encode(data).decode("ascii"), "encoding": codec}
        try:
            result["text"] = decode_text(data, codec, "job output chunk")
        except ValueError as e:
            result["decoding_error"] = f"{e}; raw bytes retained; a multibyte character may cross the chunk boundary"
        return json.dumps(result)
    except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError, ValueError) as e:
        return f"[job error] {e}"


@srv.tool()
def legacy_job_cancel(machine: str, job_id: str) -> str:
    """Request cancellation of a job, then inspect its returned state.
    cancelling means requested, not confirmed stopped. Scope 'process' stops
    only the direct child; a shell's children may survive. Retains output.
    Platforms without safe cancellation reject the request explicitly."""
    try:
        AgentClient._job_id(job_id)
        return json.dumps(_job_agent(machine).job_cancel(job_id))
    except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError) as e:
        return f"[job error] {e}"


@srv.tool()
def legacy_job_release(machine: str, job_id: str) -> str:
    """Discard a completed job and its captured output, freeing a job slot.
    Running jobs cannot be released. Retrieve needed output first."""
    try:
        AgentClient._job_id(job_id)
        _job_agent(machine).job_release(job_id)
        return "Released completed job and retained output."
    except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError) as e:
        return f"[job error] {e}"


if __name__ == "__main__":
    srv.run(transport="stdio")
