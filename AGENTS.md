# Contributing to Any4Quest

## Scope and repository map

Read README.md and README-ANY4QUEST.md before changing behavior. Any4Quest targets
broader PSVR support but has limited verified compatibility. Do not advertise
universal support or infer compatibility from successful game selection.

- `shadps4-arm64-main/src/core/vr`: OpenXR, host transport, runtime and Move mapping.
- `src/core/libraries/{move,vr_tracker,hmd}` within that tree: guest-facing APIs.
- `src/input`: production scripted-input support; keep compiled code here.
- `src/video_core` and `src/shader_recompiler`: rendering and shader proofs.
- `pc-vr`: launcher and default settings. `quest-host`: Android/OpenXR host.
- `AI_Debug`: reusable asset-free tests and manual debug controls.
- `tools`: inherited build, packaging and low-level probes.

Read any deeper AGENTS.md before editing that subtree. Preserve submodule pins
unless the task specifically calls for a dependency update.

## Working safely

Inspect status and the index before editing; preserve unrelated user changes.
Do not stage, commit, push, publish or make releases unless requested. Use isolated
build/runtime folders. Do not overwrite a working installation, migrate saves,
launch a game/headset, install software or change authentication unless authorized.
Coordinate desktop ownership with other active tasks before using UI automation.

Never put game dumps, firmware, keys, saves, downloaded packages, private logs or
screenshots in Git. Place local debug sessions under ignored `AI_Debug/local/` or
outside the checkout. Use placeholder paths in examples. Inspect `git status
--untracked-files=all` and ignore behavior before proposing publication. Do not
copy a private runtime folder into a distributable archive.

## Compatibility invariants

Gamepad remains the default. Move is explicit opt-in; preserve Astro behavior.
Do not broaden the exact executable guards in `core/known_title.cpp` without
verified evidence for the new binary. Prefer reusable guest/runtime fixes over
title IDs, shader hashes or blanket assertion suppression.

Two Moves must retain independent state, handles, poses, input and haptics. Test
disconnect, tracking loss, recenter, neutral-stick rearming and physical-input
priority. Respect pose units, coordinate conventions and ABI layout. Never
describe desktop synthetic input as hardware controller verification.

Rendering proofs must fail conservatively when data is unknown. Persistent shader
metadata layout/semantics changes require cache-version review and invalidation.
For lifetime/resource fixes, include a meaningful regression case and, where
practical, show that the old implementation fails it.

## Validation and documentation

Build instructions are in README.md. Start with the relevant tests listed in
AI_Debug/README.md, then build the affected target. Tests must not depend on private
game assets. Do not run games just to validate documentation changes. Parse-check
PowerShell and syntax-check Python/Bash after moving scripts; update all callers.
Use `git diff --check` and verify the staging area remains unchanged.

Report exactly what was tested: source assertions, isolated C++ fixtures, full
build, desktop gameplay or physical headset. Give title/version, flags and limits.
A stationary soak, menu pass or intermediate checkpoint is not game completion.
Keep the root compatibility table current. Historical platform guides are not
evidence of a new fork/hardware validation.

Keep GPL headers, LICENSE and third-party notices. Credit incorporated upstream
code separately from research references and external services. Rebrand visible
project copy without blindly renaming ABI symbols, package IDs or attribution.
Review binary/source license obligations before distributing a release.
