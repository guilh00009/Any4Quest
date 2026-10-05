# Starts a locally dumped game with the PC OpenXR runtime. The original Astro launcher keeps
# its preferred title; the Any4Quest launcher lets the player choose among discovered games.
# Selecting a game or an input profile is not a claim that the game is compatible.
param([string]$SettingsFile = "", [switch]$NoMenu,
      [ValidateSet("astro", "any")][string]$LauncherProfile = "astro")

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$launcherName = if ($LauncherProfile -eq "any") { "Any4Quest VR" } else { "Any4Quest Gamepad" }
Set-Location $here
if ($SettingsFile -eq "") { $SettingsFile = Join-Path $here "settings.txt" }

function Say([string]$text, [string]$color = "Gray") { Write-Host $text -ForegroundColor $color }

# --- settings -------------------------------------------------------------------------------
function Read-Settings {
    $script:settings = [ordered]@{}
    $script:extraEnv = @()
    if (Test-Path -LiteralPath $SettingsFile) {
        foreach ($line in Get-Content -LiteralPath $SettingsFile) {
            $line = $line.Trim()
            if ($line -eq "" -or $line.StartsWith("#")) { continue }
            $at = $line.IndexOf("=")
            if ($at -lt 1) { continue }
            $key = $line.Substring(0, $at).Trim().ToLower()
            $value = $line.Substring($at + 1).Trim()
            if ($key -eq "env") { $script:extraEnv += $value } else { $script:settings[$key] = $value }
        }
    }
}
function Setting([string]$key, [string]$default = "") {
    if ($settings.Contains($key)) { return $settings[$key] }
    return $default
}
# Writes key=value into the settings file: in place of the line that sets it, or at the end.
function Save-Setting([string]$key, [string]$value) {
    $lines = @()
    if (Test-Path -LiteralPath $SettingsFile) { $lines = @(Get-Content -LiteralPath $SettingsFile) }
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ("^\s*" + [regex]::Escape($key) + "\s*=")) {
            $lines[$i] = "$key=$value"
            $done = $true
        }
    }
    if (-not $done) { $lines += "$key=$value" }
    Set-Content -LiteralPath $SettingsFile -Value $lines -Encoding UTF8
}
Read-Settings

# The sizes an eye can be drawn at: the console's largest (1440x1536, what a PlayStation 4 Pro
# draws) and larger, all the same shape.
$widths = @(1440, 1800, 2160, 2520, 2880, 3240, 3600)
function EyeHeight([int]$width) { return [int]([math]::Round(1536.0 * $width / 1440 / 8) * 8) }
$caps = @(120, 90, 72, 60, 45, 40, 36, 30)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# A message box, on top of whatever else is on the desktop (which is what the headset shows).
# Returns the name of the button chosen: OK, Yes, No, Cancel.
function Show-Box([string]$text, [string]$buttons = "OK", [string]$icon = "Information",
                  [string]$default = "Button1") {
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        return [System.Windows.Forms.MessageBox]::Show($owner, $text, $launcherName, $buttons,
                                                       $icon, $default).ToString()
    } finally { $owner.Dispose() }
}

# Shows one of the launcher's windows and waits for it. In its normal size and in front, also
# when the launcher itself was started minimized (Windows would open its first window the same).
function Show-Form($form) {
    $form.Add_Shown({ $this.WindowState = "Normal"; $this.Activate() })
    return $form.ShowDialog()
}

# --- the game ---------------------------------------------------------------------------------
# The game is looked for in the games folder next to this one, as deep as three folders down.
# An unpacked game is a folder with eboot.bin in it; a package (.pkg) is unpacked first, once.
# game= in the settings names one that is elsewhere (its eboot.bin, its folder or its package),
# and when none is found a window asks where it is.
$gamesFolder = Join-Path $root "games"
$madeFor = "CUSA12392"
# The longest path of a file inside that game: the emulator cannot open a file whose whole path
# is longer than 259 characters.
$longestInside = 126
$zeroPasscode = "0" * 32
$unpackFolder = ".unpacking"

function Gigabytes([double]$bytes) { return ("{0:N1} GB" -f ($bytes / 1073741824.0)) }

# The folders under one, itself first and the nearest first, down to so many levels. What is
# inside an unpacked game, or one being unpacked, is left out.
function Get-Folders([string]$top, [int]$depth = 3) {
    $all = New-Object System.Collections.Generic.List[string]
    if (-not [System.IO.Directory]::Exists($top)) { return $all }
    $level = @($top)
    for ($i = 0; $i -le $depth -and $level.Count -gt 0; $i++) {
        $next = @()
        foreach ($folder in $level) {
            $all.Add($folder)
            if ([System.IO.File]::Exists([System.IO.Path]::Combine($folder, "eboot.bin"))) { continue }
            try {
                foreach ($sub in [System.IO.Directory]::GetDirectories($folder)) {
                    if ([System.IO.Path]::GetFileName($sub) -ne $unpackFolder) { $next += $sub }
                }
            } catch {}
        }
        $level = $next
    }
    return $all
}

# What a param.sfo says, by name (TITLE_ID, APP_VER, TITLE...): the texts only.
function Read-Sfo([string]$path) {
    $values = @{}
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 20 -or [System.BitConverter]::ToUInt32($bytes, 0) -ne 0x46535000) {
            return $values
        }
        $keys = [System.BitConverter]::ToInt32($bytes, 8)
        $data = [System.BitConverter]::ToInt32($bytes, 12)
        $count = [System.BitConverter]::ToInt32($bytes, 16)
        for ($i = 0; $i -lt $count; $i++) {
            $at = 20 + 16 * $i
            $keyAt = $keys + [System.BitConverter]::ToUInt16($bytes, $at)
            $format = [System.BitConverter]::ToUInt16($bytes, $at + 2)
            $length = [System.BitConverter]::ToInt32($bytes, $at + 4)
            $valueAt = $data + [System.BitConverter]::ToInt32($bytes, $at + 12)
            $end = [Array]::IndexOf($bytes, [byte]0, $keyAt)
            $key = [System.Text.Encoding]::ASCII.GetString($bytes, $keyAt, $end - $keyAt)
            if ($format -eq 0x0204 -and $length -gt 0) {
                $values[$key] = [System.Text.Encoding]::UTF8.GetString($bytes, $valueAt, $length - 1)
            }
        }
    } catch {}
    return $values
}
function Get-GameInfo([string]$eboot) {
    $folder = [System.IO.Path]::GetDirectoryName($eboot)
    return Read-Sfo ([System.IO.Path]::Combine($folder, "sce_sys", "param.sfo"))
}

# Discovery is deterministic and never guesses from file size which package is the base game.
# Metadata is for selection and diagnostics only; it does not establish compatibility.
function Find-Games([string]$top) {
    foreach ($folder in (Get-Folders $top)) {
        $eboot = [System.IO.Path]::Combine($folder, "eboot.bin")
        if ([System.IO.File]::Exists($eboot)) { $eboot }
    }
}
function Find-Packages([string]$top) {
    foreach ($folder in (Get-Folders $top)) {
        try { $files = [System.IO.Directory]::GetFiles($folder, "*.pkg") } catch { continue }
        foreach ($file in $files) {
            if (Read-PackageId $file) { $file }
        }
    }
}
function Test-AstroProfile($info) {
    return ($info["TITLE_ID"] -eq "CUSA12392" -and $info["APP_VER"] -eq "01.00")
}
function New-GameCandidate([string]$path) {
    $package = [System.IO.Path]::GetExtension($path) -ieq ".pkg"
    $info = if ($package) { @{} } else { Get-GameInfo $path }
    $title = if ($info["TITLE"]) { $info["TITLE"] } else { [System.IO.Path]::GetFileName([System.IO.Path]::GetDirectoryName($path)) }
    $serial = if ($info["TITLE_ID"]) { $info["TITLE_ID"] } else { "unknown ID" }
    $version = if ($info["APP_VER"]) { $info["APP_VER"] } else { "unknown version" }
    $label = "$title | $serial | $version | $path"
    if ($package) { $label = "Package (verify base game, not an update): " + [System.IO.Path]::GetFileName($path) + " | " + (Read-PackageId $path) + " | " + $path }
    return [pscustomobject]@{ Path = $path; Label = $label; Info = $info; IsPackage = $package }
}
function Get-GameCandidates([string]$top) {
    $paths = @(@(Find-Games $top) + @(Find-Packages $top) | Sort-Object -Unique)
    foreach ($path in $paths) { New-GameCandidate $path }
}
# Astro retains its default when exactly one matching copy exists. All ambiguous choices,
# including multiple copies/versions, require the player; no arbitrary first game is run.
function Get-PreferredCandidate($candidates) {
    if ($candidates.Count -eq 1) { return $candidates[0] }
    if ($LauncherProfile -eq "astro") {
        $astro = @($candidates | Where-Object { -not $_.IsPackage -and (Test-AstroProfile $_.Info) })
        if ($astro.Count -eq 1) { return $astro[0] }
    }
    return $null
}
function Select-GameCandidate($candidates) {
    $preferred = Get-PreferredCandidate $candidates
    if ($preferred) { return $preferred }
    if ($candidates.Count -eq 0) { return $null }
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "$launcherName - choose a game"
    $form.ClientSize = New-Object System.Drawing.Size(860, 330)
    $form.StartPosition = "CenterScreen"
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Choose your own local dump. Other games are experimental; discovery does not mean compatibility."
    $label.SetBounds(16, 12, 828, 40)
    $form.Controls.Add($label)
    $list = New-Object System.Windows.Forms.ListBox
    $list.SetBounds(16, 54, 828, 220)
    $list.HorizontalScrollbar = $true
    foreach ($candidate in $candidates) { [void]$list.Items.Add($candidate.Label) }
    $form.Controls.Add($list)
    $play = New-Object System.Windows.Forms.Button
    $play.Text = "Choose"
    $play.SetBounds(656, 288, 88, 30)
    $play.Enabled = $false
    $play.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $list.Add_SelectedIndexChanged({ $play.Enabled = $list.SelectedIndex -ge 0 })
    $form.Controls.Add($play)
    $form.AcceptButton = $play
    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Text = "Cancel"
    $cancel.SetBounds(756, 288, 88, 30)
    $cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($cancel)
    $form.CancelButton = $cancel
    try {
        if ((Show-Form $form) -eq [System.Windows.Forms.DialogResult]::OK -and $list.SelectedIndex -ge 0) {
            return $candidates[$list.SelectedIndex]
        }
        $script:selectionCancelled = $true
        return $null
    } finally { $form.Dispose() }
}
# Kept as a discovery helper for callers that only want unpacked games.
function Find-Game([string]$top) {
    $candidates = @(Find-Games $top | ForEach-Object { New-GameCandidate $_ })
    $chosen = Get-PreferredCandidate $candidates
    if ($chosen) { return $chosen.Path }
    return $null
}

# What a package calls its content ("EP9000-CUSA12392_00-..."), or nothing if the file is not
# a PlayStation 4 package.
function Read-PackageId([string]$path) {
    try {
        $head = New-Object byte[] 128
        $stream = [System.IO.File]::OpenRead($path)
        try { $read = $stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ($read -lt $head.Length) { return $null }
        if ($head[0] -ne 0x7F -or $head[1] -ne 0x43 -or $head[2] -ne 0x4E -or $head[3] -ne 0x54) {
            return $null
        }
        return [System.Text.Encoding]::ASCII.GetString($head, 0x40, 36).Trim([char]0)
    } catch { return $null }
}

# The window shown while a package is unpacked. True when the unpacking ran to its end, false
# when it was cancelled (and stopped).
function Show-Unpacking($process, [string]$drive, [double]$freeBefore) {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = $launcherName
    $form.ClientSize = New-Object System.Drawing.Size(460, 132)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.ControlBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Unpacking the game. This is done once and takes a minute or a few."
    $label.SetBounds(16, 14, 428, 22)
    $form.Controls.Add($label)
    $bar = New-Object System.Windows.Forms.ProgressBar
    $bar.Style = "Marquee"
    $bar.MarqueeAnimationSpeed = 30
    $bar.SetBounds(16, 44, 428, 18)
    $form.Controls.Add($bar)
    $state = New-Object System.Windows.Forms.Label
    $state.Name = "state"
    $state.SetBounds(16, 72, 300, 22)
    $form.Controls.Add($state)
    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Name = "cancel"
    $cancel.Text = "Cancel"
    $cancel.SetBounds(356, 92, 88, 30)
    $form.Controls.Add($cancel)

    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 500
    $timer.Add_Tick({
        if ($process.HasExited) {
            $timer.Stop()
            $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
            $form.Close()
            return
        }
        if ($freeBefore -ge 0) {
            try {
                $free = (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace
                $state.Text = (Gigabytes ([math]::Max(0, $freeBefore - $free))) + " unpacked"
            } catch {}
        }
    })
    $cancel.Add_Click({
        $timer.Stop()
        try { $process.Kill() } catch {}
        try { $process.WaitForExit(10000) | Out-Null } catch {}
        $form.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
        $form.Close()
    })
    $form.Add_Shown({ $timer.Start() })
    $result = Show-Form $form
    $timer.Dispose()
    $form.Dispose()
    return ($result -eq [System.Windows.Forms.DialogResult]::OK)
}

# Unpacks a package into the games folder, under the game's serial, and returns the eboot.bin
# there; nothing if it was not done. PkgTool (of LibOrbisPkg) does the work. It reads packages
# that are not encrypted, which is what a dump of a game is; what the PlayStation Store hands
# out is encrypted and cannot be unpacked by anything here.
function Expand-Package($package) {
    $contentId = Read-PackageId $package.FullName
    if ($null -eq $contentId) {
        [void](Show-Box ($package.FullName + "`n`nis not a PlayStation 4 package.") "OK" "Warning")
        return $null
    }
    $tool = $null
    foreach ($candidate in @((Join-Path $here "pkgtool\PkgTool.exe"),
                             (Join-Path $root "tools\pkgtool\PkgTool.exe"))) {
        if ([System.IO.File]::Exists($candidate)) { $tool = $candidate; break }
    }
    if ($null -eq $tool) {
        [void](Show-Box ("The game is here as a package:`n" + $package.FullName + "`n`nbut PkgTool, which unpacks packages, is missing from`n" + (Join-Path $here "pkgtool") + "`n`nUnzip the whole Any4Quest package again.") "OK" "Warning")
        return $null
    }
    $serial = "game"
    if ($contentId -match "[A-Z]{4}[0-9]{5}") { $serial = $Matches[0] }
    $target = Join-Path $gamesFolder $serial
    if ([System.IO.File]::Exists((Join-Path $target "eboot.bin"))) {
        [void](Show-Box ("A game already exists in`n" + $target + "`n`nThe launcher will not replace it with a package or update. Choose its eboot.bin, or unpack a different copy to a separate folder yourself.") "OK" "Warning")
        return $null
    }
    if ($serial -eq $madeFor -and $target.Length + 1 + $longestInside -gt 259) {
        [void](Show-Box ("The game cannot be unpacked into`n" + $target + "`n`nThat path is too long: some of the game's files would have a path of more than 259 characters, which the emulator cannot open. Move the Any4Quest folder somewhere with a shorter path, for example C:\Games\Any4Quest, and start again.") "OK" "Warning")
        return $null
    }
    # Estimate based on Astro's dump; other games can require substantially more space.
    $needed = $package.Length * 1.85
    $drive = [System.IO.Path]::GetPathRoot($target)
    $free = -1
    try { $free = (New-Object System.IO.DriveInfo($drive)).AvailableFreeSpace } catch {}
    if ($free -ge 0 -and $free -lt $needed) {
        [void](Show-Box ("Unpacking the game takes about " + (Gigabytes $needed) + ", and drive " + $drive + " has " + (Gigabytes $free) + " free.`n`nMake room, or move the Any4Quest folder to a drive that has it.") "OK" "Warning")
        return $null
    }
    $answer = Show-Box ("The game is here as a package:`n`n" + $package.Name + "   (" + (Gigabytes $package.Length) + ")`n`nIt has to be unpacked before it can be played. That is done once, takes a minute or a few, and about " + (Gigabytes $needed) + " in`n" + $target + "`n`nThis is a space estimate, not a guarantee for other games. Use a base-game dump, not an update package.`n`nUnpack it now?") "YesNo" "Question"
    if ($answer -ne "Yes") { return $null }

    # Unpacked into a folder of its own first: what is left of an unpacking that did not finish
    # is never taken for the game.
    $work = Join-Path $target $unpackFolder
    try {
        if ([System.IO.Directory]::Exists($work)) { [System.IO.Directory]::Delete($work, $true) }
        [void][System.IO.Directory]::CreateDirectory($work)
    } catch {
        [void](Show-Box ("Could not write to`n" + $target + "`n`n" + $_.Exception.Message) "OK" "Warning")
        return $null
    }
    $errors = Join-Path $work "pkgtool-errors.txt"
    $arguments = "pkg_extract --passcode " + $zeroPasscode + " `"" + $package.FullName + "`" `"" + (Join-Path $work "files") + "`""
    Say ("Unpacking " + $package.FullName)
    $process = Start-Process -FilePath $tool -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $work "pkgtool-output.txt") -RedirectStandardError $errors
    # (Without this the exit code is not to be had later.)
    $null = $process.Handle
    $finished = Show-Unpacking $process $drive $free

    $unpacked = Join-Path $work "files\uroot"
    if (-not [System.IO.Directory]::Exists($unpacked)) { $unpacked = Join-Path $work "files" }
    $failure = $null
    if (-not $finished) {
        $failure = "cancelled"
    } elseif ($process.ExitCode -ne 0 -or -not [System.IO.File]::Exists((Join-Path $unpacked "eboot.bin"))) {
        $why = ""
        try { $why = (@(Get-Content -LiteralPath $errors -ErrorAction Stop | Where-Object { $_.Trim() -ne "" })[0]) } catch {}
        $failure = "The package could not be unpacked.`n`nOnly a package made from a dump of the game can be: it is not encrypted. A package from the PlayStation Store is, and cannot be used.`n`n" + $package.FullName
        if ($why) { $failure += "`n`n(PkgTool: " + $why.Trim() + ")" }
    } else {
        try {
            # Into place: over anything of the same name that an earlier attempt left there.
            foreach ($item in [System.IO.Directory]::GetFileSystemEntries($unpacked)) {
                $to = Join-Path $target ([System.IO.Path]::GetFileName($item))
                if ([System.IO.Directory]::Exists($to)) { [System.IO.Directory]::Delete($to, $true) }
                elseif ([System.IO.File]::Exists($to)) { [System.IO.File]::Delete($to) }
                [System.IO.Directory]::Move($item, $to)
            }
            # What the package keeps outside its file system: the game's own description
            # (param.sfo, which tells the emulator what game this is), pictures, trophies.
            # Named ICON0_PNG, PLAYGO_CHUNK_DAT, TROPHY__TROPHY00_TRP... there; icon0.png,
            # playgo-chunk.dat, trophy/trophy00.trp in the game's sce_sys folder.
            $leftOut = @("DIGESTS", "ENTRY_KEYS", "IMAGE_KEY", "GENERAL_DIGESTS", "METAS",
                         "ENTRY_NAMES", "LICENSE_DAT", "LICENSE_INFO")
            foreach ($line in (& $tool pkg_listentries $package.FullName 2>$null)) {
                if ($line -notmatch '^0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\d+)\s+(?:\d+\s+)?([A-Z0-9_]+)\s*$') { continue }
                $index = $Matches[1]
                $name = $Matches[2]
                if ($leftOut -contains $name -or $name.EndsWith("_DDS")) { continue }
                $file = $name.ToLower().Replace("__", "\")
                $at = $file.LastIndexOf("_")
                if ($at -gt 0) { $file = $file.Substring(0, $at) + "." + $file.Substring($at + 1) }
                if ($file.StartsWith("playgo_")) { $file = "playgo-" + $file.Substring(7) }
                $file = Join-Path (Join-Path $target "sce_sys") $file
                [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($file))
                & $tool pkg_extractentry --passcode $zeroPasscode $package.FullName $index $file 2>$null | Out-Null
            }
            if (-not [System.IO.File]::Exists((Join-Path $target "sce_sys\param.sfo"))) {
                $failure = "The package was unpacked, but its description of the game (param.sfo) could not be read from it.`n`n" + $package.FullName
            }
        } catch {
            $failure = "The package was unpacked, but the game could not be put in`n" + $target + "`n`n" + $_.Exception.Message
        }
    }
    try { [System.IO.Directory]::Delete($work, $true) } catch {}
    if ($failure) {
        if ($failure -ne "cancelled") { [void](Show-Box $failure "OK" "Warning") }
        return $null
    }
    Say ("Unpacked into " + $target)
    $answer = Show-Box ("The game is unpacked and ready.`n`nThe package is not needed any more:`n" + $package.FullName + "`n`nDelete it, to get " + (Gigabytes $package.Length) + " back? (Keep it if it is your only copy of the game.)") "YesNo" "Question" "Button2"
    if ($answer -eq "Yes") {
        try { [System.IO.File]::Delete($package.FullName) } catch {
            [void](Show-Box ("Could not delete the package:`n" + $_.Exception.Message) "OK" "Warning")
        }
    }
    return (Join-Path $target "eboot.bin")
}

# What a path named in the settings, or picked in the window, gives: the eboot.bin to run, or
# nothing.
function Use-Path([string]$path) {
    if ([System.IO.Directory]::Exists($path)) {
        $candidate = Select-GameCandidate @(Get-GameCandidates $path)
        if (-not $candidate) { return $null }
        $path = $candidate.Path
    }
    if (-not [System.IO.File]::Exists($path)) { return $null }
    if ([System.IO.Path]::GetExtension($path) -ieq ".pkg") {
        return Expand-Package (New-Object System.IO.FileInfo($path))
    }
    if ([System.IO.Path]::GetFileName($path) -ine "eboot.bin") { return $null }
    return $path
}

# The window for when the game is nowhere to be found. Returns pick (show where it is), again
# (look again) or quit.
function Show-NotFound {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = $launcherName
    $form.ClientSize = New-Object System.Drawing.Size(600, 250)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $title = New-Object System.Windows.Forms.Label
    $title.Text = "Where is the game?"
    $title.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $title.SetBounds(16, 14, 568, 26)
    $form.Controls.Add($title)
    $text = New-Object System.Windows.Forms.Label
    $text.Text = "No game was selected. This project does not contain games: it plays your own licensed local dumps.`n`nPut that copy in the games folder - either the game's folder (the one with eboot.bin in it) or its .pkg file - and choose Look again. Or leave it where it is and show where that is."
    $text.SetBounds(16, 50, 568, 96)
    $form.Controls.Add($text)
    $where = New-Object System.Windows.Forms.Label
    $where.Text = "The games folder: " + $gamesFolder
    $where.ForeColor = [System.Drawing.SystemColors]::GrayText
    $where.SetBounds(16, 150, 568, 40)
    $form.Controls.Add($where)

    $pick = New-Object System.Windows.Forms.Button
    $pick.Name = "pick"
    $pick.Text = "Show where it is..."
    $pick.SetBounds(16, 204, 150, 30)
    $pick.DialogResult = [System.Windows.Forms.DialogResult]::Yes
    $form.Controls.Add($pick)
    $open = New-Object System.Windows.Forms.Button
    $open.Name = "open"
    $open.Text = "Open the games folder"
    $open.SetBounds(174, 204, 170, 30)
    $open.Add_Click({
        try {
            [void][System.IO.Directory]::CreateDirectory($gamesFolder)
            Start-Process explorer.exe -ArgumentList ("`"" + $gamesFolder + "`"")
        } catch {}
    })
    $form.Controls.Add($open)
    $again = New-Object System.Windows.Forms.Button
    $again.Name = "again"
    $again.Text = "Look again"
    $again.SetBounds(392, 204, 96, 30)
    $again.DialogResult = [System.Windows.Forms.DialogResult]::Retry
    $form.Controls.Add($again)
    $quit = New-Object System.Windows.Forms.Button
    $quit.Name = "quit"
    $quit.Text = "Quit"
    $quit.SetBounds(496, 204, 88, 30)
    $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($quit)
    $form.AcceptButton = $again
    $form.CancelButton = $quit

    $result = Show-Form $form
    $form.Dispose()
    if ($result -eq [System.Windows.Forms.DialogResult]::Yes) { return "pick" }
    if ($result -eq [System.Windows.Forms.DialogResult]::Retry) { return "again" }
    return "quit"
}

# The file picker: the game's eboot.bin or its package, wherever they are.
function Select-GameFile {
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Choose your game's eboot.bin, or its dumped .pkg file"
    $dialog.Filter = "The game (eboot.bin, *.pkg)|eboot.bin;*.pkg|All files (*.*)|*.*"
    $dialog.CheckFileExists = $true
    if ([System.IO.Directory]::Exists($gamesFolder)) { $dialog.InitialDirectory = $gamesFolder }
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
            return $dialog.FileName
        }
        return $null
    } finally {
        $owner.Dispose()
        $dialog.Dispose()
    }
}

# The eboot.bin to run, or nothing when the player gives up.
function Resolve-Game {
    $script:selectionCancelled = $false
    $named = Setting "game"
    if ($named -ne "") {
        $eboot = Use-Path $named
        if ($eboot) { return $eboot }
        if ($selectionCancelled) { return $null }
        Say ("The settings path could not be used: " + $named + ". Looking in " + $gamesFolder) "Yellow"
    }
    while ($true) {
        $candidates = @(Get-GameCandidates $gamesFolder)
        if ($candidates.Count -gt 0) {
            $candidate = Select-GameCandidate $candidates
            if (-not $candidate) { return $null }
            $eboot = Use-Path $candidate.Path
            if ($eboot) { return $eboot }
        }
        $choice = Show-NotFound
        if ($choice -eq "pick") {
            $file = Select-GameFile
            if ($file) {
                $eboot = Use-Path $file
                if ($eboot) {
                    # An explicit external selection is remembered; the generic launcher still
                    # asks among multiple games unless game= names one specific dump.
                    $prefix = $gamesFolder.TrimEnd([char[]]"\/") + [System.IO.Path]::DirectorySeparatorChar
                    $inGames = $eboot.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
                    if (-not $inGames) { Save-Setting "game" $eboot }
                    return $eboot
                }
            }
        } elseif ($choice -ne "again") {
            return $null
        }
    }
}

# The Microsoft Visual C++ runtime, which the emulator is built against.
function Test-Runtime {
    $system = [System.Environment]::SystemDirectory
    foreach ($name in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                        "msvcp140_2.dll", "msvcp140_atomic_wait.dll")) {
        if (-not [System.IO.File]::Exists((Join-Path $system $name))) { return $false }
    }
    return $true
}

# --- the window -------------------------------------------------------------------------------
function Show-Menu($info) {
    $astro = Test-AstroProfile $info

    $form = New-Object System.Windows.Forms.Form
    $form.Text = $launcherName
    $form.ClientSize = New-Object System.Drawing.Size(560, 510)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

    $y = 14
    $title = New-Object System.Windows.Forms.Label
    $title.Text = "$launcherName - PC VR"
    $title.Font = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $title.SetBounds(16, $y, 520, 26)
    $form.Controls.Add($title)
    $y += 40

    # Explicit input profile; never inferred from a title or attached controller.
    $inputLabel = New-Object System.Windows.Forms.Label
    $inputLabel.Text = "Input profile"
    $inputLabel.SetBounds(16, $y + 4, 150, 24)
    $form.Controls.Add($inputLabel)
    $inputMode = New-Object System.Windows.Forms.ComboBox
    $inputMode.DropDownStyle = "DropDownList"
    [void]$inputMode.Items.Add("gamepad")
    [void]$inputMode.Items.Add("move")
    $inputMode.SelectedIndex = if ((Get-InputMode) -eq "move") { 1 } else { 0 }
    $inputMode.SetBounds(170, $y, 200, 26)
    $form.Controls.Add($inputMode)
    $y += 48

    # Resolution (Astro-only).
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Resolution of each eye"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $resolution = New-Object System.Windows.Forms.TrackBar
    $resolution.Enabled = $astro
    $resolution.Minimum = 0
    $resolution.Maximum = $widths.Count - 1
    $resolution.TickFrequency = 1
    $resolution.LargeChange = 1
    $resolution.SetBounds(12, $y, 530, 40)
    $current = 0
    [void][int]::TryParse((Setting "resolution" "2880"), [ref]$current)
    $index = [array]::IndexOf($widths, $current)
    if ($index -lt 0) { $index = 4 }
    $resolution.Value = $index
    $form.Controls.Add($resolution)
    $y += 42
    $resolutionText = New-Object System.Windows.Forms.Label
    $resolutionText.SetBounds(16, $y, 530, 38)
    $form.Controls.Add($resolutionText)
    $update = {
        $w = $widths[$resolution.Value]
        $h = EyeHeight $w
        $times = ($w * $h) / (1440.0 * 1536.0)
        $what = if ($w -eq 1440) { "the console's own, as a PlayStation 4 Pro draws it" } else { "{0:N2} times the pixels of the console" -f $times }
        $resolutionText.Text = if ($astro) { "$w x $h pixels an eye: $what. The game draws smaller by itself when the graphics card cannot keep up." } else { "Astro-only resolution settings are disabled for this title/version. The game chooses its own rendering resolution." }
    }
    $resolution.Add_ValueChanged($update)
    & $update
    $y += 46

    # Frame rate.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Frames a second, at most"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 24
    $fps = New-Object System.Windows.Forms.ComboBox
    $fps.Enabled = $astro
    $fps.DropDownStyle = "DropDownList"
    foreach ($cap in $caps) {
        $text = "$cap"
        if ($cap -eq 60) { $text = "60 (the console's own)" }
        [void]$fps.Items.Add($text)
    }
    $fps.SetBounds(16, $y, 200, 26)
    $index = [array]::IndexOf($caps, [int](Setting "fps" "60"))
    if ($index -lt 0) { $index = 3 }
    $fps.SelectedIndex = $index
    $form.Controls.Add($fps)
    $y += 32
    $fpsText = New-Object System.Windows.Forms.Label
    $fpsText.Text = "A frame lasts a whole number of the headset's refreshes, so the headset's refresh rate decides what is possible: at 120 Hz 120, 60, 40 or 30 frames a second, at 90 Hz 90, 45 or 30, at 72 Hz 72 or 36. Virtual Desktop sets the refresh rate (Settings > Streaming > Frame rate): choose 120 for 60 frames a second."
    if (-not $astro) { $fpsText.Text = "Astro-only frame pacing and time-step enhancements are disabled for this title/version. Runtime refresh rate and the game control presentation; compatibility and correct game speed still need testing." }
    $fpsText.SetBounds(16, $y, 530, 84)
    $form.Controls.Add($fpsText)
    $y += 88

    # Field of view.
    $label = New-Object System.Windows.Forms.Label
    $label.Text = "Field of view"
    $label.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
    $label.SetBounds(16, $y, 520, 20)
    $form.Controls.Add($label)
    $y += 22
    $fov = New-Object System.Windows.Forms.TrackBar
    $fov.Minimum = 14
    $fov.Maximum = 20
    $fov.TickFrequency = 1
    $fov.LargeChange = 1
    $fov.SetBounds(12, $y, 300, 40)
    $fov.Value = [math]::Max(14, [math]::Min(20, [int]([int](Setting "fov" "100") / 5)))
    $form.Controls.Add($fov)
    $fovText = New-Object System.Windows.Forms.Label
    $fovText.SetBounds(316, $y + 4, 230, 40)
    $form.Controls.Add($fovText)
    $ofPsvr = (Setting "fov_of" "headset") -eq "psvr"
    $updateFov = {
        $percent = $fov.Value * 5
        if ($percent -eq 100) {
            $fovText.Text = $(if ($ofPsvr) { "100%: PlayStation VR's own" } else { "100%: all that the headset shows" })
        } else {
            $fovText.Text = "$percent% of it: sharper, with a dark border"
        }
    }
    $fov.Add_ValueChanged($updateFov)
    & $updateFov
    $y += 46

    $again = New-Object System.Windows.Forms.CheckBox
    $again.Text = "Show this window at every start"
    $again.Checked = (Setting "menu" "1") -ne "0"
    $again.SetBounds(16, $y, 300, 24)
    $form.Controls.Add($again)

    $play = New-Object System.Windows.Forms.Button
    $play.Text = "Play"
    $play.SetBounds(360, $y - 2, 88, 30)
    $play.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $form.Controls.Add($play)
    $form.AcceptButton = $play
    $quit = New-Object System.Windows.Forms.Button
    $quit.Text = "Quit"
    $quit.SetBounds(456, $y - 2, 88, 30)
    $quit.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($quit)
    $form.CancelButton = $quit

    $result = Show-Form $form
    if ($result -ne [System.Windows.Forms.DialogResult]::OK) { $form.Dispose(); return $false }
    if ($astro) {
        Save-Setting "resolution" ($widths[$resolution.Value])
        Save-Setting "fps" ($caps[$fps.SelectedIndex])
    }
    Save-Setting "input_mode" ($inputMode.SelectedItem.ToString())
    Save-Setting "fov" ($fov.Value * 5)
    Save-Setting "menu" ($(if ($again.Checked) { "1" } else { "0" }))
    Read-Settings
    $form.Dispose()
    return $true
}

# The launcher owns these environment variables. Clear inherited values on every launch so
# switching title/profile cannot leak Astro settings or an old controller mode into a game.
function Get-ManagedEnvironmentNames {
    return @("SHADPS4_TITLE_RESOLUTION", "SHADPS4_TITLE_EYE_WIDTH", "SHADPS4_TITLE_TIMESTEP",
        "SHADPS4_VR_FPS_CAP", "SHADPS4_VR_FASTEST_PACE", "SHADPS4_VR_PACE",
        "SHADPS4_MOVE_LOCOMOTION", "SHADPS4_MOVE_LOCOMOTION_BUTTONS",
        "SHADPS4_VR_INPUT_MODE", "SHADPS4_VR_SHARPEN", "SHADPS4_MAX_MSAA", "SHADPS4_RESOLVE_AA",
        "SHADPS4_XR_HANDS", "SHADPS4_XR_PREDICT_MS", "SHADPS4_STICK_TOUCHPAD",
        "SHADPS4_VIRTUAL_SURROUND", "SHADPS4_VR_FOV", "SHADPS4_VR_FOV_OF", "SHADPS4_OPENXR",
        "SHADPS4_XR_PAUSE", "SHADPS4_XR_CONTROLLERS", "SHADPS4_XR_PAD_HAND", "SHADPS4_XR_WAIT")
}
function Test-AstroEnvironment([string]$name) {
    return $name -in @("SHADPS4_TITLE_RESOLUTION", "SHADPS4_TITLE_EYE_WIDTH", "SHADPS4_TITLE_TIMESTEP",
                       "SHADPS4_VR_FPS_CAP", "SHADPS4_VR_FASTEST_PACE", "SHADPS4_VR_PACE")
}
function Get-InputMode {
    $mode = (Setting "input_mode" "gamepad").ToLowerInvariant()
    if ($mode -notin @("gamepad", "move")) {
        throw "Invalid input_mode='$mode'. Choose input_mode=gamepad or input_mode=move in $SettingsFile."
    }
    return $mode
}
# Pure settings translation so the profile boundary is testable without launching a game.
function Get-LaunchEnvironment($info) {
    $values = [ordered]@{}
    $astro = Test-AstroProfile $info
    if ($astro) {
        $resolution = Setting "resolution" "2880"
        $dynamic = (Setting "dynamic" "1") -ne "0"
        if ($resolution -eq "game") {
            $values["SHADPS4_TITLE_RESOLUTION"] = "title"
        } else {
            $width = 0
            if (-not [int]::TryParse($resolution, [ref]$width)) { $width = 2880 }
            $smaller = @{ 816 = "3"; 960 = "4"; 1200 = "5" }
            if ($smaller.ContainsKey($width)) {
                $values["SHADPS4_TITLE_RESOLUTION"] = $smaller[$width]
            } else {
                $width = [math]::Max(1440, [math]::Min(4320, [int]([math]::Round($width / 8) * 8)))
                if ($width -gt 1440) { $values["SHADPS4_TITLE_EYE_WIDTH"] = "$width" }
                if (-not $dynamic) { $values["SHADPS4_TITLE_RESOLUTION"] = "6" }
            }
        }
        if ((Setting "real_time" "1") -eq "0") { $values["SHADPS4_TITLE_TIMESTEP"] = "0" }
        $values["SHADPS4_VR_FPS_CAP"] = Setting "fps" "60"
        $pace = Setting "pace"
        if ($pace -eq "1") {
            $values["SHADPS4_VR_FASTEST_PACE"] = "1"
            $values.Remove("SHADPS4_VR_FPS_CAP")
        } elseif ($pace -ne "" -and $pace -ne "2") { $values["SHADPS4_VR_PACE"] = $pace }
    }
    # General OpenXR/render/audio settings: no title-specific memory patches are requested here.
    $values["SHADPS4_VR_INPUT_MODE"] = Get-InputMode
    $locomotion = (Setting "move_locomotion" "legacy").ToLowerInvariant()
    if ($locomotion -notin @("legacy", "buttons", "directional")) {
        throw "Invalid move_locomotion; choose legacy, buttons or directional."
    }
    $values["SHADPS4_MOVE_LOCOMOTION"] = $locomotion
    $values["SHADPS4_MOVE_LOCOMOTION_BUTTONS"] = Setting "move_locomotion_buttons" "4,32,128,64,128,32"
    $values["SHADPS4_VR_SHARPEN"] = Setting "sharpen" "0.3"
    if ((Setting "msaa") -ne "") { $values["SHADPS4_MAX_MSAA"] = Setting "msaa" }
    if ((Setting "antialias" "1") -eq "0") { $values["SHADPS4_RESOLVE_AA"] = "0" }
    if ((Setting "hands" "1") -eq "0") { $values["SHADPS4_XR_HANDS"] = "0" }
    if ((Setting "predict_ms") -ne "") { $values["SHADPS4_XR_PREDICT_MS"] = Setting "predict_ms" }
    if ((Setting "stick_touchpad" "1") -eq "0") { $values["SHADPS4_STICK_TOUCHPAD"] = "0" }
    if ((Setting "surround" "1") -eq "0") { $values["SHADPS4_VIRTUAL_SURROUND"] = "0" }
    if ((Setting "fov" "100") -ne "100") { $values["SHADPS4_VR_FOV"] = Setting "fov" }
    if ((Setting "fov_of" "headset") -ne "psvr") { $values["SHADPS4_VR_FOV_OF"] = "headset" }
    if ((Setting "headset" "1") -eq "0") { $values["SHADPS4_OPENXR"] = "0" }
    if ((Setting "pause" "1") -eq "0") { $values["SHADPS4_XR_PAUSE"] = "0" }
    if ((Setting "controllers" "1") -eq "0") { $values["SHADPS4_XR_CONTROLLERS"] = "0" }
    if ((Setting "controller_hand" "right") -eq "left") { $values["SHADPS4_XR_PAD_HAND"] = "left" }
    $values["SHADPS4_XR_WAIT"] = Setting "wait" "60"
    foreach ($pair in $extraEnv) {
        $at = $pair.IndexOf("=")
        if ($at -lt 1) { continue }
        $name = $pair.Substring(0, $at).Trim()
        if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { throw "Invalid environment variable name '$name'." }
        if ($name -ieq "SHADPS4_VR_INPUT_MODE") { throw "Use input_mode=gamepad or input_mode=move, not env=SHADPS4_VR_INPUT_MODE." }
        if (-not $astro -and (Test-AstroEnvironment $name)) { continue }
        $values[$name] = $pair.Substring($at + 1)
    }
    return $values
}
function Set-LaunchEnvironment($info) {
    $values = Get-LaunchEnvironment $info
    foreach ($name in (Get-ManagedEnvironmentNames)) { Remove-Item -LiteralPath ("Env:" + $name) -ErrorAction SilentlyContinue }
    foreach ($name in $values.Keys) { Set-Item -LiteralPath ("Env:" + $name) -Value $values[$name] }
}
function Get-LaunchSummary($info) {
    $title = if ($info["TITLE"]) { $info["TITLE"] } else { "unknown title" }
    $serial = if ($info["TITLE_ID"]) { $info["TITLE_ID"] } else { "unknown ID" }
    $version = if ($info["APP_VER"]) { $info["APP_VER"] } else { "unknown version" }
    $profile = if (Test-AstroProfile $info) { "astro-cusa12392-01.00 (runtime code guard still required)" } else { "generic-experimental" }
    return "Title: $title | ID: $serial | Version: $version | Input: $env:SHADPS4_VR_INPUT_MODE | Enhancements: $profile"
}

$emulator = Join-Path $here "shadps4.exe"
if (-not [System.IO.File]::Exists($emulator)) {
    [void](Show-Box ("The emulator, shadps4.exe, is missing from`n" + $here + "`n`nUnzip the whole Any4Quest package again. (Built from the source: see README.md, Building.)") "OK" "Error")
    exit 1
}
if (-not (Test-Runtime)) {
    $answer = Show-Box "The emulator needs the Microsoft Visual C++ runtime, which is not installed on this PC.`n`nDownload its installer from Microsoft now? Run it, then start Play Any4Quest VR again." "YesNo" "Warning"
    if ($answer -eq "Yes") { Start-Process "https://aka.ms/vs/17/release/vc_redist.x64.exe" }
    exit 1
}

$game = Resolve-Game
if (-not $game) { exit 0 }
Say ("The game: " + $game)
$info = Get-GameInfo $game
if ($info.Count -eq 0) {
    Say "sce_sys\param.sfo is missing next to it: this is not a complete copy of the game, and the emulator may not know it." "Yellow"
} elseif (-not (Test-AstroProfile $info)) {
    Say "Experimental title/version: Astro-specific resolution, time-step and frame-pacing settings are disabled. Launching does not establish compatibility." "Yellow"
} elseif ([System.IO.Path]::GetDirectoryName($game).Length + 1 + $longestInside -gt 259) {
    [void](Show-Box ("The game is in`n" + [System.IO.Path]::GetDirectoryName($game) + "`n`nThat path is too long: some of the game's files have a path of more than 259 characters there, which the emulator cannot open, and the game would stop when it needs them. Move the folder somewhere with a shorter path, for example C:\Games\Any4Quest, and start again.") "OK" "Warning")
    exit 1
}

try { $null = Get-InputMode } catch {
    [void](Show-Box $_.Exception.Message "OK" "Error")
    exit 1
}
if (-not $NoMenu -and (Setting "menu" "1") -ne "0") {
    if (-not (Show-Menu $info)) { exit 0 }
}

try { Set-LaunchEnvironment $info } catch {
    [void](Show-Box $_.Exception.Message "OK" "Error")
    exit 1
}

# --- what is there ----------------------------------------------------------------------------
Say "$launcherName - PC VR" "Cyan"
$launchSummary = Get-LaunchSummary $info
Say $launchSummary "Cyan"
if ($env:SHADPS4_TITLE_EYE_WIDTH) {
    Say ("Each eye up to " + $env:SHADPS4_TITLE_EYE_WIDTH + " x " + (EyeHeight ([int]$env:SHADPS4_TITLE_EYE_WIDTH)) + ", at most " + $env:SHADPS4_VR_FPS_CAP + " frames a second.")
}
$runtime = ""
if ($env:XR_RUNTIME_JSON) {
    $runtime = $env:XR_RUNTIME_JSON
} else {
    try { $runtime = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction Stop).ActiveRuntime } catch {}
}
if ($runtime -eq "") {
    Say "No OpenXR runtime is set up on this PC: the game will only show on the monitor." "Yellow"
    Say "Virtual Desktop Streamer installs one (Options > OpenXR Runtime: VDXR)."
} else {
    Say "OpenXR runtime: $runtime"
    if ($runtime -match "virtualdesktop") {
        $streamer = Get-Process "VirtualDesktop.Streamer" -ErrorAction SilentlyContinue
        if (-not $streamer) {
            $exe = Join-Path (Split-Path -Parent (Split-Path -Parent $runtime)) "VirtualDesktop.Streamer.exe"
            if (Test-Path $exe) {
                Say "Starting Virtual Desktop Streamer..."
                Start-Process $exe
            } else {
                Say "Virtual Desktop Streamer is not running: start it, then connect from the headset." "Yellow"
            }
        }
    }
}
Say ""
Say "In the headset: connect Virtual Desktop to this PC. The game moves into the headset by itself."
if ($env:SHADPS4_OPENXR -ne "0" -and [int]$env:SHADPS4_XR_WAIT -gt 0) {
    Say ("The game waits up to " + $env:SHADPS4_XR_WAIT + " seconds for the headset before it starts on the monitor.")
}
if ($env:SHADPS4_VR_INPUT_MODE -eq "move") {
    Say "Input: experimental dual Move emulation from tracked OpenXR controllers. See README-ANY4QUEST.md."
    Say "Use tracked controllers, not inferred hand-only poses. Game support still requires validation."
} else {
    Say "The DualSense: connect it to THIS PC (USB cable, or Bluetooth paired with the PC). Paired with"
    Say "the headset, it reaches the PC through Virtual Desktop without motion sensors or touchpad."
    Say "Where it is in the game comes from your hands: hand tracking on in the headset, and in"
    Say "Virtual Desktop's settings hand tracking forwarded to the PC."
    Say "Hold OPTIONS for a second (or press the PS button) to reset the view."
    if (Test-AstroProfile $info) {
        Say "No gamepad: the headset's own controllers play (A jump, B punch, right stick = touchpad,"
        Say "press both sticks in to reset the view)."
    } else {
        Say "Without a gamepad, headset controllers can stand in for one. Buttons depend on the game."
    }
}
Say "Close the game's window to quit."
Say ""
# The emulator asks Windows for about 14 GB at once (the console's memory, and what the larger
# pictures take). Where Windows cannot promise that much, the emulator stops as it starts.
$memoryShort = $false
try {
    $spare = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).FreeVirtualMemory * 1024.0
    if ($spare -lt 16GB) {
        $memoryShort = $true
        Say ("Windows has " + (Gigabytes $spare) + " of memory left to hand out, and the emulator asks for about 14 GB: if the game does not start, close other programs and start again.") "Yellow"
        Say ""
    }
} catch {}

# --- run --------------------------------------------------------------------------------------
$logDir = Join-Path $here "user\log"
$log = Join-Path $logDir "shad_log.txt"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
@((Get-Date -Format o), $launchSummary, ("Game: " + $game)) |
    Set-Content -LiteralPath (Join-Path $logDir "launcher.txt") -Encoding UTF8
if (Test-Path $log) { Copy-Item $log (Join-Path $logDir "shad_log.prev.txt") -Force }

# (In this console, with what it prints kept out of the way: a window style given here would
# also be the game window's.)
$startedAt = Get-Date
$process = Start-Process -FilePath $emulator -ArgumentList @("-g", "`"$game`"") -WorkingDirectory $here `
    -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $logDir "console.txt") `
    -RedirectStandardError (Join-Path $logDir "console-errors.txt")
# (Without this the exit code is not to be had later.)
$null = $process.Handle
$position = 0
$shown = @{}
function Show-Log {
    if (-not (Test-Path $log)) { return }
    try {
        $stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    } catch { return }
    try {
        if ($stream.Length -lt $script:position) { $script:position = 0 }
        [void]$stream.Seek($script:position, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { break }
            if ($line -match '^\[Core\.Vr\] <(Info|Warning)> \([^)]*\) \S+ (?:\w+: )?(.*)$') {
                $warning = $Matches[1] -eq "Warning"
                $text = $Matches[2]
                # The lines that repeat every few seconds only once in a while.
                if ($text -match '^(The title (has|now takes) the player|Hands:|Virtual headset connected)') { continue }
                if ($text -match '^Headset: the title delivered') {
                    $script:reports++
                    if (($script:reports % 6) -ne 1) { continue }
                }
                if ($text -match '^Controllers: standing in') {
                    if (($script:reports % 6) -ne 1) { continue }
                }
                if ($warning) { Say ("  " + $text) "Yellow" } else { Say ("  " + $text) }
            } elseif ($line -match '^\[Input\] <Info> \([^)]*\) \S+ (?:\w+: )?(Controller .*)$') {
                Say ("  " + $Matches[1])
            } elseif ($line -match '^\[Core\] <Info> \([^)]*\) \S+ (?:\w+: )?(The title draws at up to .*|The scene is drawn at .*|Frames are given .*)$') {
                Say ("  " + $Matches[1])
            } elseif ($line -match '<Critical>.*?: (.*)$') {
                $text = $Matches[1]
                if (-not $shown.ContainsKey($text)) { $shown[$text] = 1; Say ("  ! " + $text) "Red" }
            }
        }
        $script:position = $stream.Position
    } finally { $stream.Dispose() }
}
$reports = 0
while (-not $process.HasExited) {
    Start-Sleep -Milliseconds 700
    Show-Log
}
Show-Log
Say ""
if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
    Say ("The emulator ended with code " + $process.ExitCode + ". Its log is $log") "Yellow"
    if ($memoryShort -and ((Get-Date) - $startedAt).TotalSeconds -lt 30) {
        Say "It stopped as it started, and Windows was short of memory then (see above): close other programs and start again."
    }
    Read-Host "Press Enter to close"
} else {
    Say "The game was closed."
    Start-Sleep -Seconds 2
}
