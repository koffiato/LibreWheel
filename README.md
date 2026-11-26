# LibreWheel

LibreWheel is a free, open-source (`GPL-3.0-or-later`) user-mode Windows driver for the **Logitech G29**. It uses Windows' inbox HID stack for steering, pedals, and buttons, paired with a user-mode DirectInput COM force-feedback driver (`LibreWheelFF.dll`) for both 64-bit and 32-bit games. No kernel driver, Windows service, or virtual controller is installed.

> **Experimental Status:** Game compatibility and torque shutdown behavior under USB or host failure remain experimental. Keep your hands clear of the wheel rim while testing or switching modes, and review the [architecture and protocol evidence](docs/architecture.md) before use. This project assumes no liability for hardware damage.

## Requirements

* **OS:** Windows 10 or Windows 11 (64-bit)
* **Hardware:** Logitech G29 with its physical mode switch set to **PS3** (`046d:c294` compatibility mode is automatically switched to `046d:c24f` native mode; PS4 mode `046d:c260` is unsupported)

## Installation

Prebuilt binaries and `SHA256SUMS.txt` checksums are published on [GitHub Releases](https://github.com/koffiato/LibreWheel/releases).

### Option 1: Native Installer (Recommended)

Run `LibreWheel-<version>-Setup.exe` (a native C++20 self-extracting installer built solely with Visual Studio and the Windows SDK). It installs the 64-bit and 32-bit drivers, `LibreWheelTray.exe`, and `librewheel.exe` per-user into `%LOCALAPPDATA%\Programs\LibreWheel`, copies the default configuration and LUT into `%LOCALAPPDATA%\LibreWheel` if absent, creates a Start Menu shortcut, and registers an uninstaller in Windows *Installed apps*.

* **Logitech / OEM Registry Conflict Detection:** If an existing Logitech (G HUB / LGS) or other third-party G29 `OEMForceFeedback` registry entry is detected in `HKLM` or `HKCU`, Setup warns that LibreWheel force feedback will not work while it remains and offers to export a `.reg` backup (`%LOCALAPPDATA%\LibreWheel\backups\`) and remove it. Removing a machine-wide (`HKLM`) entry prompts for Administrator permission (UAC); choosing to keep the existing entry warns again and installs the files without registering LibreWheel.
* **Silent Installation:** Pass `--silent` (add `--remove-logitech` to opt into automatic backup and removal, and `--launch` to start `LibreWheelTray.exe` afterward).
* **Uninstall:** Close running games and uninstall from Windows Settings (*Installed apps*) or run `Uninstall.exe --uninstall`. LibreWheel's per-user COM/DirectInput registration, Start Menu shortcut, and startup entry are removed cleanly without deleting your user config or registry backups.

### Option 2: Portable Archive

Extract `LibreWheel-<version>-portable.zip` to a permanent directory (DirectInput COM registration stores absolute DLL paths) and register the driver with `Register-LibreWheel.ps1` as shown in [Manual Registration](#manual-registration-portable-and-source-builds).

## Tray App and Configuration

`LibreWheelTray.exe` runs in the Windows notification area and handles day-to-day wheel setup:

* Automatically switches arriving PS3 compatibility-mode G29 wheels (`046d:c294`) to native mode (`046d:c24f`).
* Applies configured `range_degrees` on wheel connection when no game holds FFB output.
* Selects linear output or `.csv` / `.lut` force-correction curves and displays a menu warning if an invalid config or LUT falls back to linear output.
* Respects Windows dark mode and Per-Monitor V2 DPI scaling, provides an **Open Config Folder** shortcut and a **Start with Windows** toggle, and cleanly stops active motor output when exited.

### Settings (`settings.ini`)

Settings are read from `%LOCALAPPDATA%\LibreWheel\settings.ini`. The installer initializes this folder automatically if no config exists. For portable or source builds:

1. Copy [settings.example.ini](settings.example.ini) to `%LOCALAPPDATA%\LibreWheel\settings.ini`.
2. Copy [LibreWheel-G29-Custom.lut](LibreWheel-G29-Custom.lut) into the same directory.
3. Set `lut_enabled=true` in `settings.ini` (or select a curve from `LibreWheelTray.exe`) to enable LUT correction.
4. Run `librewheel.exe settings` to print the active config path and verify that it parses cleanly.
5. Reopen your game to apply configuration or LUT changes.

Settings and LUT curves are loaded once when a game acquires the wheel (they are not live-reloaded mid-session). If `settings.ini` or an enabled LUT file is missing or invalid, the driver falls back to linear output instead of failing force-feedback initialization, and `LibreWheelTray.exe` displays a warning in its menu.

| Key | Description |
| --- | --- |
| `gain_percent` | User force multiplier from `0` to `100`. Defaults to `100`. |
| `range_degrees` | Optional hardware rotation range from `40` to `900`. Sent when a force-feedback session initializes and when `LibreWheelTray.exe` detects a native G29 while idle. |
| `lut_enabled` | Boolean enabling the force-feedback LUT curve. Defaults to `false` for linear output. |
| `lut_file` | UTF-8 path to the LUT file (`.csv` or `.lut`). Relative paths resolve relative to `settings.ini`. |
| `diagnostics` | Boolean. Emits torque idle/active transitions to the Windows debug output stream. |
| `log_ffb` | Boolean. Saves ring-buffered DirectInput command, mixer, and USB output traces to `%LOCALAPPDATA%\LibreWheel\logs\`. |

LUT rows contain comma-delimited (`,`) or pipe-delimited (`|`) normalized input and corrected output magnitudes from `0` to `1`. An optional header is `input,output`. Inputs must be strictly increasing and include `0,0` and an entry at input `1`. The parser accepts blank lines and full-line `#` comments. Correction linearly interpolates between entries and preserves the original sign.

## Input and DirectInput Diagnostics

Connect the wheel with its physical mode switch set to **PS3**. On power-up, the G29 can present the shared `046d:c294` compatibility identity. `librewheel.exe native` verifies the hardware revision and switches a recognized G29 to `046d:c24f`. The wheel may recalibrate during the switch, so keep its travel clear. Run `native` again after reconnecting if the wheel returns to compatibility mode, or keep `LibreWheelTray.exe` running to switch automatically.

```powershell
librewheel.exe native
librewheel.exe list
librewheel.exe monitor
librewheel.exe directinput
librewheel.exe settings
```

| Command | Description |
| --- | --- |
| `native` | Verifies the hardware revision and switches the wheel from `046d:c294` to `046d:c24f`. |
| `list` | Displays connected native G29 HID collections with revision, input/output report sizes, and classic output availability. |
| `monitor` | Streams raw steering, three pedal values, POV state, 25-bit button mask, and live input report rate (`Hz`). Watches for USB reconnections automatically. |
| `directinput` | Lists the physical product, Windows FFB advertisement status, `LibreWheel` registration flag, steering actuator flags, and advertised effects. |
| `settings` | Prints the active `settings.ini` path and verifies that it (and any enabled LUT) parses cleanly. |
| `range <40-900>` | Sends a hardware steering operating range (40°–900°) to a connected native G29 when no game holds FFB output. |
| `pulse <left\|right> [percent]` | Runs a 1-second DirectInput constant-force pulse (1–100%, default 5%) and checks effect start and expiration status. |
| `version` | Prints the LibreWheel Semantic Version (`--version` and `-v` are also accepted). |

Set `LIBREWHEEL_DISABLE_OUTPUT=1` in the environment to run `LibreWheelFF.dll` in software-only mode without motor output (useful for testing DirectInput initialization or running `pulse` with motors disabled).

## Manual Registration (Portable and Source Builds)

### Register

`Register-LibreWheel.ps1` requires both the 64-bit and 32-bit DLLs and records their **absolute paths**, so keep the binaries in place after registering (or use `LibreWheel-<version>-Setup.exe` to install into `%LOCALAPPDATA%\Programs\LibreWheel`).

```powershell
# From an extracted portable archive:
.\Register-LibreWheel.ps1 -Action Install `
  -Dll64 .\x64\LibreWheelFF.dll `
  -Dll32 .\x86\LibreWheelFF.dll

# Or from a source build:
.\tools\Register-LibreWheel.ps1 -Action Install `
  -Dll64 .\build\x64\Release\LibreWheelFF.dll `
  -Dll32 .\build\x86\Release\LibreWheelFF.dll
```

* The script preserves compatible existing steering actuator entries and refuses to overwrite an OEM FFB registration or an incompatible axis override by default.
* To inspect or back up and remove a conflicting Logitech/OEM G29 FFB registration from PowerShell, use `-Action DetectConflict`, `-Action RemoveExistingOem` (with optional `-Scope User|Machine|All` and `-BackupDir`), or pass `-RemoveExistingOem` alongside `-Action Install` (run elevated for `HKLM` entries). Backups are exported to `%LOCALAPPDATA%\LibreWheel\backups\` before deletion.

**Verify the registration:**

1. Reopen your DirectInput client or game.
2. Run `librewheel.exe directinput` and confirm it shows `LibreWheel=yes` for the G29 and reports the steering actuator.
3. Run `librewheel.exe pulse right` and `librewheel.exe pulse left` (default 5% torque; keep the wheel clear, or set `LIBREWHEEL_DISABLE_OUTPUT=1` for a software-only check) to exercise the DirectInput API and verify effect status on the physical device.

### Switch Builds

To point an existing LibreWheel registration to newly compiled binaries, run `-Action Switch` with the updated DLL paths, then reopen your game so it loads the new DLL:

```powershell
.\tools\Register-LibreWheel.ps1 -Action Switch `
  -Dll64 <path-to-new-x64-LibreWheelFF.dll> `
  -Dll32 <path-to-new-x86-LibreWheelFF.dll>
```

### Remove

Close all running games before removing registration:

```powershell
.\tools\Register-LibreWheel.ps1 -Action Remove
```

This removes LibreWheel's per-user FFB and COM registrations only. Binaries, user settings, and backups can be deleted separately.

## Build from Source

**Prerequisites:** Visual Studio 2022 (or Build Tools for Visual Studio 2022) with the *Desktop development with C++* workload, a Windows SDK, and CMake 3.24 or newer. No external installer tool or third-party package manager is required.

LibreWheel provides only two build configurations:

* **Release:** Full MSVC optimizations with cross-file IPO/LTCG (`/O2 /Ob3 /Oi /Ot /Gw /Gy`, `/LTCG /OPT:REF /OPT:ICF`, `/DEBUG:NONE`).
* **Debug:** Zero optimizations (`/Od /Ob0 /RTC1`) with full `.pdb` debug symbols (`/Zi`, `/DEBUG:FULL`) shipped alongside Debug binaries.

### Compile and Test

```powershell
# 64-bit Release build and tests
cmake --preset x64
cmake --build --preset release-x64
ctest --preset release-x64

# 32-bit Release build and tests
cmake --preset x86
cmake --build --preset release-x86
ctest --preset release-x86

# Unoptimized Debug builds with full symbols
cmake --build --preset debug-x64
ctest --preset debug-x64
cmake --build --preset debug-x86
ctest --preset debug-x86
```

Each `build/<architecture>/<Configuration>` directory produces `LibreWheelFF.dll`, `LibreWheelOutput.exe`, `librewheel.exe`, and `LibreWheelTray.exe` (plus `.pdb` files in `Debug`). The DLL architecture must match the target game's process bitness.

The `ctest` suites (`core`, `driver`, `output`, `installer`) test effect scheduling, waveforms and conditions, settings/LUT linear fallback, HID report encoding, COM DLL loading and lifecycle, FFB trace ring buffering, in-process and out-of-process broker IPC shutdown paths, and installer LZMS archive / PE validation / `.reg` export.

### Build the Installer and Portable ZIP

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\Build-Installer.ps1
```

`tools/Build-Installer.ps1` auto-detects Visual Studio's CMake, reads the [Semantic Versioning 2.0.0](https://semver.org/) version from [VERSION](VERSION), builds both `x64` and `x86` (with optional `-PgoProfileX64` / `-PgoProfileX86` for Release or `-Configuration Debug` to bundle `.pdb` symbols), runs `ctest` on both architectures (pass `-SkipTests` to skip), and writes `LibreWheel-<version>-Setup.exe`, `LibreWheel-<version>-portable.zip`, and `SHA256SUMS.txt` into `build/installer/` (override with `-OutputDir`). Use `tools/Set-Version.ps1` (`-Bump major|minor|patch` or `-Version X.Y.Z[-prerelease]`) to update the version; see [CHANGELOG.md](CHANGELOG.md).

## Troubleshooting

* **Existing Logitech software (G HUB / LGS) installed:** LibreWheel force feedback cannot activate while another G29 `OEMForceFeedback` entry remains registered in `HKLM` or `HKCU`. Let `LibreWheel-<version>-Setup.exe` back up and remove it, or run `Register-LibreWheel.ps1 -Action DetectConflict` / `-Action RemoveExistingOem` (a `.reg` backup is always saved to `%LOCALAPPDATA%\LibreWheel\backups\` first).
* **Device not detected:** Confirm the G29's physical switch is on **PS3** and check its USB identity (`046d:c294` or `046d:c24f`) and Windows HID driver in Device Manager. LibreWheel does not alter system HID bindings.
* **Raw input works, but FFB discovery fails:** Run `librewheel.exe directinput` to verify `LibreWheel=yes` and check that the registered 64-bit and 32-bit `LibreWheelFF.dll` paths still exist on disk.
* **Quitting `LibreWheelTray.exe` stops FFB:** Exiting the tray intentionally stops active wheel output and prevents already-running games from recreating `LibreWheelOutput.exe`. Reopen the game to resume force feedback.
* **Linear fallback warning in tray:** Run `librewheel.exe settings` to inspect the parse error in `settings.ini` or your LUT file; until fixed, the driver stays operational with linear output.
* **USB disconnection:** Reacquire the wheel or reopen the game after reconnecting the device.

## Scope and Limitations

The driver runs a 500 Hz single-axis software mixer paired with an out-of-process HID output broker (`LibreWheelOutput.exe`) that stays idle when neutral and enforces a 150 ms active-torque watchdog. DirectInput registration advertises Constant Force, Ramp, Square, Sine, Triangle, Sawtooth Up/Down, and Spring (with attack/fade envelopes). The evaluator also contains Damper, Friction, and Inertia calculations, which are intentionally not advertised yet pending derivative filtering and hardware calibration (see [TODO.md](TODO.md)).

When the driver opens the wheel for force feedback, the output broker disables the firmware's default autocentering spring, leaving game-created spring effects to the mixer. PS4 mode (`046d:c260`), `DIPROP_AUTOCENTER`, effect triggers, custom forces, non-DirectInput FFB APIs, and game-specific workarounds are outside this release's verified scope.

## License and Development Notes

This project utilizes AI tools in development. Source is licensed under **GPL-3.0-or-later**; see [LICENSE](LICENSE). Builds use only the Windows SDK and standard Windows system components. Protocol and API references are documented in [docs/architecture.md](docs/architecture.md).
