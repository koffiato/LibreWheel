# LibreWheel TODO

## Damper, Friction, and Inertia Condition Effects

`src/effects.cpp` already evaluates `Damper`, `Friction`, and `Inertia`, but `tools/Register-LibreWheel.ps1` intentionally does not advertise them to DirectInput yet because raw single-step finite-difference derivatives over short tick intervals are too noisy and uncalibrated for stable force feedback.

Before advertising these effects:

- [ ] Replace raw single-step finite-difference velocity and acceleration in `Session::track()` (`src/windows/driver.cpp`) with a low-latency filtered estimator (such as an alpha-beta or low-pass filter) so 1-count encoder quantization does not create large acceleration spikes.
- [ ] Calibrate velocity and acceleration normalization to realistic G29 angular rates, accounting for the active `range_degrees`.
- [ ] Replace the discontinuous `(velocity > 0) - (velocity < 0)` step in `ConditionKind::friction` (`src/effects.cpp`) with a narrow linear ramp around zero velocity (`std::clamp(v / v_knee, -1.0, 1.0)`) to avoid buzz when holding the rim stationary.
- [ ] Bench-verify stability, scaling, and feel on physical G29 hardware, then advertise `Damper`, `Inertia`, and `Friction` in `tools/Register-LibreWheel.ps1`.
