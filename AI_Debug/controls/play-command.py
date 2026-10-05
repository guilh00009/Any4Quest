# SPDX-License-Identifier: GPL-2.0-or-later
"""Write synthetic Move input for an explicitly authorized desktop debug session."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import tempfile

DEFAULT = {"head": "0,1.6,0,0,0,0", "move0": "-0.25,1.2,-0.5,0,0,0",
           "move1": "0.25,1.2,-0.5,0,0,0"}

def pose(value):
    try:
        values = [float(v) for v in value.split(",")]
        if len(values) != 6 or not all(math.isfinite(v) for v in values):
            raise ValueError()
    except (ValueError, AttributeError):
        raise ValueError("pose requires six finite comma-separated numbers") from None
    return ",".join(format(v, ".9g") for v in values)

def buttons(value):
    result = []
    seen = set()
    for token in value.split():
        match = re.fullmatch(r"move[01]_(primary|secondary|menu|stick_click)", token)
        analog = re.fullmatch(r"(move[01]_(trigger|grip|stick_x|stick_y))=(.+)", token)
        if analog:
            try:
                number = float(analog[3])
            except ValueError:
                raise ValueError("invalid analog Move value") from None
            minimum = -1 if analog[2].startswith("stick_") else 0
            if not math.isfinite(number) or not minimum <= number <= 1:
                raise ValueError("Move analog value outside allowed range")
            token = analog[1] + "=" + format(number, ".9g")
        elif not match:
            raise ValueError("unknown Move token: " + token)
        key = token.split("=", 1)[0]
        if key in seen:
            raise ValueError("duplicate Move token: " + key)
        seen.add(key)
        result.append(token)
    return " ".join(result)

def atomic_write(path, text):
    # Unique temporary file, same filesystem; readers see the old or complete new file.
    fd, name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session-dir", type=Path, required=True)
    parser.add_argument("--head")
    parser.add_argument("--left")
    parser.add_argument("--right")
    parser.add_argument("--hold", type=float, default=0.3)
    parser.add_argument("--buttons", default="")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--release", action="store_true")
    group.add_argument("--reset", action="store_true")
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.hold) or not 0 < args.hold <= 60:
            raise ValueError("hold must be finite and in (0, 60] seconds")
        if (args.release or args.reset) and args.buttons:
            raise ValueError("release/reset cannot also press buttons")
        state_path = args.session_dir / "state.json"
        state = dict(DEFAULT)
        if state_path.exists() and not args.reset:
            saved = json.loads(state_path.read_text(encoding="utf-8"))
            if not isinstance(saved, dict) or set(saved) != set(DEFAULT):
                raise ValueError("invalid state keys; use --reset to restore defaults")
            state = {k: pose(v) for k, v in saved.items()}
        for key, value in (("head", args.head), ("move0", args.left), ("move1", args.right)):
            if value is not None:
                state[key] = pose(value)
        pressed = buttons(args.buttons)
        text = "0 36000 " + " ".join(k + "=" + v for k, v in state.items()) + "\n"
        if pressed:
            text += f"0 {args.hold:.9g} {pressed}\n"
    except (ValueError, TypeError) as error:
        parser.error(str(error))
    args.session_dir.mkdir(parents=True, exist_ok=True)
    atomic_write(args.session_dir / "controls.txt", text)
    atomic_write(state_path, json.dumps(state, indent=2) + "\n")
    print(text, end="")

if __name__ == "__main__":
    main()
