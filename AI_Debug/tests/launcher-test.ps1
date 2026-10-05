# Asset-free tests: load launcher functions through the PowerShell parser, not its Windows UI.
# Windows: powershell -NoProfile -ExecutionPolicy Bypass -File AI_Debug/tests/launcher-test.ps1
# Also runs with pwsh on other platforms. No actual game, emulator, headset or PkgTool needed.
$ErrorActionPreference = "Stop"
$top = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$launcher = Join-Path $top "pc-vr/launch.ps1"
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($launcher, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { $parseErrors | ForEach-Object { Write-Error $_ }; exit 1 }
foreach ($function in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}
$unpackFolder = ".unpacking"
$madeFor = "CUSA12392"
$LauncherProfile = "astro"
$base = Join-Path ([System.IO.Path]::GetTempPath()) ("any4quest-launcher-test-" + [guid]::NewGuid())
[void][System.IO.Directory]::CreateDirectory($base)
$SettingsFile = Join-Path $base "[test] settings.txt"
$failed = 0
$checks = 0
function Check([string]$what, $got, $want) {
    $script:checks++
    if ("$got" -eq "$want") { "ok    $what" } else { $script:failed++; "FAIL  $what`n      got:  $got`n      want: $want" }
}
function Check-Throws([string]$what, [scriptblock]$action) {
    $threw = $false
    try { & $action | Out-Null } catch { $threw = $true }
    Check $what $threw $true
}
# A tiny original SFO fixture, containing strings only; no game assets or real executable.
function Make-Sfo([string]$path, [string]$serial, [string]$version, [string]$title) {
    $entries = [ordered]@{ TITLE_ID = $serial; APP_VER = $version; TITLE = $title }
    $keys = New-Object System.IO.MemoryStream
    $data = New-Object System.IO.MemoryStream
    $index = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($index)
    foreach ($key in $entries.Keys) {
        $keyBytes = [System.Text.Encoding]::UTF8.GetBytes($key + [char]0)
        $valueBytes = [System.Text.Encoding]::UTF8.GetBytes($entries[$key] + [char]0)
        $writer.Write([uint16]$keys.Length); $writer.Write([uint16]0x0204)
        $writer.Write([int]$valueBytes.Length); $writer.Write([int]$valueBytes.Length)
        $writer.Write([int]$data.Length)
        $keys.Write($keyBytes, 0, $keyBytes.Length); $data.Write($valueBytes, 0, $valueBytes.Length)
    }
    $file = [System.IO.File]::Create($path)
    $header = New-Object System.IO.BinaryWriter($file)
    try {
        $header.Write([uint32]0x46535000); $header.Write([uint32]0x00000101)
        $header.Write([int](20 + $index.Length)); $header.Write([int](20 + $index.Length + $keys.Length))
        $header.Write([int]$entries.Count)
        $header.Write($index.ToArray()); $header.Write($keys.ToArray()); $header.Write($data.ToArray())
    } finally { $header.Dispose(); $writer.Dispose(); $keys.Dispose(); $data.Dispose() }
}
function Make-Game([string]$folder, [string]$serial = "", [string]$version = "01.00", [string]$title = "Synthetic game") {
    [void][System.IO.Directory]::CreateDirectory($folder)
    [System.IO.File]::WriteAllText((Join-Path $folder "eboot.bin"), "synthetic test fixture; not executable")
    if ($serial) {
        [void][System.IO.Directory]::CreateDirectory((Join-Path $folder "sce_sys"))
        Make-Sfo (Join-Path $folder "sce_sys/param.sfo") $serial $version $title
    }
}
function Make-Package([string]$path, [string]$contentId, [int]$size) {
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($path))
    $bytes = New-Object byte[] $size
    $bytes[0] = 0x7F; $bytes[1] = 0x43; $bytes[2] = 0x4E; $bytes[3] = 0x54
    [System.Text.Encoding]::ASCII.GetBytes($contentId).CopyTo($bytes, 0x40)
    [System.IO.File]::WriteAllBytes($path, $bytes)
}
$savedEnvironment = @{}
foreach ($name in (Get-ManagedEnvironmentNames)) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    $g = Join-Path $base "a/games"
    $astroPath = Join-Path $g "CUSA12392/eboot.bin"
    Make-Game (Split-Path -Parent $astroPath) "CUSA12392" "01.00" "ASTRO BOT Rescue Mission"
    Check "plain games folder" (Find-Game $g) $astroPath
    $info = Get-GameInfo $astroPath
    Check "SFO serial" $info["TITLE_ID"] "CUSA12392"
    Check "SFO version" $info["APP_VER"] "01.00"
    Check "SFO title" $info["TITLE"] "ASTRO BOT Rescue Mission"
    Check "known Astro profile" (Test-AstroProfile $info) $true
    Check "wrong version not known Astro" (Test-AstroProfile @{TITLE_ID="CUSA12392"; APP_VER="01.01"}) $false
    Check "wrong ID not known Astro" (Test-AstroProfile @{TITLE_ID="CUSA99999"; APP_VER="01.00"}) $false
    Check "missing metadata not known Astro" (Test-AstroProfile @{}) $false

    $g = Join-Path $base "b/games"
    $nestedPath = Join-Path $g "[ABC] my dump (1)/title/eboot.bin"
    Make-Game (Split-Path -Parent $nestedPath) "CUSA12392"
    Check "nested path with spaces and brackets" (Find-Game $g) $nestedPath
    $g = Join-Path $base "c/games"
    $otherPath = Join-Path $g "Another/eboot.bin"
    $preferredPath = Join-Path $g "Zzz/eboot.bin"
    Make-Game (Split-Path -Parent $otherPath) "CUSA99999" "02.01" "Other synthetic title"
    Make-Game (Split-Path -Parent $preferredPath) "CUSA12392"
    $candidates = @(Get-GameCandidates $g)
    Check "discovers both titles" $candidates.Count 2
    Check "legacy prefers unique known Astro" (Find-Game $g) $preferredPath
    $LauncherProfile = "any"
    Check "generic never auto-picks among titles" (Get-PreferredCandidate $candidates) ""
    Check "generic helper refuses ambiguity" (Find-Game $g) ""
    Check "label includes ID version and path" (($candidates | Where-Object { $_.Path -eq $otherPath }).Label -like "*CUSA99999*02.01*$otherPath") $true
    $LauncherProfile = "astro"
    Make-Game (Join-Path $g "Second Astro") "CUSA12392"
    Check "multiple Astro copies require selection" (Find-Game $g) ""

    $g = Join-Path $base "d/games"
    Make-Game (Join-Path $g "CUSA12392/.unpacking/files/uroot") "CUSA12392"
    Check "ignores partial unpacking" @(Get-GameCandidates $g).Count 0
    $g = Join-Path $base "e/games"
    $package = Join-Path $g "[ABC] game.pkg"
    Make-Package $package "EP9000-CUSA12392_00-PLATFORMERVR00EU" 4096
    Make-Package (Join-Path $g "update.pkg") "EP9000-CUSA12392_00-PLATFORMERVR00EU" 1024
    [System.IO.File]::WriteAllText((Join-Path $g "invalid.pkg"), ("x" * 300))
    Check "package content ID" (Read-PackageId $package) "EP9000-CUSA12392_00-PLATFORMERVR00EU"
    Check "invalid package rejected" (Read-PackageId (Join-Path $g "invalid.pkg")) ""
    Check "all valid packages discovered" @(Find-Packages $g).Count 2
    Check "ambiguous packages not chosen by size" (Get-PreferredCandidate @(Get-GameCandidates $g)) ""
    Check "packages are labeled, not compatibility inferred" (@(Get-GameCandidates $g)[0].Label -like "Package (verify base game*") $true
    $g = Join-Path $base "f/games"
    Make-Game (Join-Path $g "1/2/3/4") "CUSA12392"
    Check "search depth limited" @(Get-GameCandidates $g).Count 0
    Check "missing folder" @(Get-GameCandidates (Join-Path $base "nowhere")).Count 0
    Check "folder path resolves" (Use-Path (Join-Path $base "a")) $astroPath
    Check "explicit eboot resolves" (Use-Path $astroPath) $astroPath
    Check "nonexistent file rejected" (Use-Path (Join-Path $base "missing.bin")) ""
    $notGame = Join-Path $base "other.exe"
    [System.IO.File]::WriteAllText($notGame, "not a game")
    Check "arbitrary existing file rejected" (Use-Path $notGame) ""

    # Cancelling an ambiguous named folder must not fall back to a different game.
    $originalSelect = (Get-Command Select-GameCandidate).ScriptBlock
    $originalNotFound = (Get-Command Show-NotFound).ScriptBlock
    $settings = [ordered]@{ game = (Join-Path $base "c/games") }
    $gamesFolder = Join-Path $base "a/games"
    try {
        function Select-GameCandidate($candidates) { $script:selectionCancelled = $true; return $null }
        function Show-NotFound { throw "Cancellation must not open another selection flow" }
        Check "cancel does not fall back to another title" (Resolve-Game) ""
    } finally {
        Set-Item -Path Function:Select-GameCandidate -Value $originalSelect
        Set-Item -Path Function:Show-NotFound -Value $originalNotFound
    }

    # Missing SFO and corrupt metadata cannot opt into title patches.
    $g = Join-Path $base "g"
    Make-Game $g
    Check "missing SFO" (Get-GameInfo (Join-Path $g "eboot.bin")).Count 0
    $badSfo = Join-Path $base "bad.sfo"
    [System.IO.File]::WriteAllBytes($badSfo, [byte[]](1, 2, 3))
    Check "truncated SFO" (Read-Sfo $badSfo).Count 0

    $settings = [ordered]@{}; $extraEnv = @()
    $astroInfo = @{TITLE_ID="CUSA12392"; APP_VER="01.00"; TITLE="ASTRO BOT Rescue Mission"}
    $genericInfo = @{TITLE_ID="CUSA99999"; APP_VER="02.01"; TITLE="Other title"}
    $values = Get-LaunchEnvironment $astroInfo
    Check "default input remains gamepad" $values["SHADPS4_VR_INPUT_MODE"] "gamepad"
    Check "Astro eye width unchanged" $values["SHADPS4_TITLE_EYE_WIDTH"] "2880"
    Check "Astro FPS unchanged" $values["SHADPS4_VR_FPS_CAP"] "60"
    Check "general FOV default" $values["SHADPS4_VR_FOV_OF"] "headset"
    Check "general wait default" $values["SHADPS4_XR_WAIT"] "60"
    $settings["input_mode"] = "move"
    Check "explicit Move translation" (Get-LaunchEnvironment $genericInfo)["SHADPS4_VR_INPUT_MODE"] "move"
    $settings["input_mode"] = "automatic"
    Check-Throws "invalid input fails closed" { Get-LaunchEnvironment $genericInfo }
    $settings["input_mode"] = ""
    Check-Throws "empty input fails closed" { Get-InputMode }
    $settings["input_mode"] = "gamepad"
    $extraEnv = @("SHADPS4_VR_INPUT_MODE=move")
    Check-Throws "raw env cannot bypass explicit mode" { Get-LaunchEnvironment $genericInfo }
    $extraEnv = @("BAD NAME=value")
    Check-Throws "invalid env name rejected" { Get-LaunchEnvironment $genericInfo }

    $settings = [ordered]@{ input_mode="move"; resolution="3600"; dynamic="0"; fps="120"; real_time="0"; pace="1"; fov="90"; msaa="2" }
    $extraEnv = @("SHADPS4_TITLE_EYE_WIDTH=4320", "shadps4_vr_fps_cap=120")
    foreach ($identity in @($genericInfo, @{}, @{TITLE_ID="CUSA12392"; APP_VER="01.01"})) {
        $values = Get-LaunchEnvironment $identity
        foreach ($name in (Get-ManagedEnvironmentNames | Where-Object { Test-AstroEnvironment $_ })) {
            Check "generic/missing/wrong version excludes $name" $values.Contains($name) $false
        }
        Check "generic still has shared FOV" $values["SHADPS4_VR_FOV"] "90"
        Check "generic still has shared MSAA" $values["SHADPS4_MAX_MSAA"] "2"
    }
    $settings["move_locomotion"] = "directional"
    Check "directional stick profile" (Get-LaunchEnvironment $genericInfo)["SHADPS4_MOVE_LOCOMOTION"] "directional"
    $settings["move_locomotion"] = "bad"
    Check-Throws "invalid stick profile rejected" { Get-LaunchEnvironment $genericInfo }
    $settings.Remove("move_locomotion")
    Check "stick default is legacy" (Get-LaunchEnvironment $genericInfo)["SHADPS4_MOVE_LOCOMOTION"] "legacy"
    $extraEnv = @()
    Set-LaunchEnvironment $astroInfo
    Check "Astro launch sets requested width" $env:SHADPS4_TITLE_EYE_WIDTH "3600"
    Check "Astro pace=1 applied" $env:SHADPS4_VR_FASTEST_PACE "1"
    Set-LaunchEnvironment $genericInfo
    Check "switch to generic clears width" $env:SHADPS4_TITLE_EYE_WIDTH ""
    Check "switch to generic clears pace" $env:SHADPS4_VR_FASTEST_PACE ""
    Check "switch to generic clears timestep" $env:SHADPS4_TITLE_TIMESTEP ""
    Check "Move mode reaches child environment" $env:SHADPS4_VR_INPUT_MODE "move"
    Check "summary includes title and input" ((Get-LaunchSummary $genericInfo) -like "*Other title*CUSA99999*02.01*move*generic-experimental*") $true
    $settings = [ordered]@{}; $extraEnv = @()
    Set-LaunchEnvironment $astroInfo
    Check "switch back restores gamepad" $env:SHADPS4_VR_INPUT_MODE "gamepad"
    Check "switch back clears optional FOV" $env:SHADPS4_VR_FOV ""
    Check "switch back clears optional MSAA" $env:SHADPS4_MAX_MSAA ""
    $settings["resolution"] = "game"
    Check "Astro native resolution retained" (Get-LaunchEnvironment $astroInfo)["SHADPS4_TITLE_RESOLUTION"] "title"
    $settings["resolution"] = "816"
    Check "Astro legacy small resolution retained" (Get-LaunchEnvironment $astroInfo)["SHADPS4_TITLE_RESOLUTION"] "3"
    $settings["resolution"] = "9999"
    Check "Astro maximum width clamp" (Get-LaunchEnvironment $astroInfo)["SHADPS4_TITLE_EYE_WIDTH"] "4320"

    Save-Setting "input_mode" "move"
    Save-Setting "game" $otherPath
    Read-Settings
    Check "settings persist explicit mode" (Get-InputMode) "move"
    Check "settings persist literal game path" (Setting "game") $otherPath
    Save-Setting "input_mode" "gamepad"
    Read-Settings
    Check "settings update mode" (Get-InputMode) "gamepad"
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    [System.IO.Directory]::Delete($base, $true)
}
"checks: $checks; failed: $failed"
if ($failed -gt 0) { exit 1 }
