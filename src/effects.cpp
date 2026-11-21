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
    if (elapsed < envelope.attack) {
        return std::lerp(envelope.attack_level, level, fraction(elapsed, envelope.attack));
    }
    if (effect.duration != forever && envelope.fade > Time::zero()) {
        const auto remaining = effect.duration - elapsed;
        if (remaining < envelope.fade) {
            return std::lerp(envelope.fade_level, level, fraction(remaining, envelope.fade));
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
        case ConditionKind::damper: metric = motion.velocity; break;
        case ConditionKind::inertia: metric = motion.acceleration; break;
        case ConditionKind::friction: metric = (motion.velocity > 0) - (motion.velocity < 0); break;
        }
    }
    const double delta = metric - condition.offset;
    if (std::abs(delta) <= condition.deadband) {
        return 0;
    }
    const double coefficient = delta > 0 ? condition.positive_coefficient : condition.negative_coefficient;
    const double displacement = std::copysign(std::abs(delta) - condition.deadband, delta);
    return std::clamp(-coefficient * displacement, -condition.negative_limit, condition.positive_limit);
}

} // namespace

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
            while (next_id_ == 0 || contains(next_id_)) {
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

bool Mixer::contains(Id id) const noexcept { return find(id) != nullptr; }

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

bool Mixer::playing(Id id, Time now) const noexcept {
    const auto* slot = find(id);
    return slot && playing_at(*slot, clock_at(now));
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
        const auto elapsed = time - slot.started - slot.effect.delay;
        if (elapsed >= Time::zero() && output) {
            const auto duration = slot.effect.duration;
            const auto phase = duration == forever || elapsed < duration ? elapsed : elapsed % duration;
            mixed += evaluate(slot.effect, phase, motion);
        }
    }
    return {mixed * gain_, active};
}

} // namespace lw
