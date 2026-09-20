"""Update verification must distinguish acceptance, rollback, and replacement."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from test_session_timeouts import compile_run
from test_uploads import function

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp-server"))
import server
from agent_client import AgentProtocolError

PAYLOAD = b"intended executable\0\xff"
SHA = hashlib.sha256(PAYLOAD).hexdigest()
TARGET = r"C:\LLM\LLMAGENT.EXE"
IDENTITY = {"agent_exe": TARGET, "agent_sha256": SHA, "agent_started": "new"}


class UpdateBridgeTests(unittest.TestCase):
    def setUp(self):
        self.agent = Mock()
        self.agent.sysinfo.return_value = dict(IDENTITY)
        self.agent.get.side_effect = lambda remote, local: Path(local).write_bytes(PAYLOAD)
        self.clock = 0
        def sleep(seconds):
            self.clock += seconds
        for name, value in (("_agent", lambda machine: self.agent),
                            ("time.sleep", sleep), ("time.monotonic", lambda: self.clock)):
            p = patch("server." + name, value)
            p.start()
            self.addCleanup(p.stop)

    def wait(self, old="old"):
        return server._wait_for_replaced_agent("test", TARGET, old, SHA,
                                               timeout_seconds=3, interval_seconds=1)

    def test_success_checks_startup_and_disk_after_quiet_period(self):
        self.assertIn("update verified", self.wait())
        self.assertEqual(self.clock, 15)
        self.agent.get.assert_called_once()
        self.assertEqual(self.agent.sysinfo.call_count, 2)

    def test_old_agent_and_rollback_never_verify(self):
        for info in ({}, dict(IDENTITY, agent_started="old"),
                     dict(IDENTITY, agent_sha256="bad"),
                     dict(IDENTITY, agent_exe=r"C:\OTHER\LLMAGENT.EXE")):
            with self.subTest(info=info):
                self.agent.sysinfo.return_value = info
                self.assertIn("NOT verified", self.wait())

    def test_matching_disk_does_not_disguise_old_loaded_image(self):
        self.agent.sysinfo.return_value = dict(IDENTITY, agent_sha256="old hash")
        self.assertIn("NOT verified", self.wait())
        self.agent.get.assert_not_called()

    def test_installed_file_mismatch_and_unavailable_identity(self):
        self.agent.get.side_effect = lambda r, p: Path(p).write_bytes(b"wrong")
        self.assertIn("NOT verified", self.wait())
        self.agent.sysinfo.return_value = {}
        self.assertIn("NOT verified", self.wait(None))

    def test_migration_from_old_agent_requires_new_identity(self):
        self.assertIn("update verified", self.wait(None))

    def test_identity_changes_during_readback(self):
        self.agent.sysinfo.side_effect = [IDENTITY, dict(IDENTITY, agent_started="third")] * 3
        self.assertIn("NOT verified", self.wait())

    def test_transient_disconnect_retries(self):
        self.agent.sysinfo.side_effect = [OSError("restarting"), IDENTITY, IDENTITY]
        self.assertIn("update verified", self.wait())
        self.assertEqual(self.clock, 16)

    def test_staged_corruption_blocks_launch_for_both_tools(self):
        with tempfile.TemporaryDirectory() as td:
            binary = Path(td) / "agent.exe"
            binary.write_bytes(PAYLOAD)
            helper = Path(td) / "helper.exe"
            helper.write_bytes(b"helper")
            self.agent.sysinfo.return_value = {}
            self.agent.get.side_effect = lambda r, p: Path(p).write_bytes(b"corrupt")
            for tool in (server.legacy_self_update, server.legacy_win16_self_update):
                result = tool("test", str(binary), str(helper), r"C:\LLM")
                self.assertIn("readback differs", result)
                self.agent.exec_detach.assert_not_called()
                self.agent.update.assert_not_called()

    def test_both_tools_snapshot_verify_stage_and_pass_baseline(self):
        with tempfile.TemporaryDirectory() as td:
            binary = Path(td) / "agent.exe"
            helper = Path(td) / "helper.exe"
            files = {}
            self.agent.sysinfo.return_value = dict(IDENTITY, agent_started="old")
            def put(local, remote):
                files[remote] = Path(local).read_bytes()
                binary.write_bytes(b"rebuilt during upload")
                return len(files[remote])
            self.agent.put.side_effect = put
            self.agent.get.side_effect = lambda r, p: Path(p).write_bytes(files[r])
            self.agent.exec_detach.return_value.reply = "OK"
            for tool in (server.legacy_self_update, server.legacy_win16_self_update):
                binary.write_bytes(PAYLOAD)
                helper.write_bytes(b"helper")
                kwargs = {"target_agent_name": "LLMAGENT.EXE"} if tool is server.legacy_self_update else {}
                with patch.object(server, "_wait_for_replaced_agent", return_value="verified") as wait:
                    result = tool("test", str(binary), str(helper), r"C:\LLM", **kwargs)
                    wait.assert_called_once_with("test", TARGET, "old", SHA)
                    self.assertIn("verified", result)
                binary.write_bytes(PAYLOAD)
                with patch.object(server, "_wait_for_replaced_agent") as wait:
                    result = tool("test", str(binary), str(helper), r"C:\LLM", False, **kwargs)
                    wait.assert_not_called()
                    self.assertIn("not yet verified", result)
                binary.write_bytes(PAYLOAD)
                command = self.agent.exec_detach if tool is server.legacy_self_update else self.agent.update
                command.side_effect = AgentProtocolError("connection closed before response line ended")
                with patch.object(server, "_wait_for_replaced_agent", return_value="update verified") as wait:
                    result = tool("test", str(binary), str(helper), r"C:\LLM", **kwargs)
                    self.assertIn("outcome unknown", result)
                    self.assertIn("update verified", result)
                    wait.assert_called_once()
                command.side_effect = None

    def test_aliases_wrong_target_and_invalid_paths_abort_before_put(self):
        for folder, names in (("relative", ("a", "b", "c")),
                              (r"C:\LLM", ("same.exe", "b", "SAME.EXE")),
                              (r"C:\LLM", ("..\\target.exe", "b", "c")),
                              (r"C:\LLM", ("target.exe.", "b", "c"))):
            with self.assertRaises(ValueError):
                server._update_paths(folder, *names)
        result = server.legacy_self_update("test", "missing", "missing", r"C:\WRONG")
        self.assertIn("preflight error", result)
        self.agent.put.assert_not_called()
        result = server.legacy_win16_self_update("test", "missing", "missing", "C:\\" + "d" * 124)
        self.assertIn("preflight error", result)
        self.agent.put.assert_not_called()


class NativeIdentityTests(unittest.TestCase):
    def test_sha256_vectors_and_startup_snapshot(self):
        # Padding boundaries, binary data, multi-block and realistic EXE size.
        with tempfile.TemporaryDirectory() as td:
            cases = [b"", b"abc"] + [bytes(range(256)) * (n // 256) + bytes(range(n % 256))
                                      for n in (55, 56, 63, 64, 65, 1000, 200000)]
            checks = []
            for i, data in enumerate(cases):
                p = Path(td) / str(i)
                p.write_bytes(data)
                checks.append(f'assert(update_file_sha256("{p.as_posix()}", out)==0);'
                              f'assert(strcmp(out,"{hashlib.sha256(data).hexdigest()}")==0);')
            header = (ROOT / "common/update_identity.h").as_posix()
            source = f'#include <assert.h>\n#include "{header}"\nint main(void) {{ char out[65]; FILE *f;\n'
            source += "\n".join(checks)
            p = (Path(td) / "1").as_posix()
            source += f'update_identity_init("{p}",123,42); assert(g_update_started[0]);'
            source += f'f=fopen("{p}","wb"); assert(f); fputs("changed",f); fclose(f);'
            source += f'assert(strcmp(g_update_sha256,"{hashlib.sha256(b"abc").hexdigest()}")==0);'
            source += 'assert(update_file_sha256("nonexistent-update-test-file",out)==-1); assert(!out[0]); return 0;}'
            compile_run(source)

    def test_win16_transaction_faults(self):
        source = (ROOT / "agent-win16/restart.c").read_text(encoding="utf-8")
        body = function(source, "static int restart_replace(")
        compile_run(r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
#define SW_SHOWNORMAL 1
static int target, backup, stage, fault, launches;
static int remove(const char *p) {
    assert(!strcmp(p,"backup"));
    if (fault==1) { errno=EACCES; return -1; }
    if (!backup) { errno=ENOENT; return -1; }
    backup=0; return 0;
}
static int rename(const char *a,const char *b) {
    if (!strcmp(a,"target") && !strcmp(b,"backup")) {
        if (fault==2) return -1;
        assert(target && !backup); backup=target; target=0;
    } else if (!strcmp(a,"stage")) {
        if (fault==3 || fault==5) return -1;
        assert(stage && !target); target=stage; stage=0;
    } else if (!strcmp(a,"backup")) {
        if (fault==5) return -1;
        assert(backup && !target); target=backup; backup=0;
    } else {
        assert(!strcmp(a,"target") && !strcmp(b,"stage"));
        assert(target && !stage); stage=target; target=0;
    }
    return 0;
}
static unsigned WinExec(const char *p,int show) {
    (void)show; assert(!strcmp(p,"target")); ++launches;
    return fault==4 && target==2 ? 2 : 33;
}
''' + body + r'''
int main(void) {
    int result;
    for (fault=0;fault<=5;fault++) {
        target=1; stage=2; backup=0; launches=0;
        result=restart_replace("stage","target","backup");
        if (fault==0) { assert(result==0 && target==2 && backup==1); }
        else if (fault==5) { assert(result==2 && !target && backup==1 && !launches); }
        else { assert(result==1 && target==1 && stage==2); }
    }
    return 0;
}
''')

    def test_win16_helper_launch_failure_keeps_agent_running(self):
        source = (ROOT / "agent-win16/llm_agent.c").read_text(encoding="utf-8")
        body = function(source, "static int handle_update(void)")
        compile_run(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char g_exedir[144]="C:\\LLM";
static int g_shutdown, missing, launch_result;
static char reply[512];
#define SW_SHOWMINNOACTIVE 1
static int send_cstr(const char *p) { assert(!g_shutdown); strcpy(reply,p); return 0; }
static unsigned WinExec(const char *p,int show) { (void)p; (void)show; return launch_result; }
static FILE *fake_open(const char *p,const char *mode) { (void)p; (void)mode; return missing?NULL:tmpfile(); }
#define fopen fake_open
''' + body + r'''
int main(void) {
    memset(g_exedir,'x',127); g_exedir[127]=0;
    handle_update(); assert(!g_shutdown && !strncmp(reply,"ERR:",4));
    strcpy(g_exedir,"C:\\LLM");
    launch_result=2; handle_update(); assert(!g_shutdown && !strncmp(reply,"ERR:",4));
    missing=1; launch_result=33; handle_update(); assert(!g_shutdown && !strncmp(reply,"ERR:",4));
    missing=0; handle_update(); assert(g_shutdown && !strcmp(reply,"OK\r\n"));
    return 0;
}
''')
