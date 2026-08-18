"""MCP bridge exposing shell exec + file transfer + screenshot + input
injection on legacy Windows boxes (95/98/ME/NT4/2000/XP) to an LLM
tool-calling harness.

Supports multiple legacy machines from one bridge process: each is a
section in machines.ini (see machines.ini.example), and every tool takes
a `machine` argument naming which one to target. Call legacy_list_machines
to discover what's configured.

Two channels per machine, both assumed to be on an isolated lab/host-only
network (see docs/ARCHITECTURE.md for the trust model):

  - exec/file-transfer: llm_agent (agent/llm_agent.c), a tiny token-authed
    TCP service.
  - screen/input: a VNC server you install on the box yourself (TightVNC
    1.3.x or UltraVNC - see docs/ARCHITECTURE.md for why we didn't roll a
    custom protocol for this half). This bridge speaks RFB via vncdotool.

LEGACY_MACHINES_FILE points at the ini file; defaults to machines.ini next
to this script.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from mcp.server.mcpserver import Image, MCPServer

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
        "you don't already know current on-screen coordinates."
    ),
)


def _agent(machine: str) -> AgentClient:
    m = _machine(machine)
    return AgentClient(m.host, m.exec_port, m.exec_token)


def _vnc_connect(machine: str):
    from vncdotool import api

    m = _machine(machine)
    server = f"{m.host}::{m.vnc_port}"
    return api.connect(server, password=m.vnc_password)


@srv.tool()
def legacy_list_machines() -> str:
    """List the legacy machines configured in machines.ini, with their
    host and ports (not tokens/passwords). Call this first if you don't
    already know which `machine` name to pass to the other tools."""
    try:
        machines = _machines()
    except MachineConfigError as e:
        return f"[config error] {e}"
    if not machines:
        return "no machines configured"
    return "\n".join(
        f"{m.name}: host={m.host} exec_port={m.exec_port} vnc_port={m.vnc_port}"
        for m in machines.values()
    )


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
    """Capture the current screen of the named legacy machine over VNC."""
    client = _vnc_connect(machine)
    try:
        fd, path = tempfile.mkstemp(suffix=".png")
        os.close(fd)
        client.captureScreen(path)
        data = Path(path).read_bytes()
        os.unlink(path)
        return Image(data=data, format="png")
    finally:
        client.disconnect()


@srv.tool()
def legacy_click(machine: str, x: int, y: int, button: int = 1) -> str:
    """Move the mouse to (x, y) in screen coordinates on the named legacy
    machine and click. button: 1=left, 2=middle, 3=right (VNC numbering)."""
    client = _vnc_connect(machine)
    try:
        client.mouseMove(x, y)
        client.mousePress(button)
        return f"clicked ({x}, {y}) button {button} on {machine}"
    finally:
        client.disconnect()


@srv.tool()
def legacy_key(machine: str, key: str) -> str:
    """Press a single key or key combo on the named legacy machine, using
    vncdotool key names, e.g. 'enter', 'esc', 'ctrl-alt-del',
    'alt-tab', 'a', 'shift-a'."""
    client = _vnc_connect(machine)
    try:
        client.keyPress(key)
        return f"pressed {key} on {machine}"
    finally:
        client.disconnect()


@srv.tool()
def legacy_type(machine: str, text: str) -> str:
    """Type a string of plain ASCII text on the named legacy machine, one
    keystroke per character. For special keys/combos use legacy_key."""
    client = _vnc_connect(machine)
    try:
        for ch in text:
            client.keyPress(ch)
        return f"typed {len(text)} characters on {machine}"
    finally:
        client.disconnect()


if __name__ == "__main__":
    srv.run(transport="stdio")
