/*
 * update.exe: replaces a running llm_agent installation with a new
 * binary, then restarts it. Meant to be PUT alongside a freshly-built
 * llm_agent.exe and launched via EXECDETACH, so it can safely stop and
 * replace the very agent that's driving it (EXECDETACH launches it as a
 * direct child, not tied to the parent's lifetime the way EXEC's pipe
 * relationship is - killing the parent doesn't touch this process).
 *
 * Usage: update.exe <new-exe-path> [target-exe-path]
 * Both paths must be absolute. target-exe-path defaults to "llm_agent.exe"
 * in update.exe's own directory - a service's default working directory
 * is system32, not wherever the agent/update.exe actually live, so a
 * relative path here would silently resolve to the wrong place.
 *
 * Same -march=i486 / no-CRT-formatted-IO discipline as llm_agent.c - see
 * docs/ARCHITECTURE.md ("Bugs found via live testing") for why: a real
 * crash on Windows 95, not a style preference. No _snprintf/printf/
 * sscanf here either.
 */

#include <windows.h>
#include <winsvc.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

#define SERVICE_NAME_A "LLMAgent"
#define TARGET_EXE_NAME "llm_agent.exe"

static int is_windows_9x(void) {
    return (GetVersion() & 0x80000000) != 0;
}

static void con_msg(const char *text) {
    fputs(text, stdout);
}

/* ---- NT-family: stop the installed service and WAIT for it to actually
   report SERVICE_STOPPED. ControlService only requests the stop; polling
   afterward matters because "assume a stop is instant" is the exact bug
   already found and fixed once in llm_agent.c's own net-stop handling -
   not repeating it here. ---- */
static int stop_nt_service(void) {
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    int i;

    ZeroMemory(&st, sizeof(st));
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    svc = OpenServiceA(scm, SERVICE_NAME_A, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return 0; }

    if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    ControlService(svc, SERVICE_CONTROL_STOP, &st);

    for (i = 0; i < 100; i++) { /* up to ~20s */
        Sleep(200);
        if (!QueryServiceStatus(svc, &st)) break;
        if (st.dwCurrentState == SERVICE_STOPPED) break;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return (st.dwCurrentState == SERVICE_STOPPED);
}

static int start_nt_service(void) {
    SC_HANDLE scm, svc;
    BOOL ok;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    svc = OpenServiceA(scm, SERVICE_NAME_A, SERVICE_START);
    if (!svc) { CloseServiceHandle(scm); return 0; }
    ok = StartServiceA(svc, 0, NULL);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 1 : 0;
}

typedef HANDLE (WINAPI *CreateToolhelp32SnapshotFn)(DWORD, DWORD);
typedef BOOL (WINAPI *Process32FirstFn)(HANDLE, LPPROCESSENTRY32);
typedef BOOL (WINAPI *Process32NextFn)(HANDLE, LPPROCESSENTRY32);

/* ---- Windows 9x: no SCM, so find llm_agent.exe by name and terminate
   it - the same Toolhelp32 pattern llm_agent.c's own PSLIST already
   established (dynamically resolved for the same reason: NT4's
   kernel32.dll doesn't export these, so a static reference here would
   break loading there too, even though this specific function is only
   ever called on 9x). Skips its own PID so update.exe can never
   self-terminate if it somehow shares a name collision. ---- */
static int stop_9x_agent(void) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    CreateToolhelp32SnapshotFn pCreateSnap;
    Process32FirstFn pFirst;
    Process32NextFn pNext;
    HANDLE hSnap;
    PROCESSENTRY32 pe;
    int killed = 0;
    DWORD myPid = GetCurrentProcessId();

    if (!k32) return 0;
    pCreateSnap = (CreateToolhelp32SnapshotFn)GetProcAddress(k32, "CreateToolhelp32Snapshot");
    pFirst = (Process32FirstFn)GetProcAddress(k32, "Process32First");
    pNext = (Process32NextFn)GetProcAddress(k32, "Process32Next");
    if (!pCreateSnap || !pFirst || !pNext) return 0;

    hSnap = pCreateSnap(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (pFirst(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID != myPid && _stricmp(pe.szExeFile, TARGET_EXE_NAME) == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                    killed = 1;
                }
            }
        } while (pNext(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return killed;
}

static int file_exists(const char *path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* ---- Swap targetPath's contents for newPath's, keeping a same-directory
   backup until the swap succeeds. Retries the initial rename briefly -
   the just-stopped process may take a moment to release its file
   mapping, so an immediate sharing-violation isn't treated as fatal. If
   putting the new file in place fails, restores the backup so a bad
   upload can't strand the machine with no agent at all. ---- */
static int replace_file(const char *newPath, const char *targetPath) {
    char backupPath[MAX_PATH + 8];
    int targetLen = (int)strlen(targetPath);
    int i;
    int renamedOld = 0;

    if (targetLen + 5 >= (int)sizeof(backupPath)) return 0;
    memcpy(backupPath, targetPath, targetLen);
    memcpy(backupPath + targetLen, ".old", 5); /* includes the NUL */

    DeleteFileA(backupPath); /* clear out any stale backup from a prior update */

    for (i = 0; i < 25; i++) { /* up to ~5s */
        if (!file_exists(targetPath)) {
            break; /* nothing to back up - e.g. first-ever install via update.exe */
        }
        if (MoveFileA(targetPath, backupPath)) {
            renamedOld = 1;
            break;
        }
        Sleep(200);
    }

    if (!MoveFileA(newPath, targetPath)) {
        if (renamedOld) MoveFileA(backupPath, targetPath);
        return 0;
    }

    if (renamedOld) DeleteFileA(backupPath); /* best-effort cleanup */
    return 1;
}

static int launch_9x_run(const char *targetPath) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 16];
    int n = (int)strlen(targetPath);
    int ok;

    if (n + 8 >= (int)sizeof(cmd)) return 0;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    memcpy(cmd, targetPath, n);
    memcpy(cmd + n, " --run", 7); /* includes the NUL */

    ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *newPath;
    char targetPathBuf[MAX_PATH];
    const char *targetPath;
    int stopped, replaced, started;

    if (argc < 2) {
        con_msg("usage: update.exe <new-exe-path> [target-exe-path]\n"
                "Both paths must be absolute.\n");
        return 1;
    }
    newPath = argv[1];

    if (argc >= 3) {
        targetPath = argv[2];
    } else {
        /* Default: llm_agent.exe next to update.exe itself. */
        DWORD n = GetModuleFileNameA(NULL, targetPathBuf, MAX_PATH);
        char *slash;
        if (n == 0 || n >= MAX_PATH) {
            con_msg("ERROR: could not determine update.exe's own path\n");
            return 1;
        }
        slash = strrchr(targetPathBuf, '\\');
        if (slash) *(slash + 1) = '\0';
        strcat(targetPathBuf, TARGET_EXE_NAME);
        targetPath = targetPathBuf;
    }

    if (is_windows_9x()) {
        stopped = stop_9x_agent();
    } else {
        stopped = stop_nt_service();
    }
    if (!stopped) {
        con_msg("warning: could not confirm the running agent stopped - continuing anyway\n");
    }

    Sleep(500); /* settle, on top of replace_file()'s own retry loop */

    replaced = replace_file(newPath, targetPath);
    if (!replaced) {
        con_msg("ERROR: failed to replace the agent binary - old binary restored if possible\n");
        return 1;
    }

    if (is_windows_9x()) {
        started = launch_9x_run(targetPath);
    } else {
        started = start_nt_service();
    }

    if (!started) {
        con_msg("warning: new binary is in place but could not be started automatically - start it manually\n");
        return 1;
    }

    con_msg("update complete\n");
    return 0;
}
