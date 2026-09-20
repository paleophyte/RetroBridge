"""Platform extension routing, binary update framing and honest capability reports."""
import ast
import json
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp-server"))
from agent_client import AgentClient, AgentInputError, AgentProtocolError
from capabilities import describe, TOOLS
import server
from test_agent_client import FakeSocket


def macbinary():
    data = bytearray(512)
    data[1] = 9
    data[2:11] = b"llm_agent"
    data[65:69] = b"APPL"
    data[86] = 3
    data[87:91] = (256).to_bytes(4, "big")
    data[128:131] = b"x\0\xff"
    return bytes(data)


class ExtensionClientTests(unittest.TestCase):
    def test_mouse_fields_and_button_validation(self):
        client = AgentClient("test.invalid", 2222, "example")
        for payload in (b"x=1\ny=2", b"x=1\ny=2\nbutton=2", b"x=1\nx=2\ny=3\nbutton=0",
                        b"x=-32769\ny=2\nbutton=0", b"x=32768\ny=2\nbutton=0", b"x=nan\ny=2\nbutton=0"):
            sock = FakeSocket(b"OK\nSIZE:" + str(len(payload)).encode() + b"\n" + payload)
            with patch.object(socket, "create_connection", return_value=sock):
                with self.assertRaises(AgentProtocolError): client.mouse_position()
            self.assertTrue(sock.closed)
            self.assertNotIn(b"QUIT\n", sock.sent)
        for args in ((1, 2, 2), (32768, 0), (-32769, 0)):
            with patch.object(socket, "create_connection") as connect:
                with self.assertRaises(AgentInputError): client.double_click(*args)
                connect.assert_not_called()
        with patch.object(socket, "create_connection") as connect:
            with self.assertRaises(AgentInputError): client.winclose(" ")
            connect.assert_not_called()

    def test_macbinary_exact_transfer_and_failures(self):
        client = AgentClient("test.invalid", 2222, "example")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "agent.bin"
            path.write_bytes(macbinary())
            sock = FakeSocket(b"OK\nOK\n")
            with patch.object(socket, "create_connection", return_value=sock):
                self.assertEqual(client.mac_update(path), 512)
            self.assertEqual(sock.sent, [b"example\n", b"UPDATE 512\n", macbinary(), b"QUIT\n"])
            for sock in (FakeSocket(b"OK\nERR:helper missing\n"), FakeSocket(b"OK\n"),
                         FakeSocket(b"OK\n", send_error_at=3)):
                with patch.object(socket, "create_connection", return_value=sock):
                    with self.assertRaises((AgentProtocolError, OSError)): client.mac_update(path)
                self.assertTrue(sock.closed)
                self.assertNotIn(b"QUIT\n", sock.sent)
            bad = [b"", b"MZ" * 256, macbinary()[:-1], macbinary() + b"padding"]
            for offset, value in ((0, 1), (1, 0), (65, 0), (74, 1), (82, 1), (120, 1), (121, 1), (89, 0)):
                blob = bytearray(macbinary()); blob[offset] = value; bad.append(bytes(blob))
            for blob in bad:
                path.write_bytes(blob)
                with patch.object(socket, "create_connection") as connect:
                    with self.assertRaises(AgentInputError): client.mac_update(path)
                    connect.assert_not_called()
            path.write_bytes(macbinary())
            client.max_response_bytes = 511
            with patch.object(socket, "create_connection") as connect:
                with self.assertRaises(AgentInputError): client.mac_update(path)
                connect.assert_not_called()


class BridgeCoverageTests(unittest.TestCase):
    CASES = [
        ("win16", "legacy_winmsg", "winmsg", (1, 2, 3, -1), -1),
        ("win16", "legacy_postmsg", "postmsg", (1, 2, 3, 4), None),
        ("win16", "legacy_lbgettext", "lbgettext", (1, 0), "a\"b"),
        ("os2", "legacy_winclose", "winclose", ("Settings",), None),
        ("os2-13", "legacy_winclose", "winclose", ("Settings",), None),
        ("netware", "legacy_screens", "screens", (), [(12, True, "Console")]),
        ("netware", "legacy_autoexec", "autoexec", (), "autoexec=present"),
        ("netware", "legacy_debug", "debug", (None,), 0),
        ("netware", "legacy_debug", "debug", (True,), 1),
        ("mac68k", "legacy_double_click", "double_click", (1, 2, 1), None),
        ("mac68k", "legacy_mouse_position", "mouse_position", (), {"x": 1, "y": 2, "button": 0}),
    ]

    @staticmethod
    def info(profile):
        return {"os_family": "os2" if profile.startswith("os2") else profile,
                "agent": "llm_agent-" + profile}

    def test_platform_routing_and_errors(self):
        for profile, tool, method, args, result in self.CASES:
            with self.subTest(tool=tool, profile=profile):
                agent = Mock()
                agent.sysinfo.return_value = self.info(profile)
                getattr(agent, method).return_value = result
                with patch.object(server, "_agent", return_value=agent):
                    reply = getattr(server, tool)("fixture", *args)
                    self.assertEqual(reply, "OK (request accepted)" if result is None else json.dumps(result))
                    getattr(agent, method).assert_called_once_with(*args)
                    agent.reset_mock()
                    agent.sysinfo.return_value = {"os_family": "dos"}
                    self.assertIn("command not sent", getattr(server, tool)("fixture", *args))
                    getattr(agent, method).assert_not_called()
                    agent.sysinfo.return_value = self.info(profile)
                    getattr(agent, method).side_effect = AgentProtocolError("ERR:unsupported")
                    self.assertIn("ERR:unsupported", getattr(server, tool)("fixture", *args))

    def test_winlist_parent_is_forwarded_only_to_win16(self):
        agent = Mock(); agent.winlist.return_value = []
        with patch.object(server, "_agent", return_value=agent):
            server.legacy_winlist("fixture")
            agent.winlist.assert_called_once_with()
            agent.reset_mock(); agent.sysinfo.return_value = self.info("win16")
            server.legacy_winlist("fixture", 42)
            agent.winlist.assert_called_once_with(42)
            agent.reset_mock(); agent.sysinfo.return_value = {"os_family": "nt"}
            self.assertIn("command not sent", server.legacy_winlist("fixture", 42))
            agent.winlist.assert_not_called()

    def test_advisory_profiles_only_advertise_registered_tools(self):
        tree = ast.parse((ROOT / "mcp-server/server.py").read_text())
        registered = {n.name for n in tree.body if isinstance(n, ast.FunctionDef)
                      and any(ast.unparse(d) == "srv.tool()" for d in n.decorator_list)}
        for profile, tools in TOOLS.items():
            self.assertLessEqual(tools, registered)
        for profile in ("dos", "win16", "os2", "os2-13", "mac68k", "netware"):
            self.assertEqual(describe(self.info(profile))["profile"], profile)
        for family in ("9x", "nt"):
            self.assertEqual(describe({"os_family": family})["profile"], "win32")
        self.assertNotIn("legacy_enable_autologon", describe({"os_family": "9x"})["tools"])
        self.assertIn("legacy_enable_autologon", describe({"os_family": "nt"})["tools"])
        for info in ({}, {"os_family": "future"}, {"os_family": "os2", "agent": "custom"}):
            self.assertEqual(describe(info)["profile"], "unknown")
        agent = Mock(); agent.sysinfo.return_value = dict(self.info("mac68k"), token="sensitive-value")
        agent.text_encoding = agent.exec_encoding = agent.exec_command_encoding = agent.file_encoding = "mac-roman"
        with patch.object(server, "_agent", return_value=agent):
            result = server.legacy_capabilities("fixture")
        self.assertNotIn("sensitive-value", result)
        self.assertIn("not negotiated", result)
        self.assertEqual([c[0] for c in agent.mock_calls], ["sysinfo"])

    def test_mac_update_wrong_platform_does_not_send(self):
        agent = Mock(); agent.sysinfo.return_value = self.info("netware")
        with patch.object(server, "_agent", return_value=agent):
            self.assertIn("command not sent", server.legacy_mac_self_update("fixture", "app.bin"))
            agent.mac_update.assert_not_called()

    def test_netware_staging_verifies_both_before_update(self):
        agent = Mock(); agent.sysinfo.return_value = self.info("netware")
        with tempfile.TemporaryDirectory() as td, patch.object(server, "_agent", return_value=agent):
            binary, helper = Path(td) / "agent", Path(td) / "helper"
            binary.write_bytes(b"agent"); helper.write_bytes(b"helper RETRO_NW_UPDATE_PROTOCOL_2")
            files = {}
            def put(local, remote):
                files[remote] = Path(local).read_bytes()
                helper.write_bytes(b"changed after first upload")
                return len(files[remote])
            agent.put.side_effect = put
            agent.get.side_effect = lambda r, p: Path(p).write_bytes(files[r])
            reply = server.legacy_netware_self_update("fixture", str(binary), str(helper), False)
            self.assertEqual(files, {r"SYS:SYSTEM\LLMAGENT.NEW": b"agent", r"SYS:SYSTEM\UPDATE.NLM": b"helper RETRO_NW_UPDATE_PROTOCOL_2"})
            agent.update.assert_called_once_with()
            self.assertIn("replacement NOT verified", reply)
            self.assertEqual([c[0] for c in agent.mock_calls], ["sysinfo", "put", "get", "put", "get", "update"])
            for bad_remote in files:
                helper.write_bytes(b"helper RETRO_NW_UPDATE_PROTOCOL_2")
                agent.reset_mock()
                agent.get.side_effect = lambda r, p: Path(p).write_bytes(b"corrupt" if r == bad_remote else files[r])
                self.assertIn("readback differs", server.legacy_netware_self_update("fixture", str(binary), str(helper), False))
                agent.update.assert_not_called()
            agent.reset_mock(); helper.write_bytes(b"")
            self.assertIn("preflight error", server.legacy_netware_self_update("fixture", str(binary), str(helper), False))
            agent.put.assert_not_called(); agent.update.assert_not_called()
            agent.sysinfo.return_value = self.info("mac68k")
            self.assertIn("command not sent", server.legacy_netware_self_update("fixture", str(binary), str(helper), False))


if __name__ == "__main__":
    unittest.main()
