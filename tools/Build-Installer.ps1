# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Builds LibreWheel-Setup.exe with the full-range DirectInput drivers and the native tray app.
.DESCRIPTION
Supply trained MSVC profile databases to enable PGO for the corresponding drivers.
.EXAMPLE
.\tools\Build-Installer.ps1 -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
#>
[CmdletBinding()]
param(
    [string] $InnoCompiler = 'ISCC.exe',
    [string] $CMake = 'cmake.exe',
    [string] $PgoProfileX64,
    [string] $PgoProfileX86,
    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'
$InnoCompiler = (Get-Command $InnoCompiler -CommandType Application).Source
$CMake = (Get-Command $CMake -CommandType Application).Source
$CTest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
if (-not $SkipTests -and -not (Test-Path -LiteralPath $CTest)) {
    $CTest = (Get-Command 'ctest.exe' -CommandType Application).Source
}
$root = Split-Path -Parent $PSScriptRoot
$cmakeText = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\s*\(\s*LibreWheel\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not determine LibreWheel version from CMakeLists.txt.'
}
$version = $Matches[1]
$profiles = @{ x64 = $PgoProfileX64; x86 = $PgoProfileX86 }
foreach ($architecture in 'x64', 'x86') {
    if ($profiles[$architecture]) {
        $profiles[$architecture] = (Resolve-Path -LiteralPath $profiles[$architecture]).Path
    }
}

Push-Location -LiteralPath $root
try {
    foreach ($architecture in 'x64', 'x86') {
        $build = "build/setup-$architecture"
        $profile = $profiles[$architecture]
        $pgoMode = if ($profile) { 'USE' } else { 'OFF' }
        & $CMake --preset "full-force-$architecture" -B $build "-DLIBREWHEEL_PGO_MODE=$pgoMode" "-DLIBREWHEEL_PGO_PROFILE=$profile"
        if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture configuration failed." }

        $targets = @('LibreWheelFF')
        if ($architecture -eq 'x64') { $targets += @('LibreWheelTray', 'librewheel') }
        if (-not $SkipTests) { $targets += @('core_tests', 'driver_tests', 'output_tests', 'broker_test_helper') }
        if ($profile) {
            & $CMake --build $build --config Release --target $targets 2>&1 |
                Tee-Object -Variable buildOutput
            if (($buildOutput -join "`n") -match 'C4961|profile-guided optimizations disabled') {
                throw "LibreWheel $architecture was built without the requested profile data."
            }
        } else {
            & $CMake --build $build --config Release --target $targets
        }
        if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture build failed." }

        if (-not $SkipTests) {
            & $CTest --test-dir $build -C Release --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture tests failed." }
        }
    }

    & $InnoCompiler "/DAppVersion=$version" (Join-Path $PSScriptRoot 'LibreWheel.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
    Get-Item -LiteralPath 'build/installer/LibreWheel-Setup.exe'
} finally {
    Pop-Location
}
