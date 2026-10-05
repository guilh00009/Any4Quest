# Any4Quest

Any4Quest is an experimental PSVR game emulator project built on AstroQuest and
shadPS4. Its goal is broader PSVR compatibility, including two emulated PS Move
controllers driven by Quest Touch controllers, while preserving gamepad games.
**It does not currently run every PSVR game.** Game selection and controller
emulation alone cannot fix missing graphics, audio, OS or game-specific behavior.

## Platform priority: PCVR first

**The Quest standalone/Android version inherited from AstroQuest has not been
updated with the current Any4Quest PCVR improvements.** Development is focused on
finishing the PCVR emulation work first; standalone updates are deferred until
afterward. PCVR is still incomplete. Do not assume that PCVR fixes, controller
features or compatibility results apply to the standalone Quest version.

## Tested games

Status reflects this fork's testing, not a guarantee for other versions or systems.

| Game / tested version | Input | Evidence and remaining limits |
| --- | --- | --- |
| ASTRO BOT Rescue Mission — CUSA12392 01.00 | `gamepad` | Desktop boot, menu and new-adventure regression checks passed. Existing Astro gamepad behavior and executable guards are preserved. Full completion in this fork has not been verified. Upstream AstroQuest reported broader play testing; that is separate evidence. |
| The Inpatient — CUSA08291 01.00 | Experimental `move` | Automated desktop gameplay reached ACT1_COMA, picked up the flashlight and traversed a doorway/corridor. The ending was not reached. Experimental wide-near presentation still has peripheral/overlay artifacts; physical VR reports include drift and flickering. Backward movement, strafing and a complete physical headset test remain unverified. |
| Other PSVR titles | Explicit profile required | Untested unless a reproducible report establishes otherwise. |

Quest 2, Quest 3 and Quest 3S Touch controllers are intended PC OpenXR targets;
there is no completed physical test matrix across those devices. The inherited
standalone Android/Quest path is experimental and is not the same as PCVR.
Standalone Quest 2 performance/support is not established.

## Windows PC setup and play

1. Obtain a build of **this fork**, or build from source below. An upstream
   AstroQuest release does not include this fork's changes. Keep the installation
   separate from an existing working emulator and saves.
2. Place `shadps4.exe` in `pc-vr/`. Use a Windows x64 PC with an AVX2-capable CPU,
   a Vulkan-capable GPU and the required Microsoft Visual C++ runtime. Driver,
   memory and performance requirements depend on the game; no universal minimum
   performance specification has been validated here.
3. Use your own legally obtained, decrypted game dump. A typical layout is
   `games/<title>/eboot.bin` and `games/<title>/sce_sys/param.sfo`. No games,
   firmware or keys are provided. Do not add dumps or saves to Git.
4. Start **Play Any4Quest VR.bat**. The chooser supports multiple titles; you can
   also select an external game without moving it. Decrypted `.pkg` extraction
   needs PkgTool/LibOrbisPkg in `pc-vr/pkgtool/`; building the emulator alone does
   not install it. Encrypted store packages are unsupported.
5. Set the input profile explicitly in `pc-vr/settings.txt`: `gamepad` for Astro
   and other pad games, or experimental `move` for games requiring two PS Moves.
   The launcher never infers Move support from a title ID.

For PCVR, connect your headset through a PC OpenXR runtime (the inherited workflow
uses Virtual Desktop and VDXR), select that runtime, and connect before launching.
A DualSense should connect directly to the PC to retain its motion/touchpad data.
Touch fallback in gamepad mode and two independent Touch devices in Move mode are
different mappings. The gamepad compatibility entry **Play Any4Quest Gamepad.bat**
retains the older Astro-specific launcher profile; use the VR entry for the generic
chooser. Existing Astro enhancements remain restricted to the verified executable.

### Desktop / no headset

Set `headset=0` and `wait=0` in a separate settings file, then run:

```powershell
.\pc-vr\launch.ps1 -LauncherProfile any -SettingsFile (Join-Path $PWD 'desktop-settings.txt')
```

Use `input_mode=gamepad` for normal manual menu interaction. `headset=0` disables
host OpenXR; the emulated PSVR device remains available to the game. `menu=0`
skips the launcher settings dialog, not the game's main menu. Desktop display is
useful for diagnosis and does not establish stereo VR correctness. Synthetic Move
controls are opt-in developer tools described in [AI_Debug](AI_Debug/README.md).

### Move controls and joystick limits

Trigger maps to the Move trigger; grip/squeeze maps to the Move button. X/A maps
to Cross, Y/B to Circle. Legacy stick directions map up to Triangle, left to
Square and down to Start; stick click maps to Select and left menu to Start.
Controller grip poses act as virtual Move sphere positions; calibration and
synthesized inertial data are experimental.

`move_locomotion=legacy` is the default. PC OpenXR can opt into `buttons` (a
configurable six-button mapping) or `directional` (wand-directed movement and
discrete turn button gestures). These are digital Move actions, not a universal
analog locomotion API. Games still decide movement/turning behavior. Neutral-stick
gating after tracking loss/recenter and physical-button priority prevent queued
actions; return sticks to neutral before retrying. Android transport does not yet
carry the raw axes needed for these profiles. See [input details](README-ANY4QUEST.md).

### Experimental Inpatient presentation

For this tested build only, the current experimental paths are enabled in the
chosen settings file with `env=SHADPS4_EXPERIMENTAL_WIDE_NEAR_VR=1` for VR, or
`env=SHADPS4_EXPERIMENTAL_WIDE_NEAR_PREVIEW=1` for mono desktop preview (`2` for
stereo desktop preview). Choose one path and clear the other override. This is an
approximation with known peripheral/overlay limitations, not a general PSVR fix.

## Building

Clone this repository and initialize its submodules:

```powershell
git clone --recurse-submodules https://github.com/guilh00009/Any4Quest.git
cd Any4Quest
git submodule update --init --recursive
```

Windows builds have been verified with Visual Studio 2022 Build Tools, its Windows
SDK, LLVM clang-cl 19.1.5, CMake and Ninja. CMake requires at least 3.24 and C++23.
From an x64 developer shell with `clang-cl`, `cmake` and `ninja` on PATH:

```powershell
cmake -S shadps4-arm64-main -B build/win-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DENABLE_OPENXR=ON
cmake --build build/win-x64 --parallel
Copy-Item build/win-x64/shadps4.exe pc-vr/shadps4.exe
```

Configuration can download dependencies; review your environment before running
it. Keep generated output outside source directories. The legacy packaging scripts
expect an initialized runtime folder and additional dependencies; they are not a
fresh-build installer. Release packaging has not been revalidated in this pass.

The inherited standalone version is not updated with the current PCVR improvements;
updates are deferred while PCVR is the priority. Standalone Android builds additionally need the Android SDK/NDK, JDK, an ARM64
sysroot, FEX, the OpenXR Android loader and the pinned Bachata runtime. Follow the
dependency/version notes in [the inherited Quest guide](README-QUEST-VR.md) and
`quest-host/build.sh`; a new standalone Any4Quest build has not been verified.
Keep existing Android app/package identifiers until an explicit migration is
implemented so data locations and native loading remain consistent.

## Troubleshooting and reports

- Missing executable: build/copy `shadps4.exe` into `pc-vr/`; do not overwrite a
  working installation as a diagnostic shortcut.
- No headset: check the active OpenXR runtime and streaming connection, or use
  `headset=0` for a desktop check. Controller tracking needs `controllers=1`.
- Wrong controls: verify `input_mode`, return sticks to neutral and check which
  game actions the selected locomotion profile actually generates.
- Black areas, flicker or crashes: report the title ID/version, revision, GPU,
  driver, runtime, headset and exact reproduction steps. Distinguish desktop
  observations from physical headset results and say which experimental flags
  were enabled. A boot/menu pass is not a completion test.
- Long paths: keep the installation and dump paths short. Some inherited paths
  still encounter the Windows 260-character limit.

Runtime logs normally live under `pc-vr/user/log/`, and saves under `pc-vr/user/home/`.
Review/redact logs before sharing; never attach game files, keys, saves or raw
private captures. [AI_Debug](AI_Debug/README.md) contains reusable tests and controls;
[AGENTS.md](AGENTS.md) explains contributor checks and safety rules.

## Credits and license

Any4Quest retains the work of AstroQuest, shadPS4 and their contributors. See
[CREDITS.md](CREDITS.md) for provenance and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
for dependency notices. The project is [GPL-2.0-or-later](LICENSE); dependencies keep
their respective licenses. No affiliation with Sony, Team Asobi or Meta is implied.
PlayStation, PSVR, ASTRO BOT and Quest names belong to their respective owners.
