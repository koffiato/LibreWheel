// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.hpp"
#include "protocol.hpp"
#include "settings.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
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
    const Motion stationary{0.65, -0.5, 0.2, true, false};
    effect.parameters = Condition{ConditionKind::spring, 0.1, 0.05, 1, 0.5, 0.8, 0.3};
    equal(evaluate(effect, 0ms, motion), -0.3, "spring uses position, offset and negative limit");
    equal(evaluate(effect, 0ms, stationary), -0.3, "stationary spring retains the last steering position");
    auto& condition = std::get<Condition>(effect.parameters);
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
    check(!mixer.playing(finite, 520ms), "expired effect status");

    effect.duration = 100ms;
    check(mixer.update(finite, effect, 520ms), "edit expired effect");
    check(!mixer.playing(finite, 520ms), "edit does not restart expired effect");

    effect.delay = {};
    effect.duration = forever;
    effect.parameters = Constant{1};
    const auto first = mixer.add(effect);
    const auto second = mixer.add(effect);
    mixer.start(first, 520ms, repeat_forever);
    mixer.start(second, 520ms, repeat_forever);
    mixer.set_gain(0.25);
    equal(mixer.sample(521ms, {}).force, 0.5, "effects are summed before global gain");
    mixer.set_enabled(false);
    equal(mixer.sample(522ms, {}).force, 0, "actuators off");
    check(mixer.playing(first, 522ms), "muted effects continue timing");
    mixer.set_enabled(true);
    mixer.start(second, 523ms, 1, true);
    equal(mixer.sample(523ms, {}).force, 0.25, "solo stops other effects");
    check(mixer.erase(second), "destroy active effect");
    equal(mixer.sample(524ms, {}).force, 0, "destroy silences output");
    mixer.reset();
    check(mixer.size() == 0 && !mixer.contains(first), "reset unloads effects");
    Mixer::Id last = 0;
    for (unsigned n = 0; n < Mixer::capacity; ++n) {
        last = mixer.add(effect);
        check(last != 0, "allocate slot");
    }
    check(mixer.add(effect) == 0, "fixed maximum effect count");
    check(mixer.start(last, 525ms, 1) && mixer.sample(525ms, {}).active, "last slot can play");
    mixer.stop_all();
    check(!mixer.sample(526ms, {}).active, "stop all clears the last slot");
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

void live_lut() {
    const std::filesystem::path fixture = "lut-reload-test";
    if (!std::filesystem::create_directory(fixture)) {
        check(false, "create LUT reload fixture");
        return;
    }
    const auto ini = fixture / "settings.ini";
    const auto first_curve = fixture / "first.csv";
    const auto second_curve = fixture / "second.lut";
    struct Cleanup {
        std::array<std::filesystem::path, 4> paths;
        ~Cleanup() {
            for (const auto& path : paths) {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        }
    } cleanup{{ini, first_curve, second_curve, fixture}};

    const std::string initial = "gain_percent=50\nlut_enabled=false\nlut_file=first.csv\n";
    std::ofstream(ini) << initial;
    std::ofstream(first_curve) << "0,0\n.25,.5\n1,1\n";
    std::ofstream(second_curve) << "0|0\n.25|.75\n1|1\n";
    auto settings = load_settings(ini);
    auto loaded = std::filesystem::last_write_time(ini);
    auto modified = loaded;
    const auto select = [&](LutSelection selection) {
        std::ofstream(ini) << write_lut_selection("gain_percent=10\n", selection);
        modified += 2s;
        std::filesystem::last_write_time(ini, modified);
        return reload_lut(ini, loaded, settings.lut);
    };
    check(!reload_lut(ini, loaded, settings.lut), "unchanged INI skips reload");
    check(select({"first.csv", true}), "enable a curve in a running session");
    equal(corrected_force(0.5, settings), 0.5, "reload applies the curve at the existing gain");
    check(select({"second.lut", true}), "switch curves in a running session");
    equal(corrected_force(0.5, settings), 0.75, "replacement curve applies");
    check(!select({"missing.csv", true}), "reject a missing replacement curve");
    equal(corrected_force(0.5, settings), 0.75, "failed reload keeps the previous curve");
    check(select({"second.lut", false}), "disable the active curve in a running session");
    equal(corrected_force(0.5, settings), 0.25, "disabling the curve restores linear output");
}

void protocol() {
    using namespace lw::g29;
    check(native_switch_candidate(vendor_id, compatibility_product_id, 0x1350), "G29 compatibility revision");
    check(native_switch_candidate(vendor_id, compatibility_product_id, 0x8900), "later G29 revision family");
    check(!native_switch_candidate(vendor_id, compatibility_product_id, 0x1300), "reject DFGT compatibility revision");
    check(!native_switch_candidate(vendor_id, product_id, 0x1350), "native G29 does not need a mode switch");
    check(!native_switch_candidate(0x1234, compatibility_product_id, 0x1350), "reject unrelated vendor");
    check(reset_mode_on_usb_reset == std::array<std::uint8_t, 8>{0, 0xf8, 0x0a},
          "mode reverts on USB reset");
    check(switch_to_native_mode == std::array<std::uint8_t, 8>{0, 0xf8, 0x09, 5, 1, 1},
          "native mode switch with detach");
    const auto force = torque(force_byte(1));
    check(force == OutputReport{0, 0x11, 0x08, 0x00, 0x80}, "right force and zero padding");
    check(torque(force_byte(-1)) == OutputReport{0, 0x11, 0x08, 0xff, 0x80}, "left force");
    check(torque(force_byte(0)) == OutputReport{0, 0x13}, "neutral stops the slot");
    check(disable_centering() == OutputReport{0, 0xf5}, "disable built-in spring");
    check(operating_range(900) == OutputReport{0, 0xf8, 0x81, 0x84, 0x03}, "range little endian");
    check(!operating_range(39) && !operating_range(901), "range bounds");
}

} // namespace

int main() {
    effects();
    lifecycle();
    correction();
    lut_selection();
    live_lut();
    protocol();
    return failures != 0;
}
