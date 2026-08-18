"""MCP bridge exposing shell exec + file transfer + screenshot + input
injection on legacy Windows boxes (95/98/ME/NT4/2000/XP) to an LLM
tool-calling harness.

Supports multiple legacy machines from one bridge process: each is a
section in machines.ini (see machines.ini.example), and every tool takes
a `machine` argument naming which one to target. Call legacy_list_machines
to discover what's configured.

Everything goes through one channel per machine: llm_agent
(agent/llm_agent.c), a tiny token-authed TCP service, assumed to be on an
isolated lab/host-only network (see docs/ARCHITECTURE.md for the trust
model). Screenshot/click/key/type are built into the agent itself (GDI
capture + mouse_event/keybd_event injection) rather than requiring a
separately-installed VNC server - see docs/ARCHITECTURE.md for why that
changed from the original design.

LEGACY_MACHINES_FILE points at the ini file; defaults to machines.ini next
to this script.
"""

from __future__ import annotations

import io
import os
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
        "Tools for driving legacy Windows machines (95/98/ME/NT4/2000/XP) "
        "on an isolated lab network: run shell commands, transfer files, "
        "take screenshots, and send mouse/keyboard input. Every tool takes "
        "a `machine` argument naming which configured machine to target - "
        "call legacy_list_machines first if you don't already know the "
        "name. Use legacy_screenshot before legacy_click/legacy_key when "
        "you don't already know current on-screen coordinates. Note: a "
        "synthetic ctrl-alt-del will not unlock a locked/secure-desktop "
        "screen - that's an OS security measure, not a bug."
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
    return "\n".join(f"{m.name}: host={m.host} exec_port={m.exec_port}" for m in machines.values())


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


if __name__ == "__main__":
    srv.run(transport="stdio")
