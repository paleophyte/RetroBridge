"""Verify and stage locally supplied SDK files; never download vendor material."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "vendor-manifest.json"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(component, source_root=None, destination=None, check=False):
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    files = [f for f in manifest["files"]
             if f["component"] == component and f["required_for_build"]]
    if not files:
        raise ValueError("No build inputs recorded for component: " + component)
    target = Path(destination).resolve() if destination else ROOT / ".deps" / component
    source = Path(source_root).resolve() if source_root else None
    if not check and source is None:
        raise ValueError("Supply --source-root, or use --check for installed files")
    planned = []
    # Validate the entire set before creating directories or copying any files.
    for entry in files:
        dest = target / entry["install_name"]
        expected = entry["sha256"]
        if check:
            candidates = [dest]
        else:
            candidates = list(dict.fromkeys([
                source / entry["former_path"], source / entry["package_path"]]))
            candidates = [p for p in candidates if p.is_file()]
            if not candidates:
                raise ValueError("Missing input: " + entry["package_path"])
        for candidate in candidates:
            if not candidate.is_file() or digest(candidate) != expected:
                raise ValueError("Missing or unexpected SHA-256: " + str(candidate))
        if not check:
            if dest.exists() and (not dest.is_file() or digest(dest) != expected):
                raise ValueError("Refusing to overwrite conflicting destination: " + str(dest))
            planned.append((candidates[0], dest, expected))
    for src, dest, expected in planned:
        data = src.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Input changed during preparation: " + str(src))
        dest.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation avoids replacing files written since preflight.
        if dest.exists():
            if digest(dest) != expected:
                raise ValueError("Destination changed during preparation: " + str(dest))
            continue
        with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as tmp:
            temporary = Path(tmp.name)
            tmp.write(data)
        try:
            # A hard link installs a complete file without overwriting a raced destination.
            os.link(temporary, dest)
        finally:
            temporary.unlink()
    return len(files), target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", choices=("mac-sdk", "netware-sdk"), required=True)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        count, target = prepare(args.component, args.source_root, args.destination, args.check)
    except (OSError, ValueError) as exc:
        parser.exit(1, "Dependency setup failed: " + str(exc) + "\nSee THIRD_PARTY.md.\n")
    print(f"Verified {count} {args.component} inputs in {target}")


if __name__ == "__main__":
    main()
