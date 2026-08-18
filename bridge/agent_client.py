"""TCP client for the llm_agent protocol (see agent/llm_agent.c).

Wire protocol, one connection = one session:
    client -> server: token line
    server -> client: "OK\\n" | "FAIL\\n"
    client -> server: "EXEC <cmdline>\\n" | "PUT <path> <size>\\n" | "GET <path>\\n"
                       | "SCREENSHOT\\n" | "CLICK <x> <y> <button>\\n"
                       | "KEY <keyspec>\\n" | "TYPE <text>\\n" | "PING\\n" | "QUIT\\n"
    server -> client (EXEC): repeated "LEN:<n>\\n" + <n> raw bytes, then "EXIT:<code>\\n"
    server -> client (PUT):  "OK\\n" | "ERR:<msg>\\n"
    server -> client (GET):  "SIZE:<n>\\n" + <n> raw bytes, or "ERR:<msg>\\n"
    server -> client (SCREENSHOT): "SIZE:<n>\\n" + <n> raw BMP bytes, or "ERR:<msg>\\n"
    server -> client (CLICK/KEY/TYPE): "OK\\n" | "ERR:<msg>\\n"
    server -> client (PING): "PONG\\n"
"""

from __future__ import annotations

import socket
from dataclasses import dataclass
from pathlib import Path


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
