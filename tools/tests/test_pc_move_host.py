#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run source-extraction tests for the PC OpenXR Move input path.

From the repository root:
    python3 tools/tests/test_pc_move_host.py
    python3 tools/tests/test_pc_move_host.py --sanitize \
        --check-legacy-against 8431e43a9a2a26e716b35e944611254d65f8d84f

Requires Python 3 and a C++20 compiler (CXX, --cxx, or c++). No Windows SDK,
OpenXR runtime, headset, or external package installation is required.

The production Move methods and dispatch/reference-space guards are extracted
from openxr_host.cpp into a mock host. The fixture uses the actual OpenXR header,
MoveHostState definition, and shared Touch mapping. Runtime, OpenXR calls, and the steady clock are
mocked. This checks those extracted input paths, NOT a full Windows translation
unit, emulator build, runtime integration, or physical headset compatibility.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
PROJECT = REPO / "shadps4-arm64-main"
HOST_PATH = "shadps4-arm64-main/src/core/vr/openxr_host.cpp"
FIXTURE = Path(__file__).with_name("pc_move_host_fixture.cpp")


def section(source: str, start: str, end: str) -> str:
    """Fail loudly when production structure changes rather than testing stale code."""
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


def check_lifecycle_order(source: str) -> None:
    destroy = section(source, "    void DestroySession()", "    void PollEvents()")
    events = section(source, "    void PollEvents()", "    void CheckWanted(")
    if destroy.index("ReleaseMoveControllers(") > destroy.index("xrDestroyActionSet("):
        raise AssertionError("Move input must be released before action-set destruction")
    if events.index("ReleaseMoveControllers(") > events.index("xrEndSession("):
        raise AssertionError("Move input must be released before ending the XR session")
    frame = section(source, "    void RunSession(", "    // --- tracking")
    if frame.index("UpdateHead(pose_time)") > frame.index("UpdateControllers(pose_time)"):
        raise AssertionError("Reference-space change must precede new-space Move samples")
    print("PASS source ordering: release before teardown; space reset before input", flush=True)


def check_legacy(source: str, revision: str) -> None:
    baseline = subprocess.check_output(
        ["git", "show", f"{revision}:{HOST_PATH}"], cwd=REPO, text=True
    )
    guards = (
        ("        if (!use_controllers)", "    /// What the title asks of the gamepad"),
        ("    void ApplyRumble(", "    bool CreateCopyResources("),
    )
    for start, end in guards:
        if section(source, start, end) != section(baseline, start, end):
            raise AssertionError(f"Legacy gamepad source changed after {revision}: {start.strip()}")
    bindings = lambda text: [line for line in text.splitlines() if line.strip().startswith("bind(")]
    if bindings(source) != bindings(baseline):
        raise AssertionError(f"OpenXR binding list changed after {revision}")
    print("PASS baseline: legacy gamepad reads/mapping, haptics and bindings unchanged", flush=True)


def build_source(source: str) -> str:
    methods = section(source, "    void ReleaseMoveControllers(", "    /// Reads the headset")
    # Capture arguments for assertions while still invoking the real shared mapping helper.
    methods = methods.replace(
        "MoveInput::MapTouchButtons(", "MoveInput::CapturingMapTouchButtons("
    )
    dispatch = section(source, "    void UpdateControllers(", "        if (!use_controllers)")
    dispatch += "        ++gamepad_calls;\n    }\n"
    space_check = "    void CheckSpace(XrTime time) {\n        auto& runtime = Runtime::Instance();\n"
    space_check += section(
        source,
        "        if (space_change_time != 0 && time >= space_change_time)",
        "\n\n        // What the headset shows",
    )
    space_check += "\n    }\n"
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker, contents in {
        "// @HOST_KEYS@": section(source, "    void PollDiagnosticKeys(", "    void CaptureMoveHost("),
        "// @HOST_LIFECYCLE@": section(source, "    void DiagnosticEvent(", "    void PollDiagnosticKeys("),
        "// @HOST_MOVE_METHODS@": methods,
        "// @HOST_CAPTURE@": section(source, "    void CaptureMoveHost(", "    // What Connect found."),
        "// @HOST_DISPATCH@": dispatch,
        "// @HOST_SPACE_CHANGE@": space_check,
    }.items():
        if fixture.count(marker) != 1:
            raise AssertionError(f"Fixture marker must appear once: {marker}")
        fixture = fixture.replace(marker, contents)
    return fixture


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++20 compiler command")
    parser.add_argument("--sanitize", action="store_true", help="Enable UndefinedBehaviorSanitizer")
    parser.add_argument("--check-legacy-against", metavar="REV", help="Compare unchanged gamepad paths with a Git revision")
    args = parser.parse_args()
    source = (REPO / HOST_PATH).read_text(encoding="utf-8")
    check_lifecycle_order(source)
    if args.check_legacy_against:
        check_legacy(source, args.check_legacy_against)
    with tempfile.TemporaryDirectory(prefix="any4quest-pc-move-") as temporary:
        build = Path(temporary)
        generated = build / "pc_move_host_test.cpp"
        executable = build / "pc_move_host_test"
        generated.write_text(build_source(source), encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
            "-I", str(PROJECT / "src"),
            "-I", str(PROJECT / "externals/openxr-sdk/include"),
            str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
        subprocess.run(command, cwd=REPO, check=True)
        subprocess.run([str(executable)], cwd=REPO, check=True)
    print("PASS extracted PC OpenXR Move paths (not a Windows build or headset test)")


if __name__ == "__main__":
    main()
