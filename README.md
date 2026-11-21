# LibreWheel

LibreWheel is an experimental Windows driver for the Logitech G29. It uses the native Windows HID stack for steering, pedals and buttons, paired with a user-mode DirectInput COM driver for game-controlled force feedback. Both 64-bit and 32-bit DirectInput games share the same physical joystick identity. No kernel driver, Windows service, or virtual controller is installed.

> **Experimental Status:** Game compatibility and torque shutdown behavior under USB or host failure remain experimental. Review the [validation procedure](docs/validation.md) and [architecture and protocol evidence](docs/architecture.md) before use. This project assumes no liability for hardware damage.

## Build and Test

**Prerequisites:** Visual Studio 2022 (or Build Tools for Visual Studio 2022) with the *Desktop development with C++* workload, a Windows SDK, and CMake 3.24 or newer.

Run the following commands from the repository root:

```powershell
# 64-bit build and tests
cmake --preset x64
cmake --build --preset x64
ctest --preset x64

# 32-bit build and tests
cmake --preset x86
cmake --build --preset x86
ctest --preset x86
```

Each `build/<architecture>/Release` directory produces `librewheel.exe` and `LibreWheelFF.dll`. The DLL architecture must match the target game's process bitness.

* **CLI (`librewheel.exe`):** Provides raw HID diagnostics and a DirectInput client.
* **Test suite (`ctest`):** Covers effect scheduling, waveforms and conditions, LUT behavior, report encoding, COM DLL loading, and software shutdown paths for the output owner.

## Input and DirectInput Diagnostics

Connect the wheel with its physical mode switch set to PS3. The G29 can start with the shared `046d:c294` compatibility identity. `native` checks the hardware revision and switches a recognized G29 to `046d:c24f`. The wheel may recalibrate during the switch, so keep its travel clear. Run `native` again after reconnecting if the wheel returns to compatibility mode. PS4-mode identity `046d:c260` is currently unsupported by this implementation.

```powershell
.\build\x64\Release\librewheel.exe native
.\build\x64\Release\librewheel.exe list
.\build\x64\Release\librewheel.exe monitor
.\build\x64\Release\librewheel.exe directinput
```

Command | Description |
| --- | --- |
| `native` | Verifies the hardware revision and switches the wheel from `046d:c294` to `046d:c24f`. |
| `list` | Displays the HID collection along with input and output report sizes. |
| `monitor` | Streams raw steering, three pedal values, POV state, and a 25-bit button mask (paddles and shifter gears appear as ordinary HID buttons). Useful for verifying accessory mappings and pedal polarity without game-side calibration or transformation. Automatically watches for USB reconnections. |
| `directinput` | Lists the physical product, Windows FFB advertisement status, steering actuator flags, and advertised effects. |

## Register and Remove

Install

The registration script requires both the 64-bit and 32-bit DLLs and records their **absolute paths**, so keep the binaries in place after registering.

```powershell
.\tools\Register-LibreWheel.ps1 -Action Install `
  -Dll64 .\build\x64\Release\LibreWheelFF.dll `
  -Dll32 .\build\x86\Release\LibreWheelFF.dll
```

* The script preserves compatible existing steering actuator entries and refuses to overwrite an OEM FFB registration or an incompatible axis override.
* Any machine-wide Logitech FFB registration must be removed manually or through its own uninstaller first.

**Verify the installation:**

1. Reopen your DirectInput client or game.
2. Run `directinput` and confirm it shows `LibreWheel=yes` for the G29 and reports the steering actuator.
3. In the default build, `pulse right` and `pulse left` exercise the native API and verify effect status on the physical device in software. See the [validation procedure](docs/validation.md) for bench setup and torque checks.

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

The example configuration enables the bundled `LibreWheel-G29-Custom` force-feedback curve. To apply it:

1. Copy [settings.example.ini](settings.example.ini) to `%LOCALAPPDATA%\LibreWheel\settings.ini`.
2. Place [LibreWheel-G29-Custom.csv](LibreWheel-G29-Custom.csv) in the same directory.
3. Run `librewheel.exe settings` to print the active config path and verify that it parses cleanly.
4. Reopen your game to load the updated settings.

| Key | Description |
| --- | --- |
| `gain_percent` | User force multiplier from `0` to `100`. Defaults to `100`. |
| `range_degrees` | Optional hardware rotation range from `40` to `900`. Sent when a bench output session initializes. |
| `lut_enabled` | Boolean enabling the force-feedback LUT curve. Set to `true` in the example; set to `false` for linear output. |
| `lut_file` | UTF-8 path to the LUT CSV file. Relative paths resolve relative to `settings.ini`. |
| `diagnostics` | Boolean. Emits preview output state changes to the Windows debug output stream. |

CSV rows contain normalized input and corrected output magnitudes from 0 to 1. An optional header is `input,output`. Inputs must be strictly increasing and include `0,0` and an entry at input 1. The parser accepts blank lines and full-line `#` comments. Correction interpolates between entries and applies the original sign.

## Troubleshooting

* **Device not detected:** The installer does not manage system HID bindings. Confirm the USB identity and HID driver in Device Manager before troubleshooting COM registration.
* **Raw input works, but FFB discovery fails:** Check the `LibreWheel` flag using both the 64-bit and 32-bit CLI builds, and verify that the registered DLL paths still exist on disk.
* **USB disconnection:** Reacquire the wheel or reopen the game after reconnecting the device.

## Scope and Limitations

The registration advertises constant force, ramp, square, sine, triangle, both sawtooths and spring. The evaluator also contains damper, friction and inertia calculations for further bench development. Their derivative scaling and stability are unverified, so the registry does not advertise them. The bench driver disables the firmware's default centering when it opens the hardware, leaving game-created spring effects to the mixer. The `DIPROP_AUTOCENTER` toggle, effect triggers, custom forces, non-DirectInput FFB APIs and game-specific compatibility are outside this build's verified functionality.

## License and Development Notes

This project utilises AI tools in development. Source is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE). The build uses the Windows SDK and links to standard Windows components. The architecture records protocol and API references separately from the build dependencies.
