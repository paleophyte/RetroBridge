"""Drive NetWare INSTALL menus via StuffKey + software selection tracking.

Highlight is not visible in StuffKey DUMP (no color attrs) and CopyFromScreenMemory
on Install has hung/abended this 3.12 lab — so we track the cursor ourselves and
confirm with DUMP text after navigation.

IMPORTANT: each StuffKey LOAD/UNLOAD can leave zombies. On this box, rapid
per-key LOAD STUFFKEY cycles abend TCP/IP with "how many zombies?". Always batch
keystrokes into one L.SK and wait for unload before the next run.
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from agent_client import AgentClient

MAIN_ITEMS = [
    "Disk Options",
    "Volume Options",
    "System Options",
    "Product Options",
    "Exit",
]

SYSTEM_ITEMS = [
    "Copy System and Public Files",
    "Create AUTOEXEC.NCF File",
    "Create STARTUP.NCF File",
    "Edit AUTOEXEC.NCF File",
    "Edit STARTUP.NCF File",
    "Return To Main Menu",
]

DISK_ITEMS = [
    "Partition Tables",
    "Mirroring",
    "Surface Test",
    "Return To Main Menu",
]

HOST = "10.102.10.197"
PORT = 2222
TOKEN = "REPLACE_WITH_UNIQUE_TOKEN"

BASE = Path(__file__).resolve().parent

# StuffKey token names (same map as llm_agent KEY).
_KEY_TOKENS = {
    "enter": "<CR>",
    "return": "<CR>",
    "esc": "<ESC>",
    "escape": "<ESC>",
    "tab": "<TAB>",
    "space": " ",
    "backspace": "<BS>",
    "bksp": "<BS>",
    "delete": "<DEL>",
    "del": "<DEL>",
    "home": "<HOME>",
    "end": "<END>",
    "pageup": "<PGUP>",
    "pgup": "<PGUP>",
    "pagedown": "<PGDN>",
    "pgdn": "<PGDN>",
    "up": "<UP>",
    "down": "<DN>",
    "left": "<LEFT>",
    "right": "<RIGHT>",
    "cend": "<CEND>",
    "ctrl-end": "<CEND>",
    "chome": "<CHOME>",
    "ctrl-home": "<CHOME>",
    "ins": "<INS>",
    "insert": "<INS>",
}

_SKIP = re.compile(
    r"Installation Options|NetWare Server Installation|Use the arrow keys|"
    r"Loadable Module|press <ENTER>|press \<ENTER\>|Exit Install|"
    r"Available System Options|Available Disk Options",
    re.I,
)


def _token(keyspec: str) -> str:
    k = keyspec.strip().lower()
    if k in _KEY_TOKENS:
        return _KEY_TOKENS[k]
    if len(k) == 1:
        if k in "<>\\":
            return "\\" + k
        return k
    raise ValueError(f"unknown key {keyspec!r}")


def run_stuffkey(c: AgentClient, body_lines: list[str], *, delay_ms: int = 80, settle: float = 2.5) -> None:
    """One StuffKey load for the whole body — never one LOAD per key."""
    script = "<SCREEN=Install Screen>\r\n" + "\r\n".join(body_lines) + "\r\n"
    ncf = f"LOAD STUFFKEY SYS:SYSTEM/L.SK /sr /d={delay_ms}\r\n"
    (BASE / "_menu_batch.sk").write_bytes(script.encode("ascii", "replace"))
    (BASE / "_menu_sk.ncf").write_bytes(ncf.encode("ascii"))
    c.exec("LOAD CLIBAUX")
    time.sleep(0.2)
    c.put(str(BASE / "_menu_batch.sk"), r"SYS:SYSTEM\L.SK")
    c.put(str(BASE / "_menu_sk.ncf"), r"SYS:SYSTEM\SK.NCF")
    c.exec("SK")
    # Let StuffKey finish and unload before next LOAD (zombie abend guard).
    time.sleep(settle)


def dump_install_text(c: AgentClient) -> str:
    """StuffKey DUMP → SYS:SYSTEM/MD.TXT. One LOAD only."""
    try:
        c.exec(r"DELETE SYS:SYSTEM\MD.TXT")
    except Exception:
        pass
    time.sleep(0.3)
    script = [
        "<LOG NEW=SYS:SYSTEM/MD.TXT>",
        "<DUMP>",
    ]
    run_stuffkey(c, script, delay_ms=50, settle=3.0)
    out = BASE / "_menu_md.txt"
    c.get(r"SYS:SYSTEM\MD.TXT", out)
    return out.read_text(encoding="cp437", errors="replace")


def parse_menu_items(dump: str) -> list[str]:
    """Pull selectable rows out of a StuffKey DUMP of Install."""
    # Prefer known lists when headers are present (overlay dumps are noisy).
    if re.search(r"Exit Install|Save AUTOEXEC", dump, re.I):
        return ["No", "Yes"]
    if re.search(r"Available System Options", dump, re.I):
        found = [x for x in SYSTEM_ITEMS if re.search(re.escape(x), dump, re.I)]
        return found or list(SYSTEM_ITEMS)
    if re.search(r"Available Disk Options", dump, re.I):
        found = [x for x in DISK_ITEMS if re.search(re.escape(x), dump, re.I)]
        return found or list(DISK_ITEMS)
    if re.search(r"Installation Options", dump, re.I) and not re.search(
        r"Available System Options|Available Disk Options", dump, re.I
    ):
        found = [x for x in MAIN_ITEMS if re.search(re.escape(x), dump, re.I)]
        if found:
            return found

    items: list[str] = []
    for raw in dump.splitlines():
        if _SKIP.search(raw):
            continue
        m = re.search(r"[│|]\s*([^│║|]+?)\s*[│║|]", raw)
        if not m:
            m2 = re.search(
                r"\b(Disk Options|Volume Options|System Options|Product Options|Exit|"
                r"Copy System and Public Files|Create AUTOEXEC\.NCF File|"
                r"Create STARTUP\.NCF File|Edit AUTOEXEC\.NCF File|"
                r"Edit STARTUP\.NCF File|Return To Main Menu|Yes|No)\b",
                raw,
            )
            if m2 and m2.group(1) not in items:
                items.append(m2.group(1))
            continue
        label = re.sub(r"\s+", " ", m.group(1).strip())
        if len(label) < 3 or label in items:
            continue
        if re.fullmatch(r"[\W_]+", label):
            continue
        if label in ("No", "Yes") and "Exit Install" not in dump:
            continue
        items.append(label)
    return items


def escape_stuffkey_text(text: str) -> str:
    """Escape StuffKey meta chars in literal text."""
    out: list[str] = []
    for ch in text:
        if ch in "<>\\":
            out.append("\\" + ch)
        else:
            out.append(ch)
    return "".join(out)


def print_dump_summary(dump: str, tracked: str) -> None:
    print("--- dump (menu-ish lines) ---")
    for ln in dump.splitlines():
        if any(
            x in ln
            for x in (
                "Options",
                "Exit",
                "Copy",
                "Create",
                "Edit",
                "AUTOEXEC",
                "Install",
                "Disk",
                "Volume",
                "System",
                "Product",
                "Partition",
                "Mirror",
                "Yes",
                "No",
            )
        ):
            safe = ln.encode("ascii", errors="replace").decode("ascii")
            print(safe)
    print(f"(tracked selection: {tracked})")


class InstallMenu:
    def __init__(self, client: AgentClient):
        self.c = client
        self.index = 0
        self.items: list[str] = list(MAIN_ITEMS)
        self.stack: list[tuple[list[str], int]] = []

    def ensure_loaded(self) -> None:
        names = [n for _, _, n in self.c.screens()]
        if not any("Install" in n for n in names):
            self.c.exec("LOAD INSTALL")
            time.sleep(2.5)
        names = [n for _, _, n in self.c.screens()]
        if not any("Install" in n for n in names):
            raise RuntimeError("Install Screen did not appear")

    def stuff(self, *keys: str, settle: float = 2.5) -> None:
        run_stuffkey(self.c, [_token(k) for k in keys], settle=settle)

    def refresh_items(self) -> list[str]:
        dump = dump_install_text(self.c)
        parsed = parse_menu_items(dump)
        if parsed:
            self.items = parsed
        print("items:", self.items)
        print_dump_summary(dump, self.selected())
        return self.items

    def selected(self) -> str:
        if not self.items:
            return "?"
        return self.items[max(0, min(self.index, len(self.items) - 1))]

    def soft_reset(self) -> None:
        """Get back to Installation Options without ESC-spamming Exit Install."""
        self.stack.clear()
        self.ensure_loaded()
        dump = dump_install_text(self.c)
        self.items = parse_menu_items(dump) or list(MAIN_ITEMS)
        print_dump_summary(dump, self.selected())

        # Editor left open from a prior run — ESC (then No if save prompt).
        if re.search(r"AUTOEXEC\.NCF File|Press <ESCAPE> when done", dump, re.I):
            print("leave AUTOEXEC editor via ESC")
            self.stuff("esc", settle=2.5)
            dump = dump_install_text(self.c)
            print_dump_summary(dump, self.selected())
            if re.search(r"\bYes\b|\bNo\b|Save", dump, re.I):
                print("abandon save (No)")
                self._answer_yes_no(save=False)
                dump = dump_install_text(self.c)
            self.items = parse_menu_items(dump) or list(MAIN_ITEMS)
            print_dump_summary(dump, self.selected())

        # Exit confirm: ESC cancels (safer than arrow+Enter on Yes).
        if re.search(r"Exit Install", dump, re.I):
            print("dismiss Exit Install via ESC")
            self.stuff("esc", settle=2.5)
            dump = dump_install_text(self.c)
            self.items = parse_menu_items(dump) or list(MAIN_ITEMS)
            print_dump_summary(dump, self.selected())

        # Submenu → one ESC back to main.
        if re.search(r"Available System Options|Available Disk Options", dump, re.I):
            print("leave submenu via ESC")
            self.stuff("esc", settle=2.5)
            dump = dump_install_text(self.c)
            self.items = parse_menu_items(dump) or list(MAIN_ITEMS)
            print_dump_summary(dump, self.selected())

        if not any("System Options" in x for x in self.items):
            self.items = list(MAIN_ITEMS)
        self.index = 0

    def _answer_yes_no(self, *, save: bool) -> None:
        """On a Yes/No dialog, pick Yes (save) or No (discard)."""
        dump = dump_install_text(self.c)
        items = parse_menu_items(dump)
        print_dump_summary(dump, "?")
        want = "Yes" if save else "No"
        if items and any(want.lower() == x.lower() for x in items):
            self.items = items
            # Dialogs often default to No (first) — home then goto.
            self.index = 0
            self.home()
            self.goto(want)
            self.stuff("enter", settle=2.8)
            return
        # Fallback: No is usually first (UP), Yes second (DN) on Exit Install.
        if save:
            self.stuff("down", "enter", settle=2.8)
        else:
            self.stuff("up", "enter", settle=2.8)

    def type_line(self, text: str, *, newline: bool = True, settle: float = 3.5) -> None:
        """Type literal text into Install via one StuffKey run."""
        body = [escape_stuffkey_text(text)]
        if newline:
            body.append("<CR>")
        print(f"type_line {text!r} newline={newline}")
        run_stuffkey(self.c, body, delay_ms=60, settle=settle)

    def enter_quiet(self) -> None:
        """Enter without DUMP refresh (editor / dialog transitions)."""
        print(f"enter_quiet on [{self.index}] {self.selected()}")
        self.stack.append((list(self.items), self.index))
        self.stuff("enter", settle=2.8)
        self.index = 0

    def open_edit_autoexec(self) -> None:
        if not any("System Options" in x for x in self.items):
            self.items = list(MAIN_ITEMS)
            self.index = 0
        self.home()
        self.goto("System Options")
        self.enter()
        if not any("Edit AUTOEXEC" in x for x in self.items):
            self.items = list(SYSTEM_ITEMS)
            self.index = 0
        self.home()
        self.goto("Edit AUTOEXEC")
        self.enter_quiet()
        time.sleep(1.0)

    def home(self) -> str:
        """Move to first item — one StuffKey with enough UPs."""
        n = len(self.items) + 2
        self.stuff(*(["up"] * n), settle=2.8)
        self.index = 0
        print(f"home -> [0] {self.selected()}")
        return self.selected()

    def down(self, n: int = 1) -> str:
        if n > 0:
            self.stuff(*(["down"] * n), settle=2.5)
            self.index = min(self.index + n, len(self.items) - 1)
        print(f"down x{n} -> [{self.index}] {self.selected()}")
        return self.selected()

    def up(self, n: int = 1) -> str:
        if n > 0:
            self.stuff(*(["up"] * n), settle=2.5)
            self.index = max(self.index - n, 0)
        print(f"up x{n} -> [{self.index}] {self.selected()}")
        return self.selected()

    def goto(self, name: str) -> str:
        target = None
        for i, it in enumerate(self.items):
            if name.lower() in it.lower():
                target = i
                break
        if target is None:
            raise RuntimeError(f"{name!r} not in {self.items}")
        if target > self.index:
            self.down(target - self.index)
        elif target < self.index:
            self.up(self.index - target)
        return self.selected()

    def enter(self) -> None:
        print(f"enter on [{self.index}] {self.selected()}")
        self.stack.append((list(self.items), self.index))
        self.stuff("enter", settle=2.8)
        self.index = 0
        self.refresh_items()

    def esc(self) -> None:
        print("esc")
        self.stuff("esc", settle=2.5)
        if self.stack:
            self.items, self.index = self.stack.pop()
            print(f"back to [{self.index}] {self.selected()} ({len(self.items)} items)")
        else:
            try:
                self.refresh_items()
                self.index = min(self.index, max(0, len(self.items) - 1))
            except Exception:
                pass


def smoke_system_options(c: AgentClient) -> int:
    menu = InstallMenu(c)
    menu.soft_reset()

    print("=== main menu ===")
    if not any("System Options" in x for x in menu.items):
        menu.items = list(MAIN_ITEMS)
    menu.home()
    menu.goto("System Options")

    print("=== enter System Options ===")
    menu.enter()

    print("=== leave submenu ===")
    menu.esc()
    menu.refresh_items()

    print("done. screens:", c.screens())
    print("tracked:", menu.selected(), "stack depth", len(menu.stack))
    return 0


def smoke_edit_autoexec(c: AgentClient) -> int:
    """System Options → Edit AUTOEXEC.NCF → PGDN to bottom → ESC out (no save)."""
    menu = InstallMenu(c)
    menu.soft_reset()
    menu.open_edit_autoexec()

    print("=== PGDN toward end of AUTOEXEC ===")
    menu.stuff("pgdn", "pgdn", "pgdn", "end", settle=3.0)
    dump = dump_install_text(c)
    print_dump_summary(dump, menu.selected())
    (BASE / "_edit_autoexec_dump.txt").write_text(dump, encoding="utf-8")
    if re.search(r"LLMAGENT|CLIBAUX", dump, re.I):
        print("CONFIRMED: agent load lines visible in editor DUMP")
    else:
        print("NOTE: LLMAGENT/CLIBAUX not in this DUMP window (may still be on disk)")

    print("=== esc out (editor then menus) ===")
    menu.stuff("esc", settle=2.5)
    menu.stuff("esc", settle=2.5)
    if menu.stack:
        menu.stack.clear()
    menu.refresh_items()
    print("done. screens:", c.screens())
    return 0


REM_MARKER = "REM agent-install-edit ok"


def smoke_write_autoexec(c: AgentClient) -> int:
    """Insert a REM line via INSTALL editor, save, verify on disk."""
    before = BASE / "_autoexec_before.txt"
    after = BASE / "_autoexec_after.txt"
    c.get(r"SYS:SYSTEM\AUTOEXEC.NCF", before)
    text0 = before.read_text(encoding="cp437", errors="replace")
    print("--- AUTOEXEC before ---")
    print(text0)
    if not text0.lstrip().lower().startswith("file server name"):
        print("REFUSING: AUTOEXEC does not start with 'file server name' — fix manually")
        return 2

    menu = InstallMenu(c)
    menu.soft_reset()
    menu.open_edit_autoexec()

    # CEND is unreliable in NWSNUT; page to bottom and END of line instead.
    print("=== page to end of AUTOEXEC (PGDN + END) ===")
    menu.stuff("pgdn", "pgdn", "pgdn", "pgdn", "end", settle=3.2)
    dump = dump_install_text(c)
    print_dump_summary(dump, "editor")
    if not re.search(r"LOAD\s+LLMAGENT", dump, re.I):
        print("REFUSING: LLMAGENT not visible — cursor likely not at EOF; abort ESC/No")
        menu.stuff("esc", settle=2.5)
        dump = dump_install_text(c)
        if re.search(r"\bYes\b|\bNo\b|Save", dump, re.I):
            menu._answer_yes_no(save=False)
        return 3

    print("=== new line after last line, then type REM ===")
    # Ensure insert mode, end-of-line, Enter, type, Enter.
    menu.stuff("end", "ins", settle=2.5)
    menu.stuff("enter", settle=2.5)
    menu.type_line(REM_MARKER, newline=True, settle=4.0)

    dump = dump_install_text(c)
    print_dump_summary(dump, "editor")
    (BASE / "_edit_autoexec_dump.txt").write_text(dump, encoding="utf-8")
    if "agent-install-edit" not in dump:
        print("WARN: REM not in DUMP — abort without save")
        menu.stuff("esc", settle=2.5)
        dump = dump_install_text(c)
        if re.search(r"\bYes\b|\bNo\b|Save", dump, re.I):
            menu._answer_yes_no(save=False)
        return 4
    if re.search(r"ile server name", dump) and not re.search(r"file server name", dump, re.I):
        print("REFUSING: first line looks corrupted in DUMP — discard")
        menu.stuff("esc", settle=2.5)
        dump = dump_install_text(c)
        if re.search(r"\bYes\b|\bNo\b|Save", dump, re.I):
            menu._answer_yes_no(save=False)
        return 5
    print("CONFIRMED: REM visible in editor DUMP")

    print("=== ESC editor + Save Yes ===")
    menu.stuff("esc", settle=2.8)
    dump = dump_install_text(c)
    print_dump_summary(dump, "?")
    (BASE / "_save_prompt_dump.txt").write_text(dump, encoding="utf-8")

    if re.search(r"Save AUTOEXEC|Save.*NCF|\bYes\b", dump, re.I):
        menu._answer_yes_no(save=True)
    else:
        print("No save prompt — checking disk anyway")

    for _ in range(4):
        dump = dump_install_text(c)
        if re.search(r"Installation Options", dump, re.I) and not re.search(
            r"Available System Options|AUTOEXEC\.NCF File|Save AUTOEXEC", dump, re.I
        ):
            break
        if re.search(r"Exit Install", dump, re.I):
            menu.stuff("esc", settle=2.5)
            continue
        if re.search(r"Save AUTOEXEC|\bYes\b|\bNo\b", dump, re.I):
            menu._answer_yes_no(save=True)
            continue
        print("esc toward main")
        menu.stuff("esc", settle=2.5)

    menu.stack.clear()
    c.get(r"SYS:SYSTEM\AUTOEXEC.NCF", after)
    text = after.read_text(encoding="cp437", errors="replace")
    print("--- AUTOEXEC after ---")
    print(text)

    ok_rem = "agent-install-edit" in text
    ok_head = text.lstrip().lower().startswith("file server name")
    ok_agent = re.search(r"LOAD\s+LLMAGENT", text, re.I) is not None

    if ok_rem and ok_head and ok_agent:
        print("SUCCESS: REM persisted; header + LLMAGENT intact (left in place)")
        return 0

    print(
        f"FAIL: rem={ok_rem} head={ok_head} agent={ok_agent} — restoring backup via PUT"
    )
    c.put(str(before), r"SYS:SYSTEM\AUTOEXEC.NCF")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--token", default=TOKEN)
    ap.add_argument(
        "--path",
        default="system",
        choices=("system", "main", "disk", "edit-autoexec", "write-autoexec"),
        help="smoke path to run",
    )
    args = ap.parse_args()

    c = AgentClient(args.host, args.port, args.token, timeout=120)
    print("build", c.sysinfo().get("agent_build"))

    if args.path == "system":
        return smoke_system_options(c)
    if args.path == "edit-autoexec":
        return smoke_edit_autoexec(c)
    if args.path == "write-autoexec":
        return smoke_write_autoexec(c)
    if args.path == "disk":
        menu = InstallMenu(c)
        menu.soft_reset()
        menu.home()
        menu.goto("Disk Options")
        menu.enter()
        menu.esc()
        return 0

    menu = InstallMenu(c)
    menu.soft_reset()
    menu.home()
    menu.down(2)
    menu.refresh_items()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
