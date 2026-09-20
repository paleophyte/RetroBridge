/* Native Win16 fixture for LBGETTEXT. Build with Open Watcom:
 * wcl -zq -zW -bt=windows -os -w4 -i=C:\WATCOM\H\WIN
 *     listbox_fixture.c -fe=LBTEST.EXE -l=windows
 * Run with EXECDETACH, never DOS EXEC. Writes LBTEST.TXT next to the EXE.
 * Close the fixture with POSTMSG <parent> 16 0 0 (WM_CLOSE).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HINSTANCE instance;
static const unsigned lengths[] = {0, 1, 159, 160, 161, 4096, 32767};

long __export FAR PASCAL FixtureProc(HWND hwnd, unsigned msg,
                                    UINT wparam, LONG lparam) {
    if (msg == WM_MEASUREITEM) {
        ((LPMEASUREITEMSTRUCT)lparam)->itemHeight = 18;
        return TRUE;
    }
    if (msg == WM_DRAWITEM) return TRUE;
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASS wc;
    MSG msg;
    HWND parent, normal, data, strings, button;
    HGLOBAL memory;
    LPSTR text;
    char path[144];
    char *slash;
    FILE *report;
    unsigned i, j;
    long index;
    (void)prev; (void)cmd; (void)show;
    instance = inst;
    if (FindWindow("LLMLBTEST", NULL)) return 1;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = FixtureProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "LLMLBTEST";
    if (!RegisterClass(&wc)) return 2;
    parent = CreateWindow("LLMLBTEST", "LBGETTEXT regression fixture",
                          WS_OVERLAPPEDWINDOW, 10, 10, 600, 260,
                          NULL, NULL, instance, NULL);
    if (!parent) return 3;
    normal = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                          8, 8, 260, 190, parent, (HMENU)100, instance, NULL);
    data = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | LBS_OWNERDRAWFIXED,
                        278, 8, 140, 70, parent, (HMENU)101, instance, NULL);
    strings = CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE |
                           LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                           278, 90, 140, 70, parent, (HMENU)102, instance, NULL);
    button = CreateWindow("BUTTON", "Not a listbox", WS_CHILD | WS_VISIBLE,
                          430, 8, 140, 30, parent, (HMENU)103, instance, NULL);
    if (!normal || !data || !strings || !button) return 4;
    GetModuleFileName(instance, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (!slash) return 5;
    strcpy(slash + 1, "LBTEST.TXT");
    report = fopen(path, "w");
    if (!report) return 6;
    fprintf(report, "parent=%u\nnormal=%u\nownerdata=%u\nownerstrings=%u\nbutton=%u\n",
            (unsigned)parent, (unsigned)normal, (unsigned)data,
            (unsigned)strings, (unsigned)button);
    memory = GlobalAlloc(GMEM_MOVEABLE, 32768UL);
    text = memory ? (LPSTR)GlobalLock(memory) : NULL;
    if (!text) { fclose(report); if (memory) GlobalFree(memory); return 7; }
    for (i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
        for (j = 0; j < lengths[i]; j++) text[j] = (char)('A' + j % 26);
        text[lengths[i]] = '\0';
        index = SendMessage(normal, LB_ADDSTRING, 0, (LPARAM)text);
        fprintf(report, "item%u=%ld,%u,%ld\n", i, index, lengths[i],
                SendMessage(normal, LB_GETTEXTLEN, (WPARAM)index, 0));
    }
    SendMessage(normal, LB_ADDSTRING, 0, (LPARAM)(LPCSTR)"first\nsecond");
    SendMessage(data, LB_ADDSTRING, 0, 0x12345678L);
    SendMessage(strings, LB_ADDSTRING, 0, (LPARAM)(LPCSTR)"Owner text");
    GlobalUnlock(memory);
    GlobalFree(memory);
    fclose(report);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
