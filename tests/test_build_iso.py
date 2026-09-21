"""Deployment media completeness, private-input exclusion, and ISO readback."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_iso", ROOT / "tools/build_iso.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class MediaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.stage = self.root / "media"
        # Public text inputs are real; synthetic binaries exercise packaging
        # without requiring every cross-compiler in the host test environment.
        for name in set(builder.EXAMPLES.values()) | {
            "LICENSE", "THIRD_PARTY.md", "docs/BINARY_RELEASE.md",
            "docs/MCP_COVERAGE.md", "docs/NETWORK_TIMEOUTS.md",
            "docs/binary-provenance.json", "agent-dos/LLMSTART.BAT",
            "agent-dos/wattcp.cfg.example",
        }:
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        notices = json.loads((ROOT / "docs/binary-provenance.json").read_text())["notices"]
        for entry in notices:
            target = self.source / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / entry["path"], target)
        for entries in builder.ARTIFACTS.values():
            for relative, _ in entries:
                target = self.source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.suffix.lower() == ".bin":
                    data = bytearray(384)
                    data[1:6] = b"\x04TEST"
                    data[65:69] = b"APPL"
                    data[83:87] = (4).to_bytes(4, "big")
                    data[87:91] = (4).to_bytes(4, "big")
                    data[128:132] = b"data"
                    data[256:260] = b"rsrc"
                else:
                    signature = b"NetWare Loadable Module" if target.suffix.lower() == ".nlm" else b"MZ"
                    data = signature + bytes(range(128))
                target.write_bytes(data)

    def stage_all(self):
        builder.stage_media(self.source, self.stage, {"test": True})

    def test_complete_media_excludes_private_configs_and_unlisted_payloads(self):
        for name in ("agent-win32/llm_agent.ini", "machines.ini", "unexpected.exe",
                     "agent-netware/vendor/private-sdk.obj"):
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"private sentinel never belongs on the CD")
        self.stage_all()
        builder.validate_media_names(self.stage)
        for folder, entries in builder.ARTIFACTS.items():
            for original, name in entries:
                self.assertEqual((self.stage / folder / name).read_bytes(),
                                 (self.source / original).read_bytes())
            self.assertIn(b"token=REPLACE_WITH_UNIQUE_TOKEN\r\n",
                          (self.stage / folder / "CONFIG.INI").read_bytes())
        for path in self.stage.rglob("*"):
            if path.is_file():
                self.assertNotIn(b"private sentinel", path.read_bytes())
        self.assertIn(b"LOAD RETROBRG:\\NW411\\LLMAGENT.NLM\r\n",
                      (self.stage / "NW411/CDLOAD.NCF").read_bytes())
        listed = {}
        for line in (self.stage / "CHECKSUM.TXT").read_text().splitlines():
            digest, name = line.split("  ", 1)
            self.assertEqual(digest, builder.sha((self.stage / name).read_bytes()))
            listed[name] = digest
        self.assertEqual(set(listed), {p.relative_to(self.stage).as_posix()
                                      for p in self.stage.rglob("*")
                                      if p.is_file() and p.name != "CHECKSUM.TXT"})
        self.assertIn("BUILD.JSN", listed)

    def test_missing_helper_stops_packaging(self):
        (self.source / "agent-win16/restart.exe").unlink()
        with self.assertRaises(FileNotFoundError):
            self.stage_all()

    def test_real_token_cannot_replace_template(self):
        (self.source / builder.EXAMPLES["WIN32"]).write_text("token=not-a-template\n")
        with self.assertRaisesRegex(ValueError, "placeholder configuration"):
            self.stage_all()

    def test_truncated_resource_fork_and_wrong_nlm_rejected(self):
        target = self.source / "agent-mac-system7/build/llm_agent.bin"
        target.write_bytes(target.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError, "complete classic MacBinary"):
            builder.check_artifact(target)
        target = self.source / "agent-netware/LLMAGENT.NLM"
        target.write_bytes(b"not an NLM" * 30)
        with self.assertRaisesRegex(ValueError, "Not a NetWare NLM"):
            builder.check_artifact(target)

    def test_changed_notice_rejected(self):
        entries = json.loads((self.source / "docs/binary-provenance.json").read_text())["notices"]
        (self.source / entries[0]["path"]).write_bytes(b"missing original terms")
        with self.assertRaisesRegex(ValueError, "Notice changed"):
            self.stage_all()

    def test_long_iso_name_rejected_before_creating_image(self):
        self.stage.mkdir()
        (self.stage / "BUILD.JSON").write_text("{}")
        output = self.root / "invalid.iso"
        with self.assertRaisesRegex(ValueError, "8.3"):
            builder.create_iso(self.stage, output)
        self.assertFalse(output.exists())

    @unittest.skipUnless(os.name == "nt", "Windows build orchestrator")
    def test_failed_build_preserves_existing_iso_even_with_force(self):
        config = self.root / "config.json"
        config.write_text("{}")
        output = self.root / "previous.iso"
        output.write_bytes(b"previous deployment media")
        work = self.root / "work"
        work.mkdir()
        with patch.object(builder.tempfile, "mkdtemp", return_value=str(work)), \
             patch.object(builder, "snapshot", return_value={}), \
             patch.object(builder, "prepare", return_value={}), \
             patch.object(builder, "build_windows", side_effect=RuntimeError("compiler failed")):
            self.assertEqual(builder.main(["--config", str(config), "--output", str(output), "--force"]), 1)
        self.assertEqual(output.read_bytes(), b"previous deployment media")

    def test_both_iso_namespaces_roundtrip_and_detect_changed_payload(self):
        import pycdlib
        self.stage_all()
        output = self.root / "media.iso"
        builder.create_iso(self.stage, output)
        iso = pycdlib.PyCdlib()
        iso.open(str(output))
        try:
            self.assertEqual(iso.pvd.volume_identifier.rstrip(), b"RETROBRG")
            self.assertIsNone(iso.eltorito_boot_catalog)
        finally:
            iso.close()
        target = self.stage / "NW312/LLMAGENT.NLM"
        target.write_bytes(target.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "readback mismatch"):
            builder.verify_iso(self.stage, output)


if __name__ == "__main__":
    unittest.main()
