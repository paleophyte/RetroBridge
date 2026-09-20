"""Host-side fault injection for the production power handler.

Run with Python 3 and a host C compiler (CC, or cc). Toolbox calls are
stubbed; live System 7 tests are still needed for Finder/save-dialog behavior.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


class PowerTests(unittest.TestCase):
    def test_delivery_and_failures(self):
        source = (Path(__file__).resolve().parents[1] / "llm_agent.c").read_text()
        start = source.index("static void HandlePower(int restart)")
        end = source.index("\n/* DRAGRESET", start)
        handler = source[start:end]
        start = source.index("static int SendPowerReply(const char *s)")
        end = source.index("static int SendAll(", start)
        reply_helper = source[start:end]
        constants = "\n".join(re.findall(r"^#define AE_.*$", source, re.M))
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t OSType;
typedef uint32_t DescType;
typedef uint32_t AEEventClass;
typedef uint32_t AEEventID;
typedef int32_t Size;
typedef int OSErr;
typedef int Boolean;
enum { false=0, true=1 };
typedef struct { int live; } AEAddressDesc;
typedef AEAddressDesc AppleEvent;
typedef struct { uint32_t highLongOfPSN, lowLongOfPSN; } ProcessSerialNumber;
typedef struct { unsigned long processInfoLength; OSType processSignature; } ProcessInfoRec;
enum { noErr=0, typeApplSignature=0x7369676e,
       kNoProcess=0, kAECanSwitchLayer=64, kAEAlwaysInteract=48 };
static int failure, creates, sends, disposes, stopped, activated, yielded, quit_during_send;
static int gQuitRequested;
static uint32_t expected_event;
static char response[160];
static int gStream=42;
static OSErr TCPSendBytes(int stream, const void *data, unsigned short len,
                         Boolean allowQuit) {
    const char *s=data;
    assert(stream == gStream && allowQuit && len == strlen(s));
    assert(response[0] == 0);
    strcpy(response, s);
    return noErr;
}
static void StopMouseAutomation(void) { ++stopped; }
static void Idle(void) {
    assert(activated == 1); ++yielded;
    if (failure == 6) gQuitRequested=1;
}
static OSErr GetNextProcess(ProcessSerialNumber *psn) {
    assert(psn->highLongOfPSN == 0);
    if (failure == 4 || psn->lowLongOfPSN == 2) return -600;
    ++psn->lowLongOfPSN; return noErr;
}
static OSErr GetProcessInformation(const ProcessSerialNumber *psn, ProcessInfoRec *info) {
    assert(info->processInfoLength == sizeof(*info));
    info->processSignature=psn->lowLongOfPSN == 2 ? 0x4d414353 : 0x54455354;
    return noErr;
}
static OSErr SetFrontProcess(const ProcessSerialNumber *psn) {
    assert(psn->highLongOfPSN == 0 && psn->lowLongOfPSN == 2);
    if (failure == 5) return -600;
    ++activated; return noErr;
}
static OSErr AECreateDesc(int type, const void *data, Size size,
                           AEAddressDesc *out) {
    const ProcessSerialNumber *psn=data;
    assert(type == 0x70736e20 && size == 8);
    assert(psn->highLongOfPSN == 0 && psn->lowLongOfPSN == 2);
    if (failure == 1) return -108;
    out->live=1; ++creates; return noErr;
}
static OSErr AECreateAppleEvent(AEEventClass cls, AEEventID id,
                               const AEAddressDesc *target, int ret,
                               long transaction, AppleEvent *out) {
    assert(target->live && cls == 0x464e4452 && id == expected_event);
    assert(ret == -1 && transaction == 0);
    if (failure == 2) return -1708;
    out->live=1; ++creates; return noErr;
}
static OSErr AESend(const AppleEvent *event, AppleEvent *reply, int mode,
                    int priority, long timeout, void *idle, void *filter) {
    (void)reply;
    assert(event->live && response[0] == 0 && stopped == 1 && activated == 1 && yielded == 1);
    assert(mode == (1 | 64 | 48)); /* no reply; allow UI and layer switch */
    assert(priority == 0 && timeout == -1 && !idle && !filter);
    ++sends;
    if (quit_during_send) gQuitRequested=1;
    return failure == 3 ? -600 : noErr;
}
static OSErr AEDisposeDesc(AEAddressDesc *desc) {
    assert(desc->live); desc->live=0; ++disposes; return noErr;
}
'''
        main = r'''
int main(void) {
    int restart, fail, reentrant;
    for (restart=0; restart<=1; ++restart)
      for (fail=0; fail<=6; ++fail)
        for (reentrant=0; reentrant<=1; ++reentrant) {
            failure=fail; quit_during_send=reentrant;
            creates=sends=disposes=stopped=activated=yielded=gQuitRequested=0;
            response[0]=0;
            expected_event=restart ? 0x72657374 : 0x73687574;
            HandlePower(restart);
            assert(disposes == creates);
            assert(sends == (fail == 0 || fail == 3));
            assert(stopped == (sends || fail == 5 || fail == 6));
            assert(gQuitRequested == ((reentrant && sends) || fail == 6));
            if (fail == 0) assert(strcmp(response,"OK\n") == 0);
            else {
                assert(strncmp(response,"ERR:",4) == 0);
                assert(strstr(response,fail == 1 ? "-108" : fail == 2 ? "-1708" :
                                       fail == 6 ? "quitting" : "-600"));
            }
        }
    puts("Both power requests: delivery, API failures, cleanup and reentrant quit passed.");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cfile = root / "power.c"
            executable = root / ("power.exe" if os.name == "nt" else "power")
            cfile.write_text(harness + constants + "\n" + reply_helper + handler + main)
            compiler = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                                      str(cfile), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
