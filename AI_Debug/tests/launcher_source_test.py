#!/usr/bin/env python3
"""Source-boundary checks only; run launcher-test.ps1 for actual PowerShell behavior."""
import hashlib
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LauncherSourceTests(unittest.TestCase):
    def test_known_title_implementation_unchanged_from_v013(self):
        # The exact implementation is deliberately preserved, not just a few guard strings.
        contents = (ROOT / "shadps4-arm64-main/src/core/known_title.cpp").read_bytes()
        # Normalize checkout line endings: these C++ sources may have CRLF on Windows.
        contents = contents.replace(b"\r\n", b"\n")
        self.assertEqual(hashlib.sha256(contents).hexdigest(),
                         "c5cf5a825328ea287542cb544df05f95e093064065c634f6863fc575ac8c43d1")

    def test_original_launcher_entry_keeps_default_profile(self):
        batch = (ROOT / "Play Any4Quest Gamepad.bat").read_text()
        self.assertIn('pc-vr\\launch.ps1" %*', batch)
        self.assertNotIn("-LauncherProfile any", batch)

    def test_generic_entry_selects_generic_profile(self):
        batch = (ROOT / "Play Any4Quest VR.bat").read_text()
        self.assertIn('pc-vr\\launch.ps1" -LauncherProfile any %*', batch)

    def test_shipped_default_is_gamepad(self):
        lines = (ROOT / "pc-vr/settings.txt").read_text(encoding="utf-8-sig").splitlines()
        settings = dict(line.split("=", 1) for line in lines if line and not line.startswith("#"))
        self.assertEqual(settings["input_mode"], "gamepad")
        self.assertEqual(settings["resolution"], "2880")
        self.assertEqual(settings["fps"], "60")

    def test_launcher_tests_are_asset_free_and_parse_checked(self):
        test = (ROOT / "AI_Debug/tests/launcher-test.ps1").read_text()
        self.assertIn("$parseErrors", test)
        self.assertIn("function Make-Sfo", test)
        self.assertIn("synthetic test fixture; not executable", test)
        self.assertNotIn("$realSfo", test)
        self.assertNotIn("$haveSfo", test)

    def test_new_entry_and_guide_are_allowlisted(self):
        ignore = (ROOT / ".gitignore").read_text()
        self.assertIn("!/Play Any4Quest VR.bat", ignore)
        self.assertIn("!/README-ANY4QUEST.md", ignore)

    def test_release_includes_generic_entry_and_guide(self):
        release = (ROOT / "tools/make-release.sh").read_text()
        self.assertIn('$root/Play Any4Quest VR.bat', release)
        self.assertIn('$root/README-ANY4QUEST.md', release)


if __name__ == "__main__":
    unittest.main(verbosity=2)
