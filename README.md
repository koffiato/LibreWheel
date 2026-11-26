# LibreWheel

LibreWheel is an experimental Windows driver for the Logitech G29. It uses the native Windows HID stack for steering, pedals and buttons, paired with a user-mode DirectInput COM driver for game-controlled force feedback. Both 64-bit and 32-bit DirectInput games share the same physical joystick identity. No kernel driver, Windows service, or virtual controller is installed.

> **Experimental Status:** Game compatibility and torque shutdown behavior under USB or host failure remain experimental. Review the [architecture and protocol evidence](docs/architecture.md) before use. This project assumes no liability for hardware damage.

## Installer (Recommended)

Run `LibreWheel-Setup.exe` to install the full-range 64-bit and 32-bit drivers, `LibreWheelTray.exe`, and `librewheel.exe` per-user into `%LOCALAPPDATA%\Programs\LibreWheel` (default config and LUT files are copied to `%LOCALAPPDATA%\LibreWheel` if absent).

* **Logitech/OEM Registry Conflict Detection:** If an existing Logitech or other third-party G29 `OEMForceFeedback` registry entry is detected (in `HKLM` or `HKCU`), the installer warns that LibreWheel force feedback will not work while it remains and offers to back it up (`%LOCALAPPDATA%\LibreWheel\backups\`) and remove it. Removing a machine-wide (`HKLM`) entry prompts for Administrator permission (UAC); choosing to keep the existing entry installs the files without registering LibreWheel. For silent installs, pass `/REMOVELOGITECH` to opt into automatic backup and removal.
* **Uninstall:** Uninstall from Windows Settings (*Installed apps*) after closing games; LibreWheel's per-user COM/DirectInput registration and startup entry are cleaned up automatically without touching user config or backups.

## Build and Test

**Prerequisites:** Visual Studio 2022 (or Build Tools for Visual Studio 2022) with the *Desktop development with C++* workload, a Windows SDK, and CMake 3.24 or newer.

Run the following commands from the repository root:

```powershell
# 64-bit build and tests (software-only default, no motor output)
cmake --preset x64
cmake --build --preset x64
ctest --preset x64

# 32-bit build and tests (software-only default, no motor output)
cmake --preset x86
cmake --build --preset x86
ctest --preset x86
```

Additional presets enable the out-of-process hardware output owner (`LibreWheelOutput.exe`):

* `bench-x64` / `bench-x86`: Torque-limited (~10%) hardware validation builds (opt-in at runtime via `LIBREWHEEL_BENCH_OUTPUT=1`).
* `full-force-x64` / `full-force-x86`: Full-range motor output builds (enabled by default unless `LIBREWHEEL_BENCH_OUTPUT=0`).

Each `build/<preset>/Release` directory produces `LibreWheelFF.dll`, `librewheel.exe`, `LibreWheelTray.exe`, and (for `bench-*` / `full-force-*` presets) `LibreWheelOutput.exe`. The DLL architecture must match the target game's process bitness.

* **CLI (`librewheel.exe`):** Raw HID diagnostics (including live input polling rate), DirectInput probing, settings checks, steering range control, and pulse tests.
* **Tray (`LibreWheelTray.exe`):** Automatically switches arriving PS3 compatibility-mode G29 wheels to native mode, applies configured `range_degrees` on connect, selects `.csv`/`.lut` curves, warns in the menu if invalid settings/LUTs trigger a linear fallback, respects Windows dark mode and Per-Monitor V2 DPI scaling, opens the config folder, manages Start with Windows, and stops active wheel output when exited.
* **Installer (`tools/Build-Installer.ps1`):** Auto-detects Visual Studio CMake and Inno Setup 6, builds `full-force-x64` and `full-force-x86` (with optional MSVC PGO profiles), runs `ctest` on both architectures (pass `-SkipTests` to skip), and outputs `build/installer/LibreWheel-Setup.exe` plus `SHA256SUMS.txt`.
* **Test suite (`ctest`):** Presets cover `x64`, `x86`, `bench-*`, and `full-force-*`, testing effect scheduling, waveforms and conditions, settings/LUT linear fallback, report encoding, COM DLL loading and lifecycle, FFB trace ring buffering, and both in-process and out-of-process broker IPC shutdown paths.

## Input and DirectInput Diagnostics

Connect the wheel with its physical mode switch set to PS3. The G29 can start with the shared `046d:c294` compatibility identity. `native` checks the hardware revision and switches a recognized G29 to `046d:c24f`. The wheel may recalibrate during the switch, so keep its travel clear. Run `native` again after reconnecting if the wheel returns to compatibility mode (or keep `LibreWheelTray.exe` running to switch automatically). PS4-mode identity `046d:c260` is currently unsupported by this implementation.

```powershell
.\build\x64\Release\librewheel.exe native
.\build\x64\Release\librewheel.exe list
.\build\x64\Release\librewheel.exe monitor
.\build\x64\Release\librewheel.exe directinput
.\build\x64\Release\librewheel.exe settings
```

| Command | Description |
| --- | --- |
| `native` | Verifies the hardware revision and switches the wheel from `046d:c294` to `046d:c24f`. |
| `list` | Displays connected native G29 HID collections with revision, input/output report sizes, and classic output availability. |
| `monitor` | Streams raw steering, three pedal values, POV state, 25-bit button mask, and live input report rate (`Hz`). Automatically watches for USB reconnections. |
| `directinput` | Lists the physical product, Windows FFB advertisement status, `LibreWheel` registration flag, steering actuator flags, and advertised effects. |
| `settings` | Prints the active `settings.ini` path and verifies that it (and any enabled LUT) parses cleanly. |
| `range <40-900>` | Sends a hardware steering operating range (40°–900°) to a connected native G29 when no game holds FFB output. |
| `pulse <left\|right> [percent]` | Runs a 1-second DirectInput constant-force pulse (1–100%, default 5%) and checks effect start and expiration status. |

## Register and Remove

### Install

The registration script requires both the 64-bit and 32-bit DLLs and records their **absolute paths**, so keep the binaries in place after registering (or use `LibreWheel-Setup.exe` to install into `%LOCALAPPDATA%\Programs\LibreWheel`).

```powershell
.\tools\Register-LibreWheel.ps1 -Action Install `
  -Dll64 .\build\x64\Release\LibreWheelFF.dll `
  -Dll32 .\build\x86\Release\LibreWheelFF.dll
```

* The script preserves compatible existing steering actuator entries and refuses to overwrite an OEM FFB registration or an incompatible axis override by default.
* To inspect or back up and remove a conflicting Logitech/OEM G29 FFB registration from the CLI, use `-Action DetectConflict`, `-Action RemoveExistingOem` (with optional `-Scope User|Machine|All` and `-BackupDir`), or pass `-RemoveExistingOem` alongside `-Action Install` (run elevated for `HKLM` entries). Backups are exported to `%LOCALAPPDATA%\LibreWheel\backups\` before deletion.

**Verify the installation:**

1. Reopen your DirectInput client or game.
2. Run `directinput` and confirm it shows `LibreWheel=yes` for the G29 and reports the steering actuator.
3. Run `pulse right` and `pulse left` to exercise the DirectInput API and verify effect status on the physical device. In the default (`x64`/`x86`) build this runs without motor output; physical torque requires a `bench-*` or `full-force-*` build (see [docs/architecture.md](docs/architecture.md)).

### Switch Builds

To point an existing LibreWheel installation to newly compiled binaries, run `-Action Switch` with the updated DLL paths, then reopen your game so it loads the new DLL:

```powershell
.\tools\Register-LibreWheel.ps1 -Action Switch `
  -Dll64 <path-to-new-x64-LibreWheelFF.dll> `
  -Dll32 <path-to-new-x86-LibreWheelFF.dll>
```

### Remove

Close all running games before uninstalling:

```powershell
.\tools\Register-LibreWheel.ps1 -Action Remove
```

This removes LibreWheel's per-user FFB and COM registrations only. Compiled binaries and user settings can be deleted separately.

## Configuration

Settings are read from `%LOCALAPPDATA%\LibreWheel\settings.ini`. The example configuration defaults to linear output (`lut_enabled=false`) and references the bundled `LibreWheel-G29-Custom.lut` curve.

1. Copy [settings.example.ini](settings.example.ini) to `%LOCALAPPDATA%\LibreWheel\settings.ini`.
2. Place [LibreWheel-G29-Custom.lut](LibreWheel-G29-Custom.lut) in the same directory.
3. Set `lut_enabled=true` in `settings.ini` (or select a curve from `LibreWheelTray.exe`) to enable LUT correction.
4. Run `librewheel.exe settings` to print the active config path and verify that it parses cleanly.
5. Reopen your game to apply configuration or LUT changes.

Settings and LUT curves are loaded once when a game acquires the wheel (they are not live-reloaded mid-session). If `settings.ini` or an enabled LUT file is missing or invalid, the driver falls back to linear output instead of failing force-feedback initialization, and `LibreWheelTray.exe` displays a warning in its menu.

| Key | Description |
| --- | --- |
| `gain_percent` | User force multiplier from `0` to `100`. Defaults to `100`. |
| `range_degrees` | Optional hardware rotation range from `40` to `900`. Sent when a hardware output session initializes and when `LibreWheelTray.exe` detects a native G29 while idle. |
| `lut_enabled` | Boolean enabling the force-feedback LUT curve. Defaults to `false` for linear output. |
| `lut_file` | UTF-8 path to the LUT file (`.csv` or `.lut`). Relative paths resolve relative to `settings.ini`. |
| `diagnostics` | Boolean. Emits torque idle/active transitions to the Windows debug output stream. |
| `log_ffb` | Boolean. Saves DirectInput command, mixer, and USB output traces to `%LOCALAPPDATA%\LibreWheel\logs\`. |

LUT rows contain comma-delimited (`,`) or pipe-delimited (`|`) normalized input and corrected output magnitudes from `0` to `1`. An optional header is `input,output`. Inputs must be strictly increasing and include `0,0` and an entry at input `1`. The parser accepts blank lines and full-line `#` comments. Correction linearly interpolates between entries and preserves the original sign.

## Troubleshooting

* **Device not detected:** The installer does not manage system HID bindings. Confirm the USB identity and HID driver in Device Manager before troubleshooting COM registration.
* **Raw input works, but FFB discovery fails:** Check the `LibreWheel` flag using both the 64-bit and 32-bit CLI builds, and verify that the registered DLL paths still exist on disk.
* **USB disconnection:** Reacquire the wheel or reopen the game after reconnecting the device.

## Scope and Limitations

The registration advertises constant force, ramp, square, sine, triangle, both sawtooths and spring. The evaluator also contains damper, friction and inertia calculations (see [TODO.md](TODO.md) for the derivative filtering and calibration work required before advertising them). When a hardware output build opens the wheel, the output owner disables the firmware's default centering, leaving game-created spring effects to the mixer. The `DIPROP_AUTOCENTER` toggle, effect triggers, custom forces, non-DirectInput FFB APIs and game-specific compatibility are outside this build's verified functionality.

## License and Development Notes

This project utilises AI tools in development. Source is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE). The build uses the Windows SDK and links to standard Windows components. The architecture records protocol and API references separately from the build dependencies.
