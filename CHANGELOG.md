# Changelog

All notable changes to LibreWheel are documented in this file.

This project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

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
