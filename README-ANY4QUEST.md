# Any4Quest: experimental multi-game and PS Move foundation

This source adds an explicit dual-Move input path and a multi-game PC launcher to the
AstroQuest v0.13 foundation (`8431e43a9a2a26e716b35e944611254d65f8d84f`). It is an experimental
compatibility foundation, **not universal PSVR game support**. Opening a game's executable,
recognizing its title, or exposing two Move controllers does not establish that it boots,
renders correctly, or is playable.

The established Astro path remains available through **Play Astro Bot VR.bat**, with
`input_mode=gamepad` as the default. Its existing DualSense/gamepad and Touch-to-gamepad
fallback behavior is preserved. Use [README-PC-VR.md](README-PC-VR.md) for that workflow and
[README-QUEST-VR.md](README-QUEST-VR.md) for the original standalone build workflow. Those
upstream guides describe their original scope; they are not compatibility evidence for
this extension.

## Headset and platform scope

- **Quest 2 PC VR is an intended target** through a Windows PC OpenXR runtime that provides
  headset poses, Touch controller actions and grip poses. The emulator runs on the PC; the
  Quest receives the streamed VR view. This change has not been tested on Quest 2 hardware
- **Quest 3 and Quest 3S PC VR are intended targets** under the same conditions. Both need
  actual device/runtime/game validation for this new Move path
- The standalone Quest host has a corresponding experimental controller transport path.
  Existing Quest 3/3S build targets remain separate from PC VR. **Standalone Quest 2 build
  support or acceptable performance is not claimed**
- No headset, Windows launcher UI, full game session, or new APK/Windows emulator build was
  hardware-validated for this change. Source-level and isolated runtime checks are useful,
  but cannot substitute for those tests

Use only games you own and are licensed to use, dumped locally from your own copies. This
repository and its tests do not contain games, game assets, console firmware, decryption
keys, or licensed system files. Nothing here acquires or bypasses encryption on Store
packages. Package support is for your existing unencrypted local dumps.

## PC launch and explicit input profiles

1. Build the updated Windows emulator and prepare `pc-vr/shadps4.exe` using the existing
   build workflow (`tools/make-pc-vr.sh`). An older downloaded emulator does not acquire Move
   support merely by using the new launcher
2. Configure the PC's OpenXR runtime and connect your headset. For the original Virtual
   Desktop workflow, use its PC Streamer and an OpenXR runtime that exposes Touch actions
3. Put local unpacked games under `games/`, or set `game=` in `pc-vr/settings.txt` to a
   specific `eboot.bin`, containing folder, or dumped package
4. Run **Play Any4Quest VR.bat**. It searches up to three folders below `games/`, excludes
   unfinished `.unpacking` folders, and asks you to choose when there are multiple
   candidates. Labels show title, title ID, version, and path where metadata is available.
   A missing `param.sfo` is reported as unknown, not treated as a known compatible title
5. Choose `gamepad` or `move` in the launch window. The selection persists as
   `input_mode=gamepad` or `input_mode=move`. With `menu=0` or `-NoMenu`, edit the setting
   yourself first. `-NoMenu` skips only settings, not an ambiguous game selection

`gamepad` is the default, including when the setting is absent. `move` is never inferred
from a game's name, title ID, file size, or connected device. Unsupported or empty mode
values stop the launch with an error. The setting becomes the emulator environment
variable `SHADPS4_VR_INPUT_MODE`; setting that variable through `env=` in the launcher's
settings is rejected so the visible input selection remains authoritative. A direct
emulator launch can instead set `SHADPS4_VR_INPUT_MODE=move` itself.

The Astro launcher prefers a single CUSA12392 version 01.00 copy when multiple games are
present; multiple matching copies still require a choice. A specific `game=` path takes
priority in either launcher. Leave it unset, or point it at a multi-game folder, to keep the
generic chooser. Multiple packages are offered for explicit selection; the largest file
is not assumed to be the right game. Select a base-game dump, not an update. Extraction
never replaces an existing `eboot.bin` in the target title folder. Space estimates come
from the original Astro dump and are not reliable requirements for other games.

For separate per-game settings, create your own text file using the same keys and run:

```powershell
.\pc-vr\launch.ps1 -LauncherProfile any -SettingsFile 'C:\My configs\game-one.txt'
```

Only use `move` with a title that requires or supports PS Move. Keep `headset=1` and
`controllers=1`; the controllers need an active tracked OpenXR input session. Gamepad
presence does not silently switch a selected Move profile back to gamepad. Hand-only
tracking is not a substitute for two Move controllers in this path.

## Standalone experimental settings

The app reads `/sdcard/Android/data/com.astrobotquest.vrhost/files/vrhost.txt`. Add
`input_mode=move` to opt into its native Move action/transport path; `gamepad` remains the
default. On standalone, an invalid value logs a warning and retains the prior/default mode.
The app writes the final `SHADPS4_VR_INPUT_MODE` from this setting after processing extra
environment entries, so those entries cannot desynchronize the host and emulator modes.

Use `game_path=` to select an unpacked game's directory or `eboot.bin`; a relative path is
resolved beside `vrhost.txt`. An explicitly invalid path stops launch rather than silently
running another game. With the setting absent, the original discovery behavior is retained:
prefer Astro, otherwise the first discovered game. This is not the PC multi-game chooser,
and neither path infers whether the chosen game supports Move.

For Move recentering, the native host honors the headset's reference-space change at its
reported time and clears derivative history before sending poses in the new local space.
The existing physical-gamepad Options recenter path is retained. Verify alignment and
recovery in the actual game; no new universal in-game recenter shortcut is promised.

## Move controller mapping

Each tracked Touch controller becomes one independent virtual Move: left hand is index 0,
right hand is index 1. This assignment does not depend on which controller is opened first.
The same mapping is used on the PC and standalone transport paths:

| Touch input, on that hand | Virtual Move input |
| --- | --- |
| X on left / A on right | Cross |
| Y on left / B on right | Circle |
| Squeeze/grip | Move button |
| Thumbstick up | Triangle |
| Thumbstick left | Square |
| Thumbstick down | Start |
| Thumbstick click | Select |
| Left menu button | Start on the left Move |
| Trigger | Analog T, scaled to 0–255; T button asserted above 12 |

These are deliberate emulation mappings, not exact physical Move ergonomics. A game's
prompt may need a thumbstick direction for a face button that the Touch controller lacks.
The OS-reserved Meta button is not exposed as a guest Move button. In `gamepad` mode the
existing Touch-to-gamepad mapping remains unchanged.

## What the implementation covers

- Two independent Move slots with guest handles, explicit open/close lifecycle, initialized
  state, button/trigger samples, timestamps, and bounded recent history (up to 32 samples)
- Shared runtime input-profile selection, controller samples, pose/tracking state, and
  per-hand feedback. Stale/disconnected controllers release input; optical tracking loss
  retains connected buttons/trigger while reporting no tracked pose and stopping vibration
- PC OpenXR per-hand grip poses and actions, and versioned standalone host/core transport
- Move pose exposure through the emulated VR tracker, plus existing headset tracking
- Independent haptic intensity for each hand, stopped when tracking/session input is lost
- Virtual sphere RGB state, without pretending Touch controllers have a controllable Move
  light sphere

The OpenXR grip origin is treated as the virtual sphere/sensor center. The emulated device
information reports a 22.5 mm sphere radius; device-info offsets/radius are millimetres,
while tracked positions use metres. Accelerometer data is synthesized from tracked motion
and gravity. It is **not raw Touch IMU data**, and sensor/pose expectations can differ by
game. This grip-frame geometry is internally consistent, but is not calibrated to a
physical PS Move controller or validated against hardware/game expectations. Haptics, axes, origin alignment, prediction, gestures, recovery after focus loss, and
guest ABI usage all require real game and hardware checks.

This does not add arbitrary game's missing APIs, title-specific fixes, a PSVR camera video
pipeline, PS Aim support, or universal rendering compatibility. Any currently unimplemented
API or title-specific rendering behavior can still block a game. The VR tracker's linear
and angular acceleration output fields remain zero because their guest-side frame/unit
contract has not been verified; they are not raw or validated acceleration readings.
Existing legacy MoveTracker and calibration/orientation API stubs are outside this change.

## Astro-only enhancements vs shared settings

The launcher applies `resolution`, `dynamic`, `fps`, `pace`, and `real_time` only when
metadata says **CUSA12392, APP_VER 01.00**. The emulator still checks the existing known-title
serial and executable code signature independently before using its memory addresses. Those
code guards are unchanged. Other IDs, versions, or missing metadata use the generic
experimental profile; resolution and FPS controls are disabled in the settings window.

Shared settings include `input_mode`, OpenXR enable/wait, FOV, sharpening, MSAA/AA controls,
tracking/prediction, pause, and surround rendering. Shared means they address runtime code,
not that all games handle every setting correctly. Astro's `fps=` control is not a general
emulator frame limiter. The launcher clears its managed inherited environment variables
on each run; Astro-only `env=` overrides are ignored for generic titles so settings do not
leak when switching games.

## Diagnostics and verification

`pc-vr/user/log/launcher.txt` records the selected game path, title, title ID, version,
explicit input mode, and enhancement profile for the latest launch. `shad_log.txt`,
`console.txt`, and `console-errors.txt` remain in the same log folder. Include the runtime,
headset model, game ID/version, input mode, and relevant logs in a reproducible report.
Avoid publishing private paths or licensed files.

Run the asset-free launcher suite on Windows PowerShell 5.1 or PowerShell 7:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/tests/launcher-test.ps1
# Or, with PowerShell 7:
pwsh -NoProfile -File tools/tests/launcher-test.ps1
```

It loads functions through the PowerShell parser without launching the emulator or UI, and
uses original synthetic SFO/package fixtures. It covers metadata, bounded discovery,
ambiguous selections, Astro defaults, mode validation, environment translation/reset,
title/version isolation, diagnostics, and settings persistence. It does not test package
extraction, dialogs, the actual process launch, or a game.

Additional source boundary checks:

```sh
python3 tools/tests/launcher_source_test.py
```

See `tools/tests/run-any4quest-runtime.sh` for isolated runtime tests. A full emulator build,
Windows UI smoke test, and real Quest 2/3/3S game testing are separate validation gates.
Before claiming a new game works, verify boot, correct stereo/FOV, two-hand pose and every
mapped button, single-controller loss/recovery, haptics, recentering, pause/resume,
reconnection, and return to the original Astro `gamepad` flow.

## Reproducible focused checks

Run these from the repository root with a C++23 compiler and Python 3. Initialize the
pinned JSON source for Runtime tests if needed:

```sh
git submodule update --init shadps4-arm64-main/externals/json
bash tools/tests/run-any4quest-runtime.sh
python3 shadps4-arm64-main/tests/libraries/test_move_guest_api.py
python3 shadps4-arm64-main/tests/libraries/test_move_runtime_integration.py
python3 tools/tests/test_pc_move_host.py --sanitize --check-legacy-against 8431e43a9a2a26e716b35e944611254d65f8d84f
bash quest-host/tests/test-move-actions.sh
bash quest-host/tests/test-game-selection.sh
bash quest-host/tests/test-move-transport.sh --mock-socket
python3 tools/tests/launcher_source_test.py
```

The guest/runtime tests compile the production source with platform services stubbed.
The PC test extracts production host methods and mocks OpenXR dependencies; the Quest
adapter test compiles its production controller code against mocked OpenXR calls. Neither
is a full Windows/Android build. Under ptrace-based test runners, LeakSanitizer cannot
run; use `ASAN_OPTIONS=detect_leaks=0` for the ASan/UBSan guest tests and record that leak
checking was unavailable. Do not describe these checks as headset or game testing.

The transport test executes the production parser and Runtime with wrapped socket calls
when `--mock-socket` is supplied. Omit it for a real Unix `SOCK_SEQPACKET` integration run
on a Linux environment that permits local sockets (exit 77 means blocked). The cloud
validation environment denied that socket operation with `EPERM`; only the wrapped-socket
variant ran there. The Java selection test requires a JDK but not the Android SDK.


## Optional Quest thumbstick locomotion (PC OpenXR)

Use `input_mode=move` with `move_locomotion=buttons` or
`move_locomotion=directional` in the PC launcher settings. Default `legacy`
retains the previous mappings. This applies to Quest controllers through PC
OpenXR (including Quest 2/3/3S runtimes exposing the Touch bindings). Physical
headset verification is still required. The standalone Android host keeps its
existing mapping; its wire protocol does not yet transmit raw stick axes.

PS Move has no stick axis in its guest interface. These profiles translate sticks
into the game's own Move controls; they cannot add movement directions or smooth
analog movement that the game does not implement. Head tracking, physical hand
positions and the gamepad path are unchanged. No title IDs or game patches select
these profiles automatically.

* `buttons`: left stick emits the dominant forward/back/left/right button; right
  stick emits one 150 ms turn press per deflection. This preserves hand orientation.
  Configure `move_locomotion_buttons=4,32,128,64,128,32` as the six decimal button
  masks (left forward, back, left, right; right turn-left, turn-right), matching the
  game's control scheme. Defaults are an example mapping, not universal bindings.
  Available bits: Move=4, Triangle=16, Circle=32, Cross=64, Square=128; 0 disables
  a direction. Trigger, Start and Select are rejected for locomotion.
* `directional`: left stick holds Move with the emulated wand oriented in the
  requested direction relative to the recentered forward axis. Right stick points
  the right wand 90 degrees left/right, then pulses Move for 150 ms after a 50 ms
  lead-in. The game determines the actual turn angle. This is a fallback for
  wand-direction locomotion: the visible virtual hand rotates while the stick is
  active. It never moves the hand position or camera and reports no synthetic gyro
  impulse. Backward, strafe and diagonal behavior depend on the guest game.

Both profiles activate at 0.65 stick deflection and release at 0.35. Release the
right stick to turn again; holding or reversing it does not repeat turns. Right
stick vertical motion does nothing; look up/down with the headset. Release both
sticks once after startup, recenter, tracking loss or disconnect. Physical face,
menu, click, squeeze and trigger actions take priority on their respective hand;
release that hand's stick to rearm afterward. Consequently, the directional
fallback cannot turn with a right-hand trigger held for an interaction. Stick
down no longer opens Start in these profiles; use the physical left menu button.

Direct executable equivalents: `SHADPS4_MOVE_LOCOMOTION` and
`SHADPS4_MOVE_LOCOMOTION_BUTTONS`. These settings have no effect in gamepad mode.
The desktop input script accepts `move0_stick_x`, `move0_stick_y`,
`move1_stick_x`, `move1_stick_y` for repeatable checks, only with the existing
headset-disabled script opt-in. Full-game completion and physical Quest tracking
are separate verification tasks.
