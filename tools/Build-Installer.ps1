# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Builds LibreWheel-Setup.exe with the full-range DirectInput drivers and the native tray app.
.EXAMPLE
.\tools\Build-Installer.ps1 -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
#>
[CmdletBinding()]
param(
    [string] $InnoCompiler = 'ISCC.exe',
    [string] $CMake = 'cmake.exe'
)

$ErrorActionPreference = 'Stop'
$InnoCompiler = (Get-Command $InnoCompiler -CommandType Application).Source
$CMake = (Get-Command $CMake -CommandType Application).Source
$root = Split-Path -Parent $PSScriptRoot

Push-Location -LiteralPath $root
try {
    foreach ($architecture in 'x64', 'x86') {
        $build = "build/setup-$architecture"
        & $CMake --preset "full-force-$architecture" -B $build
        if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture configuration failed." }

        $targets = @('LibreWheelFF')
        if ($architecture -eq 'x64') { $targets += @('LibreWheelTray', 'librewheel') }
        & $CMake --build $build --config Release --target $targets
        if ($LASTEXITCODE -ne 0) { throw "LibreWheel $architecture build failed." }
    }

    & $InnoCompiler (Join-Path $PSScriptRoot 'LibreWheel.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
    Get-Item -LiteralPath 'build/installer/LibreWheel-Setup.exe'
} finally {
    Pop-Location
}
