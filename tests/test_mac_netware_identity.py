"""Two-fork and NLM updates must not confuse acceptance with replacement."""
import hashlib
from pathlib import Path
import tempfile
import sys
from unittest.mock import Mock, patch

import unittest
from test_session_timeouts import compile_run
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp-server"))
import server
from agent_client import AgentProtocolError


def resource_bytes():
    resource = bytearray(range(256))
    resource[:16] = (256).to_bytes(4, "big") * 2 + bytes(8)
    return bytes(resource)


def container():
    header = bytearray(128)
    header[1:5] = b"\x03app"
    header[65:69] = b"APPL"
    header[83:87] = (3).to_bytes(4, "big")
    header[87:91] = (256).to_bytes(4, "big")
    return bytes(header) + b"abc" + bytes(125) + resource_bytes()


class MacNetwareIdentityTests(unittest.TestCase):
    def setUp(self):
        self.agent = Mock()
        self.clock = 0
        def sleep(seconds):
            self.clock += seconds
        for name, value in (("_agent", lambda machine: self.agent),
                            ("time.sleep", sleep), ("time.monotonic", lambda: self.clock)):
            p = patch("server." + name, value)
            p.start()
            self.addCleanup(p.stop)

    def mac_info(self, **changes):
        info = {"os_family": "mac68k", "agent_started": "new", "agent_location": "volume:folder:name", "agent_resource_hash_mode": "sha256-zero-system-16-127-v1"}
        for prefix in ("agent", "disk"):
            info[prefix + "_data_sha256"] = hashlib.sha256(b"abc").hexdigest()
            info[prefix + "_resource_sha256"] = server._mac_resource_sha256(resource_bytes())
        return dict(info, **changes)

    def mac_wait(self, before=None):
        return server._wait_for_mac_replacement("test", before or {"agent_started": "old"},
            self.mac_info()["agent_data_sha256"], self.mac_info()["agent_resource_sha256"],
            timeout_seconds=3, interval_seconds=1)

    def test_resource_hash_normalizes_only_system_metadata(self):
        original = resource_bytes()
        for n in (16, 50, 127):
            changed = bytearray(original); changed[n] ^= 1
            self.assertEqual(server._mac_resource_sha256(changed), server._mac_resource_sha256(original))
        for n in (128, 255):
            changed = bytearray(original); changed[n] ^= 1
            self.assertNotEqual(server._mac_resource_sha256(changed), server._mac_resource_sha256(original))
        for value in (b"", bytes(256), bytes([255]) * 256):
            with self.assertRaises(ValueError):
                server._mac_resource_sha256(value)

    def test_mac_both_startup_and_disk_forks_required(self):
        info = self.mac_info()
        self.agent.sysinfo.return_value = info
        self.assertIn("update verified", self.mac_wait())
        self.assertEqual(self.clock, 15)
        for key in ("agent_data_sha256", "agent_resource_sha256", "disk_data_sha256", "disk_resource_sha256",
                    "agent_location", "agent_started", "os_family", "agent_resource_hash_mode"):
            self.agent.sysinfo.return_value = dict(info, **{key: ""})
            self.assertIn("NOT verified", self.mac_wait(), key)
        self.agent.sysinfo.return_value = dict(info, agent_started="old")
        self.assertIn("NOT verified", self.mac_wait())
        self.agent.sysinfo.return_value = info
        self.assertIn("NOT verified", self.mac_wait({"agent_location": "elsewhere"}))

    def test_mac_race_and_migration(self):
        info = self.mac_info()
        for key in ("agent_started", "agent_location", "disk_resource_sha256"):
            self.agent.sysinfo.side_effect = [info, dict(info, **{key: "changed"})] * 3
            self.assertIn("NOT verified", self.mac_wait())
        self.agent.sysinfo.side_effect = [OSError("restarting"), info, info]
        self.assertIn("update verified", self.mac_wait({}))

    def test_mac_snapshot_no_retry_and_optional_wait(self):
        self.agent.sysinfo.return_value = self.mac_info(agent_started="old")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "app.bin"
            for failure in (None, OSError("lost response"), AgentProtocolError("lost acknowledgment")):
                path.write_bytes(container())
                self.agent.mac_update.reset_mock()
                def send(snapshot):
                    path.write_bytes(b"rebuilt")
                    self.assertEqual(Path(snapshot).read_bytes(), container())
                    if failure:
                        raise failure
                    return len(container())
                self.agent.mac_update.side_effect = send
                with patch.object(server, "_wait_for_mac_replacement", return_value="update verified") as wait:
                    result = server.legacy_mac_self_update("test", str(path))
                    self.assertIn("update verified", result)
                    wait.assert_called_once_with("test", self.mac_info(agent_started="old"),
                        self.mac_info()["agent_data_sha256"], self.mac_info()["agent_resource_sha256"])
                    self.agent.mac_update.assert_called_once()
                    if failure:
                        self.assertIn("outcome unknown", result)
            path.write_bytes(container())
            with patch.object(server, "_wait_for_mac_replacement") as wait:
                self.assertIn("NOT verified", server.legacy_mac_self_update("test", str(path), False))
                wait.assert_not_called()
            self.agent.mac_update.reset_mock()
            path.write_bytes(b"invalid")
            self.assertIn("preflight error", server.legacy_mac_self_update("test", str(path)))
            self.agent.mac_update.assert_not_called()

    def test_netware_baseline_and_uncertain_handoff(self):
        target = r"SYS:SYSTEM\LLMAGENT.NLM"
        self.agent.sysinfo.return_value = {"os_family": "netware", "agent_started": "old", "agent_exe": target}
        files = {}
        self.agent.put.side_effect = lambda p, r: files.update({r: Path(p).read_bytes()})
        self.agent.get.side_effect = lambda r, p: Path(p).write_bytes(files[r])
        with tempfile.TemporaryDirectory() as td:
            binary, helper = Path(td) / "agent", Path(td) / "helper"
            binary.write_bytes(b"agent"); helper.write_bytes(b"helper")
            for failure in (None, OSError("lost ack"), AgentProtocolError("incomplete response")):
                self.agent.update.reset_mock()
                self.agent.update.side_effect = failure
                with patch.object(server, "_wait_for_replaced_agent", return_value="update verified") as wait:
                    self.assertIn("update verified", server.legacy_netware_self_update("test", str(binary), str(helper)))
                    wait.assert_called_once_with("test", target, "old", hashlib.sha256(b"agent").hexdigest())
                    self.agent.update.assert_called_once()
            self.agent.reset_mock()
            self.agent.sysinfo.return_value = {"os_family": "netware", "agent_exe": r"SYS:OTHER\LLMAGENT.NLM"}
            self.assertIn("preflight error", server.legacy_netware_self_update("test", str(binary), str(helper)))
            self.agent.put.assert_not_called()

    def test_native_mac_forks_and_failures(self):
        header = (ROOT / "agent-mac-system7/update_identity.h").as_posix()
        empty = hashlib.sha256(b"").hexdigest()
        content = bytearray(bytes(range(256)) * 9)
        content[:16] = b"".join(n.to_bytes(4, "big") for n in (256, 1024, 768, 1280))
        binary = hashlib.sha256(content).hexdigest()
        normalized = server._mac_resource_sha256(bytes(content))
        source = r'''
#include <assert.h>
#include <string.h>
typedef int OSErr;
typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;
typedef struct { unsigned long highLongOfPSN,lowLongOfPSN; } ProcessSerialNumber;
#define noErr 0
#define fsRdPerm 1
static FSSpec gApplicationSpec={-1,123,{3,'a','p','p'}};
static int gApplicationLocated=1, fault, closes, reads;
static long length, position;
static unsigned char content[2304];
static int FSpOpenDF(FSSpec *s,int mode,short *ref) {
 (void)s;(void)mode;position=0;*ref=1;return fault==1?-1:0;
}
static int FSpOpenRF(FSSpec *s,int mode,short *ref) {return FSpOpenDF(s,mode,ref);}
static int GetEOF(short ref,long *size) {
 (void)ref;*size=length;if(fault==6&&reads)*size+=1;return fault==2?-1:0;
}
static int FSRead(short ref,long *count,void *p) {
 (void)ref;reads++;if(fault==3)return -1;if(fault==4)--*count;
 memcpy(p,content+position,(size_t)*count);position+=*count;return 0;
}
static int FSClose(short ref) {(void)ref;closes++;return fault==5?-1:0;}
static int GetCurrentProcess(ProcessSerialNumber *p) {p->highLongOfPSN=0;p->lowLongOfPSN=42;return 0;}
static unsigned long TickCount(void) {return 123;}
'''
        source += f'#include "{header}"\n'
        source += f'''
int main(void) {{
 char out[65], saved[65]; int i,r;
 for(i=0;i<2304;i++)content[i]=(unsigned char)i;
 content[0]=0;content[1]=0;content[2]=1;content[3]=0;
 content[4]=0;content[5]=0;content[6]=4;content[7]=0;
 content[8]=0;content[9]=0;content[10]=3;content[11]=0;
 content[12]=0;content[13]=0;content[14]=5;content[15]=0;
 for(r=0;r<2;r++) {{
   length=0;if(!r) {{assert(MacForkSHA(&gApplicationSpec,r,out)==0);assert(!strcmp(out,"{empty}"));}}
   else assert(MacForkSHA(&gApplicationSpec,r,out)==-1);
   length=2304;assert(MacForkSHA(&gApplicationSpec,r,out)==0);assert(!strcmp(out,r?"{normalized}":"{binary}"));
   for(fault=1;fault<=6;fault++) {{
     closes=reads=0;strcpy(out,"old");assert(MacForkSHA(&gApplicationSpec,r,out)==-1);
     assert(!out[0]);assert(closes==(fault==1?0:1));
   }}
   fault=0;length=-1;assert(MacForkSHA(&gApplicationSpec,r,out)==-1);
   length=67108865L;assert(MacForkSHA(&gApplicationSpec,r,out)==-1);
 }}
 fault=0;length=2304;MacUpdateIdentityInit();strcpy(saved,gMacResourceSHA);
 assert(gMacStarted[0]);assert(!strcmp(gMacLocation,"0000ffff:0000007b:617070"));
 content[16]^=1;content[127]^=1;assert(MacForkSHA(&gApplicationSpec,1,out)==0);assert(!strcmp(out,saved));
 content[128]^=1;assert(MacForkSHA(&gApplicationSpec,1,out)==0);
 assert(strcmp(out,saved));assert(!strcmp(gMacResourceSHA,saved));
 content[0]=255;assert(MacForkSHA(&gApplicationSpec,1,out)==-1);assert(!out[0]);
 return 0;
}}
'''
        compile_run(source)
