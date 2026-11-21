# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'InstallOrSwitch', 'Switch', 'Remove', 'Describe')]
    [string] $Action,
    [string] $Dll64,
    [string] $Dll32,
    [switch] $ValidateOnly
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
    $names = @('Constant Force', 'Ramp Force', 'Square', 'Sine', 'Triangle',
        'Sawtooth Up', 'Sawtooth Down', 'Spring')
    $categories = @(1, 2, 3, 3, 3, 3, 3, 4)
    for ($i = 0; $i -lt $names.Count; ++$i) {
        # SDK DIEFT_*, DIEP_* and DIEFF_* flags. Phase and envelope are type-specific.
        $capabilities = if ($i -eq 7) { 0xF800 } else { 0x8600 }
        $dynamic = if ($i -eq 7) { 0x345 } else { 0x3C5 }
        [pscustomobject] @{
            Guid = '{{13541C{0:X2}-8E33-11D0-9AD0-00A0C9A06E35}}' -f (0x20 + $i)
            Name = $names[$i]
            Attributes = Get-Bytes @($i, ($categories[$i] -bor $capabilities), ($dynamic -bor 0x20), $dynamic, 0x10)
        }
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

if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'LibreWheel targets Windows x64.'
}
if ($ValidateOnly -and $Action -eq 'Remove') {
    throw 'ValidateOnly is for installation and switching.'
}
if (-not $ValidateOnly -and $Action -ne 'Remove') {
    $path64 = Get-Dll $Dll64 0x8664
    $path32 = Get-Dll $Dll32 0x014C
}
if (-not $ValidateOnly -and
    -not $PSCmdlet.ShouldProcess('current user, G29 VID_046D&PID_C24F', "$Action DirectInput registration")) {
    return
}

$views = [Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32
$user64 = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', $views[0])
$user32 = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', $views[1])
$machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $views[0])
try {
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
    if ($Action -eq 'InstallOrSwitch') {
        $Action = if ((Get-RegValue $user64 $forceKey 'CLSID') -eq $class) { 'Switch' } else { 'Install' }
    }
    if ($Action -eq 'Switch') {
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
            } finally {
                if ($null -ne $server32) { $server32.Dispose() }
            }
        } finally {
            if ($null -ne $server64) { $server64.Dispose() }
        }
        Write-Output 'Updated LibreWheel DLLs for 64-bit and 32-bit DirectInput clients. Reopen the game.'
        return
    }

    if (Test-RegKey $machine $forceKey) {
        throw 'A machine-wide G29 force-feedback driver is registered. Use a clean inbox-HID test installation.'
    }
    if ((Test-RegKey $user64 $forceKey) -or
        (Test-RegKey $user64 $comKey) -or (Test-RegKey $user32 $comKey)) {
        throw 'The G29 registration or LibreWheel COM class is already present. Existing entries were preserved.'
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
            $driver.SetValue('Attributes', (Get-Bytes @(0, 4000, 4000)), $binary)
            foreach ($effect in (Get-Effects)) {
                $key = $driver.CreateSubKey("Effects\$($effect.Guid)")
                try {
                    $key.SetValue('', $effect.Name, $string)
                    $key.SetValue('Attributes', $effect.Attributes, $binary)
                } finally { $key.Dispose() }
            }
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
    $machine.Dispose()
    $user32.Dispose()
    $user64.Dispose()
}
