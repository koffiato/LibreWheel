# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Builds the native LibreWheel-Setup.exe self-extracting installer and portable ZIP archive.
.DESCRIPTION
Requires only Visual Studio 2022 (or Build Tools) with Desktop development with C++, a Windows SDK,
and CMake 3.24+ (auto-detected from Visual Studio). No external installer tool is required.
Supports Release (full optimizations) and Debug (no optimizations, ships PDB debug symbols).
.EXAMPLE
.\tools\Build-Installer.ps1
.\tools\Build-Installer.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [string] $CMake,
    [string] $PgoProfileX64,
    [string] $PgoProfileX86,
    [string] $OutputDir = 'build/installer',
    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'

function Resolve-Tool([string] $Requested, [string] $Name, [string[]] $FallbackPaths) {
    if ($Requested) {
        return (Get-Command $Requested -CommandType Application).Source
    }
    $onPath = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }
    foreach ($candidate in $FallbackPaths) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }
    throw "Could not locate $Name. Install Visual Studio Build Tools with CMake or pass -CMake."
}

$vsCMake = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsRoot) {
        $vsCMake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
}

$CMake = Resolve-Tool $CMake 'cmake.exe' @($vsCMake)
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
$isDebug = $Configuration -eq 'Debug'
if ($isDebug -and ($PgoProfileX64 -or $PgoProfileX86)) {
    throw 'PGO profiles apply only to Release builds.'
}
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
        & $CMake --preset $architecture -B $build "-DLIBREWHEEL_PGO_MODE=$pgoMode" "-DLIBREWHEEL_PGO_PROFILE=$profile" "-DLIBREWHEEL_PAYLOAD_FILE="
        if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture configuration failed." }

        $targets = @('LibreWheelFF')
        if ($architecture -eq 'x64') { $targets += @('LibreWheelTray', 'librewheel', 'pack_payload') }
        if (-not $SkipTests) { $targets += @('core_tests', 'driver_tests', 'output_tests', 'broker_test_helper', 'installer_tests') }
        if ($profile) {
            & $CMake --build $build --config $Configuration --target $targets 2>&1 |
                Tee-Object -Variable buildOutput
            if (($buildOutput -join "`n") -match 'C4961|profile-guided optimizations disabled') {
                throw "LibreWheel $architecture was built without the requested profile data."
            }
        } else {
            & $CMake --build $build --config $Configuration --target $targets
        }
        if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture $Configuration build failed." }

        if (-not $SkipTests) {
            & $CTest --test-dir $build -C $Configuration --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture $Configuration tests failed." }
        }
    }

    $bin64 = Join-Path $root "build\setup-x64\$Configuration"
    $bin86 = Join-Path $root "build\setup-x86\$Configuration"
    $payloadBin = Join-Path $root "build\setup-x64\payload-$Configuration.bin"
    $packer = Join-Path $bin64 'pack_payload.exe'

    $payloadSpecs = [Collections.Generic.List[string]]::new()
    $payloadSpecs.Add("app:LibreWheelTray.exe=$(Join-Path $bin64 'LibreWheelTray.exe')")
    $payloadSpecs.Add("app:librewheel.exe=$(Join-Path $bin64 'librewheel.exe')")
    $payloadSpecs.Add("app:x64/LibreWheelFF.dll=$(Join-Path $bin64 'LibreWheelFF.dll')")
    $payloadSpecs.Add("app:x64/LibreWheelOutput.exe=$(Join-Path $bin64 'LibreWheelOutput.exe')")
    $payloadSpecs.Add("app:x86/LibreWheelFF.dll=$(Join-Path $bin86 'LibreWheelFF.dll')")
    $payloadSpecs.Add("app:x86/LibreWheelOutput.exe=$(Join-Path $bin86 'LibreWheelOutput.exe')")
    if ($isDebug) {
        $payloadSpecs.Add("app:LibreWheelTray.pdb=$(Join-Path $bin64 'LibreWheelTray.pdb')")
        $payloadSpecs.Add("app:librewheel.pdb=$(Join-Path $bin64 'librewheel.pdb')")
        $payloadSpecs.Add("app:x64/LibreWheelFF.pdb=$(Join-Path $bin64 'LibreWheelFF.pdb')")
        $payloadSpecs.Add("app:x64/LibreWheelOutput.pdb=$(Join-Path $bin64 'LibreWheelOutput.pdb')")
        $payloadSpecs.Add("app:x86/LibreWheelFF.pdb=$(Join-Path $bin86 'LibreWheelFF.pdb')")
        $payloadSpecs.Add("app:x86/LibreWheelOutput.pdb=$(Join-Path $bin86 'LibreWheelOutput.pdb')")
    }
    $payloadSpecs.Add("app:LICENSE=$(Join-Path $root 'LICENSE')")
    $payloadSpecs.Add("app:Register-LibreWheel.ps1=$(Join-Path $root 'tools\Register-LibreWheel.ps1')")
    $payloadSpecs.Add("config:LibreWheel-G29-Custom.lut=$(Join-Path $root 'LibreWheel-G29-Custom.lut')")
    $payloadSpecs.Add("config:settings.ini=$(Join-Path $root 'settings.example.ini')")

    & $packer $payloadBin @payloadSpecs
    if ($LASTEXITCODE -ne 0) { throw 'Packing LibreWheel installer payload failed.' }

    $profile64 = $profiles['x64']
    $pgoMode64 = if ($profile64) { 'USE' } else { 'OFF' }
    & $CMake --preset 'x64' -B 'build/setup-x64' "-DLIBREWHEEL_PGO_MODE=$pgoMode64" "-DLIBREWHEEL_PGO_PROFILE=$profile64" "-DLIBREWHEEL_PAYLOAD_FILE=$payloadBin"
    if ($LASTEXITCODE -ne 0) { throw 'LibreWheel-Setup configuration failed.' }
    & $CMake --build 'build/setup-x64' --config $Configuration --target LibreWheel-Setup
    if ($LASTEXITCODE -ne 0) { throw "LibreWheel-Setup $Configuration build failed." }

    $outPath = if ([IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $root $OutputDir }
    New-Item -ItemType Directory -Force -Path $outPath | Out-Null

    $suffix = if ($isDebug) { '-Debug' } else { '' }
    $installerDest = Join-Path $outPath "LibreWheel-Setup$suffix.exe"
    Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheel-Setup.exe') -Destination $installerDest -Force
    $artifacts = [Collections.Generic.List[IO.FileInfo]]::new()
    $artifacts.Add((Get-Item -LiteralPath $installerDest))
    if ($isDebug) {
        $setupPdbDest = Join-Path $outPath "LibreWheel-Setup$suffix.pdb"
        Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheel-Setup.pdb') -Destination $setupPdbDest -Force
        $artifacts.Add((Get-Item -LiteralPath $setupPdbDest))
    }

    # Build portable ZIP archive alongside the native installer.
    $portableStageRoot = Join-Path $root "build\setup-portable-$Configuration"
    $portableName = "LibreWheel-$version$suffix-portable"
    $portableStage = Join-Path $portableStageRoot $portableName
    if (Test-Path -LiteralPath $portableStageRoot) {
        Remove-Item -LiteralPath $portableStageRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $portableStage 'x64'), (Join-Path $portableStage 'x86') | Out-Null
    Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheelTray.exe') -Destination $portableStage
    Copy-Item -LiteralPath (Join-Path $bin64 'librewheel.exe') -Destination $portableStage
    Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheelFF.dll') -Destination (Join-Path $portableStage 'x64')
    Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheelOutput.exe') -Destination (Join-Path $portableStage 'x64')
    Copy-Item -LiteralPath (Join-Path $bin86 'LibreWheelFF.dll') -Destination (Join-Path $portableStage 'x86')
    Copy-Item -LiteralPath (Join-Path $bin86 'LibreWheelOutput.exe') -Destination (Join-Path $portableStage 'x86')
    if ($isDebug) {
        Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheelTray.pdb') -Destination $portableStage
        Copy-Item -LiteralPath (Join-Path $bin64 'librewheel.pdb') -Destination $portableStage
        Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheelFF.pdb') -Destination (Join-Path $portableStage 'x64')
        Copy-Item -LiteralPath (Join-Path $bin64 'LibreWheelOutput.pdb') -Destination (Join-Path $portableStage 'x64')
        Copy-Item -LiteralPath (Join-Path $bin86 'LibreWheelFF.pdb') -Destination (Join-Path $portableStage 'x86')
        Copy-Item -LiteralPath (Join-Path $bin86 'LibreWheelOutput.pdb') -Destination (Join-Path $portableStage 'x86')
    }
    Copy-Item -LiteralPath (Join-Path $root 'tools\Register-LibreWheel.ps1') -Destination $portableStage
    Copy-Item -LiteralPath (Join-Path $root 'settings.example.ini') -Destination $portableStage
    Copy-Item -LiteralPath (Join-Path $root 'LibreWheel-G29-Custom.lut') -Destination $portableStage
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $portableStage
    Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $portableStage

    $zipDest = Join-Path $outPath "$portableName.zip"
    if (Test-Path -LiteralPath $zipDest) {
        Remove-Item -LiteralPath $zipDest -Force
    }
    Push-Location -LiteralPath $portableStageRoot
    try {
        & $CMake -E tar cf $zipDest --format=zip $portableName
        if ($LASTEXITCODE -ne 0) { throw 'Creating portable ZIP archive failed.' }
    } finally {
        Pop-Location
    }
    $artifacts.Add((Get-Item -LiteralPath $zipDest))

    $checksumLines = foreach ($artifact in $artifacts) {
        $hash = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash *$($artifact.Name)"
    }
    $checksumFile = Join-Path $outPath "SHA256SUMS$suffix.txt"
    $checksumLines | Set-Content -LiteralPath $checksumFile -Encoding ascii

    Write-Output "Built LibreWheel $version ($Configuration) release artifacts in ${outPath}:"
    foreach ($line in $checksumLines) { Write-Output "  $line" }
    $artifacts
} finally {
    Pop-Location
}
