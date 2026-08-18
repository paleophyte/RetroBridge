"""MCP bridge exposing shell exec + screenshot + input injection on a legacy
Windows box (95/98/ME/NT4/2000/XP) to an LLM tool-calling harness.

Two channels to the legacy machine, both assumed to be on an isolated
lab/host-only network (see docs/ARCHITECTURE.md for the trust model):

  - exec: retro-agent (agent/agent.c), a tiny token-authed TCP exec service.
  - screen/input: a VNC server you install on the box yourself (TightVNC
    1.3.x or UltraVNC - see docs/ARCHITECTURE.md for why we didn't roll a
    custom protocol for this half). This bridge speaks RFB via vncdotool.

Configured entirely through environment variables so it drops into an MCP
client config (e.g. Claude Code's mcp servers config) without a separate
config file:

  LEGACY_HOST          - hostname/IP of the legacy machine (required)
  LEGACY_EXEC_PORT      - retro-agent port (default 2222)
  LEGACY_EXEC_TOKEN      - retro-agent shared token (required)
  LEGACY_VNC_PORT        - VNC port (default 5900)
  LEGACY_VNC_PASSWORD    - VNC password, if the server has one set (optional)
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from mcp.server.mcpserver import Image, MCPServer

from agent_client import AgentAuthError, AgentClient, AgentProtocolError

HOST = os.environ.get("LEGACY_HOST")
EXEC_PORT = int(os.environ.get("LEGACY_EXEC_PORT", "2222"))
EXEC_TOKEN = os.environ.get("LEGACY_EXEC_TOKEN", "")
VNC_PORT = int(os.environ.get("LEGACY_VNC_PORT", "5900"))
VNC_PASSWORD = os.environ.get("LEGACY_VNC_PASSWORD") or None

srv = MCPServer(
    "retro-ssh-server",
    instructions=(
        "Tools for driving a legacy Windows machine (95/98/ME/NT4/2000/XP) "
        "on an isolated lab network: run shell commands, take screenshots, "
        "and send mouse/keyboard input. Use legacy_screenshot before "
        "legacy_click/legacy_key when you don't already know current "
        "on-screen coordinates."
    ),
)


def _require_host() -> str:
    if not HOST:
        raise RuntimeError("LEGACY_HOST is not set")
    return HOST


def _agent() -> AgentClient:
    if not EXEC_TOKEN:
        raise RuntimeError("LEGACY_EXEC_TOKEN is not set")
    return AgentClient(_require_host(), EXEC_PORT, EXEC_TOKEN)


def _vnc_connect():
    from vncdotool import api

    server = f"{_require_host()}::{VNC_PORT}"
    return api.connect(server, password=VNC_PASSWORD)


@srv.tool()
def legacy_exec(command: str) -> str:
    """Run a command line on the legacy machine via cmd.exe /C and return
    combined stdout+stderr plus the exit code. One-shot per call (no
    persisted shell state / working directory across calls)."""
    try:
        result = _agent().exec(command)
    except AgentAuthError as e:
        return f"[auth error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection error] {e}"
    text = result.output.decode("utf-8", "replace")
    return f"{text}\n[exit code: {result.exit_code}]"


@srv.tool()
def legacy_ping() -> str:
    """Check that the retro-agent exec service on the legacy machine is
    reachable and the configured token is accepted."""
    try:
        ok = _agent().ping()
    except AgentAuthError as e:
        return f"unreachable/auth failed: {e}"
    except OSError as e:
        return f"unreachable: {e}"
    return "ok" if ok else "unexpected response"


@srv.tool()
def legacy_upload(local_path: str, remote_path: str) -> str:
    """Copy a file from this control machine to the legacy machine."""
    try:
        n = _agent().put(local_path, remote_path)
    except AgentAuthError as e:
        return f"[auth error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"
    return f"uploaded {n} bytes: {local_path} -> {remote_path}"


@srv.tool()
def legacy_download(remote_path: str, local_path: str) -> str:
    """Copy a file from the legacy machine to this control machine."""
    try:
        n = _agent().get(remote_path, local_path)
    except AgentAuthError as e:
        return f"[auth error] {e}"
    except AgentProtocolError as e:
        return f"[protocol error] {e}"
    except OSError as e:
        return f"[connection/file error] {e}"
    return f"downloaded {n} bytes: {remote_path} -> {local_path}"


@srv.tool()
def legacy_screenshot() -> Image:
    """Capture the current screen of the legacy machine over VNC."""
    client = _vnc_connect()
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
def legacy_click(x: int, y: int, button: int = 1) -> str:
    """Move the mouse to (x, y) in screen coordinates and click.
    button: 1=left, 2=middle, 3=right (matches VNC button numbering)."""
    client = _vnc_connect()
    try:
        client.mouseMove(x, y)
        client.mousePress(button)
        return f"clicked ({x}, {y}) button {button}"
    finally:
        client.disconnect()


@srv.tool()
def legacy_key(key: str) -> str:
    """Press a single key or key combo on the legacy machine, using
    vncdotool key names, e.g. 'enter', 'esc', 'ctrl-alt-del',
    'alt-tab', 'a', 'shift-a'."""
    client = _vnc_connect()
    try:
        client.keyPress(key)
        return f"pressed {key}"
    finally:
        client.disconnect()


@srv.tool()
def legacy_type(text: str) -> str:
    """Type a string of plain ASCII text on the legacy machine, one
    keystroke per character. For special keys/combos use legacy_key."""
    client = _vnc_connect()
    try:
        for ch in text:
            client.keyPress(ch)
        return f"typed {len(text)} characters"
    finally:
        client.disconnect()


if __name__ == "__main__":
    srv.run(transport="stdio")
