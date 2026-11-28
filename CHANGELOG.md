# Changelog

All notable changes to LibreWheel are documented in this file.

This project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-11-28

### Added
- Advertised the DirectInput `Friction` condition effect (`GUID_Friction`, ID `10`) alongside effects `0–7`, using a critically-damped `tanh` viscous-to-Coulomb breakaway curve (`60°/s` knee, `0.14` torque scale) to provide realistic steering drag without zero-velocity buzz or delayed-loop oscillation.
- Added `MotionTracker` (`src/effects.cpp`), a continuous-time low-latency exponential derivative filter (`τ_v = 5 ms`, `τ_a = 20 ms`) calibrated in physical steering degrees using the active `range_degrees`, holding smoothly across 2 ms HID intervals and decaying cleanly when stationary.
- Refreshed `OEMForceFeedback\Effects` during upgrades and DLL switches in both `src/windows/registration.cpp` and `tools/Register-LibreWheel.ps1` so newly advertised or removed effect entries stay in sync.

### Changed
- Calibrated `Spring` stiffness in physical steering degrees (`125°` full-scale deflection at maximum coefficient, scaling proportionally with `range_degrees`) so centering torque at normal driving angles feels natural instead of falling inside the G29's mechanical deadzone.
- Calibrated `Damper` (`500°/s` reference velocity) and `Inertia` (`18000°/s²` reference acceleration) in the evaluator (remaining unadvertised until bench-verified in games that exercise them).

### Fixed
- Fixed a concurrency timestamp race in `Session::run()` (`src/windows/driver.cpp`) where sampling `now()` before acquiring `mutex_` could precede a concurrent `DownloadEffect(..., DIEP_START)` start timestamp, causing single-tick zero-torque dropouts and spurious `0x13` motor-stop USB reports in games streaming Constant Force updates with `DIEP_START` (such as Euro Truck Simulator 2).

## [0.1.0] - 2026-11-26

### Added
- Native user-mode DirectInput force-feedback COM driver (`LibreWheelFF.dll`, 64-bit and 32-bit) for the Logitech G29 in PS3 native mode (`046d:c24f`), using Windows' inbox HID stack with no kernel driver or Windows service.
- Single-axis software effect mixer supporting Constant Force, Ramp Force, Periodic waveforms (Square, Sine, Triangle, Sawtooth Up/Down), Spring condition, and attack/fade envelopes.
- Out-of-process HID output broker (`LibreWheelOutput.exe`) with atomic shared-memory mailbox, idle-neutral zero-wakeup wait, 150 ms active torque watchdog, transparent timeout recovery, and safe shutdown on game exit, tray quit, or external termination.
- Configurable gain, hardware steering operating range (40°–900°), optional comma- or pipe-delimited LUT force correction (bundled `LibreWheel-G29-Custom.lut`), linear fallback on invalid settings/LUTs, and optional ring-buffered FFB tracing (`log_ffb`).
- System tray utility (`LibreWheelTray.exe`) with automatic PS3 compatibility-to-native mode switching, operating range application on connect, LUT selection, linear-fallback menu warning, Per-Monitor V2 DPI scaling, Windows dark mode menu support, config folder shortcut, startup toggle, and clean driver output stop on exit.
- Diagnostic CLI (`librewheel.exe`) supporting `list`, `native`, `monitor` (with live input Hz readout), `directinput`, `settings`, `range`, `pulse`, and `version`.
- Native C++20 GPL-3.0-or-later self-extracting installer (`LibreWheel-0.1.0-Setup.exe`) and portable ZIP builder (`tools/Build-Installer.ps1`) with optional Logitech/OEM G29 registry conflict detection, automatic `.reg` backup export, conditional UAC elevation for `HKLM` removal, and Windows *Installed Apps* uninstaller integration.
- Automated `ctest` test suites (`core`, `driver`, `output`, `installer`) covering effects, settings fallback, COM lifecycle, out-of-process broker IPC, LZMS payload packing, PE checks, and `.reg` backup serialization.
