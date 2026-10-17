# -TradeCpuForGpu: com.oculus.trade_cpu_for_gpu_amount for this APK (Quest 3/3S): 1 = one GPU
# performance level more and one CPU level less, -1 the reverse, 0 neither. Default: the
# Gradle default (-1).
#
# -HandsOptional: test APK that declares optional hand tracking, so adb can launch it while the
# controllers are asleep (no "controller required" dialog). Not for playing: see
# app/src/questHandsOptional/AndroidManifest.xml.
param(
    [ValidateSet(-1, 0, 1)]
    [Nullable[int]]$TradeCpuForGpu = $null,
    [switch]$HandsOptional
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$javaHome = "C:\Program Files\Java\jdk-17"
$androidRoot = "C:\Users\Christophe\AppData\Local\Android"
$androidHome = Join-Path $androidRoot "Sdk"
$ndkHome = Join-Path $androidRoot "android-ndk-r29"
$vsCmakeBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$vsNinjaBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\CommonExtensions\Microsoft\CMake\Ninja"

if (-not (Test-Path (Join-Path $javaHome "bin\java.exe"))) {
    throw "JDK 17 not found at $javaHome"
}

if (-not (Test-Path $androidHome)) {
    throw "Android SDK root not found at $androidHome"
}

if (-not (Test-Path $ndkHome)) {
    throw "Android NDK not found at $ndkHome"
}

$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $androidHome
$env:ANDROID_SDK_ROOT = $androidHome
$env:ANDROID_NDK_HOME = $ndkHome
$pathPrefixes = @("$javaHome\bin", "$androidHome\platform-tools")
if (Test-Path (Join-Path $vsCmakeBin "cmake.exe")) {
    $pathPrefixes += $vsCmakeBin
}
if (Test-Path (Join-Path $vsNinjaBin "ninja.exe")) {
    $pathPrefixes += $vsNinjaBin
}
$env:Path = (($pathPrefixes -join ";") + ";" + $env:Path)

Push-Location $scriptDir
try {
    $gradleArgs = @("app:assembleQuestDebug")
    if ($null -ne $TradeCpuForGpu) {
        $gradleArgs += "-PquestTradeCpuForGpu=$TradeCpuForGpu"
    }
    if ($HandsOptional) {
        $gradleArgs += "-PquestHandsOptional=1"
    }
    & ".\gradlew.bat" @gradleArgs
}
finally {
    Pop-Location
}
