"""Shared-client protocol tests using a byte-stream socket double."""
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mcp-server"))
from agent_client import AgentClient, AgentInputError, AgentProtocolError, AgentAuthError
import agent_client
import server
from machines import MachineConfig


class FakeSocket:
    def __init__(self, reply=b"OK\n", send_error_at=0):
        self.pending = bytearray(reply)
        self.sent = []
        self.closed = False
        self.send_error_at = send_error_at

    def sendall(self, data):
        self.sent.append(data)
        if len(self.sent) == self.send_error_at:
            raise OSError("injected send failure")

    def recv(self, n):
        take = min(n, len(self.pending), 3)
        data = bytes(self.pending[:take])
        del self.pending[:take]
        return data

    def close(self):
        self.closed = True


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.src = Path(self.temp.name) / "upload"
        self.dst = Path(self.temp.name) / "download"
        self.src.write_bytes(b"\x00\r\nPING\n\xff")
        self.client = AgentClient("test.invalid", 2222, "private-test-token")

    def reject(self, method, *args):
        with patch.object(socket, "create_connection") as connect:
            with self.assertRaises(AgentInputError) as error:
                getattr(self.client, method)(*args)
            connect.assert_not_called()
            self.assertNotIn("private-test-token", str(error.exception))

    def test_command_delimiters_before_connect(self):
        for bad in ("x\nPING", "x\rPING", "x\x00PING"):
            for method, args in (
                ("exec", (bad,)), ("exec_detach", (bad,)),
                ("get", (bad, self.dst)), ("put", (self.src, bad)),
                ("key", (bad,)), ("type_text", (bad,)), ("clipboard_set", (bad,))):
                with self.subTest(method=method, bad=repr(bad)):
                    self.reject(method, *args)

    def test_registry_fields_cannot_change_delimiters(self):
        for method, valid in (("reg_get", ["HKLM", "Software", "name"]),
                              ("reg_set", ["HKCU", "Software", "name", "SZ", "value"])):
            for index in range(len(valid)):
                for bad in ("\t", "\r", "\n", "\x00"):
                    with self.subTest(method=method, index=index, bad=repr(bad)):
                        args = valid.copy(); args[index] += bad + "extra"
                        self.reject(method, *args)

    def test_numbers_are_not_string_interpolation_channels(self):
        for method, args in (("click", [1, 2, 1]), ("pskill", [12]),
                              ("winlist", [12]), ("winmsg", [1, 2, 3, 4]),
                              ("postmsg", [1, 2, 3, 4]), ("lbgettext", [1, 2])):
            for index in range(len(args)):
                bad = args.copy(); bad[index] = "1\nPING"
                self.reject(method, *bad)

    def test_token_validation_is_strict_and_private(self):
        for token in ("", "private-test-token\nPING", "private-test-token\r", "x\x00y", "caf\u00e9", "x" * 128):
            self.client.token = token
            self.reject("ping")

    def test_encoded_command_boundary(self):
        for limit in (510, 4094):
            self.client = AgentClient("test.invalid", 2222, "private-test-token", max_command_bytes=limit)
            command = "x" * (limit - 5)
            sock = FakeSocket(b"OK\nEXIT:0\n")
            with patch.object(socket, "create_connection", return_value=sock):
                self.client.exec(command)
            self.assertEqual(len(sock.sent[1]), limit + 1)
            self.reject("exec", command + "x")
        self.client = AgentClient("test.invalid", 2222, "private-test-token")
        # 5-byte prefix + 252 two-byte characters + one ASCII byte = 510.
        command = "\u00e9" * 252 + "x"
        sock = FakeSocket(b"OK\nEXIT:0\n")
        with patch.object(socket, "create_connection", return_value=sock):
            self.client.exec(command)
        self.assertEqual(len(sock.sent[1]), 511)
        self.reject("exec", command + "x")
        self.reject("exec", "\ud800")

    def test_valid_commands_have_one_frame(self):
        cases = [
            ("exec", ("echo hello",), b"EXEC echo hello\n", b"LEN:2\na\x00EXIT:0\n"),
            ("exec_detach", ("app.exe",), b"EXECDETACH app.exe\n", b"OK pid=12\n"),
            ("get", ("C:\\space name", self.dst), b"GET C:\\space name\n", b"SIZE:3\n\x00\r\n"),
            ("screenshot", (), b"SCREENSHOT\n", b"SIZE:0\n"),
            ("click", (1, 2), b"CLICK 1 2 1\n", b"OK\n"),
            ("key", ("down,up", "enter"), b"KEY down up enter\n", b"OK\n"),
            ("type_text", ("text",), b"TYPE text\n", b"OK\n"),
            ("clipboard_set", ("text",), b"CLIPSET text\n", b"OK\n"),
            ("pslist", (), b"PSLIST\n", b"SIZE:0\n"),
            ("screens", (), b"SCREENS\n", b"SIZE:0\n"),
            ("sysinfo", (), b"SYSINFO\n", b"SIZE:0\n"),
            ("winlist", (), b"WINLIST\n", b"SIZE:0\n"),
            ("winlist", (12,), b"WINLIST 12\n", b"SIZE:0\n"),
            ("pskill", (12,), b"PSKILL 12\n", b"OK\n"),
            ("winmsg", (1, 2, 3, -1), b"WINMSG 1 2 3 -1\n", b"OK:-1\n"),
            ("postmsg", (1, 2, 3, 4), b"POSTMSG 1 2 3 4\n", b"OK\n"),
            ("lbgettext", (1, 2), b"LBGETTEXT 1 2\n", b"OK:hello\n"),
            ("reg_get", ("HKLM", "Software", ""), b"REGGET\tHKLM\tSoftware\t\n", b"DWORD:42\n"),
            ("reg_set", ("HKCU", "Software", "", "SZ", ""), b"REGSET\tHKCU\tSoftware\t\tSZ\t\n", b"OK\n"),
            ("autoexec", (), b"AUTOEXEC\n", b"OK autoexec=present\n"),
            ("debug", (), b"DEBUG\n", b"OK debug=0\n"),
            ("debug", (True,), b"DEBUG 1\n", b"OK debug=1\n"),
            ("debug", (False,), b"DEBUG 0\n", b"OK debug=0\n"),
        ]
        for method in ("reboot", "shutdown", "update", "ping"):
            cases.append((method, (), method.upper().encode() + b"\n", b"PONG\n" if method == "ping" else b"OK\n"))
        for method, args, wire, reply in cases:
            with self.subTest(method=method, args=args):
                sock = FakeSocket(b"OK\r\n" + reply)
                with patch.object(socket, "create_connection", return_value=sock):
                    getattr(self.client, method)(*args)
                self.assertEqual(sock.sent, [b"private-test-token\n", wire, b"QUIT\n"])
                self.assertTrue(sock.closed)

    def test_binary_upload_not_validated_as_text(self):
        data = self.src.read_bytes()
        sock = FakeSocket(b"OK\nOK\n")
        with patch.object(socket, "create_connection", return_value=sock):
            self.assertEqual(self.client.put(self.src, "C:\\file"), len(data))
        self.assertEqual(sock.sent, [b"private-test-token\n", f"PUT C:\\file {len(data)}\n".encode(), data, b"QUIT\n"])

    def test_exec_launch_failure_returns_diagnostic_and_exit(self):
        for api, error in (("CreatePipe", 8), ("CreateProcessA", 2)):
            message = f"EXEC launch failed: {api} (Win32 error {error})\r\n".encode()
            sock = FakeSocket(b"OK\n" + f"LEN:{len(message)}\n".encode()
                              + message + b"EXIT:-1\n")
            with patch.object(socket, "create_connection", return_value=sock):
                result = self.client.exec("echo hello")
            self.assertEqual(result.output, message)
            self.assertEqual(result.exit_code, -1)
            self.assertEqual(sock.sent[-1], b"QUIT\n")
            self.assertTrue(sock.closed)

    def test_invalid_sizes_on_all_payload_operations(self):
        client = AgentClient("test.invalid", 2222, "token", max_response_bytes=4)
        methods = [("get", ("x", self.dst)), ("screenshot", ()), ("screens", ()),
                   ("pslist", ()), ("sysinfo", ()), ("winlist", ()),
                   ("reg_get", ("HKLM", "Software", "X"))]
        self.dst.write_bytes(b"preserve")
        for method, args in methods:
            for size in (b"-1", b"+1", b" 1", b"1x", b"5", b"9" * 5000):
                with self.subTest(method=method, size=size[:10]):
                    sock = FakeSocket(b"OK\nSIZE:" + size + b"\n")
                    with patch.object(socket, "create_connection", return_value=sock):
                        with self.assertRaises(AgentProtocolError):
                            getattr(client, method)(*args)
                    self.assertTrue(sock.closed)
                    self.assertNotIn(b"QUIT\n", sock.sent)
        self.assertEqual(self.dst.read_bytes(), b"preserve")

    def test_exec_cumulative_limit_and_heartbeats(self):
        client = AgentClient("test.invalid", 2222, "token", max_response_bytes=4)
        for response in (b"LEN:3\nabcLEN:2\ndeEXIT:0\n", b"LEN:-1\n", b"EXIT:bogus\n"):
            sock = FakeSocket(b"OK\n" + response)
            with patch.object(socket, "create_connection", return_value=sock):
                with self.assertRaises(AgentProtocolError):
                    client.exec("echo")
            self.assertTrue(sock.closed)
            self.assertNotIn(b"QUIT\n", sock.sent)
        sock = FakeSocket(b"OK\nLEN:0\nLEN:4\na\x00\r\nEXIT:-1\n")
        with patch.object(socket, "create_connection", return_value=sock):
            result = client.exec("echo")
        self.assertEqual((result.output, result.exit_code), (b"a\x00\r\n", -1))

    def test_line_limits_and_partial_replies(self):
        for response in (b"PONG", b"PO\rNG\n", b"\x00PONG\n", b"x" * 65537 + b"\n"):
            sock = FakeSocket(b"OK\n" + response)
            with patch.object(socket, "create_connection", return_value=sock):
                with self.assertRaises(AgentProtocolError): self.client.ping()
            self.assertTrue(sock.closed)
            self.assertNotIn(b"QUIT\n", sock.sent)
        sock = FakeSocket(b"OK\nOK:" + b"x" * 32767 + b"\r\n")
        with patch.object(socket, "create_connection", return_value=sock):
            self.assertEqual(len(self.client.lbgettext(1, 0)), 32767)

    def test_close_on_auth_send_and_payload_failures(self):
        for sock, method, args in (
            (FakeSocket(b"FAIL\n"), "ping", ()),
            (FakeSocket(b""), "ping", ()),
            (FakeSocket(send_error_at=1), "ping", ()),
            (FakeSocket(send_error_at=2), "ping", ()),
            (FakeSocket(send_error_at=3), "put", (self.src, "x")),
            (FakeSocket(b"OK\nSIZE:5\nab"), "get", ("x", self.dst))):
            with patch.object(socket, "create_connection", return_value=sock):
                with self.assertRaises((AgentProtocolError, AgentAuthError, OSError)):
                    getattr(self.client, method)(*args)
            self.assertTrue(sock.closed)
            self.assertNotIn(b"QUIT\n", sock.sent)

    def test_bridge_rejects_injection_before_connect(self):
        with patch.object(server, "_machines_cache", {"test": MachineConfig("test", "test.invalid", exec_token="token")}), patch.object(socket, "create_connection") as connect:
            result = server.legacy_exec("test", "echo\nSHUTDOWN")
            self.assertIn("must not contain", result)
            connect.assert_not_called()


if __name__ == "__main__":
    unittest.main()
