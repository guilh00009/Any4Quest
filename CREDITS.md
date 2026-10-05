# Credits and provenance

Any4Quest is a fork of [AstroQuest by bigmak94 and contributors](https://github.com/bigmak94/AstroQuest),
with the v0.13 baseline retained in Git history at
`8431e43a9a2a26e716b35e944611254d65f8d84f`. AstroQuest supplied the PC/Quest VR bridge,
launch/build tooling and Astro-specific integration. Existing authorship, copyright
headers and Git history remain the authoritative attribution record.

## Incorporated code and runtime dependencies

| Project and contributors | Relationship |
| --- | --- |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | PS4 emulator foundation; GPL-2.0-or-later. |
| [zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64) | ARM64 fork used by the inherited core. |
| [JICA98/Bachata-S4](https://github.com/JICA98/Bachata-S4) | Android/FEX integration and pinned Android runtime provenance. |
| [FEX-Emu](https://github.com/FEX-Emu/FEX) | x86-64 execution on ARM64; MIT. |
| [Mesa](https://www.mesa3d.org/) and [Vauzi-17/mesa-tu8](https://github.com/Vauzi-17/mesa-tu8) | Turnip graphics driver in the inherited Android runtime. |
| [Maxton/LibOrbisPkg](https://github.com/maxton/LibOrbisPkg) | Optional PkgTool package extraction; LGPL-3.0. |
| [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) | OpenXR headers/loader; Apache-2.0. |

All vendored/submodule contributors also retain credit. The exact dependency
repositories and checkout paths are listed in [.gitmodules](.gitmodules); their
license files remain in each dependency. [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
records the broader inventory, including Vulkan, compiler/C-library runtimes,
audio, graphics, input and utility libraries. Do not treat this short table as a
replacement for those notices or the corresponding-source obligations.

## References and external services

- [vgmstream](https://github.com/vgmstream/vgmstream): credited by upstream for
  Sony audio-format documentation; this reference does not assert bundled code.
- [Virtual Desktop](https://www.vrdesktop.net/): external PC streaming/OpenXR path,
  not code owned or distributed by Any4Quest.
- [The Khronos Group](https://www.khronos.org/): OpenXR/Vulkan specifications and
  ecosystem. Implementation dependencies keep their own notices.
- Source-local `*-reference.txt` and reviewed-source notes identify technical
  research for individual fixes; a citation does not by itself mean code was copied.

Inherited donation solicitations have been removed. Attribution does not imply
that upstream projects endorse this fork. No new sponsorship destination is set.
Before producing binaries, audit the actual archive and its dependency versions;
the inherited release inventory is not proof that a new package is compliant.
