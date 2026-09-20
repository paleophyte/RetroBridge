"""Production NetWare transaction logic with filesystem and loader faults."""
from pathlib import Path
import unittest
from test_session_timeouts import compile_run
from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]

class NetwareRecoveryTests(unittest.TestCase):
    def test_transaction_faults(self):
        source = (ROOT / "agent-netware/update.c").read_text(encoding="utf-8")
        source = source.replace('#include "nwsock.h"', '').replace('#undef ferror', '')
        for name in ("../common/update_identity.h", "update_state.h"):
            path = (ROOT / "agent-netware" / name).resolve().as_posix()
            source = source.replace(f'#include "{name}"', f'#include "{path}"')
        fixture = (ROOT / "tests/netware_update_fixture.c").read_text(encoding="utf-8")
        handler = function((ROOT / "agent-netware/llm_agent.c").read_text(encoding="utf-8"), "static int handle_update(")
        compile_run(fixture.replace("PRODUCTION", source).replace("HANDLER", handler))

    def test_bridge_blocks_unresolved_recovery_and_old_helper(self):
        import tempfile
        from unittest.mock import Mock, patch
        from test_mac_netware_identity import server
        agent = Mock()
        with tempfile.TemporaryDirectory() as td, patch.object(server, "_agent", return_value=agent):
            binary, helper = Path(td) / "agent", Path(td) / "helper"
            binary.write_bytes(b"agent"); helper.write_bytes(b"RETRO_NW_UPDATE_PROTOCOL_2")
            for state in ("busy", "recovery-required", "unknown"):
                agent.sysinfo.return_value = {"os_family": "netware", "update_protocol": "2", "update_state": state}
                self.assertIn("recovery unresolved", server.legacy_netware_self_update("test", str(binary), str(helper)))
                agent.put.assert_not_called()
            agent.sysinfo.return_value = {"os_family": "netware", "update_protocol": "2", "update_state": "idle"}
            helper.write_bytes(b"old helper")
            self.assertIn("protocol-2", server.legacy_netware_self_update("test", str(binary), str(helper)))
            agent.put.assert_not_called()
            helper.write_bytes(b"RETRO_NW_UPDATE_PROTOCOL_2")
            files = {}
            agent.put.side_effect = lambda p, r: files.update({r: Path(p).read_bytes()})
            agent.get.side_effect = lambda r, p: Path(p).write_bytes(files[r])
            with patch.object(server, "_wait_for_replaced_agent", return_value="update verified on test"):
                agent.sysinfo.side_effect = [agent.sysinfo.return_value, dict(agent.sysinfo.return_value, update_state="recovery-required")]
                self.assertIn("recovery NOT cleared", server.legacy_netware_self_update("test", str(binary), str(helper)))
                agent.update.assert_called_once()

    def test_bridge_reports_matching_completed_rollback_only(self):
        from unittest.mock import Mock, patch
        from test_mac_netware_identity import server
        target = r"SYS:SYSTEM\LLMAGENT.NLM"
        agent = Mock(); clock = [0]
        def sleep(seconds): clock[0] += seconds
        info = {"os_family": "netware", "agent_exe": target, "agent_started": "new", "agent_sha256": "oldhash",
                "update_state": "idle", "update_last": "rolled-back wanted SYS:SYSTEM\\LU123456\\RESULT"}
        with patch.object(server, "_agent", return_value=agent), patch.object(server.time, "sleep", sleep), patch.object(server.time, "monotonic", lambda: clock[0]):
            agent.sysinfo.return_value = info
            result = server._wait_for_replaced_agent("test", target, "old", "wanted", timeout_seconds=1)
            self.assertIn("restored the previous", result)
            for changes in ({"agent_started": "old"}, {"update_state": "busy"}, {"update_last": "rolled-back another elsewhere"}):
                agent.sysinfo.return_value = dict(info, **changes)
                result = server._wait_for_replaced_agent("test", target, "old", "wanted", timeout_seconds=1)
                self.assertNotIn("restored the previous", result)
                self.assertIn("NOT verified", result)
