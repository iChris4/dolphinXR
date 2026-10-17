# Samples the running DolphinXR on a Meta Quest over adb for -Seconds: the VrApi frame stats
# (FPS, App = GPU time per frame, GPU% and CPU% utilization), the Dolphin perf log lines written
# in that window, errors, and the busiest threads. Clears logcat first: read
# "adb logcat -d --pid <pid>" before sampling for what was logged since launch.
param(
    [int]$Seconds = 15,
    [string]$Package = "org.dolphinemu.dolphinemu.quest.debug",
    [string]$Adb = "",
    [string]$Lines = 'XRPacing:|VKPERF:|GPUWait:|EFBCopyRAM:|NonBlockingPerfQuery|EFBFoveation:|EFBReinterpret:'
)

if (-not $Adb) {
    $candidates = @((Get-Command adb -ErrorAction SilentlyContinue).Source,
                    "$env:LOCALAPPDATA\Android\platform-tools\adb.exe",
                    "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe")
    $Adb = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $Adb) { throw "adb not found; pass -Adb" }
}

$appPid = "$(& $Adb shell "pidof $Package")".Trim()
if (-not $appPid) { "app not running"; exit 1 }
"pid=$appPid  sampling ${Seconds}s"
& $Adb logcat -c
$topAll = @(& $Adb shell "top -H -b -n 2 -d $Seconds -p $appPid -o TID,CMD,%CPU,CPU -s 3")
$log = & $Adb shell "logcat -d --pid $appPid"

"== VrApi"
$stats = $log | Select-String 'VrApi.*FPS=' | ForEach-Object {
    if ($_ -match 'FPS=(\d+)/.*App=([\d.]+)ms.*GPU%=([\d.]+),CPU%=([\d.]+)') {
        [pscustomobject]@{ fps = [int]$matches[1]; app = [double]$matches[2]; gpu = [double]$matches[3]; cpu = [double]$matches[4] }
    }
}
if ($stats) {
    "n={0} fps avg={1:N1} min={2} | App avg={3:N2}ms | GPU% avg={4:N2} | CPU% avg={5:N2}" -f $stats.Count,
        ($stats | Measure-Object fps -Average).Average, ($stats | Measure-Object fps -Minimum).Minimum,
        ($stats | Measure-Object app -Average).Average, ($stats | Measure-Object gpu -Average).Average,
        ($stats | Measure-Object cpu -Average).Average
}
"== Dolphin perf lines"
$log | Select-String $Lines | ForEach-Object { $_.Line -replace "^.*?($Lines)", '$1' }
"== errors"
$log | Select-String ' E Dolphin| F |FATAL|owner_violations=[1-9]|stalled|Unknown Opcode' | Select-Object -First 10
"== threads (CPU % over ${Seconds}s)"
$header = @(0..($topAll.Count - 1) | Where-Object { $topAll[$_] -match '^\s*TID' })
if ($header) { $topAll[($header[-1])..([Math]::Min($header[-1] + 6, $topAll.Count - 1))] }
