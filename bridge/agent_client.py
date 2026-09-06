"""TCP client for the llm_agent protocol (see agent/llm_agent.c for the
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
    server -> client (EXECDETACH): "OK pid=<pid>\\n" | "ERR:<msg>\\n"
    server -> client (PUT):  "OK\\n" | "ERR:<msg>\\n"
    server -> client (GET):  "SIZE:<n>\\n" + <n> raw bytes, or "ERR:<msg>\\n"
    server -> client (SCREENSHOT): "SIZE:<n>\\n" + <n> raw BMP bytes, or "ERR:<msg>\\n"
    server -> client (CLICK/KEY/TYPE): "OK\\n" | "ERR:<msg>\\n"
    server -> client (PSLIST): "SIZE:<n>\\n" + <n> raw bytes of "<pid>\\t<name>\\r\\n" lines
    server -> client (PSKILL): "OK\\n" | "ERR:<msg>\\n"
    server -> client (SYSINFO): "SIZE:<n>\\n" + <n> raw bytes of "key=value\\r\\n" lines
    server -> client (REBOOT/SHUTDOWN): "OK\\n" | "ERR:<msg>\\n"
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


@dataclass
class ExecResult:
    output: bytes
    exit_code: int


@dataclass
class ExecDetachResult:
    pid: int | None
    reply: str


class AgentClient:
    def __init__(self, host: str, port: int, token: str, timeout: float = 15.0):
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout

    def _connect(self) -> socket.socket:
        sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        sock.sendall((self.token + "\n").encode("ascii", "ignore"))
        reply = self._recv_line(sock)
        if reply != "OK":
            sock.close()
            raise AgentAuthError(f"agent auth failed: {reply!r}")
        return sock

    @staticmethod
    def _recv_line(sock: socket.socket) -> str:
        chunks = bytearray()
        while True:
            b = sock.recv(1)
            if not b:
                break
            if b == b"\n":
                break
            if b != b"\r":
                chunks += b
        return chunks.decode("ascii", "replace")

    @staticmethod
    def _recv_exact(sock: socket.socket, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise AgentProtocolError("connection closed mid-response")
            buf += chunk
        return bytes(buf)

    def exec(self, cmdline: str) -> ExecResult:
        sock = self._connect()
        try:
            sock.sendall(f"EXEC {cmdline}\n".encode("utf-8", "replace"))
            output = bytearray()
            exit_code = -1
            while True:
                line = self._recv_line(sock)
                if line.startswith("LEN:"):
                    n = int(line[4:])
                    output += self._recv_exact(sock, n)
                elif line.startswith("EXIT:"):
                    exit_code = int(line[5:])
                    break
                elif line == "":
                    raise AgentProtocolError("connection closed before EXIT")
                else:
                    raise AgentProtocolError(f"unexpected line: {line!r}")
            return ExecResult(output=bytes(output), exit_code=exit_code)
        finally:
            self._quit(sock)

    def exec_detach(self, cmdline: str) -> ExecDetachResult:
        """Launch a program directly (no shell - no &&, %VAR% expansion,
        redirection, or built-ins like `dir`/`start`) and return
        immediately. The returned pid is the actual launched program, safe
        to pass to pskill()/look for in pslist(). Use for GUI apps,
        browser launches, or standalone background helpers."""
        sock = self._connect()
        try:
            sock.sendall(f"EXECDETACH {cmdline}\n".encode("utf-8", "replace"))
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK"):
                raise AgentProtocolError(f"unexpected EXECDETACH response: {reply!r}")
            pid = None
            if "pid=" in reply:
                try:
                    pid = int(reply.split("pid=", 1)[1].strip())
                except ValueError:
                    pid = None
            return ExecDetachResult(pid=pid, reply=reply)
        finally:
            self._quit(sock)

    def put(self, local_path: str | Path, remote_path: str) -> int:
        """Upload local_path (on this machine) to remote_path on the agent.
        Returns the number of bytes sent."""
        data = Path(local_path).read_bytes()
        sock = self._connect()
        try:
            sock.sendall(f"PUT {remote_path} {len(data)}\n".encode("utf-8", "replace"))
            sock.sendall(data)
            reply = self._recv_line(sock)
            if reply != "OK":
                raise AgentProtocolError(f"PUT failed: {reply}")
            return len(data)
        finally:
            self._quit(sock)

    def get(self, remote_path: str, local_path: str | Path) -> int:
        """Download remote_path from the agent to local_path (on this
        machine). Returns the number of bytes received."""
        sock = self._connect()
        try:
            sock.sendall(f"GET {remote_path}\n".encode("utf-8", "replace"))
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected GET response: {header!r}")
            size = int(header[5:])
            data = self._recv_exact(sock, size)
            Path(local_path).write_bytes(data)
            return size
        finally:
            self._quit(sock)

    def screenshot(self) -> bytes:
        """Capture the agent machine's screen. Returns raw BMP bytes."""
        sock = self._connect()
        try:
            sock.sendall(b"SCREENSHOT\n")
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected SCREENSHOT response: {header!r}")
            size = int(header[5:])
            return self._recv_exact(sock, size)
        finally:
            self._quit(sock)

    def _simple_command(self, line: str) -> None:
        """Send a one-line command that replies with just OK/ERR."""
        sock = self._connect()
        try:
            sock.sendall((line + "\n").encode("utf-8", "replace"))
            reply = self._recv_line(sock)
            if reply != "OK":
                raise AgentProtocolError(reply or "connection closed before OK")
        finally:
            self._quit(sock)

    def click(self, x: int, y: int, button: int = 1) -> None:
        self._simple_command(f"CLICK {x} {y} {button}")

    def key(self, *keyspecs: str) -> None:
        """Send one or more keys. NetWare batches them in a single StuffKey run."""
        parts: list[str] = []
        for k in keyspecs:
            parts.extend(k.replace(",", " ").split())
        if not parts:
            raise ValueError("key() requires at least one keyspec")
        self._simple_command("KEY " + " ".join(parts))

    def type_text(self, text: str) -> None:
        if "\n" in text or "\r" in text:
            raise ValueError("type_text() text must not contain newlines - use key('enter') instead")
        self._simple_command(f"TYPE {text}")

    def screens(self) -> list[tuple[int, bool, str]]:
        """NetWare: list CLIB screens as (id, displayed, name)."""
        sock = self._connect()
        try:
            sock.sendall(b"SCREENS\n")
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected SCREENS response: {header!r}")
            size = int(header[5:])
            text = self._recv_exact(sock, size).decode("utf-8", "replace")
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
        finally:
            self._quit(sock)

    def pslist(self) -> list[tuple[int, str]]:
        """List running processes. Returns (pid, image_name) pairs."""
        sock = self._connect()
        try:
            sock.sendall(b"PSLIST\n")
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected PSLIST response: {header!r}")
            size = int(header[5:])
            text = self._recv_exact(sock, size).decode("utf-8", "replace")
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
        finally:
            self._quit(sock)

    def pskill(self, pid: int) -> None:
        self._simple_command(f"PSKILL {pid}")

    def sysinfo(self) -> dict[str, str]:
        """OS family/version, computer name, memory, and C: disk space."""
        sock = self._connect()
        try:
            sock.sendall(b"SYSINFO\n")
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected SYSINFO response: {header!r}")
            size = int(header[5:])
            text = self._recv_exact(sock, size).decode("utf-8", "replace")
            info = {}
            for line in text.splitlines():
                key, sep, value = line.partition("=")
                if sep:
                    info[key] = value
            return info
        finally:
            self._quit(sock)

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
        sock = self._connect()
        try:
            sock.sendall(b"AUTOEXEC\n")
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK "):
                raise AgentProtocolError(f"unexpected AUTOEXEC response: {reply!r}")
            return reply[3:]
        finally:
            self._quit(sock)

    def debug(self, enabled: bool | None = None) -> int:
        """Query or set agent verbose logging. Returns 0 or 1.

        On NetWare, DEBUG 1 truncates SYS:SYSTEM\\LLMAGENT.LOG; pull it with
        get() then DEBUG 0 when done. Other agents may return ERR.
        """
        sock = self._connect()
        try:
            if enabled is None:
                sock.sendall(b"DEBUG\n")
            else:
                sock.sendall(f"DEBUG {1 if enabled else 0}\n".encode("ascii"))
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK debug="):
                raise AgentProtocolError(f"unexpected DEBUG response: {reply!r}")
            return int(reply[9:])
        finally:
            self._quit(sock)

    def winlist(self, parent_hwnd: int | None = None) -> list[WindowInfo]:
        """List visible top-level windows with a non-empty title.

        Pass parent_hwnd (from a prior winlist() result) to instead list
        that window's immediate children -- supported by the Win16 agent
        for inspecting a dialog's controls; title comes back as
        "<control id>:<text>" in that case.
        """
        sock = self._connect()
        try:
            cmd = f"WINLIST {parent_hwnd}\n" if parent_hwnd is not None else "WINLIST\n"
            sock.sendall(cmd.encode("ascii"))
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if not header.startswith("SIZE:"):
                raise AgentProtocolError(f"unexpected WINLIST response: {header!r}")
            size = int(header[5:])
            text = self._recv_exact(sock, size).decode("utf-8", "replace")
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
        finally:
            self._quit(sock)

    def winmsg(self, hwnd: int, msg: int, wparam: int, lparam: int) -> int:
        """Win16 agent only: raw SendMessage(hwnd, msg, wparam, lparam)
        passthrough. Returns the LRESULT. A thin, general primitive --
        the caller supplies whichever message/params accomplish a given
        UI action (e.g. BM_SETCHECK to toggle a checkbox, or a
        WM_LBUTTONDOWN/WM_LBUTTONUP pair sent to a button's own hwnd to
        make it click itself and notify its parent)."""
        sock = self._connect()
        try:
            sock.sendall(f"WINMSG {hwnd} {msg} {wparam} {lparam}\n".encode("ascii"))
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK:"):
                raise AgentProtocolError(f"unexpected WINMSG response: {reply!r}")
            return int(reply[3:])
        finally:
            self._quit(sock)

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
        sock = self._connect()
        try:
            sock.sendall(f"POSTMSG {hwnd} {msg} {wparam} {lparam}\n".encode("ascii"))
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if reply != "OK":
                raise AgentProtocolError(f"unexpected POSTMSG response: {reply!r}")
        finally:
            self._quit(sock)

    def lbgettext(self, hwnd: int, index: int) -> str:
        """Win16 agent only: LB_GETTEXT passthrough -- read a listbox
        item's text by index. WINMSG can't carry this directly (its
        lParam would need to be a buffer pointer, not a plain integer),
        so it's its own command. Useful for Control-Panel-style
        owner-drawn icon lists: read items by index to find which one is
        e.g. "Network" before selecting it by index via
        winmsg(hwnd, LB_SETCURSEL, index, 0)."""
        sock = self._connect()
        try:
            sock.sendall(f"LBGETTEXT {hwnd} {index}\n".encode("ascii"))
            reply = self._recv_line(sock)
            if reply.startswith("ERR:"):
                raise AgentProtocolError(reply)
            if not reply.startswith("OK:"):
                raise AgentProtocolError(f"unexpected LBGETTEXT response: {reply!r}")
            return reply[3:]
        finally:
            self._quit(sock)

    def clipboard_set(self, text: str) -> None:
        """Set the clipboard to plain text. Pair with key('ctrl-v') to
        paste it - more reliable than type_text() for exact strings."""
        if "\n" in text or "\r" in text:
            raise ValueError("clipboard_set() text must not contain newlines")
        self._simple_command(f"CLIPSET {text}")

    def reg_get(self, root: str, subkey: str, value_name: str) -> str | int:
        """Read a registry value. Returns int for REG_DWORD, str for
        REG_SZ/REG_EXPAND_SZ. Raises AgentProtocolError for anything else
        (missing key/value, or an unsupported value type)."""
        sock = self._connect()
        try:
            sock.sendall(f"REGGET\t{root}\t{subkey}\t{value_name}\n".encode("utf-8", "replace"))
            header = self._recv_line(sock)
            if header.startswith("ERR:"):
                raise AgentProtocolError(header)
            if header.startswith("DWORD:"):
                return int(header[6:])
            if header.startswith("SIZE:"):
                size = int(header[5:])
                return self._recv_exact(sock, size).decode("utf-8", "replace")
            raise AgentProtocolError(f"unexpected REGGET response: {header!r}")
        finally:
            self._quit(sock)

    def reg_set(self, root: str, subkey: str, value_name: str, value_type: str, data: str) -> None:
        """Write a registry value. value_type is "SZ" or "DWORD" (the
        only two the agent supports). Creates the key if it doesn't exist."""
        if value_type.upper() not in ("SZ", "DWORD"):
            raise ValueError('value_type must be "SZ" or "DWORD"')
        self._simple_command(f"REGSET\t{root}\t{subkey}\t{value_name}\t{value_type}\t{data}")

    @staticmethod
    def _quit(sock: socket.socket) -> None:
        try:
            sock.sendall(b"QUIT\n")
        except OSError:
            pass
        sock.close()

    def ping(self) -> bool:
        sock = self._connect()
        try:
            sock.sendall(b"PING\n")
            return self._recv_line(sock) == "PONG"
        finally:
            sock.close()
