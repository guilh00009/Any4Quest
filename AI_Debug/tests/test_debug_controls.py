# SPDX-License-Identifier: GPL-2.0-or-later
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("play_command", Path(__file__).resolve().parents[1] / "controls/play-command.py")
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)

class Controls(unittest.TestCase):
    def run_command(self, folder, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            control.main(["--session-dir", str(folder), *args])

    def test_pose_press_release_and_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            self.run_command(folder, "--left=-0.4,1,-0.3,90,0,0", "--buttons", "move0_trigger=1 move1_stick_x=-1", "--hold", ".5")
            self.assertIn("0 0.5 move0_trigger=1 move1_stick_x=-1", (folder / "controls.txt").read_text())
            self.run_command(folder, "--release")
            text = (folder / "controls.txt").read_text()
            self.assertIn("move0=-0.4,1,-0.3,90,0,0", text)
            self.assertNotIn("trigger", text)
            self.run_command(folder, "--reset")
            self.assertIn(control.DEFAULT["move0"], (folder / "controls.txt").read_text())
            self.assertEqual(list(folder.glob("*.tmp")), [])

    def test_reject_bad_input_without_replacing_live_script(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            self.run_command(folder, "--reset")
            original = (folder / "controls.txt").read_bytes()
            for args in [("--head", "nan,0,0,0,0,0"), ("--head", "1,2"),
                         ("--buttons", "move0_trigger=2"), ("--buttons", "move0_grip=nan"),
                         ("--buttons", "move2_primary"), ("--buttons", "head=0,0,0,0,0,0"),
                         ("--buttons", "move0_primary move0_primary"), ("--hold", "inf"),
                         ("--release", "--buttons", "move0_primary")]:
                with self.subTest(args=args), self.assertRaises(SystemExit):
                    self.run_command(folder, *args)
                self.assertEqual((folder / "controls.txt").read_bytes(), original)

    def test_corrupt_state_requires_explicit_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder / "state.json").write_text('{"head":"bad"}')
            with self.assertRaises(SystemExit):
                self.run_command(folder, "--release")
            self.assertFalse((folder / "controls.txt").exists())
            self.run_command(folder, "--reset")

if __name__ == "__main__":
    unittest.main(verbosity=2)
