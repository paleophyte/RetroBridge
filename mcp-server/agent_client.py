"""TCP client for the llm_agent protocol (see agent-win32/llm_agent.c for the
authoritative wire protocol docs - this is a summary, not the source of
truth for edge cases).

Wire protocol, one connection = one session:
    client -> server: token line
    server -> client: "OK\\n" | "FAIL\\n"
    client -> server: "EXEC <cmdline>\\n" | "EXECDETACH <cmdline>\\n"
                       | "PUT <path> <size>\\n" | "GET <path>\\n"
                       | "SCREENSHOT\\n" | "CLICK <x> <y> <button>\\n"
                       | "KEY <keyspec>\\n" | "TYPE <text>\\n" | "PSLIST\\n"
                       | "PSKILL <pid>\\n" | "SYSINFO\\n" | "REBOOT\\n"
                       | "SHUTDOWN\\n" | "WINLIST\\n" | "CLIPSET <text>\\n"
                       | "REGGET\\t<root>\\t<subkey>\\t<valuename>\\n"
                       | "REGSET\\t<root>\\t<subkey>\\t<valuename>\\t<type>\\t<data>\\n"
                       | "PING\\n" | "QUIT\\n" | "UPDATE\\n" | "AUTOEXEC\\n" | "DEBUG [0|1]\\n" | "SCREENS\\n"
    server -> client (EXEC): repeated "LEN:<n>\\n" + <n> raw bytes, then "EXIT:<code>\\n"
                             (LEN:0 with no bytes may appear as a heartbeat)
                             Win32 pipe/shell launch failure: diagnostic LEN
                             payload followed by EXIT:-1; no child started.
    server -> client (EXECDETACH): "OK pid=<pid>\\n" | "ERR:<msg>\\n"
    server -> client (PUT):  "OK\\n" | "ERR:<msg>\\n"
    server -> client (GET):  "SIZE:<n>\\n" + <n> raw bytes, or "ERR:<msg>\\n"
    server -> client (SCREENSHOT): "SIZE:<n>\\n" + <n> raw BMP bytes, or "ERR:<msg>\\n"
    server -> client (CLICK/KEY/TYPE): "OK\\n" | "ERR:<msg>\\n"
    server -> client (PSLIST): "SIZE:<n>\\n" + <n> raw bytes of "<pid>\\t<name>\\r\\n" lines
    server -> client (PSKILL): "OK\\n" | "ERR:<msg>\\n"
    server -> client (SYSINFO): "SIZE:<n>\\n" + <n> raw bytes of "key=value\\r\\n" lines
    server -> client (REBOOT/SHUTDOWN): "OK\\n" | "ERR:<msg>\\n"
        On Mac, OK means delivered to Finder; save dialogs or cancellation
        may prevent completion. Verify machine state separately.
    server -> client (WINLIST): "SIZE:<n>\\n" + <n> raw bytes of
                                "<hwnd>\\t<x>\\t<y>\\t<w>\\t<h>\\t<class>\\t<title>\\r\\n" lines
    server -> client (CLIPSET): "OK\\n" | "ERR:<msg>\\n"
    server -> client (REGGET): "DWORD:<value>\\n" | "SIZE:<n>\\n" + <n> raw bytes | "ERR:<msg>\\n"
    server -> client (REGSET): "OK\\n" | "ERR:<msg>\\n"
    server -> client (PING): "PONG\\n"
    server -> client (UPDATE): "OK\\n" | "ERR:<msg>\\n"
    server -> client (DEBUG): "OK debug=<0|1>\\n" | "ERR:<msg>\\n"
    server -> client (SCREENS): "SIZE:<n>\\n" + "<id>\\t<displayed>\\t<name>\\r\\n" lines
"""

from __future__ import annotations

import socket
import re
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path


@dataclass
class WindowInfo:
    hwnd: int
    x: int
    y: int
    width: int
    height: int
    class_name: str
    title: str


class AgentAuthError(RuntimeError):
    pass


class AgentProtocolError(RuntimeError):
    pass


class AgentInputError(AgentProtocolError, ValueError):
    """Invalid caller input; no command has been sent."""


DEFAULT_MAX_COMMAND_BYTES = 510
DEFAULT_MAX_RESPONSE_BYTES = 64 * 1024 * 1024
MAX_RESPONSE_LINE_BYTES = 65536


@dataclass
class ExecResult:
    output: bytes
    exit_code: int


@dataclass
class ExecDetachResult:
    pid: int | None
    reply: str


class AgentClient:
    """One command per connection, with validated line and payload framing.

    max_command_bytes excludes LF; 510 is portable, 4094 is for Win32 only.
    max_response_bytes bounds each SIZE payload and cumulative EXEC output.
    These limits do not add a total command deadline or change target encodings.
    """
    def __init__(self, host: str, port: int, token: str, timeout: float = 15.0,
                 *, max_command_bytes: int = DEFAULT_MAX_COMMAND_BYTES,
                 max_response_bytes: int = DEFAULT_MAX_RESPONSE_BYTES):
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout
        if type(max_command_bytes) is not int or not 1 <= max_command_bytes <= 4094:
            raise AgentInputError("max_command_bytes must be an integer from 1 to 4094")
        if type(max_response_bytes) is not int or not 1 <= max_response_bytes <= 2147483647:
            raise AgentInputError("max_response_bytes must be an integer from 1 to 2147483647")
        self.max_command_bytes = max_command_bytes
        self.max_response_bytes = max_response_bytes

    @staticmethod
    def _field(value: str, name: str, *, registry: bool = False) -> str:
        if not isinstance(value, str):
            raise AgentInputError(f"{name} must be text")
        if any(c in value for c in ("\r", "\n", "\0")):
            raise AgentInputError(f"{name} must not contain CR, LF, or NUL")
        if registry and "\t" in value:
            raise AgentInputError(f"{name} must not contain tabs")
        return value

    @staticmethod
    def _numbers(**values: int) -> None:
        for name, value in values.items():
            if type(value) is not int or not -2147483648 <= value <= 4294967295:
                raise AgentInputError(f"{name} must be a 32-bit integer")

    def _encode_command(self, line: str) -> bytes:
        self._field(line, "command")
        try:
            data = line.encode("utf-8")
        except UnicodeEncodeError:
            raise AgentInputError("command contains invalid Unicode") from None
        if len(data) > self.max_command_bytes:
            raise AgentInputError(f"command exceeds {self.max_command_bytes} encoded bytes (excluding LF)")
        return data + b"\n"

    @contextmanager
    def _command_session(self, line: str):
        command = self._encode_command(line)  # Validate before auth or connecting.
        sock = self._connect()
        try:
            sock.sendall(command)
            yield sock
        except BaseException:
            # QUIT could be consumed as upload data or prolong a bad frame.
            sock.close()
            raise
        else:
            self._quit(sock)

    def _connect(self) -> socket.socket:
        self._field(self.token, "token")
        try:
            token = self.token.encode("ascii")
        except UnicodeEncodeError:
            raise AgentInputError("token must contain only ASCII characters") from None
        if not 1 <= len(token) <= 127:
            raise AgentInputError("token must contain 1-127 ASCII bytes")
        sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        try:
            sock.sendall(token + b"\n")
            reply = self._recv_line(sock)
            if reply != "OK":
                raise AgentAuthError("agent authentication failed")
        except BaseException:
            sock.close()
            raise
        return sock

    @staticmethod
    def _recv_line(sock: socket.socket) -> str:
        chunks = bytearray()
        while True:
            b = sock.recv(1)
            if not b:
                raise AgentProtocolError("connection closed before response line ended")
            if b == b"\n":
                break
            chunks += b
            if len(chunks) > MAX_RESPONSE_LINE_BYTES:
                raise AgentProtocolError("response line exceeds byte limit")
        if chunks.endswith(b"\r"):
            chunks.pop()
        if b"\0" in chunks or b"\r" in chunks:
            raise AgentProtocolError("invalid control byte in response line")
        return chunks.decode("ascii", "replace")

    @staticmethod
    def _recv_exact(sock: socket.socket, n: int,
                    limit: int = DEFAULT_MAX_RESPONSE_BYTES) -> bytes:
        if type(n) is not int or not 0 <= n <= limit:
            raise AgentProtocolError("payload size outside configured limit")
        buf = bytearray()
        while len(buf) < n:
            chunk = sock.recv(min(n - len(buf), 65536))
            if not chunk:
                raise AgentProtocolError("connection closed mid-response")
            buf += chunk
        return bytes(buf)

    @staticmethod
    def _integer(text: str, *, signed: bool = False, maximum: int = 4294967295) -> int:
        if len(text) > 11 or not re.fullmatch(r"-?[0-9]+" if signed else r"[0-9]+", text):
            raise AgentProtocolError("invalid numeric response")
        value = int(text)
        if value < (-2147483648 if signed else 0) or value > maximum:
            raise AgentProtocolError("numeric response outside allowed range")
        return value

    def _size(self, text: str) -> int:
        return self._integer(text, maximum=self.max_response_bytes)

    def exec(self, cmdline: str) -> ExecResult:
        with self._command_session(f"EXEC {cmdline}") as sock:
            output = bytearray()
            exit_code = -1
            while True:
                line = self._recv_line(sock)
                if line.startswith("LEN:"):
                    n = self._size(line[4:])
                    if len(output) + n > self.max_response_bytes:
                        raise AgentProtocolError("EXEC output exceeds configured byte limit")
                    output += self._recv_exact(sock, n, self.max_response_bytes)
                elif line.startswith("EXIT:"):
                    exit_code = self._integer(line[5:], signed=True)
                    break
                elif line == "":
                    raise AgentProtocolError("connection closed before EXIT")
                else:
                    raise AgentProtocolError(f"unexpected line: {line!r}")
            return ExecResult(output=bytes(output), exit_code=exit_code)

    def exec_detach(self, cmdline: str) -> ExecDetachResult:
        """Launch a program directly (no shell - no &&, %VAR% expansion,
        redirection, or built-ins like `dir`/`start`) and return
        immediately. The returned pid is the actual launched program, safe
        to pass to pskill()/look for in pslist(). Use for GUI apps,
        browser launches, or standalone background helpers."""
        with self._command_session(f"EXECDETACH {cmdline}") as sock:
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            pid = None
            if reply.startswith("OK pid="):
                pid = self._integer(reply[7:])
            elif reply != "OK":
                raise AgentProtocolError("invalid EXECDETACH response")
            return ExecDetachResult(pid=pid, reply=reply)

    def put(self, local_path: str | Path, remote_path: str) -> int:
        """Upload local_path (on this machine) to remote_path on the agent.
        Returns the number of bytes sent."""
        self._field(remote_path, "remote_path")
        data = Path(local_path).read_bytes()
        with self._command_session(f"PUT {remote_path} {len(data)}") as sock:
            sock.sendall(data)
            reply = self._recv_line(sock)
            if reply != "OK":
                raise AgentProtocolError(f"PUT failed: {reply}")
            return len(data)

    def get(self, remote_path: str, local_path: str | Path) -> int:
        """Download remote_path from the agent to local_path (on this
        machine). Returns the number of bytes received."""
        with self._command_session(f"GET {remote_path}") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected GET response: {header!r}")
            size = self._size(header[5:])
            data = self._recv_exact(sock, size, self.max_response_bytes)
            Path(local_path).write_bytes(data)
            return size

    def screenshot(self) -> bytes:
        """Capture the agent machine's screen. Returns raw BMP bytes."""
        with self._command_session("SCREENSHOT") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected SCREENSHOT response: {header!r}")
            size = self._size(header[5:])
            return self._recv_exact(sock, size, self.max_response_bytes)

    def _simple_command(self, line: str) -> None:
        """Send a one-line command that replies with just OK/ERR."""
        with self._command_session(line) as sock:
            reply = self._recv_line(sock)
            if reply != "OK":
                raise AgentProtocolError(reply or "connection closed before OK")

    def click(self, x: int, y: int, button: int = 1) -> None:
        self._numbers(x=x, y=y, button=button)
        self._simple_command(f"CLICK {x} {y} {button}")

    def key(self, *keyspecs: str) -> None:
        """Send one or more keys. NetWare batches them in a single StuffKey run."""
        parts: list[str] = []
        for k in keyspecs:
            self._field(k, "keyspec")
            parts.extend(k.replace(",", " ").split())
        if not parts:
            raise AgentInputError("key() requires at least one keyspec")
        self._simple_command("KEY " + " ".join(parts))

    def type_text(self, text: str) -> None:
        if "\n" in text or "\r" in text:
            raise AgentInputError("type_text() text must not contain newlines - use key('enter') instead")
        self._simple_command(f"TYPE {text}")

    def screens(self) -> list[tuple[int, bool, str]]:
        """NetWare: list CLIB screens as (id, displayed, name)."""
        with self._command_session("SCREENS") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected SCREENS response: {header!r}")
            size = self._size(header[5:])
            text = self._recv_exact(sock, size, self.max_response_bytes).decode("utf-8", "replace")
            out: list[tuple[int, bool, str]] = []
            for line in text.splitlines():
                parts = line.split("\t", 2)
                if len(parts) != 3:
                    continue
                try:
                    out.append((int(parts[0]), parts[1] != "0", parts[2]))
                except ValueError:
                    continue
            return out

    def pslist(self) -> list[tuple[int, str]]:
        """List running processes. Returns (pid, image_name) pairs."""
        with self._command_session("PSLIST") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected PSLIST response: {header!r}")
            size = self._size(header[5:])
            text = self._recv_exact(sock, size, self.max_response_bytes).decode("utf-8", "replace")
            procs = []
            for line in text.splitlines():
                pid_str, _, name = line.partition("\t")
                if not pid_str.strip():
                    continue
                try:
                    procs.append((int(pid_str), name))
                except ValueError:
                    continue
            return procs

    def pskill(self, pid: int) -> None:
        self._numbers(pid=pid)
        self._simple_command(f"PSKILL {pid}")

    def sysinfo(self) -> dict[str, str]:
        """OS family/version, computer name, memory, and C: disk space."""
        with self._command_session("SYSINFO") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected SYSINFO response: {header!r}")
            size = self._size(header[5:])
            text = self._recv_exact(sock, size, self.max_response_bytes).decode("utf-8", "replace")
            info = {}
            for line in text.splitlines():
                key, sep, value = line.partition("=")
                if sep:
                    info[key] = value
            return info

    def reboot(self) -> None:
        self._simple_command("REBOOT")

    def shutdown(self) -> None:
        self._simple_command("SHUTDOWN")

    def update(self) -> None:
        """Ask the agent to self-update (platform-specific helper)."""
        self._simple_command("UPDATE")

    def autoexec(self) -> str:
        """Ensure NetWare AUTOEXEC.NCF loads the agent (and CLIBAUX).

        Returns the OK payload (e.g. 'autoexec=added' or 'autoexec=present').
        Other platforms may return ERR.
        """
        with self._command_session("AUTOEXEC") as sock:
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK "):
                raise AgentProtocolError(f"unexpected AUTOEXEC response: {reply!r}")
            return reply[3:]

    def debug(self, enabled: bool | None = None) -> int:
        """Query or set agent verbose logging. Returns 0 or 1.

        On NetWare, DEBUG 1 truncates SYS:SYSTEM\\LLMAGENT.LOG; pull it with
        get() then DEBUG 0 when done. Other agents may return ERR.
        """
        if enabled is not None and type(enabled) is not bool:
            raise AgentInputError("enabled must be bool or None")
        with self._command_session("DEBUG" if enabled is None else f"DEBUG {1 if enabled else 0}") as sock:
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK debug="):
                raise AgentProtocolError(f"unexpected DEBUG response: {reply!r}")
            return self._integer(reply[9:], maximum=1)

    def winlist(self, parent_hwnd: int | None = None) -> list[WindowInfo]:
        """List visible top-level windows with a non-empty title.

        Pass parent_hwnd (from a prior winlist() result) to instead list
        that window's immediate children -- supported by the Win16 agent
        for inspecting a dialog's controls; title comes back as
        "<control id>:<text>" in that case.
        """
        if parent_hwnd is not None:
            self._numbers(parent_hwnd=parent_hwnd)
        with self._command_session(f"WINLIST {parent_hwnd}" if parent_hwnd is not None else "WINLIST") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected WINLIST response: {header!r}")
            size = self._size(header[5:])
            text = self._recv_exact(sock, size, self.max_response_bytes).decode("utf-8", "replace")
            windows = []
            for line in text.splitlines():
                parts = line.split("\t", 6)
                if len(parts) != 7:
                    continue
                hwnd, x, y, w, h, cls, title = parts
                try:
                    windows.append(WindowInfo(int(hwnd), int(x), int(y), int(w), int(h), cls, title))
                except ValueError:
                    continue
            return windows

    def winmsg(self, hwnd: int, msg: int, wparam: int, lparam: int) -> int:
        """Win16 agent only: raw SendMessage(hwnd, msg, wparam, lparam)
        passthrough. Returns the LRESULT. A thin, general primitive --
        the caller supplies whichever message/params accomplish a given
        UI action (e.g. BM_SETCHECK to toggle a checkbox, or a
        WM_LBUTTONDOWN/WM_LBUTTONUP pair sent to a button's own hwnd to
        make it click itself and notify its parent)."""
        self._numbers(hwnd=hwnd, msg=msg, wparam=wparam, lparam=lparam)
        with self._command_session(f"WINMSG {hwnd} {msg} {wparam} {lparam}") as sock:
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK:"):
                raise AgentProtocolError(f"unexpected WINMSG response: {reply!r}")
            return self._integer(reply[3:], signed=True)

    def postmsg(self, hwnd: int, msg: int, wparam: int, lparam: int) -> None:
        """Win16 agent only: raw PostMessage(hwnd, msg, wparam, lparam)
        passthrough -- returns the instant the message is queued,
        without waiting for it to be processed. Use this instead of
        winmsg() (SendMessage) for anything that might open a modal
        dialog: SendMessage blocks the caller until the ENTIRE receiving
        chain finishes processing, nested DialogBox() calls included, so
        e.g. double-clicking a Control Panel applet via winmsg() wedges
        this agent for as long as the resulting dialog stays open. Even
        a button's own internal click handling notifies its parent via
        SendMessage synchronously, so postmsg() has to be the entry
        point, not just the last step: post WM_LBUTTONDOWN then
        WM_LBUTTONUP to press a button, or post WM_COMMAND with an
        LBN_DBLCLK notification to activate a listbox item, and whatever
        dialog that opens runs on the normal message pump's own time,
        decoupled from this call entirely."""
        self._numbers(hwnd=hwnd, msg=msg, wparam=wparam, lparam=lparam)
        with self._command_session(f"POSTMSG {hwnd} {msg} {wparam} {lparam}") as sock:
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if reply != "OK":
                raise AgentProtocolError(f"unexpected POSTMSG response: {reply!r}")

    def lbgettext(self, hwnd: int, index: int) -> str:
        """Win16 only: read full text from a standard string-backed LISTBOX.
        Owner-drawn controls need LBS_HASSTRINGS; opaque item data is rejected.
        Invalid controls/indices, text over 32767 bytes, allocation failures,
        or text containing line terminators raise AgentProtocolError.
        The agent sizes its buffer before LB_GETTEXT. This command does not
        select an item; use WINMSG with LB_SETCURSEL for selection."""
        self._numbers(hwnd=hwnd, index=index)
        with self._command_session(f"LBGETTEXT {hwnd} {index}") as sock:
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK:"):
                raise AgentProtocolError(f"unexpected LBGETTEXT response: {reply!r}")
            return reply[3:]

    def clipboard_set(self, text: str) -> None:
        """Set the clipboard to plain text. Pair with key('ctrl-v') to
        paste it - more reliable than type_text() for exact strings."""
        if "\n" in text or "\r" in text:
            raise AgentInputError("clipboard_set() text must not contain newlines")
        self._simple_command(f"CLIPSET {text}")

    def reg_get(self, root: str, subkey: str, value_name: str) -> str | int:
        """Read a registry value. Returns int for REG_DWORD, str for
        REG_SZ/REG_EXPAND_SZ. Raises AgentProtocolError for anything else
        (missing key/value, or an unsupported value type)."""
        self._field(root, "root", registry=True)
        self._field(subkey, "subkey", registry=True)
        self._field(value_name, "value_name", registry=True)
        with self._command_session(f"REGGET\t{root}\t{subkey}\t{value_name}") as sock:
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if header.startswith("DWORD:"):
                return self._integer(header[6:])
            if header.startswith("SIZE:"):
                size = self._size(header[5:])
                return self._recv_exact(sock, size, self.max_response_bytes).decode("utf-8", "replace")
            raise AgentProtocolError(f"unexpected REGGET response: {header!r}")

    def reg_set(self, root: str, subkey: str, value_name: str, value_type: str, data: str) -> None:
        """Write a registry value. value_type is "SZ" or "DWORD" (the
        only two the agent supports). Creates the key if it doesn't exist."""
        self._field(root, "root", registry=True)
        self._field(subkey, "subkey", registry=True)
        self._field(value_name, "value_name", registry=True)
        self._field(value_type, "value_type", registry=True)
        self._field(data, "data", registry=True)
        if value_type.upper() not in ("SZ", "DWORD"):
            raise AgentInputError('value_type must be "SZ" or "DWORD"')
        self._simple_command(f"REGSET\t{root}\t{subkey}\t{value_name}\t{value_type}\t{data}")

    @staticmethod
    def _quit(sock: socket.socket) -> None:
        try:
            sock.sendall(b"QUIT\n")
        except OSError:
            pass
        sock.close()

    def ping(self) -> bool:
        with self._command_session("PING") as sock:
            return self._recv_line(sock) == "PONG"
