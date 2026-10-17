# Unattended performance run of DolphinXR on a Meta Quest over adb: restarts the app on a game,
# loads a savestate, lets it settle, then samples it (quest-perf-sample.ps1). Needs a debug APK
# (the savestate broadcast is a debug-build hook) built with build-quest.ps1 -HandsOptional, so
# that adb can start the app while the controllers sleep.
#
#   .\quest-perf-run.ps1 -Apk <app-quest-debug.apk> -GameId RMCP01 -Game WII/MarioKart.iso -NoSample
#   .\quest-perf-run.ps1 -GameId RMCP01 -Game WII/MarioKart.iso -Slot 1 -Gfx "VR/FoveateEFB=False"
#   .\quest-perf-run.ps1 -Restore -Apk <normal app-quest-debug.apk>
#
# -Game: the disc image, relative to the headset's Documents folder.
# -Gfx "Section/Key=Value": sets a key of Config/GFX.ini for this run.
# -GameIni "Section/Key=Value": sets a key of the user GameSettings/<GameId>.ini for this run. A
#   game INI (user or Sys, such as GameSettings/RMC.ini) overrides GFX.ini.
# -Apk: installs that APK first, then lets the app start once (it copies Sys on first launch).
# The first run saves the headset's own GFX.ini and game INIs in -WorkDir. Every run starts from
# them, and -Restore puts them back byte for byte and turns the unattended mode off.
param(
    [string]$GameId = "",
    [string]$Game = "",
    [int]$Slot = 1,
    [int]$Settle = 30,
    [int]$Seconds = 15,
    [string[]]$Gfx = @(),
    [string[]]$GameIni = @(),
    [string]$Apk = "",
    [switch]$NoSample,
    [switch]$Restore,
    [string]$Package = "org.dolphinemu.dolphinemu.quest.debug",
    [string]$WorkDir = (Join-Path $env:TEMP "dolphinxr-perf"),
    [string]$Adb = ""
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Adb) {
    $candidates = @((Get-Command adb -ErrorAction SilentlyContinue).Source,
                    "$env:LOCALAPPDATA\Android\platform-tools\adb.exe",
                    "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe")
    $Adb = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $Adb) { throw "adb not found; pass -Adb" }
}

$files = "/sdcard/Android/data/$Package/files"
$original = Join-Path $WorkDir "original"
New-Item -ItemType Directory -Force $original | Out-Null

# The headset's own file, saved once: <name>, or <name>.none when the headset has none.
function Save-Original([string]$remote, [string]$name) {
    $local = Join-Path $original $name
    if ((Test-Path $local) -or (Test-Path "$local.none")) { return }
    & $Adb pull $remote $local 2>&1 | Out-Null
    if (-not (Test-Path $local)) { New-Item -ItemType File "$local.none" | Out-Null }
}

function Restore-Original([string]$remote, [string]$name) {
    $local = Join-Path $original $name
    if (Test-Path $local) { & $Adb push $local $remote 2>&1 | Out-Null }
    elseif (Test-Path "$local.none") { & $Adb shell "rm -f '$remote'" | Out-Null }
}

# Pushes the saved original with the keys set (LF line endings, as the app writes them), or the
# original itself when no key is set.
function Push-Ini([string]$remote, [string]$name, [string[]]$keys) {
    if ($keys.Count -eq 0) { Restore-Original $remote $name; return }
    $local = Join-Path $original $name
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path $local) { foreach ($l in [IO.File]::ReadAllLines($local)) { $lines.Add($l) } }
    foreach ($kv in $keys) {
        $section, $rest = $kv -split '/', 2
        $key, $value = $rest -split '=', 2
        $start = $lines.IndexOf("[$section]")
        if ($start -lt 0) { $lines.Add("[$section]"); $start = $lines.Count - 1 }
        $end = $start + 1
        while ($end -lt $lines.Count -and -not $lines[$end].StartsWith('[')) { $end++ }
        $line = "$key = $value"
        $at = -1
        for ($i = $start + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match "^\s*$([regex]::Escape($key))\s*=") { $at = $i; break }
        }
        if ($at -ge 0) { $lines[$at] = $line } else { $lines.Insert($end, $line) }
        "$name [$section] $line"
    }
    $edited = Join-Path $WorkDir $name
    [IO.File]::WriteAllText($edited, ($lines -join "`n") + "`n")
    & $Adb push $edited $remote 2>&1 | Out-Null
}

function Install-Apk([string]$path) {
    "install $path"
    # -d: an APK of an older commit has a lower versionCode.
    & $Adb install -r -d $path 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 1
    # The first launch after an install copies Sys; EmulationActivity started directly would read
    # the config before it exists. Let MainActivity finish that first.
    & $Adb shell "am start -n $Package/org.dolphinemu.dolphinemu.ui.main.MainActivity" | Out-Null
    Start-Sleep -Seconds 12
    & $Adb shell "am force-stop $Package" | Out-Null
    Start-Sleep -Seconds 2
}

& $Adb shell "am force-stop $Package" | Out-Null
Start-Sleep -Seconds 2

if ($Restore) {
    if ($Apk) { Install-Apk $Apk }
    Restore-Original "$files/Config/GFX.ini" "GFX.ini"
    foreach ($saved in Get-ChildItem $original -Filter "GameSettings_*") {
        $id = $saved.Name -replace '^GameSettings_', '' -replace '\.ini(\.none)?$', ''
        Restore-Original "$files/GameSettings/$id.ini" "GameSettings_$id.ini"
    }
    & $Adb shell "setprop debug.oculus.guardian_pause 0; am broadcast -a com.oculus.vrpowermanager.automation_disable" | Out-Null
    "restored the saved GFX.ini and game INIs; unattended mode off"
    exit 0
}

if (-not $GameId -or -not $Game) { throw "-GameId and -Game are required" }
if ($Apk) { Install-Apk $Apk }

Save-Original "$files/Config/GFX.ini" "GFX.ini"
Save-Original "$files/GameSettings/$GameId.ini" "GameSettings_$GameId.ini"
Push-Ini "$files/Config/GFX.ini" "GFX.ini" $Gfx
Push-Ini "$files/GameSettings/$GameId.ini" "GameSettings_$GameId.ini" $GameIni

# Keep the headset awake and the guardian quiet for unattended runs.
& $Adb shell "input keyevent KEYCODE_WAKEUP; am broadcast -a com.oculus.vrpowermanager.prox_close; setprop debug.oculus.guardian_pause 1" | Out-Null
Start-Sleep -Seconds 2

$encode = { param($s) [Uri]::EscapeDataString($s).Replace('(', '%28').Replace(')', '%29') }
$gamePath = $Game -replace '\\', '/'
$folder = & $encode ("Documents/" + (Split-Path $gamePath -Parent).Replace('\', '/'))
$uri = "content://com.android.externalstorage.documents/tree/primary%3A$folder/document/primary%3A$folder%2F" +
       (& $encode (Split-Path $gamePath -Leaf))

& $Adb logcat -c
& $Adb shell "am start -n $Package/org.dolphinemu.dolphinemu.activities.EmulationActivity -c com.oculus.intent.category.VR --esa SelectedGames '$uri'" | Out-Null
"launched $Game; waiting for boot"
$booted = $false
for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Seconds 1
    $appPid = "$(& $Adb shell "pidof $Package")".Trim()
    if ($appPid -and (& $Adb shell "logcat -d --pid $appPid" | Select-String 'XRPacing:')) { $booted = $true; break }
}
if (-not $booted) { "boot not detected"; exit 1 }
Start-Sleep -Seconds 3
& $Adb shell "am broadcast -a org.dolphinemu.dolphinemu.SAVESTATE --es action load --ei slot $Slot" | Out-Null
"pid=$appPid slot $Slot loaded; settling ${Settle}s"
Start-Sleep -Seconds $Settle
if (-not $NoSample) { & "$scriptDir\quest-perf-sample.ps1" -Seconds $Seconds -Package $Package -Adb $Adb }
