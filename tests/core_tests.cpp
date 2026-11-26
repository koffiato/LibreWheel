// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.hpp"
#include "protocol.hpp"
#include "settings.hpp"
#include "version.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

using namespace lw;
using namespace std::chrono_literals;

int failures = 0;

void check(bool actual, const char* description) {
    if (!actual) {
        ++failures;
        std::cerr << description << '\n';
    }
}

void equal(double actual, double expected, const char* description) {
    if (!(std::abs(actual - expected) < 1e-8)) {
        ++failures;
        std::cerr << description << ": expected " << expected << ", got " << actual << '\n';
    }
}

void effects() {
    Effect effect;
    effect.parameters = Constant{-0.8};
    effect.gain = 0.5;
    effect.direction = -1;
    effect.duration = 100ms;
    effect.envelope = Envelope{0.2, 0.4, 20ms, 20ms};
    equal(evaluate(effect, 0ms, {}), 0.1, "attack start with signed direction");
    equal(evaluate(effect, 10ms, {}), 0.25, "attack interpolation");
    equal(evaluate(effect, 21ms, {}), 0.4, "sustain with effect gain");
    equal(evaluate(effect, 90ms, {}), 0.3, "fade interpolation");
    equal(evaluate(effect, 100ms, {}), 0, "finite duration boundary");

    // Overlapping attack and fade scale proportionally to meet continuously at the sustain level.
    effect.envelope = Envelope{0.2, 0.4, 60ms, 60ms};
    equal(evaluate(effect, 0ms, {}), 0.1, "overlapping envelope attack start");
    equal(evaluate(effect, 25ms, {}), 0.25, "overlapping envelope attack midpoint");
    equal(evaluate(effect, 50ms, {}), 0.4, "overlapping envelope meets continuously at sustain");
    equal(evaluate(effect, 75ms, {}), 0.3, "overlapping envelope fade midpoint");
    equal(evaluate(effect, 99ms, {}), 0.204, "overlapping envelope approaches fade level");

    effect = {};
    effect.parameters = Ramp{-1, 1};
    effect.duration = 40ms;
    equal(evaluate(effect, 10ms, {}), -0.5, "ramp interpolation");
    equal(evaluate(effect, 30ms, {}), 0.5, "ramp crosses zero");

    const double expected[][4] = {
        {1, 1, -1, -1}, {0, 1, 0, -1}, {1, 0, -1, 0},
        {-1, -0.5, 0, 0.5}, {1, 0.5, 0, -0.5}
    };
    effect = {};
    for (unsigned wave = 0; wave < std::size(expected); ++wave) {
        effect.parameters = Periodic{static_cast<Wave>(wave), 1.0, 0.0, 0.25, 40ms};
        for (unsigned quarter = 0; quarter < 4; ++quarter) {
            equal(evaluate(effect, 10ms * quarter, {}), expected[wave][(quarter + 1) % 4],
                  "periodic phase and wrap");
        }
    }
    effect.parameters = Periodic{Wave::sine, 1.0, 0.5, 0.25, 40ms};
    equal(evaluate(effect, 0ms, {}), 1, "offset periodic saturation");

    const Motion motion{0.65, -0.5, 0.2, true, true};
    const Motion negative_motion{-0.65, -0.5, 0.2, true, true};
    const Motion stationary{0.65, -0.5, 0.2, true, false};
    effect.parameters = Condition{ConditionKind::spring, 0.1, 0.05, 1, 0.5, 0.3, 0.2};
    equal(evaluate(effect, 0ms, motion), -0.3, "positive-side spring clamps to positive saturation");
    equal(evaluate(effect, 0ms, negative_motion), 0.2, "negative-side spring clamps to negative saturation");
    equal(evaluate(effect, 0ms, stationary), -0.3, "stationary spring retains the last steering position");
    auto& condition = std::get<Condition>(effect.parameters);
    condition.positive_limit = 0.8;
    condition.negative_limit = 0.8;
    condition.kind = ConditionKind::damper;
    equal(evaluate(effect, 0ms, motion), 0.275, "damper uses velocity");
    equal(evaluate(effect, 0ms, stationary), 0, "damper suppresses stale velocity");
    condition.kind = ConditionKind::inertia;
    equal(evaluate(effect, 0ms, motion), -0.05, "inertia uses acceleration");
    condition.kind = ConditionKind::friction;
    equal(evaluate(effect, 0ms, motion), 0.525, "friction uses direction of motion");
    equal(evaluate(effect, 0ms, {}), 0, "missing input has no condition force");
}

void lifecycle() {
    Mixer mixer;
    Effect effect;
    effect.duration = 20ms;
    effect.delay = 5ms;
    effect.parameters = Constant{0.75};
    const auto finite = mixer.add(effect);
    check(mixer.start(finite, 0ms, 2), "start finite effect");
    const auto delayed = mixer.sample(4ms, {});
    equal(delayed.force, 0, "start delay");
    check(delayed.active, "delayed effect keeps the mixer scheduled");
    equal(mixer.sample(5ms, {}).force, 0.75, "play after start delay");
    equal(mixer.sample(25ms, {}).force, 0.75, "second iteration starts immediately");
    mixer.pause(26ms);
    const auto paused = mixer.sample(500ms, {});
    equal(paused.force, 0, "pause suppresses output");
    check(!paused.active, "pause leaves output idle");
    mixer.resume(500ms);
    equal(mixer.sample(518ms, {}).force, 0.75, "continue preserves phase");
    const auto expired = mixer.sample(519ms, {});
    equal(expired.force, 0, "last iteration expired");
    check(!expired.active && !mixer.active(519ms), "expiration clears active state");
    effect.duration = 100ms;
    check(mixer.start(finite, 519ms, 1) && mixer.update(finite, effect, 544ms) &&
          mixer.playing(finite, 544ms) == false, "edit does not restart an expired effect between mixer ticks");

    effect.delay = {};
    effect.duration = forever;
    effect.parameters = Constant{1};
    const auto first = mixer.add(effect);
    const auto second = mixer.add(effect);
    mixer.start(first, 544ms, repeat_forever);
    mixer.start(second, 544ms, repeat_forever);
    mixer.set_gain(0.25);
    equal(mixer.sample(545ms, {}).force, 0.5, "effects are summed before global gain");
    mixer.set_enabled(false);
    equal(mixer.sample(546ms, {}).force, 0, "actuators off");
    check(mixer.playing(first, 546ms) == true, "muted effects continue timing");
    mixer.set_enabled(true);
    mixer.start(second, 547ms, 1, true);
    equal(mixer.sample(547ms, {}).force, 0.25, "solo stops other effects");
    check(mixer.erase(second), "destroy active effect");
    equal(mixer.sample(548ms, {}).force, 0, "destroy silences output");
    mixer.reset();
    check(mixer.size() == 0 && !mixer.playing(first, 548ms).has_value(), "reset unloads effects");
    Mixer::Id last = 0;
    for (unsigned n = 0; n < Mixer::capacity; ++n) {
        last = mixer.add(effect);
        check(last != 0, "allocate slot");
    }
    check(mixer.add(effect) == 0, "fixed maximum effect count");
    check(mixer.start(last, 549ms, 1) && mixer.sample(549ms, {}).active, "last slot can play");
    check(!mixer.needs_motion() && !mixer.sample(549ms, {}).uses_motion,
          "constant force alone does not require HID steering input");
    mixer.stop_all();
    check(!mixer.sample(550ms, {}).active, "stop all clears the last slot");

    mixer.reset();
    mixer.set_gain(1.0);
    Effect spring;
    spring.duration = 20ms;
    spring.parameters = Condition{ConditionKind::spring, 0, 0, 1, 1, 1, 1};
    const auto spring_id = mixer.add(spring);
    check(!mixer.needs_motion(), "stopped spring does not require motion");
    check(mixer.start(spring_id, 600ms, 1) && mixer.needs_motion(), "started spring requires motion");
    const auto active_spring = mixer.sample(605ms, Motion{0.5, 0, 0, true, false});
    check(active_spring.active && active_spring.uses_motion, "active spring reports uses_motion");
    equal(active_spring.force, -0.5, "active spring evaluates position");
    mixer.pause(606ms);
    check(!mixer.needs_motion() && !mixer.sample(610ms, Motion{0.5, 0, 0, true, false}).uses_motion,
          "paused spring does not require motion");
    mixer.resume(610ms);
    mixer.set_enabled(false);
    check(!mixer.needs_motion() && !mixer.sample(612ms, Motion{0.5, 0, 0, true, false}).uses_motion,
          "disabled actuators do not require motion");
    mixer.set_enabled(true);
    check(mixer.needs_motion(), "re-enabled spring requires motion again");
    const auto expired_spring = mixer.sample(630ms, Motion{0.5, 0, 0, true, false});
    check(!expired_spring.active && !expired_spring.uses_motion && !mixer.needs_motion(),
          "expired spring clears motion requirement");
}

void correction() {
    std::istringstream csv("# measured inverse response\ninput,output\n0,0\n.2,.4\n1,1\n");
    const auto lut = Lut::read(csv);
    Settings settings;
    settings.gain = 0.25;
    equal(corrected_force(0.8, settings), 0.2, "default linear bypass");
    settings.lut = lut;
    equal(corrected_force(0.4, settings), 0.2, "gain precedes interpolation");
    equal(corrected_force(-0.8, settings), -0.4, "LUT preserves sign");
    equal(corrected_force(2, settings), 0.625, "mix applies gain before clamp");
    equal(corrected_force(-8, settings), -1, "clamp to LUT domain");
    equal(corrected_force(0.0, settings), 0, "neutral preserved");
    equal(corrected_force(std::numeric_limits<double>::quiet_NaN(), settings), 0, "NaN is neutral");
    for (const char* bad : {"0,.01\n1,1", "0,0\n.2,.3\n.2,.4\n1,1", "0,0\n.8,1",
                            "0,0\n1,1.1", "0,0\n1,nan", "0,0\n1,1,2"}) {
        try {
            std::istringstream input(bad);
            Lut::read(input);
            check(false, "invalid LUT rejected");
        } catch (const std::runtime_error&) {}
    }

    std::istringstream assetto_corsa("0|0\n.2|.4\n1|1\n");
    equal(Lut::read(assetto_corsa).apply(0.4), lut.apply(0.4), "pipe-delimited LUT uses the same interpolation");
}

void lut_selection() {
    const std::string original = "\xef\xbb\xbf# setup\r\ngain_percent=70\r\n"
                                 "lut_enabled =false\r\nlut_file=original.csv\r\ndiagnostics=true";
    const auto updated = write_lut_selection(original, {"new curve.lut", true});
    check(updated == "\xef\xbb\xbf# setup\r\ngain_percent=70\r\n"
                     "lut_enabled =true\r\nlut_file=new curve.lut\r\ndiagnostics=true",
          "selecting a curve edits only the LUT settings");
    std::istringstream saved(updated);
    const auto enabled = read_lut_selection(saved);
    check(enabled.enabled && enabled.file == "new curve.lut", "selected LUT can be read without loading the curve");

    std::istringstream disabled(write_lut_selection(updated, {enabled.file, false}));
    const auto unchecked = read_lut_selection(disabled);
    check(!unchecked.enabled && unchecked.file == enabled.file, "unchecking a LUT preserves the configured filename");
    check(write_lut_selection("# no LUT keys", {"a.csv", true}) == "# no LUT keys\nlut_enabled=true\nlut_file=a.csv\n",
          "older settings gain the LUT keys");
}

void settings_fallback() {
    const std::filesystem::path fixture = "settings-fallback-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(fixture, cleanup_error);
    if (!std::filesystem::create_directory(fixture)) {
        check(false, "create settings fallback fixture");
        return;
    }
    const auto ini = fixture / "settings.ini";
    const auto valid_curve = fixture / "valid.csv";
    const auto invalid_curve = fixture / "invalid.csv";
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
        }
    } cleanup{fixture};

    const auto missing = load_settings_with_fallback(ini);
    check(missing.fallback_reason.empty() && !missing.settings.lut && missing.settings.gain == 1.0,
          "missing settings.ini uses default linear settings without fallback warning");

    std::ofstream(valid_curve) << "0,0\n.25,.5\n1,1\n";
    std::ofstream(invalid_curve) << "0,.1\n1,1\n";

    std::ofstream(ini) << "gain_percent=50\nrange_degrees=540\nlut_enabled=true\nlut_file=valid.csv\n";
    const auto ok = load_settings_with_fallback(ini);
    check(ok.fallback_reason.empty() && ok.settings.lut.has_value() && ok.settings.range == 540u,
          "valid settings and LUT load cleanly");
    equal(corrected_force(0.5, ok.settings), 0.5, "loaded LUT applies at configured gain");

    std::ofstream(ini) << "gain_percent=50\nrange_degrees=540\nlut_enabled=true\nlut_file=missing.csv\n";
    const auto missing_lut = load_settings_with_fallback(ini);
    check(!missing_lut.fallback_reason.empty() && !missing_lut.settings.lut && missing_lut.settings.range == 540u,
          "missing LUT falls back to linear while preserving valid settings");
    equal(corrected_force(0.5, missing_lut.settings), 0.25, "linear fallback keeps configured gain");

    std::ofstream(ini) << "gain_percent=50\nlut_enabled=true\nlut_file=invalid.csv\n";
    const auto bad_lut = load_settings_with_fallback(ini);
    check(!bad_lut.fallback_reason.empty() && !bad_lut.settings.lut, "invalid LUT falls back to linear");
    equal(corrected_force(0.5, bad_lut.settings), 0.25, "invalid LUT fallback preserves linear gain");

    std::ofstream(ini) << "not_a_valid_key=123\n";
    const auto bad_ini = load_settings_with_fallback(ini);
    check(!bad_ini.fallback_reason.empty() && !bad_ini.settings.lut && bad_ini.settings.gain == 1.0,
          "corrupt settings.ini falls back to linear defaults");
}

void protocol() {
    using namespace lw::g29;
    check(native_switch_candidate(vendor_id, compatibility_product_id, 0x1350), "G29 compatibility revision");
    check(native_switch_candidate(vendor_id, compatibility_product_id, 0x8900), "later G29 revision family");
    check(!native_switch_candidate(vendor_id, compatibility_product_id, 0x1300), "reject DFGT compatibility revision");
    check(!native_switch_candidate(vendor_id, product_id, 0x1350), "native G29 does not need a mode switch");
    check(!native_switch_candidate(0x1234, compatibility_product_id, 0x1350), "reject unrelated vendor");
    const auto force = torque(force_byte(1));
    check(force == OutputReport{0, 0x11, 0x08, 0x00, 0x80}, "right force and zero padding");
    check(torque(force_byte(-1)) == OutputReport{0, 0x11, 0x08, 0xff, 0x80}, "left force");
    check(torque(force_byte(0)) == OutputReport{0, 0x13}, "neutral stops the slot");
    check(disable_centering() == OutputReport{0, 0xf5}, "disable built-in spring");
    check(operating_range(900) == OutputReport{0, 0xf8, 0x81, 0x84, 0x03}, "range little endian");
    check(!operating_range(39) && !operating_range(901), "range bounds");
}

void versioning() {
    const std::string expected_core = std::to_string(lw::version::major) + "." +
                                      std::to_string(lw::version::minor) + "." +
                                      std::to_string(lw::version::patch);
    check(lw::version::core == expected_core, "SemVer core matches major.minor.patch");
    check(lw::version::full.starts_with(lw::version::core), "SemVer full starts with core version");
    check(lw::version::wide.size() == lw::version::full.size(), "wide version string matches narrow length");
}

} // namespace

int main() {
    effects();
    lifecycle();
    correction();
    lut_selection();
    settings_fallback();
    protocol();
    versioning();
    return failures != 0;
}
