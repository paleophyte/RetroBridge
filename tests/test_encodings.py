"""Code-page conversion without changing wire framing or binary payloads."""
import json
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp-server"))
from agent_client import AgentClient, AgentInputError, AgentProtocolError
from machines import MachineConfig, MachineConfigError, load_machines
from text_codec import normalize_encoding, TEXT_ENCODINGS
import server
from test_agent_client import FakeSocket


class EncodingTests(unittest.TestCase):
    def client(self, encoding="cp1252", **kwargs):
        return AgentClient("test.invalid", 2222, "example", text_encoding=encoding, **kwargs)

    def test_explicit_codecs_preserve_ascii_and_reject_transforms(self):
        ascii_bytes = bytes(range(128))
        for encoding in TEXT_ENCODINGS:
            self.assertEqual(ascii_bytes.decode("ascii").encode(normalize_encoding(encoding)), ascii_bytes)
        self.assertEqual(normalize_encoding("windows-1252"), "cp1252")
        self.assertEqual(normalize_encoding("macroman"), "mac-roman")
        for encoding in ("utf-16", "utf-7", "utf-8-sig", "cp932", "iso2022_jp", "unicode_escape", "base64", "mbcs", "unknown"):
            with self.subTest(encoding=encoding):
                with self.assertRaises(ValueError): normalize_encoding(encoding)
                with self.assertRaises(AgentInputError): self.client(encoding)
        self.assertEqual(normalize_encoding("utf-16le", output=True), "utf-16-le")

    def test_command_bytes_are_guest_codepage_and_limits_are_bytes(self):
        for encoding in ("cp1252", "cp437", "cp850", "mac_roman", "utf-8"):
            client = self.client(encoding)
            command = "WINCLOSE Caf\u00e9"
            sock = FakeSocket(b"OK\nOK\n")
            with patch.object(socket, "create_connection", return_value=sock): client.winclose("Caf\u00e9")
            self.assertEqual(sock.sent[1], command.encode(encoding) + b"\n")
        client = self.client("cp1252", max_command_bytes=8)
        self.assertEqual(client._encode_command("GET caf\u00e9"), b"GET caf\xe9\n")
        with patch.object(socket, "create_connection") as connect:
            with self.assertRaises(AgentInputError): client.get("caf\u00e9x", "unused")
            connect.assert_not_called()

    def test_unrepresentable_input_and_keyboard_text_never_connect(self):
        for client, text in ((self.client("ascii"), "caf\u00e9"), (self.client(), "\u2603"),
                             (self.client(), "\ud800")):
            with patch.object(socket, "create_connection") as connect:
                with self.assertRaises(AgentInputError) as error: client.exec(text)
                self.assertNotIn(text, str(error.exception)); connect.assert_not_called()
        for method in ("key", "type_text"):
            with patch.object(socket, "create_connection") as connect:
                with self.assertRaises(AgentInputError): getattr(self.client(), method)("abc\u00e9")
                connect.assert_not_called()

    def test_ansi_commands_oem_exec_and_utf16_output_override(self):
        client = self.client(exec_encoding="cp437")
        payload = "caf\u00e9\r\n".encode("cp437")
        sock = FakeSocket(b"OK\nLEN:6\n" + payload + b"EXIT:0\n")
        with patch.object(socket, "create_connection", return_value=sock): result = client.exec("echo caf\u00e9")
        self.assertEqual(sock.sent[1], b"EXEC echo caf\xe9\n")
        self.assertEqual(result.output, payload)
        self.assertEqual(client.decode_text(result.output, exec_output=True), "caf\u00e9\r\n")
        # An explicit EXEC-input override must not affect EXECDETACH's encoding.
        client = self.client(exec_command_encoding="cp437", exec_encoding="cp437")
        sock = FakeSocket(b"OK\nEXIT:0\n")
        with patch.object(socket, "create_connection", return_value=sock): client.exec("echo caf\u00e9")
        self.assertEqual(sock.sent[1], b"EXEC echo caf\x82\n")
        sock = FakeSocket(b"OK\nOK pid=1\n")
        with patch.object(socket, "create_connection", return_value=sock): client.exec_detach("caf\u00e9.exe")
        self.assertEqual(sock.sent[1], b"EXECDETACH caf\xe9.exe\n")
        machine = MachineConfig("test", "test.invalid", exec_token="example", text_encoding="cp1252", exec_encoding="cp437")
        with patch.object(server, "_machines_cache", {"test": machine}):
            payload = "\u03a9".encode("utf-16-le")
            sock = FakeSocket(b"OK\nLEN:2\n" + payload + b"EXIT:7\n")
            with patch.object(socket, "create_connection", return_value=sock):
                reply = server.legacy_exec("test", "program", output_encoding="utf-16-le")
            self.assertEqual(reply, "\u03a9\n[exit code: 7]")
            with patch.object(socket, "create_connection") as connect:
                self.assertIn("input error", server.legacy_exec("test", "program", output_encoding="base64"))
                connect.assert_not_called()

    def test_all_text_responses_preserve_non_ascii(self):
        cases = [
            ("sysinfo", (), "computer_name=caf\u00e9\r\n", lambda r: r["computer_name"]),
            ("pslist", (), "1\tcaf\u00e9\r\n", lambda r: r[0][1]),
            ("screens", (), "1\t1\tcaf\u00e9\r\n", lambda r: r[0][2]),
            ("winlist", (), "1\t0\t0\t1\t1\tFrame\tcaf\u00e9\r\n", lambda r: r[0].title),
            ("reg_get", ("HKCU", "Software", "value"), "caf\u00e9", lambda r: r),
        ]
        for encoding in ("cp1252", "cp437", "mac_roman", "utf-8"):
            for method, args, text, extract in cases:
                with self.subTest(encoding=encoding, method=method):
                    payload = text.encode(encoding)
                    sock = FakeSocket(b"OK\nSIZE:" + str(len(payload)).encode() + b"\n" + payload)
                    with patch.object(socket, "create_connection", return_value=sock):
                        self.assertEqual(extract(getattr(self.client(encoding), method)(*args)), "caf\u00e9")
            sock = FakeSocket(b"OK\nOK:" + "caf\u00e9".encode(encoding) + b"\r\n")
            with patch.object(socket, "create_connection", return_value=sock):
                self.assertEqual(self.client(encoding).lbgettext(1, 0), "caf\u00e9")
        # Unicode's additional line separators are data, not protocol rows.
        payload = b"1\tA\x85B\r\n"
        sock = FakeSocket(b"OK\nSIZE:" + str(len(payload)).encode() + b"\n" + payload)
        with patch.object(socket, "create_connection", return_value=sock):
            self.assertEqual(self.client("latin-1").pslist(), [(1, "A\x85B")])

    def test_decode_errors_are_strict_private_and_preserve_exec_status(self):
        sock = FakeSocket(b"OK\nOK:private-data\x81\n")
        with patch.object(socket, "create_connection", return_value=sock):
            with self.assertRaises(AgentProtocolError) as error: self.client().lbgettext(1, 0)
        self.assertNotIn("private-data", str(error.exception))
        self.assertTrue(sock.closed); self.assertNotIn(b"QUIT\n", sock.sent)
        machine = MachineConfig("test", "test.invalid", exec_token="example")
        with patch.object(server, "_machines_cache", {"test": machine}):
            sock = FakeSocket(b"OK\nLEN:1\n\xffEXIT:3\n")
            with patch.object(socket, "create_connection", return_value=sock): reply = server.legacy_exec("test", "program")
            self.assertIn("output decoding error", reply)
            self.assertIn("command already executed; exit code: 3", reply)
            self.assertNotIn("\ufffd", reply)

    def test_file_and_image_bytes_are_never_transcoded(self):
        data = bytes(range(256))
        with tempfile.TemporaryDirectory() as td:
            local = Path(td) / "file"; local.write_bytes(data)
            client = self.client(file_encoding="cp437")
            sock = FakeSocket(b"OK\nOK\n")
            with patch.object(socket, "create_connection", return_value=sock): client.put(local, "caf\u00e9.bin")
            self.assertEqual(sock.sent[1:3], [b"PUT caf\x82.bin 256\n", data])
            for method, args in (("get", ("caf\u00e9.bin", local)), ("screenshot", ())):
                sock = FakeSocket(b"OK\nSIZE:256\n" + data)
                with patch.object(socket, "create_connection", return_value=sock): result = getattr(client, method)(*args)
                self.assertEqual(result if method == "screenshot" else local.read_bytes(), data)
                if method == "get": self.assertEqual(sock.sent[1], b"GET caf\x82.bin\n")

    def test_inventory_codec_validation_defaults_and_capability_report(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "machines.ini"
            base = "[guest]\nhost=test.invalid\nexec_token=example\n"
            path.write_text(base + "text_encoding=windows-1252\nexec_encoding=cp437\nexec_command_encoding=cp850\nfile_encoding=cp437\n")
            machine = load_machines(path)["guest"]
            self.assertEqual((machine.text_encoding, machine.exec_encoding, machine.exec_command_encoding), ("cp1252", "cp437", "cp850"))
            payload = b"os_family=nt\r\n"
            sock = FakeSocket(b"OK\nSIZE:" + str(len(payload)).encode() + b"\n" + payload)
            with patch.object(server, "_machines_cache", {"guest": machine}), patch.object(socket, "create_connection", return_value=sock):
                report = json.loads(server.legacy_capabilities("guest"))
            self.assertEqual(report["text_encodings"], {"text_encoding": "cp1252", "exec_encoding": "cp437", "exec_command_encoding": "cp850", "file_encoding": "cp437"})
            path.write_text(base + "text_encoding=macroman\n")
            machine = load_machines(path)["guest"]
            self.assertEqual((machine.text_encoding, machine.exec_encoding, machine.exec_command_encoding), ("mac-roman",) * 3)
            self.assertEqual(machine.file_encoding, "mac-roman")
            path.write_text(base)
            self.assertEqual(load_machines(path)["guest"].text_encoding, "ascii")
            for key in ("text_encoding", "exec_encoding", "exec_command_encoding", "file_encoding"):
                path.write_text(base + key + "=private-invalid-value\n")
                with self.assertRaises(MachineConfigError) as error: load_machines(path)
                self.assertNotIn("private-invalid-value", str(error.exception))
                self.assertIn(key, str(error.exception))
            path.write_text(base + "agent_enabled=false\ntext_encoding=unused\n")
            self.assertFalse(load_machines(path)["guest"].agent_enabled)


if __name__ == "__main__":
    unittest.main()
