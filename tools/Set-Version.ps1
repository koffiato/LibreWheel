# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Validates or updates LibreWheel's Semantic Version (SemVer 2.0.0) in the root VERSION file.
.EXAMPLE
.\tools\Set-Version.ps1
.\tools\Set-Version.ps1 -Bump patch
.\tools\Set-Version.ps1 -Version 0.2.0-beta.1
#>
[CmdletBinding(DefaultParameterSetName = 'Show')]
param(
    [Parameter(ParameterSetName = 'Set', Mandatory = $true)]
    [string] $Version,
    [Parameter(ParameterSetName = 'Bump', Mandatory = $true)]
    [ValidateSet('major', 'minor', 'patch')]
    [string] $Bump,
    [Parameter(ParameterSetName = 'Bump')]
    [string] $Prerelease
)

$ErrorActionPreference = 'Stop'
$semverPattern = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$'

$root = Split-Path -Parent $PSScriptRoot
$versionFile = Join-Path $root 'VERSION'
if (-not (Test-Path -LiteralPath $versionFile)) {
    throw 'VERSION file not found.'
}
$current = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($current -notmatch $semverPattern) {
    throw "Current VERSION '$current' is not valid SemVer 2.0.0."
}

if ($PSCmdlet.ParameterSetName -eq 'Show') {
    Write-Output $current
    return
}

if ($PSCmdlet.ParameterSetName -eq 'Bump') {
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $patch = [int]$Matches[3]
    switch ($Bump) {
        'major' { ++$major; $minor = 0; $patch = 0 }
        'minor' { ++$minor; $patch = 0 }
        'patch' { ++$patch }
    }
    $next = "$major.$minor.$patch"
    if ($Prerelease) {
        $next = "$next-$Prerelease"
    }
} else {
    $next = $Version.Trim()
}

if ($next -notmatch $semverPattern) {
    throw "Target version '$next' is not valid Semantic Versioning 2.0.0."
}

[IO.File]::WriteAllText($versionFile, "$next`n", [Text.UTF8Encoding]::new($false))
Write-Output "Updated VERSION: $current -> $next"
Write-Output "Next steps:"
Write-Output "  1. Update CHANGELOG.md with release notes for v$next"
Write-Output "  2. Run .\tools\Build-Installer.ps1"
Write-Output "  3. git add VERSION CHANGELOG.md && git commit -m `"Release v$next`" && git tag -a v$next -m `"LibreWheel v$next`""
