> Any4Quest retains the following inherited AstroQuest notices. See [CREDITS.md](CREDITS.md) for fork provenance. The release inventory below describes upstream packaging; verify the contents, versions and corresponding source of any new Any4Quest binary before distribution.

# Third-party notices

AstroQuest itself is licensed under GPL-2.0-or-later (see [LICENSE](LICENSE)). The release files
also contain, or are built with, the following parts made by others. Each keeps its own license;
the source of every part is listed with it.

## Quest app (`AstroQuest-<version>-Quest3.apk`)

| Part | License | Source |
| --- | --- | --- |
| The emulator (`shadps4-arm64-fex` in `assets/runtime.zip`): shadPS4 with the shadps4-arm64 changes and this project's | GPL-2.0-or-later | this repository, `shadps4-arm64-main/` (from [zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), based on [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| The libraries built into the emulator (`shadps4-arm64-main/externals`: SDL3, fmt, Boost, glslang, sirit, Vulkan Memory Allocator, zlib-ng, xxHash, FFmpeg's core and others) | their own licenses (zlib, MIT, BSL-1.0, BSD, LGPL...), in each folder | the submodules listed in [.gitmodules](.gitmodules) |
| FEXCore, linked into the emulator | MIT | [FEX-Emu/FEX](https://github.com/FEX-Emu/FEX), fetched and patched by `shadps4-arm64-main/runtime/scripts/build-fexcore-smoke-aarch64.sh` |
| The app (`libastrovr.so`, `classes.dex`) and `libkgsl_compat.so` | GPL-2.0-or-later | this repository, `quest-host/` |
| LLVM libc++, linked into `libastrovr.so` | Apache-2.0 WITH LLVM-exception | [Android NDK](https://developer.android.com/ndk) / [LLVM](https://github.com/llvm/llvm-project) |
| Khronos OpenXR loader for Android 1.1.63 (`libopenxr_loader.so`) | Apache-2.0 | [KhronosGroup/OpenXR-SDK-Source](https://github.com/KhronosGroup/OpenXR-SDK-Source), release 1.1.63 |
| The GNU C Library 2.41 and its dynamic loader (`libc.so.6`, `libm`, `libdl`, `libpthread`, `librt`, `libresolv`, `libutil`, `ld-linux-aarch64.so.1`, the latter shipped as `libastro_ld.so`) | LGPL-2.1-or-later | Debian 13 `glibc` source package, with the Android compatibility patches in `shadps4-arm64-main/runtime/patches/glibc-android-sysvshm/` and `runtime/scripts/glibc-android-seccomp.mjs` |
| GCC runtime (`libstdc++.so.6`, `libgcc_s.so.1`) | GPL-3.0-or-later WITH GCC-exception-3.1 | Debian 13 `gcc-14` source package |
| X11 and XCB client libraries, libxkbcommon, libxshmfence, libdrm, libexpat, libffi, libwayland-client | MIT / X11 | Debian 13 source packages of the same names |
| libsystemd, libudev | LGPL-2.1-or-later | Debian 13 `systemd` source package |
| libdbus-1 | AFL-2.1 or GPL-2.0-or-later | Debian 13 `dbus` source package |
| libcap | BSD-3-Clause or GPL-2.0-only | Debian 13 `libcap2` source package |
| libuuid | BSD-3-Clause | Debian 13 `util-linux` source package |
| zlib | Zlib | Debian 13 `zlib` source package |
| Vulkan loader (`libvulkan.so.1`), SPIRV-Tools | Apache-2.0 | [KhronosGroup/Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader), [KhronosGroup/SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools), as packaged by Debian 13 |
| Mesa Turnip Vulkan driver for Adreno (`libvulkan_freedreno.so`) | MIT | [Vauzi-17/mesa-tu8](https://github.com/Vauzi-17/mesa-tu8), branch `gen8`, commit `9e646d59830fa220fff9a94ef2ed4a6ce418e254`, with Bachata S4's `kgsl-zero-timeout-poll` patch |

The Linux runtime (everything from glibc to the Turnip driver above) is taken unchanged from
the [Bachata S4 v0.2.4](https://github.com/JICA98/Bachata-S4/releases/tag/v0.2.4) release,
whose notice (`shadps4-arm64-main/NOTICE.android-runtime.md`) and lock files
(`shadps4-arm64-main/runtime/locks/`) record the exact packages.

## PC build (`AstroQuest-<version>-PC-VR-Windows.zip`)

| Part | License | Source |
| --- | --- | --- |
| The emulator (`shadps4.exe`) and the launcher (`launch.ps1`, `Play Any4Quest Gamepad.bat`) | GPL-2.0-or-later | this repository |
| The libraries built into the emulator (`shadps4-arm64-main/externals`) | their own licenses, in each folder | the submodules listed in [.gitmodules](.gitmodules) |
| Khronos OpenXR loader 1.1.63, built into the emulator | Apache-2.0 | `shadps4-arm64-main/externals/openxr-sdk` ([KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK), release 1.1.63) |
| PkgTool and LibOrbisPkg 0.2.231 (`pc-vr/pkgtool`), unchanged: the launcher runs it to unpack a game package | LGPL-3.0 | [maxton/LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), release [v0.2](https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2) |

The emulator needs the Microsoft Visual C++ runtime, which is not included: it comes with the
[Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

## Source of the GPL and LGPL parts

The source of AstroQuest itself is this repository, at the tag of each release. For the GPL
and LGPL parts made by others (the Linux runtime in the Quest app, the libraries built into
the emulators from `shadps4-arm64-main/externals`, and PkgTool), a copy of the exact source
used is available on request for three years from each release: open an issue on this
repository.

## Not included

No part of ASTRO BOT Rescue Mission, of the PlayStation 4 system software, or any key or other
copyrighted console file is part of this repository or of its releases.
