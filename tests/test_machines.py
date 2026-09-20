"""Inventory and bridge routing regressions; requires mcp-server requirements.

Run: python -m unittest discover -s tests -p test_machines.py
"""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mcp-server"))
from machines import MachineConfigError, load_machines
import server


MIXED = """[legacy]
host = 192.0.2.1
exec_token = example%literal-token
vm_name = Example VM
[ubuntu]
host = 192.0.2.2
agent_enabled = false
linux_username = example
linux_password = example%password
"""


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "machines.ini"

    def load(self, text):
        self.path.write_text(text, encoding="utf-8")
        return load_machines(self.path)

    def test_mixed_inventory_and_literal_credentials(self):
        machines = self.load(MIXED)
        self.assertEqual(list(machines), ["legacy", "ubuntu"])
        self.assertTrue(machines["legacy"].agent_enabled)
        self.assertEqual(machines["legacy"].exec_port, 2222)
        self.assertEqual(machines["legacy"].exec_token, "example%literal-token")
        self.assertFalse(machines["ubuntu"].agent_enabled)
        self.assertEqual(machines["ubuntu"].exec_token, "")

    def test_disabled_agent_settings_are_ignored(self):
        m = self.load(MIXED + "exec_port = unused\nexec_token = unused\n")["ubuntu"]
        self.assertEqual(m.exec_token, "")
        self.assertFalse(m.agent_enabled)

    def test_missing_enabled_token_still_rejects_whole_inventory(self):
        for flag in ("", "agent_enabled = true\n"):
            with self.subTest(flag=flag):
                with self.assertRaisesRegex(MachineConfigError, "missing 'exec_token'"):
                    self.load(MIXED + "[broken]\nhost = 192.0.2.3\n" + flag)

    def test_disabled_host_is_required(self):
        with self.assertRaisesRegex(MachineConfigError, "missing 'host'"):
            self.load("[ubuntu]\nagent_enabled = false\n")

    def test_invalid_settings_do_not_echo_values(self):
        for key, value in (("agent_enabled", "example-sensitive-value"),
                           ("exec_port", "example-sensitive-value"),
                           ("exec_port", "0"), ("exec_port", "65536")):
            with self.subTest(key=key, value=value):
                with self.assertRaises(MachineConfigError) as error:
                    self.load("[legacy]\nhost = 192.0.2.1\nexec_token = example\n"
                              + f"{key} = {value}\n")
                self.assertIn(f"invalid '{key}'", str(error.exception))
                self.assertNotIn("example-sensitive-value", str(error.exception))

    def test_explicit_enabled_port(self):
        m = self.load("[legacy]\nhost = 192.0.2.1\nexec_token = example\n"
                      "agent_enabled = true\nexec_port = 65535\n")["legacy"]
        self.assertTrue(m.agent_enabled)
        self.assertEqual(m.exec_port, 65535)

    def test_parser_errors_are_sanitized(self):
        for data in (b"example-sensitive-value\n",
                     b"[host]\nexec_token=example\nexec_token=example-sensitive-value\n",
                     b"[host]\nexec_token=example-sensitive-value\xff"):
            with self.subTest(data=data):
                self.path.write_bytes(data)
                with self.assertRaises(MachineConfigError) as error:
                    load_machines(self.path)
                self.assertIn("cannot read machine config", str(error.exception))
                self.assertNotIn("example-sensitive-value", str(error.exception))

    def test_utf8_bom(self):
        self.assertEqual(len(self.load("\ufeff" + MIXED)), 2)

    def test_missing_file(self):
        with self.assertRaisesRegex(MachineConfigError, "not found"):
            load_machines(self.path)

    def test_supplied_example(self):
        example = Path(__file__).resolve().parents[1] / "mcp-server" / "machines.ini.example"
        machines = load_machines(example)
        self.assertFalse(machines["ubuntu-host"].agent_enabled)
        self.assertTrue(machines["win2k-1"].agent_enabled)

    def test_wait_tools_reject_config_errors_without_retrying(self):
        machines = self.load(MIXED)
        with patch.object(server, "_machines_cache", machines), patch.object(server, "AgentClient") as client:
            with patch.object(server.time, "sleep", side_effect=AssertionError("must not retry config errors")):
                for tool in (server.legacy_wait_for_agent, server.legacy_wait_for_desktop):
                    self.assertIn("inventory-only", tool("ubuntu"))
                    self.assertIn("unknown machine", tool("missing"))
            client.assert_not_called()

    def test_bridge_listing_and_routing(self):
        machines = self.load(MIXED)
        with patch.object(server, "_machines_cache", machines), patch.object(server, "AgentClient") as client:
            listing = server.legacy_list_machines()
            self.assertIn("legacy: host=192.0.2.1 exec_port=2222 vm_name=Example VM", listing)
            self.assertIn("ubuntu: host=192.0.2.2 agent_enabled=false (inventory-only)", listing)
            self.assertNotIn("literal-token", listing)
            self.assertNotIn("password", listing)
            self.assertIn("inventory-only", server.legacy_ping("ubuntu"))
            with self.assertRaisesRegex(MachineConfigError, "inventory-only"):
                server._agent("ubuntu")
            with self.assertRaisesRegex(MachineConfigError, "unknown machine"):
                server._agent("missing")
            client.assert_not_called()
            server._agent("legacy")
            client.assert_called_once_with("192.0.2.1", 2222, "example%literal-token",
                                           max_command_bytes=510, max_response_bytes=67108864)

    def test_protocol_limits(self):
        m = self.load("[legacy]\nhost=192.0.2.1\nexec_token=example\n"
                      "max_command_bytes=4094\nmax_response_bytes=1048576\n")["legacy"]
        self.assertEqual((m.max_command_bytes, m.max_response_bytes), (4094, 1048576))
        for key, value in (("max_command_bytes", "4095"), ("max_command_bytes", "0"),
                           ("max_response_bytes", "-1"), ("max_response_bytes", "2147483648"),
                           ("max_command_bytes", "private-invalid-value")):
            with self.subTest(key=key, value=value):
                with self.assertRaises(MachineConfigError) as error:
                    self.load(f"[legacy]\nhost=x\nexec_token=example\n{key}={value}\n")
                self.assertNotIn("private-invalid-value", str(error.exception))


if __name__ == "__main__":
    unittest.main()
