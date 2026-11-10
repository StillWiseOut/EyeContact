# Builds EyeContact.aex and optionally installs it for Premiere Pro / After Effects.
#
#   .\build.ps1 -AeSdk "D:\SDK\AfterEffectsSDK" -MaxineSdk "D:\SDK\MAXINE-AR-SDK"
#   .\build.ps1 -AeSdk ... -MaxineSdk ... -Install      (run PowerShell as Administrator)
param(
    [Parameter(Mandatory = $true)] [string] $AeSdk,
    [Parameter(Mandatory = $true)] [string] $MaxineSdk,
    [ValidateSet("Release", "Debug")] [string] $Config = "Release",
    # Keep this short: MSVC fails with "Invalid argument" when build paths exceed 260 characters.
    [string] $BuildDir = "",
    [switch] $Install
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$buildDir = if ($BuildDir) { $BuildDir } else { Join-Path $root "build" }

# Find CMake: PATH first, then the copy bundled with Visual Studio / Build Tools.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Visual Studio / Build Tools not found. Install 'Desktop development with C++' (see README.md)."
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio found, but without the C++ x64 toolset." }
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) { throw "CMake component missing in $vs. Add 'C++ CMake tools for Windows'." }
}
Write-Host "CMake: $cmake"

& $cmake -S $root -B $buildDir -A x64 "-DAE_SDK_DIR=$AeSdk" "-DMAXINE_AR_SDK_DIR=$MaxineSdk"
if ($LASTEXITCODE) { throw "CMake configure failed" }
& $cmake --build $buildDir --config $Config
if ($LASTEXITCODE) { throw "Build failed" }

$aex = Join-Path $buildDir "$Config\EyeContact.aex"
Write-Host "Built: $aex" -ForegroundColor Green

if ($Install) {
    # Shared plug-in folder read by both Premiere Pro and After Effects.
    $dest = Join-Path ${env:ProgramFiles} "Adobe\Common\Plug-ins\7.0\MediaCore"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item $aex $dest -Force
    Write-Host "Installed to $dest. Restart Premiere Pro." -ForegroundColor Green
}
