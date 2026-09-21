"""Build every native agent into private, non-bootable ISO 9660/Joliet lab media."""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
VOLUME = "RETROBRG"
# Explicit allowlist: never gather executables, INIs, SDKs, or media by glob.
ARTIFACTS = {
    "WIN32": [("agent-win32/llm_agent.exe", "LLMAGENT.EXE"), ("agent-win32/update.exe", "UPDATE.EXE")],
    "DOS": [("agent-dos/llm_agent.exe", "LLMAGENT.EXE")],
    "WIN16": [("agent-win16/llm_agent.exe", "LLMAGENT.EXE"), ("agent-win16/redir.exe", "REDIR.EXE"), ("agent-win16/restart.exe", "RESTART.EXE"), ("agent-win16/jobrun.exe", "JOBRUN.EXE")],
    "OS2": [("agent-os2/llm_agent.exe", "LLMAGENT.EXE"), ("agent-os2/update.exe", "UPDATE.EXE"), ("agent-os2/reboot.exe", "REBOOT.EXE"), ("agent-os2/jobrun.exe", "JOBRUN.EXE")],
    "OS213": [("agent-os2-13/llm_agent.exe", "LLMAGENT.EXE"), ("agent-os2-13/update.exe", "UPDATE.EXE"), ("agent-os2-13/ioreset.exe", "IORESET.EXE"), ("agent-os2-13/ioseg.dll", "IOSEG.DLL"), ("agent-os2-13/jobrun.exe", "JOBRUN.EXE")],
    "NW312": [("agent-netware/LLMAGENT.NLM", "LLMAGENT.NLM"), ("agent-netware/UPDATE.NLM", "UPDATE.NLM")],
    "NW411": [("agent-netware/nw4/LLMAGENT.NLM", "LLMAGENT.NLM"), ("agent-netware/UPDATE.NLM", "UPDATE.NLM")],
    "MAC7": [("agent-mac-system7/build/llm_agent.bin", "AGENT.BIN"), ("agent-mac-system7/build/llm_updater.bin", "UPDATER.BIN")],
}
EXAMPLES = {
    "WIN32": "agent-win32/llm_agent.ini.example",
    "DOS": "agent-dos/LLMAGENT.INI.example",
    "WIN16": "agent-win16/LLMAGENT.INI.example",
    "OS2": "agent-os2/LLMAGENT.INI.example",
    "OS213": "agent-os2-13/LLMAGENT.INI.example",
    "NW312": "agent-netware/LLMAGENT.INI.example",
    "NW411": "agent-netware/LLMAGENT.INI.example",
    "MAC7": "agent-mac-system7/LLMAGENT.INI.example",
}
GUIDES = {
    "WIN32": """Windows 95/98/ME and NT-family agent.
Copy LLMAGENT.EXE to a writable installation folder as llm_agent.exe.
The ISO uses a short name; the installed long name is required by the updater.
Copy UPDATE.EXE beside it. Copy CONFIG.INI as llm_agent.ini and replace token=.
NT4/2000/XP: llm_agent.exe --install, then NET START LLMAgent.
Windows 9x: --install registers RunServices; --run starts immediately.
Windows 7 desktop control: run --run in the logged-in user's session.
The NT updater expects a service; interactive Win7 updates need separate restart.
Win95 needs Winsock 2. The linked CRT is not certified for a physical 486/Pentium.
""",
    "DOS": """FreeDOS / MS-DOS agent; foreground application, not a TSR.
Copy LLMAGENT.EXE and LLMSTART.BAT to a writable 8.3 folder, e.g. C:\\LLMAGENT.
Copy CONFIG.INI as LLMAGENT.INI, set a unique token, and configure WATTCP.CFG.
Supply your own packet driver and load it before the agent (not on this CD).
CALL C:\\LLMAGENT\\LLMSTART.BAT offers Agent/Console with a timed default.
CHOICE must be on PATH. Local Ctrl+C exits the idle agent to the console.
EXEC blocks networking while its child runs. No jobs or built-in self-update.
""",
    "WIN16": """Windows for Workgroups 3.11 with a working Winsock 1.1 stack.
Copy LLMAGENT.EXE, REDIR.EXE, RESTART.EXE, and JOBRUN.EXE to a writable
8.3 folder without spaces, e.g. C:\\LLMWIN. Copy CONFIG.INI as LLMAGENT.INI
and set a unique token. Start LLMAGENT.EXE from within Windows.
REDIR and JOBRUN are DOS helpers: EXEC/jobs are for DOS commands.
Use EXECDETACH for native Windows applications. SHUTDOWN exits to DOS.
Use the staged Win16 update tool; do not overwrite the running executable.
""",
    "OS2": """32-bit OS/2 2.x and later tested kernels; SO32DLL/TCP32DLL required.
Copy all four EXEs to a writable 8.3 directory without spaces.
Copy CONFIG.INI as LLMAGENT.INI and set a unique token.
Launch from a normal OS/2 session or a WPS Startup-folder object.
Do not start with CONFIG.SYS RUN=; that can break session/helper launches.
JOBRUN is required for jobs, REBOOT for reboot, UPDATE for staged replacement.
""",
    "OS213": """16-bit OS/2 1.3; TCPIPDLL.DLL must be on LIBPATH.
Copy all EXEs and IOSEG.DLL together to a writable 8.3 folder without spaces.
Copy CONFIG.INI as LLMAGENT.INI; set token and host to this guest's LAN address.
The host field is used by UPDATE for SELFEXIT, not the controller address.
Start LLMAGENT.EXE from a normal OS/2 session or STARTUP.CMD.
REBOOT requires IOPL=YES plus IORESET.EXE/IOSEG.DLL. JOBRUN is for jobs.
Inspect update verification/logs; an intermittent rename failure is documented.
""",
    "MAC7": """68k System 7; working MacTCP, CD driver and ISO 9660 File Access required.
Copy AGENT.BIN and UPDATER.BIN to a writable HFS volume. Decode BOTH using
an existing MacBinary-capable utility (e.g. StuffIt Expander, not supplied).
Decoding restores the embedded application names llm_agent and llm_updater,
including resource forks; renaming .BIN to an application does not decode it.
Keep both applications together. Copy CONFIG.INI as LLMAGENT.INI beside them
and replace token=. Launch llm_agent; use an alias in Startup Items if desired.
UPDATE replaces the agent only; the companion is installed separately.
This CD is ISO 9660/Joliet, not a bootable or HFS-hybrid Macintosh system disk.
""",
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def text_file(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii"))


def run(argv, cwd, env, log: Path, *, data=None, timeout=900):
    with log.open("ab") as stream:
        result = subprocess.run(argv, cwd=cwd, env=env, input=data, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Build command failed ({result.returncode}); see {log}")


def snapshot(root: Path, dest: Path) -> dict:
    names = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode("utf-8").split("\0")
    h = hashlib.sha256()
    for name in sorted(filter(None, names)):
        source = root / name
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"Tracked input missing or symlink: {name}")
        data = source.read_bytes()
        h.update(name.encode("utf-8") + b"\0" + hashlib.sha256(data).digest())
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return {
        "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root).decode().strip(),
        "working_tree_modified": bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=root)),
        "tracked_snapshot_sha256": h.hexdigest(),
    }


def copy_dependency(source: Path, dest: Path, records: dict, label: str):
    if not source.is_file():
        raise ValueError(f"Missing local build dependency: {source}")
    data = source.read_bytes()
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    records[label] = sha(data)


def prepare(config, root, source):
    # Only specifically named private build inputs enter the isolated build tree.
    records = {}
    manifest = json.loads((source / "docs/vendor-manifest.json").read_text())
    for component, setting in (("mac-sdk", "mac_sdk"), ("netware-sdk", "netware_sdk")):
        base = Path(config.get(setting, str(root / ".deps" / component))).expanduser().resolve()
        for entry in manifest["files"]:
            if entry["component"] == component and entry["required_for_build"]:
                name = entry["install_name"]
                copy_dependency(base / name, source / ".deps" / component / name, records, component + "/" + name)
                if records[component + "/" + name] != entry["sha256"]:
                    raise ValueError(f"Unexpected SDK hash: {component}/{name}")
    for folder, dlls in (("agent-os2", ("SO32DLL.DLL", "TCP32DLL.DLL")),
                         ("agent-os2-13", ("TCPIPDLL.DLL",))):
        for dll in dlls:
            copy_dependency(root / folder / "vendor" / dll, source / folder / "vendor" / dll, records, folder + "/" + dll)
    return records


def build_windows(config, source, work):
    env = os.environ.copy()
    mingw = Path(config.get("mingw_bin", "C:/msys64/mingw32/bin"))
    make = Path(config.get("make", "C:/msys64/usr/bin/make.exe"))
    watt = Path(config["watt32"]).expanduser().resolve()
    for path in (Path("C:/watcom/owsetenv.bat"), make, mingw / "i686-w64-mingw32-gcc.exe", watt / "lib/wattcpwl.lib"):
        if not path.is_file():
            raise ValueError(f"Missing toolchain input: {path}")
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), str(mingw), str(make.parent), env.get("PATH", "")])
    env["WATT32"] = str(watt)
    env["NLM_SDK_DIR"] = str(source / ".deps/netware-sdk")
    steps = [("agent-win32", [str(make), "-B", "all"])]
    for folder, batches in (("agent-dos", ["build.bat"]),
                            ("agent-win16", ["build.bat", "build_redir.bat", "build_restart.bat"]),
                            ("agent-os2", ["build.bat"]), ("agent-os2-13", ["build.bat"]),
                            ("agent-netware", ["build.bat"]), ("agent-netware/nw4", ["build.bat"])):
        steps.extend((folder, ["cmd.exe", "/d", "/c", batch]) for batch in batches)
    for index, (folder, argv) in enumerate(steps):
        print(f"Building {folder}: {argv[-1]}", flush=True)
        run(argv, source / folder, env, work / f"build-{index:02d}.log")
    return {"watt32_library_sha256": sha((watt / "lib/wattcpwl.lib").read_bytes()),
            "mingw_compiler": subprocess.check_output([str(mingw / "i686-w64-mingw32-gcc.exe"), "--version"], env=env).decode(errors="replace").splitlines()[0]}


def ssh_args(config, program):
    host, user = config["host"], config["user"]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", host) or not re.fullmatch(r"[A-Za-z0-9_.-]+", user) or host.startswith("-"):
        raise ValueError("Use a plain SSH hostname/IP and user (no shell syntax)")
    args = [program, "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=15"]
    if config.get("key"):
        args += ["-i", str(Path(config["key"]).expanduser().resolve())]
    if config.get("known_hosts"):
        args += ["-o", "UserKnownHostsFile=" + str(Path(config["known_hosts"]).expanduser().resolve())]
    port = int(config.get("port", 22))
    if not 1 <= port <= 65535:
        raise ValueError("Invalid SSH port")
    args += ["-p" if program == "ssh" else "-P", str(port)]
    return args, user + "@" + host


def build_mac(config, source, work):
    remote = "/tmp/retrobridge-iso-" + uuid.uuid4().hex
    files = {}
    for folder in ("agent-mac-system7", "common", ".deps/mac-sdk"):
        for p in (source / folder).rglob("*"):
            if p.is_file() and p.suffix in {".c", ".h", ".r", ".txt"}:
                files[p.relative_to(source).as_posix()] = base64.b64encode(p.read_bytes()).decode()
    for name in ("tools/prepare_dependencies.py", "docs/vendor-manifest.json"):
        files[name] = base64.b64encode((source / name).read_bytes()).decode()
    # Data is passed to Python on stdin, never interpolated into a remote shell command.
    script = "import base64,json,os,subprocess\nfrom pathlib import Path\n"
    script += "root=Path(" + repr(remote) + "); root.mkdir()\n"
    script += "files=json.loads(" + repr(json.dumps(files)) + ")\n"
    script += "for name,data in files.items():\n p=root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_bytes(base64.b64decode(data))\n"
    script += "subprocess.run(['cmake','-S',str(root/'agent-mac-system7'),'-B',str(root/'build'),'-DCMAKE_TOOLCHAIN_FILE='+" + repr(config["toolchain"]) + "],check=True)\n"
    script += "subprocess.run(['cmake','--build',str(root/'build'),'--parallel','2'],check=True)\n"
    args, host = ssh_args(config, "ssh")
    print(f"Building Mac System 7 on configured SSH host; workspace {remote}", flush=True)
    run(args + [host, "python3 -"], source, os.environ.copy(), work / "build-mac.log", data=script.encode())
    output = source / "agent-mac-system7/build"
    output.mkdir()
    scp, host = ssh_args(config, "scp")
    for name in ("llm_agent.bin", "llm_updater.bin"):
        run(scp + [host + ":" + remote + "/build/" + name, str(output / name)], source,
            os.environ.copy(), work / "build-mac.log", timeout=120)
    # Delete only this invocation's generated remote directory after collecting both outputs.
    cleanup = "from pathlib import Path\nimport shutil\np=Path(" + repr(remote) + ")\nassert p.parent==Path('/tmp') and p.name.startswith('retrobridge-iso-')\nshutil.rmtree(p)\n"
    run(args + [host, "python3 -"], source, os.environ.copy(), work / "build-mac.log", data=cleanup.encode(), timeout=60)


def check_artifact(path: Path):
    data = path.read_bytes()
    if len(data) < 128:
        raise ValueError(f"Empty/truncated build artifact: {path}")
    if path.suffix.lower() in {".exe", ".dll"} and data[:2] != b"MZ":
        raise ValueError(f"Not a DOS/Windows/OS2 executable: {path}")
    if path.suffix.lower() == ".nlm" and not data.startswith(b"NetWare Loadable Module"):
        raise ValueError(f"Not a NetWare NLM: {path}")
    if path.suffix.lower() == ".bin":
        size = 128 + ((int.from_bytes(data[83:87], 'big') + 127) // 128) * 128 + ((int.from_bytes(data[87:91], 'big') + 127) // 128) * 128
        if data[0] != 0 or not 1 <= data[1] <= 63 or data[65:69] != b"APPL" or size != len(data):
            raise ValueError(f"Not a complete classic MacBinary application: {path}")


def stage_media(source: Path, stage: Path, info: dict):
    for folder, entries in ARTIFACTS.items():
        (stage / folder).mkdir(parents=True)
        for src, name in entries:
            check_artifact(source / src)
            shutil.copyfile(source / src, stage / folder / name)
        example = (source / EXAMPLES[folder]).read_text(encoding="ascii")
        if "token=REPLACE_WITH_UNIQUE_TOKEN" not in example or len(re.findall(r"^token=", example, re.M)) != 1:
            raise ValueError(f"Not a placeholder configuration: {EXAMPLES[folder]}")
        text_file(stage / folder / "CONFIG.INI", example)
        if folder.startswith("NW"):
            guide = f"""NetWare {('3.12' if folder == 'NW312' else '4.11')} native NLM and shared protocol-2 UPDATE.NLM.
Preferred: copy LLMAGENT.NLM and UPDATE.NLM to SYS:SYSTEM using your usual
NetWare client/file utility. Create SYS:SYSTEM\\LLMAGENT.INI from CONFIG.INI
with a unique token. Keep existing configuration when updating a machine.
With TCP/IP and CLIB available: LOAD SYS:SYSTEM\\LLMAGENT.NLM

Direct CD load, only when no other LLMAGENT instance is loaded:
1. Load your controller/CD driver and CDROM.NLM; mount this ISO as RETROBRG.
   Typical console sequence after the hardware driver is ready:
     LOAD CDROM
     CD DEVICE LIST
     CD MOUNT RETROBRG
2. Create the private config in SYS:SYSTEM first; CONFIG.INI is only an example.
3. LOAD RETROBRG:\\{folder}\\LLMAGENT.NLM
   Or run RETROBRG:\\{folder}\\CDLOAD.NCF after completing steps 1-2.
The CD does not supply CDROM.NLM, disk drivers, CLIB, CLIBAUX, or StuffKey.

The direct-CD copy cannot self-update: updates require installation in SYS:SYSTEM.
Use a normal controlled server shutdown/restart to stop a CD-loaded agent.
UNLOAD LLMAGENT has abended on 3.12; do not use it as a routine stop path.
Do not load UPDATE.NLM just to start the agent: it is a replacement helper.
Optional INSTALL-menu input needs separately supplied STUFFKEY/CLIBAUX.
"""
            text_file(stage / folder / "CDLOAD.NCF", f"REM Configure SYS:SYSTEM\\LLMAGENT.INI and stop other instances first.\nLOAD {VOLUME}:\\{folder}\\LLMAGENT.NLM\n")
        else:
            guide = GUIDES[folder]
        text_file(stage / folder / "README.TXT", guide + "\nUse an isolated lab network; the protocol/token is cleartext.\n")
    text_file(stage / "DOS/LLMSTART.BAT", (source / "agent-dos/LLMSTART.BAT").read_text(encoding="ascii"))
    text_file(stage / "DOS/WATTCP.CFG", (source / "agent-dos/wattcp.cfg.example").read_text(encoding="ascii"))
    text_file(stage / "LICENSE.TXT", (source / "LICENSE").read_text(encoding="ascii"))
    notices = json.loads((source / "docs/binary-provenance.json").read_text())["notices"]
    notice_index = []
    (stage / "NOTICES").mkdir()
    for i, entry in enumerate(notices, 1):
        data = (source / entry["path"]).read_bytes()
        if sha(data) != entry["sha256"]:
            raise ValueError("Notice changed: " + entry["path"])
        name = f"N{i:02d}.TXT"
        (stage / "NOTICES" / name).write_bytes(data)
        notice_index.append(name + " = " + Path(entry["path"]).name)
    text_file(stage / "NOTICES/README.TXT", "Third-party notices (original terms retained):\n" + "\n".join(notice_index) + "\n\nCollection alone does not complete the binary redistribution review.\nSee DOCS/BINARY.TXT and DOCS/PROVEN.TXT (UTF-8 Markdown).\n")
    # Fixed documentation allowlist, with short media names and exact source bytes.
    for src, name in (("THIRD_PARTY.md", "PROVEN.TXT"), ("docs/BINARY_RELEASE.md", "BINARY.TXT"),
                      ("docs/MCP_COVERAGE.md", "MCP.TXT"), ("docs/NETWORK_TIMEOUTS.md", "NETWORK.TXT")):
        (stage / "DOCS").mkdir(exist_ok=True)
        shutil.copyfile(source / src, stage / "DOCS" / name)
    text_file(stage / "README.TXT", """RetroBridge - private lab deployment CD (non-bootable), volume RETROBRG

Choose WIN32, WIN16, DOS, OS2, OS213, NW312, NW411, or MAC7.
Each folder has a README.TXT and CONFIG.INI example. Copy onto a writable
local volume and configure a unique token before starting an agent.
Preserve existing configurations; CONFIG.INI is never a real credential.
WIN32 needs the installed name llm_agent.exe / llm_agent.ini (see its README).
MAC7 contains MacBinary applications: decode them to preserve resource forks.
NetWare has native NLMs and CDLOAD.NCF; CD driver/mount support must exist.
This is not a boot disk and does not install drivers, OS files, or SDKs.

CHECKSUM.TXT lists SHA-256 hashes; BUILD.JSN identifies source and artifacts.
NOTICES preserves dependency terms. DOCS holds selected UTF-8 Markdown source
references (their repository-relative links refer to the source checkout).
This locally built ISO is NOT a cleared public binary release. See the
source repository's docs/BINARY_RELEASE.md before redistributing binaries.
https://github.com/paleophyte/RetroBridge
""")
    info = dict(info)
    info["files"] = {p.relative_to(stage).as_posix(): {"size": p.stat().st_size, "sha256": sha(p.read_bytes())}
                     for p in sorted(stage.rglob("*")) if p.is_file()}
    text_file(stage / "BUILD.JSN", json.dumps(info, indent=2, ensure_ascii=True) + "\n")
    text_file(stage / "CHECKSUM.TXT", "".join(sha(p.read_bytes()) + "  " + p.relative_to(stage).as_posix() + "\n" for p in sorted(stage.rglob("*")) if p.is_file()))


def validate_media_names(stage: Path):
    for p in stage.rglob("*"):
        if p.is_symlink() or not (p.is_file() or p.is_dir()):
            raise ValueError(f"Unexpected media entry: {p}")
        pattern = r"[A-Z0-9_]{1,8}\.[A-Z0-9_]{1,3}" if p.is_file() else r"[A-Z0-9_]{1,8}"
        if not re.fullmatch(pattern, p.name):
            raise ValueError(f"Not ISO 9660 level-1 / 8.3: {p.name}")


def create_iso(stage: Path, output: Path):
    import pycdlib
    validate_media_names(stage)
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident=VOLUME, joliet=3)
    try:
        for p in sorted(stage.rglob("*"), key=lambda p: (len(p.parts), p.as_posix())):
            name = "/" + p.relative_to(stage).as_posix()
            if p.is_dir():
                iso.add_directory(iso_path=name, joliet_path=name)
            else:
                iso.add_file(str(p), iso_path=name + ";1", joliet_path=name)
        iso.write(str(output))
    finally:
        iso.close()
    verify_iso(stage, output)


def verify_iso(stage: Path, output: Path):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(str(output))
    try:
        expected = {"/" + p.relative_to(stage).as_posix(): p for p in stage.rglob("*") if p.is_file()}
        for mode in ("iso_path", "joliet_path"):
            actual = set()
            for directory, _, names in iso.walk(**{mode: "/"}):
                for name in names:
                    full = directory.rstrip("/") + "/" + name
                    actual.add(full.removesuffix(";1") if mode == "iso_path" else full)
            if actual != set(expected):
                raise ValueError(f"ISO file inventory mismatch ({mode})")
            for name, p in expected.items():
                data = io.BytesIO()
                iso.get_file_from_iso_fp(data, **{mode: name + ";1" if mode == "iso_path" else name})
                if sha(data.getvalue()) != sha(p.read_bytes()):
                    raise ValueError(f"ISO readback mismatch: {name} ({mode})")
    finally:
        iso.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="Local JSON toolchain/SSH settings (see build_iso.example.json)")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/RetroBridge.iso")
    parser.add_argument("--force", action="store_true", help="Atomically replace an existing ISO only after all builds/readbacks succeed")
    args = parser.parse_args(argv)
    try:
        if os.name != "nt":
            raise ValueError("This orchestrator currently requires Windows; Mac compilation runs on Linux over SSH")
        import pycdlib  # fail before compiling if the packaging dependency is missing
        config = json.loads(args.config.read_text(encoding="utf-8-sig"))
        output = args.output.expanduser().resolve()
        if output.exists() and (not args.force or not output.is_file()):
            raise ValueError("Output exists; choose another path or use --force")
        if output.suffix.lower() != ".iso":
            raise ValueError("Output must have an .iso suffix")
        output.parent.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="retrobridge-iso-"))
        print(f"Build workspace/logs: {work}", flush=True)
        source = work / "source"
        info = snapshot(ROOT, source)
        info["built_at_utc"] = datetime.now(timezone.utc).isoformat()
        info["purpose"] = "private lab media; binary redistribution review remains separate"
        info["dependencies"] = prepare(config, ROOT, source)
        info["build_environment"] = build_windows(config, source, work)
        build_mac(config["mac"], source, work)
        stage = work / "media"
        stage.mkdir()
        stage_media(source, stage, info)
        fd, name = tempfile.mkstemp(prefix=".retrobridge-", suffix=".iso", dir=output.parent)
        os.close(fd)
        temporary = Path(name)
        try:
            create_iso(stage, temporary)
            if args.force:
                os.replace(temporary, output)
            else:
                # Windows rename refuses an existing destination, including a raced creator.
                temporary.rename(output)
        finally:
            if temporary.exists():
                temporary.unlink()
        print(f"Created {output} ({output.stat().st_size} bytes)\nSHA256 {sha(output.read_bytes())}")
        print("All eight platform directories built; every ISO/Joliet file read back and checked.")
        return 0
    except (OSError, ValueError, RuntimeError, ImportError, subprocess.SubprocessError, KeyError) as exc:
        print(f"ISO build failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
