# Any4Quest developer tests and controls

This directory contains reusable, asset-free test fixtures and manual debug tools.
Production input and rendering code remains in the emulator source tree. The
`tests` directory includes preserved upstream fixtures as well as Any4Quest tests;
their existing headers and authorship remain intact. Legacy build/probe tools stay
under `tools/`. Private sessions belong in ignored `AI_Debug/local/`.

## Run checks from the repository root

Use an existing Python 3 installation and, for C++ fixtures, an x64 developer shell
with clang++ available. These commands do not start the emulator or a game:

```powershell
python AI_Debug/tests/launcher_source_test.py
powershell -NoProfile -ExecutionPolicy Bypass -File AI_Debug/tests/launcher-test.ps1
python AI_Debug/tests/test_debug_controls.py
python AI_Debug/tests/test_pc_move_host.py --cxx clang++
python AI_Debug/tests/avplayer_video_lifetime_test.py --cxx clang++
python AI_Debug/tests/test_move_runtime_windows.py
python AI_Debug/tests/run_runtime.py --cxx clang++
python AI_Debug/tests/run_resource_liveness.py --build-dir build/win-x64 --compiler clang++
python AI_Debug/tests/run_shader_module_metadata.py --build-dir build/win-x64 --compiler clang++
```

The last two commands need an already configured Ninja build for dependency include
paths. `run-any4quest-runtime.sh` is the Bash runtime fixture runner (`CXX` can
select the compiler). Other source/lifetime and standalone C++ fixtures are named
for the code they exercise; consult each script's `--help` or header. These checks
do not certify an end-to-end hardware session. Move capture controls and limits
are documented in [tests/MOVE-CAPTURE.txt](tests/MOVE-CAPTURE.txt).

## Manual synthetic desktop controls

`controls/play-command.py` writes an atomic live-input script and pose state into
an explicitly selected session folder. It never launches the emulator. For example:

```powershell
python AI_Debug/controls/play-command.py --session-dir AI_Debug/local/example --reset
python AI_Debug/controls/play-command.py --session-dir AI_Debug/local/example --buttons move0_trigger=1 --hold 0.3
python AI_Debug/controls/play-command.py --session-dir AI_Debug/local/example --release
```

For an explicitly authorized, isolated desktop run, configure `headset=0`,
`input_mode=move`, and these emulator environment variables:

```text
SHADPS4_OPENXR=0
SHADPS4_SCRIPT_MOVES=1
SHADPS4_SCRIPT_LIVE=1
SHADPS4_INPUT_SCRIPT=<absolute path to the session's controls.txt>
```

Never apply synthetic controls to an unattended game or working save. Clear all
script variables before ordinary play. `--head`, `--left` and `--right` accept
six comma-separated finite numbers: x,y,z in meters and yaw,pitch,roll in degrees.
The initial pose is a neutral synthetic fixture, not controller calibration.
`--buttons` accepts a space-separated list of validated Move tokens. A release
retains current poses while removing all buttons; reset also restores poses.
Do not use simultaneous writers for one session. The tool records no private game
data and does not upload anything; still keep session output out of commits.
