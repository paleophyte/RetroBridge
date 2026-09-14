"""MCP bridge/server exposing shell exec + file transfer + screenshot +
input injection on legacy Windows boxes (WFW 3.11, 95/98/ME/NT4/2000/XP),
FreeDOS, OS/2 (1.3 and 2.11), and NetWare to an LLM tool-calling client.

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

import io
import os
import time
from pathlib import Path

from mcp.server.mcpserver import Image, MCPServer
from PIL import Image as PILImage

from agent_client import AgentAuthError, AgentClient, AgentProtocolError
from machines import MachineConfig, MachineConfigError, load_machines

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
        "and NetWare 3.12+ (agent-netware). "
        "Run shell commands, transfer files, take screenshots, "
        "and send keyboard input. Every tool takes a `machine` argument "
        "naming which configured machine to target - call "
        "legacy_list_machines first if you don't already know the name. "
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
        "legacy_reboot/legacy_shutdown require confirm=True - they take "
        "the target down immediately and interrupt anything in progress "
        "on it, so only pass that once you actually intend it."
    ),
)


def _agent(machine: str) -> AgentClient:
    m = _machine(machine)
    return AgentClient(m.host, m.exec_port, m.exec_token)


@srv.tool()
def legacy_list_machines() -> str:
    """List the legacy machines configured in machines.ini, with their
    host and port (not tokens). Call this first if you don't already know
    which `machine` name to pass to the other tools."""
    try:
        machines = _machines()
    except MachineConfigError as e:
        return f"[config error] {e}"
    if not machines:
        return "no machines configured"
    lines = []
    for m in machines.values():
        suffix = f" vm_name={m.vm_name}" if m.vm_name else ""
        lines.append(f"{m.name}: host={m.host} exec_port={m.exec_port}{suffix}")
    return "\n".join(lines)


@srv.tool()
def legacy_exec(machine: str, command: str) -> str:
    """Run a command line on the named legacy machine via cmd.exe /C and
    return combined stdout+stderr plus the exit code. One-shot per call
    (no persisted shell state / working directory across calls)."""
    try:
        result = _agent(machine).exec(command)
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    text = result.output.decode("utf-8", "replace")
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
    """Type a string of plain text (no newlines - use legacy_key('enter')
    for that) on the named legacy machine, one keystroke per character.
    Characters that don't map to a key on the agent's keyboard layout are
    silently skipped. For special keys/combos use legacy_key."""
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
        except (MachineConfigError, AgentAuthError, OSError) as e:
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
        except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError) as e:
            last = str(e)
        time.sleep(interval)
    return f"timed out waiting for desktop on {machine}; last observation: {last}"


@srv.tool()
def legacy_reboot(machine: str, confirm: bool = False) -> str:
    """Reboot the named legacy machine. Takes it down immediately and
    interrupts anything in progress - pass confirm=True only once you
    actually intend that."""
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
    """Power off the named legacy machine. Takes it down immediately and
    interrupts anything in progress - pass confirm=True only once you
    actually intend that."""
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
def legacy_winlist(machine: str) -> str:
    """List visible top-level windows (handle, position/size, class,
    title) on the named legacy machine. Use this to find dialogs/buttons
    by title instead of screenshotting and guessing pixel coordinates."""
    try:
        windows = _agent(machine).winlist()
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


def _read_agent_pid(agent: AgentClient, remote_dir: str) -> str | None:
    """The PID the agent wrote to AGENT.PID when it started listening.

    A bare ping is not a restart signal: the agent being replaced answers
    it perfectly well right up until it exits, so polling for
    reachability straight after launching the updater reports success
    against the *outgoing* process. Confirmed live - two self-updates back
    to back had the second one connect mid-swap and fail on a reset
    connection. A changed PID is proof a new process is answering."""
    import tempfile

    try:
        with tempfile.TemporaryDirectory() as td:
            local = Path(td) / "AGENT.PID"
            agent.get(f"{remote_dir}\\AGENT.PID", local)
            return local.read_text().strip() or None
    except (AgentAuthError, AgentProtocolError, OSError):
        return None


def _wait_for_replaced_agent(
    machine: str,
    remote_dir: str,
    old_pid: str | None,
    timeout_seconds: int = 120,
    interval_seconds: int = 5,
    settle_seconds: int = 10,
) -> str:
    """Wait until a *different* agent process is answering on `machine`.

    Sits out the first `settle_seconds` deliberately. The legacy agents
    are single-threaded - one connection at a time - and during the swap
    the one being replaced is busy exiting while the updater talks to it
    over that same socket. Polling into that window is not free: an update
    run that was polled every 3s from the moment it launched left the
    updater dead just after SELFEXIT with no agent running at all, where
    the same update with nothing connecting to it succeeded repeatedly.
    Ten seconds is comfortably longer than a whole successful swap (~10s
    end to end, of which the file work is a few hundred ms).

    Falls back to plain reachability if the PID couldn't be read before
    the swap, or if the agent doesn't publish one - better a weaker check
    than a spurious failure."""
    time.sleep(max(0, settle_seconds))
    if old_pid is None:
        return legacy_wait_for_agent(
            machine, timeout_seconds=timeout_seconds, interval_seconds=interval_seconds
        )

    deadline = time.time() + max(1, timeout_seconds)
    interval = max(1, interval_seconds)
    last = "no response yet"
    attempts = 0
    while time.time() < deadline:
        attempts += 1
        try:
            new_pid = _read_agent_pid(_agent(machine), remote_dir)
            if new_pid and new_pid != old_pid:
                return (
                    f"agent restarted on {machine} after {attempts} attempt(s): "
                    f"pid {old_pid} -> {new_pid}"
                )
            last = f"still pid {new_pid}" if new_pid else "agent not answering"
        except (MachineConfigError, AgentAuthError, AgentProtocolError, OSError) as e:
            last = str(e)
        time.sleep(interval)
    return (
        f"timed out waiting for a restarted agent on {machine} "
        f"(was pid {old_pid}); last: {last}"
    )


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
    C:\\llmagent) - there's no remote way to ask the agent where it's
    installed, so this has to be supplied.

    Optional *_name args set the remote filenames (defaults match the
    Windows layout). For OS/2 8.3 deploys use e.g. new_agent_name=
    'LLMNEW.EXE', update_exe_name='UPDATE.EXE', target_agent_name=
    'LLMAGENT.EXE'.

    The connection carrying this call completes and closes cleanly before
    the old agent process actually stops. If wait_for_agent is True, polls
    afterward until the new agent responds."""
    remote_dir = remote_dir.rstrip("\\")
    remote_new_agent = f"{remote_dir}\\{new_agent_name}"
    remote_update_exe = f"{remote_dir}\\{update_exe_name}"
    remote_target_agent = f"{remote_dir}\\{target_agent_name}"

    try:
        agent = _agent(machine)
        agent.put(new_agent_local_path, remote_new_agent)
        agent.put(update_exe_local_path, remote_update_exe)
        old_pid = _read_agent_pid(agent, remote_dir)
        result = agent.exec_detach(
            f'"{remote_update_exe}" "{remote_new_agent}" "{remote_target_agent}"'
        )
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"

    msg = f"update launched on {machine}: {result.reply}"
    if wait_for_agent:
        msg += "\n" + _wait_for_replaced_agent(machine, remote_dir, old_pid)
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
    """
    remote_dir = remote_dir.rstrip("\\")
    remote_new_agent = f"{remote_dir}\\LLMNEW.EXE"
    remote_restart = f"{remote_dir}\\RESTART.EXE"

    try:
        agent = _agent(machine)
        n_agent = agent.put(new_agent_local_path, remote_new_agent)
        n_restart = agent.put(restart_exe_local_path, remote_restart)
        agent.update()
    except (MachineConfigError, AgentAuthError) as e:
        return f"[auth/config error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"

    msg = (
        f"Win16 update staged on {machine}: "
        f"{n_agent} bytes to {remote_new_agent}, "
        f"{n_restart} bytes to {remote_restart}; UPDATE accepted"
    )
    if wait_for_agent:
        msg += "\n" + legacy_wait_for_agent(machine, timeout_seconds=120, interval_seconds=5)
    return msg


if __name__ == "__main__":
    srv.run(transport="stdio")
