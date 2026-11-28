# LibreWheel TODO

## Damper and Inertia Condition Effects

`Spring` and `Friction` are advertised and verified on physical G29 hardware using the range-calibrated, low-latency filtered `MotionTracker` (`src/effects.cpp`).

`Damper` and `Inertia` are also implemented and calibrated in `src/effects.cpp` (`500 deg/s` velocity reference, `18000 deg/s²` acceleration reference), but are intentionally not advertised in DirectInput registration yet until bench-verified in games that exercise them:

- [x] Replace raw single-step finite-difference velocity and acceleration in `Session::track()` with a low-latency exponential-filtered `MotionTracker` (`src/effects.cpp`) that holds across normal 2 ms HID intervals and decays smoothly when stationary.
- [x] Calibrate Spring stiffness (`125°` full-scale physical angle), Damper velocity (`500 deg/s`), and Inertia acceleration (`18000 deg/s²`) normalization in physical degrees, accounting for the active `range_degrees`.
- [x] Replace the discontinuous `(velocity > 0) - (velocity < 0)` step in `ConditionKind::friction` with a critically-damped `tanh` viscous-to-Coulomb ramp (`60 deg/s` knee, `0.14` torque scale), and advertise `Friction` alongside `0–7`.
- [ ] Bench-verify `Damper` and `Inertia` stability, scaling, and feel on physical G29 hardware in games that use them, then advertise them in `src/windows/registration.cpp` and `tools/Register-LibreWheel.ps1`.
