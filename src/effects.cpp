// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace lw {
namespace {

double fraction(Time elapsed, Time duration) noexcept {
    return std::clamp(double(elapsed.count()) / double(duration.count()), 0.0, 1.0);
}

double shaped_level(double level, const Effect& effect, Time elapsed) noexcept {
    if (!effect.envelope) {
        return level;
    }
    const auto& envelope = *effect.envelope;
    auto attack = envelope.attack;
    auto fade = envelope.fade;
    if (effect.duration != forever && attack > Time::zero() && fade > Time::zero() &&
        attack > effect.duration - fade) {
        const double total = double(attack.count()) + double(fade.count());
        const auto scaled_attack = static_cast<Time::rep>(
            std::llround(double(effect.duration.count()) * (double(attack.count()) / total)));
        attack = Time(std::clamp<Time::rep>(scaled_attack, 0, effect.duration.count()));
        fade = effect.duration - attack;
    }
    if (elapsed < attack) {
        return std::lerp(envelope.attack_level, level, fraction(elapsed, attack));
    }
    if (effect.duration != forever && fade > Time::zero()) {
        const auto remaining = effect.duration - elapsed;
        if (remaining < fade) {
            return std::lerp(envelope.fade_level, level, fraction(remaining, fade));
        }
    }
    return level;
}

double waveform(Wave wave, double phase) noexcept {
    switch (wave) {
    case Wave::square:   return phase < 0.5 ? 1.0 : -1.0;
    case Wave::sine:     return std::sin(2 * std::numbers::pi * phase);
    case Wave::triangle: return 4.0 * std::abs(phase - 0.5) - 1.0;
    case Wave::saw_up:   return 2.0 * phase - 1.0;
    case Wave::saw_down: return 1.0 - 2.0 * phase;
    }
    return 0;
}

double condition_force(const Condition& condition, const Motion& motion) noexcept {
    double metric = 0;
    if (condition.kind == ConditionKind::spring) {
        if (!motion.position_valid) {
            return 0;
        }
        metric = motion.position;
    } else {
        if (!motion.derivatives_valid) {
            return 0;
        }
        switch (condition.kind) {
        case ConditionKind::spring: break;
        case ConditionKind::damper:
        case ConditionKind::friction:
            metric = motion.velocity;
            break;
        case ConditionKind::inertia:
            metric = motion.acceleration;
            break;
        }
    }
    const double delta = metric - condition.offset;
    if (std::abs(delta) <= condition.deadband) {
        return 0;
    }
    const double coefficient = delta > 0 ? condition.positive_coefficient : condition.negative_coefficient;
    const double limit = delta > 0 ? condition.positive_limit : condition.negative_limit;
    const double displacement = std::copysign(std::abs(delta) - condition.deadband, delta);
    if (condition.kind == ConditionKind::spring) {
        const double scale = std::max(1.0, motion.spring_scale);
        return std::clamp(-coefficient * scale * displacement, -limit, limit);
    }
    if (condition.kind == ConditionKind::friction) {
        constexpr double knee = MotionTracker::friction_knee_deg_s / MotionTracker::reference_velocity_deg_s;
        const double force = -coefficient * MotionTracker::friction_torque_scale * std::tanh(displacement / knee);
        return std::clamp(force, -limit, limit);
    }
    return std::clamp(-coefficient * displacement, -limit, limit);
}

} // namespace

MotionTracker::MotionTracker(double range_degrees) noexcept {
    set_range_degrees(range_degrees);
}

void MotionTracker::set_range_degrees(double range_degrees) noexcept {
    half_range_deg_ = std::clamp(range_degrees, 40.0, 900.0) * 0.5;
}

void MotionTracker::reset() noexcept {
    position_ = 0;
    angle_deg_ = 0;
    velocity_deg_s_ = 0;
    acceleration_deg_s2_ = 0;
    last_report_time_ = {};
    last_step_time_ = {};
    initialized_ = false;
}

Motion MotionTracker::step(std::optional<unsigned> steering_raw, Time time) noexcept {
    constexpr double tau_v = 0.005;
    constexpr double tau_a = 0.020;
    constexpr auto report_hold_window = std::chrono::microseconds(4000);
    const double spring_scale = std::max(1.0, half_range_deg_ / spring_full_degrees);

    if (steering_raw) {
        const unsigned raw = *steering_raw;
        const double next_pos = raw >= 32768 ? (double(raw) - 32768.0) / 32767.0
                                             : (double(raw) - 32768.0) / 32768.0;
        const double next_angle = next_pos * half_range_deg_;
        if (!initialized_) {
            position_ = next_pos;
            angle_deg_ = next_angle;
            velocity_deg_s_ = 0;
            acceleration_deg_s2_ = 0;
            last_report_time_ = time;
            last_step_time_ = time;
            initialized_ = true;
            return {position_, 0.0, 0.0, spring_scale, true, true};
        }
        // Use the interval between actual HID reports (unless we already started decaying).
        const auto base_time = std::max(last_report_time_, last_step_time_);
        const double dt = std::chrono::duration<double>(time - base_time).count();
        if (dt <= 0.0 || dt > 0.05) {
            position_ = next_pos;
            angle_deg_ = next_angle;
            velocity_deg_s_ = 0;
            acceleration_deg_s2_ = 0;
            last_report_time_ = time;
            last_step_time_ = time;
            return {position_, 0.0, 0.0, spring_scale, true, true};
        }
        const double decay_v = std::exp(-dt / tau_v);
        const double gain_v = dt > 1e-6 ? (1.0 - decay_v) / dt : 1.0 / tau_v;
        const double next_vel = decay_v * velocity_deg_s_ + gain_v * (next_angle - angle_deg_);

        const double decay_a = std::exp(-dt / tau_a);
        const double gain_a = dt > 1e-6 ? (1.0 - decay_a) / dt : 1.0 / tau_a;
        const double next_acc = decay_a * acceleration_deg_s2_ + gain_a * (next_vel - velocity_deg_s_);

        position_ = next_pos;
        angle_deg_ = next_angle;
        velocity_deg_s_ = std::abs(next_vel) < 1e-4 ? 0.0 : next_vel;
        acceleration_deg_s2_ = std::abs(next_acc) < 1e-3 ? 0.0 : next_acc;
        last_report_time_ = time;
        last_step_time_ = time;
        return {position_,
                velocity_deg_s_ / reference_velocity_deg_s,
                acceleration_deg_s2_ / reference_acceleration_deg_s2,
                spring_scale, true, true};
    }

    if (!initialized_) {
        return {0.0, 0.0, 0.0, spring_scale, false, false};
    }

    // Do not decay between normal 2 ms USB HID reports; only decay once reports stop arriving.
    if (time - last_report_time_ <= report_hold_window) {
        return {position_,
                velocity_deg_s_ / reference_velocity_deg_s,
                acceleration_deg_s2_ / reference_acceleration_deg_s2,
                spring_scale, true, true};
    }

    const double dt = std::chrono::duration<double>(time - last_step_time_).count();
    if (dt <= 0.0) {
        return {position_,
                velocity_deg_s_ / reference_velocity_deg_s,
                acceleration_deg_s2_ / reference_acceleration_deg_s2,
                spring_scale, true, true};
    }
    if (dt > 0.05) {
        velocity_deg_s_ = 0;
        acceleration_deg_s2_ = 0;
        last_step_time_ = time;
        return {position_, 0.0, 0.0, spring_scale, true, true};
    }
    const double decay_v = std::exp(-dt / tau_v);
    const double next_vel = decay_v * velocity_deg_s_;

    const double decay_a = std::exp(-dt / tau_a);
    const double gain_a = dt > 1e-6 ? (1.0 - decay_a) / dt : 1.0 / tau_a;
    const double next_acc = decay_a * acceleration_deg_s2_ + gain_a * (next_vel - velocity_deg_s_);

    velocity_deg_s_ = std::abs(next_vel) < 1e-4 ? 0.0 : next_vel;
    acceleration_deg_s2_ = std::abs(next_acc) < 1e-3 ? 0.0 : next_acc;
    last_step_time_ = time;
    return {position_,
            velocity_deg_s_ / reference_velocity_deg_s,
            acceleration_deg_s2_ / reference_acceleration_deg_s2,
            spring_scale, true, true};
}

double evaluate(const Effect& effect, Time elapsed, const Motion& motion) noexcept {
    if (elapsed < Time::zero() || elapsed >= effect.duration) {
        return 0;
    }
    if (const auto* value = std::get_if<Condition>(&effect.parameters)) {
        return condition_force(*value, motion) * effect.gain;
    }
    double force = 0;
    if (const auto* constant = std::get_if<Constant>(&effect.parameters)) {
        force = std::copysign(shaped_level(std::abs(constant->level), effect, elapsed), constant->level);
    } else if (const auto* ramp = std::get_if<Ramp>(&effect.parameters)) {
        const double level = std::lerp(ramp->from, ramp->to, fraction(elapsed, effect.duration));
        force = std::copysign(shaped_level(std::abs(level), effect, elapsed), level);
    } else if (const auto* periodic = std::get_if<Periodic>(&effect.parameters)) {
        if (periodic->period <= Time::zero()) {
            return 0;
        }
        const double cycle = fraction(elapsed % periodic->period, periodic->period) + periodic->phase;
        force = periodic->offset +
            waveform(periodic->wave, cycle - std::floor(cycle)) *
            shaped_level(periodic->magnitude, effect, elapsed);
        force = std::clamp(force, -1.0, 1.0);
    }
    return force * effect.direction * effect.gain;
}

Mixer::Id Mixer::add(const Effect& effect) noexcept {
    for (auto& slot : slots_) {
        if (slot.id == 0) {
            while (next_id_ == 0 || find(next_id_)) {
                ++next_id_;
            }
            slot = Slot{next_id_++, effect};
            return slot.id;
        }
    }
    return 0;
}

Mixer::Slot* Mixer::find(Id id) noexcept {
    if (id != 0) {
        for (auto& slot : slots_) {
            if (slot.id == id) {
                return &slot;
            }
        }
    }
    return nullptr;
}

const Mixer::Slot* Mixer::find(Id id) const noexcept {
    if (id != 0) {
        for (const auto& slot : slots_) {
            if (slot.id == id) {
                return &slot;
            }
        }
    }
    return nullptr;
}

bool Mixer::update(Id id, const Effect& effect, Time now) noexcept {
    auto* slot = find(id);
    if (!slot) {
        return false;
    }
    if (slot->iterations && !playing_at(*slot, clock_at(now))) {
        stop_slot(*slot);
    }
    slot->effect = effect;
    return true;
}

bool Mixer::erase(Id id) noexcept {
    auto* slot = find(id);
    if (!slot) {
        return false;
    }
    stop_slot(*slot);
    *slot = {};
    return true;
}

bool Mixer::start(Id id, Time now, std::uint32_t iterations, bool solo) noexcept {
    auto* slot = find(id);
    if (!slot || iterations == 0) {
        return false;
    }
    if (solo) {
        stop_all();
    }
    slot->started = clock_at(now);
    slot->iterations = iterations;
    started_slots_ |= slot_bit(*slot);
    return true;
}

std::uint32_t Mixer::slot_bit(const Slot& slot) const noexcept {
    return std::uint32_t{1} << (&slot - slots_.data());
}

void Mixer::stop_slot(Slot& slot) noexcept {
    slot.iterations = 0;
    started_slots_ &= ~slot_bit(slot);
}

bool Mixer::stop(Id id) noexcept {
    auto* slot = find(id);
    if (!slot) {
        return false;
    }
    stop_slot(*slot);
    return true;
}

Time Mixer::clock_at(Time now) const noexcept {
    return paused_at_.value_or(now) - paused_total_;
}

bool Mixer::playing_at(const Slot& slot, Time now) noexcept {
    if (!slot.iterations || slot.effect.duration <= Time::zero()) {
        return false;
    }
    const auto elapsed = now - slot.started - slot.effect.delay;
    return elapsed < slot.effect.duration || slot.effect.duration == forever ||
           slot.iterations == repeat_forever || elapsed / slot.effect.duration < slot.iterations;
}

std::optional<bool> Mixer::playing(Id id, Time now) const noexcept {
    const auto* slot = find(id);
    if (!slot) {
        return std::nullopt;
    }
    return playing_at(*slot, clock_at(now));
}

bool Mixer::active(Time now) const noexcept {
    const auto time = clock_at(now);
    for (auto slots = started_slots_; slots; slots &= slots - 1) {
        if (playing_at(slots_[std::countr_zero(slots)], time)) {
            return true;
        }
    }
    return false;
}

bool Mixer::needs_motion() const noexcept {
    if (!enabled_ || paused()) {
        return false;
    }
    for (auto slots = started_slots_; slots; slots &= slots - 1) {
        if (std::holds_alternative<Condition>(slots_[std::countr_zero(slots)].effect.parameters)) {
            return true;
        }
    }
    return false;
}

unsigned Mixer::size() const noexcept {
    return static_cast<unsigned>(std::ranges::count_if(slots_, [](const Slot& slot) { return slot.id != 0; }));
}

void Mixer::stop_all() noexcept {
    for (auto slots = started_slots_; slots; slots &= slots - 1) {
        slots_[std::countr_zero(slots)].iterations = 0;
    }
    started_slots_ = 0;
}

void Mixer::reset() noexcept {
    slots_ = {};
    started_slots_ = 0;
    paused_at_.reset();
    paused_total_ = Time::zero();
    enabled_ = true;
}

void Mixer::pause(Time now) noexcept {
    if (!paused_at_) {
        paused_at_ = now;
    }
}

void Mixer::resume(Time now) noexcept {
    if (paused_at_) {
        paused_total_ += now - *paused_at_;
        paused_at_.reset();
    }
}

bool Mixer::paused() const noexcept { return paused_at_.has_value(); }
void Mixer::set_enabled(bool enabled) noexcept { enabled_ = enabled; }
bool Mixer::enabled() const noexcept { return enabled_; }
void Mixer::set_gain(double gain) noexcept { gain_ = std::clamp(gain, 0.0, 1.0); }

Mixer::Sample Mixer::sample(Time now, const Motion& motion) noexcept {
    double mixed = 0;
    bool active = false;
    bool uses_motion = false;
    const bool output = enabled_ && !paused();
    const auto time = clock_at(now);
    // Visit only started slots, in slot order so the sum is unchanged.
    for (auto slots = started_slots_; slots; slots &= slots - 1) {
        auto& slot = slots_[std::countr_zero(slots)];
        if (!playing_at(slot, time)) {
            stop_slot(slot);
            continue;
        }
        active = output;
        if (output && std::holds_alternative<Condition>(slot.effect.parameters)) {
            uses_motion = true;
        }
        const auto elapsed = time - slot.started - slot.effect.delay;
        if (elapsed >= Time::zero() && output) {
            const auto duration = slot.effect.duration;
            const auto phase = duration == forever || elapsed < duration ? elapsed : elapsed % duration;
            mixed += evaluate(slot.effect, phase, motion);
        }
    }
    return {mixed * gain_, active, uses_motion};
}

} // namespace lw
