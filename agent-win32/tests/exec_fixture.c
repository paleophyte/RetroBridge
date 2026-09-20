/* Native guest smoke fixture. Uses the production EXEC handler with memory
 * socket output. Injects a pipe error without exhausting the machine, and
 * asks the real CreateProcessA to open a nonexistent application. No agent
 * configuration, listener, or OS executable is modified.
 * Build with the agent's Win32 flags/libraries and run as a console program.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fixture_mode, fixture_used, fixture_closes;
static DWORD fixture_error;
static char fixture_wire[4096];

static BOOL WINAPI fixture_pipe(PHANDLE r, PHANDLE w,
                                LPSECURITY_ATTRIBUTES sa, DWORD size) {
    if (fixture_mode == 1) {
        fixture_error = ERROR_NOT_ENOUGH_MEMORY;
        SetLastError(fixture_error);
        return FALSE;
    }
    return CreatePipe(r, w, sa, size);
}

static BOOL WINAPI fixture_process(LPCSTR app, LPSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
        DWORD flags, LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si,
        LPPROCESS_INFORMATION pi) {
    BOOL ok;
    if (fixture_mode == 2) app = "C:\\__EXEC_FIXTURE_MISSING__\\NOPE.EXE";
    ok = CreateProcessA(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    if (!ok) {
        fixture_error = GetLastError();
        SetLastError(fixture_error);
    }
    return ok;
}

static BOOL WINAPI fixture_close(HANDLE handle) {
    BOOL ok = CloseHandle(handle);
    ++fixture_closes;
    SetLastError(999); /* Cleanup must not replace the reported launch error. */
    return ok;
}

static int WSAAPI fixture_send(SOCKET s, const char *p, int n, int flags) {
    int take = n > 3 ? 3 : n;
    (void)s; (void)flags;
    if (fixture_used + take >= (int)sizeof(fixture_wire)) return SOCKET_ERROR;
    memcpy(fixture_wire + fixture_used, p, take);
    fixture_used += take;
    fixture_wire[fixture_used] = 0;
    return take;
}

#define CreatePipe fixture_pipe
#define CreateProcessA fixture_process
#define CloseHandle fixture_close
#define send fixture_send
#define main agent_program_main
#include "../llm_agent.c"
#undef main

#define CHECK(condition) do { if (!(condition)) { \
    fputs("FAIL: " #condition "\n", stdout); return 1; } } while (0)

int main(void) {
    int mode, expected_len;
    char message[128], expected[192];
    for (mode = 1; mode <= 2; ++mode) {
        fixture_mode = mode; fixture_used = fixture_closes = 0;
        net_reset();
        CHECK(run_exec(0, "echo EXECFIXTURE") == -1);
        wsprintfA(message, "EXEC launch failed: %s (Win32 error %lu)\r\n",
                  mode == 1 ? "CreatePipe" : "CreateProcessA",
                  (unsigned long)fixture_error);
        wsprintfA(expected, "LEN:%lu\n%sEXIT:-1\n",
                  (unsigned long)strlen(message), message);
        expected_len = (int)strlen(expected);
        CHECK(fixture_used == expected_len && !memcmp(fixture_wire, expected, expected_len));
        CHECK(fixture_closes == (mode == 1 ? 0 : 2));
        CHECK(!net_failed && send_cstr(0, "PONG\n") == 0);
        CHECK(!memcmp(fixture_wire + expected_len, "PONG\n", 5));
        fixture_mode = fixture_used = fixture_closes = 0;
        CHECK(run_exec(0, "echo EXECFIXTURE") == 0);
        CHECK(strstr(fixture_wire, "EXECFIXTURE") != NULL);
        CHECK(strstr(fixture_wire, "EXIT:0\n") != NULL);
        CHECK(fixture_closes == 4 && !net_failed);
    }
    fputs("PASS: pipe failure, real process-launch failure, cleanup, framing and subsequent EXEC\n", stdout);
    return 0;
}
