#!/usr/bin/env bash
# Packs the files of a GitHub release into build/release/:
#   Any4Quest-<version>-Quest3.apk           the headset app, build/quest/astro-vr-host.apk
#   Any4Quest-<version>-PC-VR-Windows.zip    the PC play folder: the emulator from
#                                             build/win-x64, the launcher, clean settings, and
#                                             PkgTool (tools/pkgtool) for unpacking a game
#                                             package; no game, saves, logs or caches
#   SHA256SUMS.txt
#   tools/make-release.sh <version>       (the version must be the APK's versionName)
# tools/pkgtool is PkgTool-0.2.231.zip of https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=${1:?usage: tools/make-release.sh <version>}
python=${PYTHON:-$(cat "$root/tools/python.local" 2>/dev/null || command -v python3 || command -v python)}
aapt2="${ANDROID_SDK:-$LOCALAPPDATA/Android/Sdk}/build-tools/36.1.0/aapt2.exe"
name="Any4Quest-$version"
out="$root/build/release"
pc="$out/$name-PC-VR-Windows"

apk="$root/build/quest/astro-vr-host.apk"
apk_version=$("$aapt2" dump badging "$apk" | sed -n "s/.*versionName='\([^']*\)'.*/\1/p")
[[ "$apk_version" == "$version" ]] || {
  echo "build/quest/astro-vr-host.apk is version $apk_version, not $version" >&2; exit 1; }

rm -rf "$out"
# The first user's home: without it the Windows build asks at its first start whether to move
# saves over from where an older shadPS4 kept them, in a message box behind the game.
mkdir -p "$pc/pc-vr/user/input_config" "$pc/games" \
         "$pc/pc-vr/user/home/1000/"{savedata,trophy,inputs}
cp "$apk" "$out/$name-Quest3.apk"

cp "$root/build/win-x64/shadps4.exe" "$pc/pc-vr/"
cp "$root/pc-vr/launch.ps1" "$pc/pc-vr/"
cp "$root/Play Any4Quest Gamepad.bat" "$root/Play Any4Quest VR.bat" "$pc/"
cp "$root/README-ANY4QUEST.md" "$root/README.md" "$root/CREDITS.md" "$root/README-PC-VR.md" "$root/README-QUEST-VR.md" "$pc/"
# Developer-only documentation is read from source, not copied with private sessions.
"$python" - "$(cygpath -m "$pc/README.md")" <<'EOF'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text(encoding="utf-8")
for target in ("AI_Debug/README.md", "AGENTS.md"):
    text = text.replace("(" + target + ")", "(https://github.com/guilh00009/Any4Quest/blob/main/" + target + ")")
p.write_text(text, encoding="utf-8")
EOF
cp "$root/pc-vr/user/input_config/default.ini" "$root/pc-vr/user/input_config/global.ini" \
   "$pc/pc-vr/user/input_config/"
# What the launcher unpacks a game package with.
mkdir -p "$pc/pc-vr/pkgtool"
cp "$root/tools/pkgtool/PkgTool.exe" "$root/tools/pkgtool/LibOrbisPkg.dll" \
   "$root/tools/pkgtool/LICENSE.txt" "$pc/pc-vr/pkgtool/"
cat > "$pc/pc-vr/pkgtool/README.txt" <<'EOF'
PkgTool and LibOrbisPkg 0.2.231, by Maxton, unchanged: the launcher uses them to unpack a game
package (.pkg). Licensed under the GNU Lesser General Public License, version 3 (LICENSE.txt).
Source: https://github.com/maxton/LibOrbisPkg (release v0.2).
EOF
# The settings file as documented: what the launcher's window saved here is left out.
sed '/^[a-z_]*=/d' "$root/pc-vr/settings.txt" > "$pc/pc-vr/settings.txt"
# The emulator's settings as the play folder has them, with everything that names this PC's
# folders or devices back at its default.
"$python" - "$(cygpath -m "$root/pc-vr/user/config.json")" \
    "$(cygpath -m "$pc/pc-vr/user/config.json")" <<'EOF'
import json, sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
config["General"].update({"addon_install_dir": "", "font_dir": "", "home_dir": "",
                          "install_dirs": [], "sys_modules_dir": "", "shadnet_server": ""})
config["Input"].update({"default_controller_id": "", "camera_id": -1,
                        "motion_controls_enabled": True, "background_controller_input": True})
config["Vulkan"].update({"gpu_id": -1, "vkvalidation_enabled": False, "renderdoc_enabled": False})
config["Log"].update({"type": "file", "sync": False, "filter": "*:Info", "append": False})
for key in [k for k in config["Audio"] if k.endswith("_device")]:
    config["Audio"][key] = "Default Device"
json.dump(config, open(sys.argv[2], "w", encoding="utf-8"), indent=2)
EOF

cat > "$pc/games/PUT YOUR GAME HERE.txt" <<'EOF'
Put your own legally obtained, decrypted game dump here (a folder with eboot.bin and
sce_sys/param.sfo). Start "Play Any4Quest VR.bat" and select the game. Decrypted .pkg
dumps require the bundled PkgTool and enough free space for extraction. Encrypted
store downloads are unsupported. See README.md for tested titles and limitations.
Selecting a game does not establish compatibility. No game files are included.
EOF
cat > "$pc/README.txt" <<EOF
Any4Quest $version - experimental PSVR game emulation through OpenXR.
Source: https://github.com/guilh00009/Any4Quest

Start "Play Any4Quest VR.bat". Read README.md for setup, the tested-games matrix,
input profiles, desktop mode and current limits. Keep paths short. Settings are
in pc-vr/settings.txt; local saves and logs are under pc-vr/user.

Any4Quest is GPL-2.0-or-later (LICENSE.txt). Upstream work is credited in CREDITS.md
and THIRD-PARTY-NOTICES.md. No game files are included. Universal compatibility is
a development goal, not a claim that every PSVR game works.
EOF
tr -d '\r' < "$root/LICENSE" > "$pc/LICENSE.txt"
cp "$root/LICENSE" "$pc/LICENSE"
cp "$root/THIRD-PARTY-NOTICES.md" "$pc/THIRD-PARTY-NOTICES.md"

"$python" - "$(cygpath -m "$out")" "$name-PC-VR-Windows" <<'EOF'
import os, sys, zipfile
out, folder = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(os.path.join(out, folder + ".zip"), "w", zipfile.ZIP_DEFLATED,
                     compresslevel=9) as archive:
    for here, folders, files in sorted(os.walk(os.path.join(out, folder))):
        if not folders and not files:
            archive.write(here, os.path.relpath(here, out).replace(os.sep, "/") + "/")
        for name in sorted(files):
            path = os.path.join(here, name)
            archive.write(path, os.path.relpath(path, out).replace(os.sep, "/"))
EOF
(cd "$out" && sha256sum "$name-Quest3.apk" "$name-PC-VR-Windows.zip" > SHA256SUMS.txt)
ls -la "$out"
cat "$out/SHA256SUMS.txt"
