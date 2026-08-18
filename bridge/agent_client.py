"""TCP client for the llm_agent protocol (see agent/llm_agent.c for the
authoritative wire protocol docs - this is a summary, not the source of
truth for edge cases).

Wire protocol, one connection = one session:
    client -> server: token line
    server -> client: "OK\\n" | "FAIL\\n"
    client -> server: "EXEC <cmdline>\\n" | "PUT <path> <size>\\n" | "GET <path>\\n"
                       | "SCREENSHOT\\n" | "CLICK <x> <y> <button>\\n"
                       | "KEY <keyspec>\\n" | "TYPE <text>\\n" | "PSLIST\\n"
                       | "PSKILL <pid>\\n" | "SYSINFO\\n" | "REBOOT\\n"
                       | "SHUTDOWN\\n" | "WINLIST\\n" | "CLIPSET <text>\\n"
                       | "REGGET\\t<root>\\t<subkey>\\t<valuename>\\n"
                       | "REGSET\\t<root>\\t<subkey>\\t<valuename>\\t<type>\\t<data>\\n"
                       | "PING\\n" | "QUIT\\n"
    server -> client (EXEC): repeated "LEN:<n>\\n" + <n> raw bytes, then "EXIT:<code>\\n"
                             (LEN:0 with no bytes may appear as a heartbeat)
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

    def key(self, keyspec: str) -> None:
        self._simple_command(f"KEY {keyspec}")

    def type_text(self, text: str) -> None:
        if "\n" in text or "\r" in text:
            raise ValueError("type_text() text must not contain newlines - use key('enter') instead")
        self._simple_command(f"TYPE {text}")

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

    def winlist(self) -> list[WindowInfo]:
        """List visible top-level windows with a non-empty title."""
        sock = self._connect()
        try:
            sock.sendall(b"WINLIST\n")
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
