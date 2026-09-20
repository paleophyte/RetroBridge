"""Dependency preparation rejects bad inputs without overwriting local files."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_dependencies", ROOT / "tools/prepare_dependencies.py")
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)


class DependencyTests(unittest.TestCase):
    def test_package_and_preservation_layouts_and_repeat_check(self):
        for archived in (False, True):
            with self.subTest(archived=archived), tempfile.TemporaryDirectory() as d:
                root = Path(d)
                source, target = root / "source with spaces", root / "target with spaces"
                entries = []
                for name in ("one.h", "two.h"):
                    data = (name + "\r\n").encode()
                    entry = dict(component="mac-sdk", required_for_build=True,
                                 former_path="old/vendor/" + name, package_path=name,
                                 install_name=name, sha256=hashlib.sha256(data).hexdigest())
                    path = source / (entry["former_path"] if archived else name)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                    entries.append(entry)
                manifest = root / "manifest.json"
                manifest.write_text(json.dumps({"files": entries}))
                with patch.object(deps, "MANIFEST", manifest):
                    self.assertEqual(deps.prepare("mac-sdk", source, target)[0], 2)
                    self.assertEqual(deps.prepare("mac-sdk", source, target)[0], 2)
                    self.assertEqual(deps.prepare("mac-sdk", destination=target, check=True)[0], 2)
                    self.assertEqual((target / "one.h").read_bytes(), b"one.h\r\n")
                    (target / "one.h").write_bytes(b"local modification")
                    with self.assertRaisesRegex(ValueError, "conflicting destination"):
                        deps.prepare("mac-sdk", source, target)
                    with self.assertRaisesRegex(ValueError, "SHA-256"):
                        deps.prepare("mac-sdk", destination=target, check=True)
                    self.assertEqual((target / "one.h").read_bytes(), b"local modification")

    def test_all_inputs_checked_before_writing(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source, target = root / "source", root / "destination"
            source.mkdir()
            (source / "one.h").write_bytes(b"good")
            entries = [dict(component="mac-sdk", required_for_build=True,
                            former_path="old/" + name, package_path=name,
                            install_name=name, sha256=hashlib.sha256(b"good").hexdigest())
                       for name in ("one.h", "two.h")]
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"files": entries}))
            with patch.object(deps, "MANIFEST", manifest):
                with self.assertRaisesRegex(ValueError, "Missing input"):
                    deps.prepare("mac-sdk", source, target)
                self.assertFalse(target.exists())
                (source / "two.h").write_bytes(b"wrong revision")
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    deps.prepare("mac-sdk", source, target)
                self.assertFalse(target.exists())
                with self.assertRaisesRegex(ValueError, "source-root"):
                    deps.prepare("mac-sdk", destination=target)


if __name__ == "__main__":
    unittest.main()
