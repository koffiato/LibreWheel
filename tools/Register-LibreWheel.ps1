# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'InstallOrSwitch', 'Switch', 'Remove', 'Describe', 'DetectConflict', 'RemoveExistingOem')]
    [string] $Action,
    [string] $Dll64,
    [string] $Dll32,
    [switch] $ValidateOnly,
    [switch] $RemoveExistingOem,
    [ValidateSet('All', 'User', 'Machine')]
    [string] $Scope = 'All',
    [string] $BackupDir = (Join-Path $env:LOCALAPPDATA 'LibreWheel\backups')
)

$ErrorActionPreference = 'Stop'
$class = '{00EB836B-81CB-4D34-B0DF-16202A83BD4A}'
$oem = 'System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_046D&PID_C24F'
$forceKey = "$oem\OEMForceFeedback"
$axisKey = "$oem\Axes\0"
$comKey = "Software\Classes\CLSID\$class"
$ownerName = 'LibreWheelOwner'
$binary = [Microsoft.Win32.RegistryValueKind]::Binary
$string = [Microsoft.Win32.RegistryValueKind]::String

function Get-Bytes([uint32[]] $Words) {
    [byte[]] $data = New-Object byte[] ($Words.Count * 4)
    [Buffer]::BlockCopy($Words, 0, $data, 0, $data.Length)
    return ,$data
}

function Get-Effects {
    $defs = @(
        @{ Id = 0;  Name = 'Constant Force'; Category = 1 }
        @{ Id = 1;  Name = 'Ramp Force';     Category = 2 }
        @{ Id = 2;  Name = 'Square';         Category = 3 }
        @{ Id = 3;  Name = 'Sine';           Category = 3 }
        @{ Id = 4;  Name = 'Triangle';       Category = 3 }
        @{ Id = 5;  Name = 'Sawtooth Up';    Category = 3 }
        @{ Id = 6;  Name = 'Sawtooth Down';  Category = 3 }
        @{ Id = 7;  Name = 'Spring';         Category = 4 }
        @{ Id = 10; Name = 'Friction';       Category = 4 }
    )
    foreach ($def in $defs) {
        # SDK DIEFT_*, DIEP_* and DIEFF_* flags. Phase and envelope are type-specific.
        $isCondition = $def.Category -eq 4
        $capabilities = if ($isCondition) { 0xF800 } else { 0x8600 }
        $dynamic = if ($isCondition) { 0x345 } else { 0x3C5 }
        [pscustomobject] @{
            Guid = '{{13541C{0:X2}-8E33-11D0-9AD0-00A0C9A06E35}}' -f (0x20 + $def.Id)
            Name = $def.Name
            Attributes = Get-Bytes @($def.Id, ($def.Category -bor $capabilities), ($dynamic -bor 0x20), $dynamic, 0x10)
        }
    }
}

function Write-Effects($DriverKey) {
    $DriverKey.DeleteSubKeyTree('Effects', $false)
    foreach ($effect in (Get-Effects)) {
        $key = $DriverKey.CreateSubKey("Effects\$($effect.Guid)")
        try {
            $key.SetValue('', $effect.Name, $string)
            $key.SetValue('Attributes', $effect.Attributes, $binary)
        } finally { $key.Dispose() }
    }
}

if ($Action -eq 'Describe') {
    Get-Effects | Select-Object Guid, Name, @{Name = 'Attributes'; Expression = { [BitConverter]::ToString($_.Attributes) }}
    return
}

function Get-Dll([string] $Path, [uint16] $Machine) {
    if (-not $Path) { throw 'Install requires both -Dll64 and -Dll32.' }
    $file = Get-Item -LiteralPath $Path
    if ($file.PSIsContainer -or $file.Name -ne 'LibreWheelFF.dll') {
        throw "Expected a built LibreWheelFF.dll at $Path."
    }
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($file.FullName))
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Invalid PE file at $Path." }
        $reader.BaseStream.Position = 0x3C
        $reader.BaseStream.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $Machine) {
            throw "Unexpected PE architecture at $Path."
        }
    } finally {
        $reader.Dispose()
    }
    $file.FullName
}

function Get-RegValue($Hive, [string] $Path, [string] $Name) {
    $key = $Hive.OpenSubKey($Path)
    if ($null -ne $key) {
        try { $key.GetValue($Name) } finally { $key.Dispose() }
    }
}

function Test-RegKey($Hive, [string] $Path) {
    $key = $Hive.OpenSubKey($Path)
    if ($null -ne $key) { $key.Dispose(); return $true }
    return $false
}

function Remove-Owned($Hive, [string] $Path, [string] $Name) {
    if ((Get-RegValue $Hive $Path $Name) -eq $class) {
        $Hive.DeleteSubKeyTree($Path, $false)
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    [Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-ComServer([string] $Clsid, $User64, $User32, $Machine64, $Machine32) {
    if (-not $Clsid) { return $null }
    $path = "Software\Classes\CLSID\$Clsid\InprocServer32"
    foreach ($hive in $User64, $User32, $Machine64, $Machine32) {
        try {
            $value = Get-RegValue $hive $path ''
            if ($value) { return [string]$value }
        } catch [Security.SecurityException] {}
    }
    $null
}

function Get-ConflictingOemEntries($User64, $User32, $Machine64, $Machine32) {
    $entries = [Collections.Generic.List[object]]::new()
    foreach ($item in @(@{ Name = 'HKLM'; Hive = $Machine64 }, @{ Name = 'HKCU'; Hive = $User64 })) {
        if (-not (Test-RegKey $item.Hive $forceKey)) { continue }
        $clsid = [string](Get-RegValue $item.Hive $forceKey 'CLSID')
        if ($clsid -eq $class) { continue }
        $server = Resolve-ComServer $clsid $User64 $User32 $Machine64 $Machine32
        $isLogitech = ($clsid -eq '{1ED6DDBB-0401-4498-A093-7D249203200C}') -or
            ($server -and ($server -match 'Logitech|LGHUB|LCore|lg_joy'))
        $entries.Add([pscustomobject]@{
            Hive = $item.Name
            Key = "$($item.Name)\$forceKey"
            Clsid = $clsid
            Server = $server
            IsLogitech = $isLogitech
        })
    }
    return ,$entries.ToArray()
}

function Backup-ForceKey([string] $HiveName, [string] $Directory) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupPath = Join-Path $Directory "G29-OEMForceFeedback-$HiveName-$timestamp.reg"
    $suffix = 1
    while (Test-Path -LiteralPath $backupPath) {
        $backupPath = Join-Path $Directory "G29-OEMForceFeedback-$HiveName-$timestamp-$suffix.reg"
        ++$suffix
    }
    $regExe = Join-Path $env:SystemRoot 'System32\reg.exe'
    $output = & $regExe export "$HiveName\$forceKey" $backupPath /y /reg:64 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $backupPath)) {
        throw "Could not back up $HiveName\$forceKey before removal: $($output -join ' ')"
    }
    $backupPath
}

function Remove-ConflictingOemEntries($User64, $Machine64, [string] $TargetScope, [string] $Directory) {
    $targets = @()
    if ($TargetScope -in 'All', 'Machine') { $targets += @{ Name = 'HKLM'; Hive = $Machine64; RequiresAdmin = $true } }
    if ($TargetScope -in 'All', 'User') { $targets += @{ Name = 'HKCU'; Hive = $User64; RequiresAdmin = $false } }

    $removed = 0
    foreach ($target in $targets) {
        if (-not (Test-RegKey $target.Hive $forceKey)) { continue }
        if ((Get-RegValue $target.Hive $forceKey 'CLSID') -eq $class) { continue }
        if ($target.RequiresAdmin -and -not (Test-IsAdministrator)) {
            throw 'Removing a machine-wide (HKLM) Logitech/OEM G29 force-feedback registration requires Administrator privileges.'
        }
        if (-not $PSCmdlet.ShouldProcess("$($target.Name)\$forceKey", 'Back up and remove conflicting Logitech/OEM G29 registration')) {
            continue
        }
        $backup = Backup-ForceKey $target.Name $Directory
        $target.Hive.DeleteSubKeyTree($forceKey, $false)
        Write-Output "Backed up $($target.Name)\$forceKey to $backup and removed it."
        ++$removed
    }
    if ($removed -eq 0) {
        Write-Output 'No conflicting Logitech/OEM G29 force-feedback registration found for the requested scope.'
    }
}

if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'LibreWheel targets Windows x64.'
}
if ($ValidateOnly -and $Action -in 'Remove', 'DetectConflict', 'RemoveExistingOem') {
    throw 'ValidateOnly is for installation and switching.'
}
if (-not $ValidateOnly -and $Action -in 'Install', 'InstallOrSwitch', 'Switch') {
    $path64 = Get-Dll $Dll64 0x8664
    $path32 = Get-Dll $Dll32 0x014C
}
if (-not $ValidateOnly -and $Action -in 'Install', 'InstallOrSwitch', 'Switch', 'Remove' -and
    -not $PSCmdlet.ShouldProcess('current user, G29 VID_046D&PID_C24F', "$Action DirectInput registration")) {
    return
}

$views = [Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32
$user64 = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', $views[0])
$user32 = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', $views[1])
$machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $views[0])
$machine32 = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $views[1])
try {
    if ($Action -eq 'DetectConflict') {
        Get-ConflictingOemEntries $user64 $user32 $machine $machine32
        return
    }
    if ($Action -eq 'RemoveExistingOem') {
        Remove-ConflictingOemEntries $user64 $machine $Scope $BackupDir
        return
    }
    if ($Action -eq 'Remove') {
        if (($Dll64 -or $Dll32) -and
            ((Get-RegValue $user64 "$comKey\InprocServer32" '') -ne $Dll64 -or
             (Get-RegValue $user32 "$comKey\InprocServer32" '') -ne $Dll32)) {
            return
        }
        Remove-Owned $user64 $forceKey 'CLSID'
        Remove-Owned $user64 $axisKey $ownerName
        Remove-Owned $user64 $comKey $ownerName
        Remove-Owned $user32 $comKey $ownerName
        Write-Output 'Removed LibreWheel DirectInput registration for this user.'
        return
    }
    if ($RemoveExistingOem -and -not $ValidateOnly) {
        Remove-ConflictingOemEntries $user64 $machine 'All' $BackupDir
    }
    if ($Action -eq 'InstallOrSwitch') {
        $Action = if ((Get-RegValue $user64 $forceKey 'CLSID') -eq $class) { 'Switch' } else { 'Install' }
    }
    if ($Action -eq 'Switch') {
        if ((Test-RegKey $machine $forceKey) -and -not ($ValidateOnly -and $RemoveExistingOem)) {
            throw 'A machine-wide Logitech/OEM G29 force-feedback driver is registered. Re-run with -RemoveExistingOem (as Administrator) or remove it before switching.'
        }
        if ((Get-RegValue $user64 $forceKey 'CLSID') -ne $class -or
            (Get-RegValue $user64 $comKey $ownerName) -ne $class -or
            (Get-RegValue $user32 $comKey $ownerName) -ne $class) {
            throw 'Only an existing LibreWheel installation can switch DLLs.'
        }
        $server64 = $user64.OpenSubKey("$comKey\InprocServer32", (-not $ValidateOnly))
        try {
            $server32 = $user32.OpenSubKey("$comKey\InprocServer32", (-not $ValidateOnly))
            try {
                if ($null -eq $server64 -or $null -eq $server32) {
                    throw 'LibreWheel is missing one of its registered in-process servers.'
                }
                if ($ValidateOnly) { return }
                $previous64 = $server64.GetValue('')
                $server64.SetValue('', $path64, $string)
                try {
                    $server32.SetValue('', $path32, $string)
                } catch {
                    $server64.SetValue('', $previous64, $string)
                    throw
                }
                $driver = $user64.OpenSubKey($forceKey, $true)
                if ($null -ne $driver) {
                    try {
                        $driver.SetValue('Attributes', (Get-Bytes @(0, 2000, 2000)), $binary)
                        Write-Effects $driver
                    } finally {
                        $driver.Dispose()
                    }
                }
            } finally {
                if ($null -ne $server32) { $server32.Dispose() }
            }
        } finally {
            if ($null -ne $server64) { $server64.Dispose() }
        }
        Write-Output 'Updated LibreWheel DLLs for 64-bit and 32-bit DirectInput clients. Reopen the game.'
        return
    }

    if ((Test-RegKey $machine $forceKey) -and -not ($ValidateOnly -and $RemoveExistingOem)) {
        throw 'A machine-wide Logitech/OEM G29 force-feedback driver is registered. Re-run with -RemoveExistingOem (as Administrator) or remove it first.'
    }
    if ((Test-RegKey $user64 $forceKey) -and -not ($ValidateOnly -and $RemoveExistingOem)) {
        throw 'An existing per-user Logitech/OEM G29 force-feedback registration is present. Re-run with -RemoveExistingOem or remove it first.'
    }
    if ((Test-RegKey $user64 $comKey) -or (Test-RegKey $user32 $comKey)) {
        throw 'The LibreWheel COM class is already present. Existing entries were preserved.'
    }
    $reuseAxis = Test-RegKey $user64 $axisKey
    if ($reuseAxis) {
        $attributes = [byte[]] (Get-RegValue $user64 $axisKey 'Attributes')
        $forceAttributes = [byte[]] (Get-RegValue $user64 $axisKey 'FFAttributes')
        if ($attributes.Length -ne 4 -or [BitConverter]::ToUInt32($attributes, 0) -ne 0x8101 -or
            $forceAttributes.Length -ne 8 -or [BitConverter]::ToUInt32($forceAttributes, 0) -eq 0 -or
            [BitConverter]::ToUInt32($forceAttributes, 4) -eq 0) {
            throw 'The existing steering axis entry does not describe a compatible G29 actuator. It was preserved.'
        }
    }
    if ($ValidateOnly) { return }

    $created = [Collections.Generic.List[object]]::new()
    try {
        foreach ($entry in @(@{Hive = $user64; Path = $path64}, @{Hive = $user32; Path = $path32})) {
            $key = $entry.Hive.CreateSubKey($comKey)
            $created.Add(@{Hive = $entry.Hive; Key = $comKey})
            try {
                $key.SetValue($ownerName, $class, $string)
                $server = $key.CreateSubKey('InprocServer32')
                try {
                    $server.SetValue('', $entry.Path, $string)
                    $server.SetValue('ThreadingModel', 'Both', $string)
                } finally { $server.Dispose() }
            } finally { $key.Dispose() }
        }

        if (-not $reuseAxis) {
            $axis = $user64.CreateSubKey($axisKey)
            $created.Add(@{Hive = $user64; Key = $axisKey})
            try {
                $axis.SetValue($ownerName, $class, $string)
                $axis.SetValue('', 'Steering', $string)
                $axis.SetValue('Attributes', (Get-Bytes @(0x8101)), $binary)
                # The physical G29's OEM data describes a 10 N actuator.
                $axis.SetValue('FFAttributes', (Get-Bytes @(10, 128)), $binary)
            } finally { $axis.Dispose() }
        }

        $driver = $user64.CreateSubKey($forceKey)
        $created.Add(@{Hive = $user64; Key = $forceKey})
        try {
            $driver.SetValue('Attributes', (Get-Bytes @(0, 2000, 2000)), $binary)
            Write-Effects $driver
            # Publish the product-to-class association after both binaries and all effects are registered.
            $driver.SetValue('CLSID', $class, $string)
        } finally { $driver.Dispose() }
    } catch {
        for ($i = $created.Count - 1; $i -ge 0; --$i) {
            $created[$i].Hive.DeleteSubKeyTree($created[$i].Key, $false)
        }
        throw
    }
    Write-Output 'Registered LibreWheel for 64-bit and 32-bit DirectInput clients. Reopen the game after registration.'
} finally {
    $machine32.Dispose()
    $machine.Dispose()
    $user32.Dispose()
    $user64.Dispose()
}
