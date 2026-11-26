// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>

namespace lw {

using Time = std::chrono::microseconds;
inline constexpr Time forever = Time::max();
inline constexpr std::uint32_t repeat_forever = UINT32_MAX;

struct Constant { double level; };
struct Ramp { double from, to; };
enum class Wave { square, sine, triangle, saw_up, saw_down };
struct Periodic { Wave wave; double magnitude, offset, phase; Time period; };
enum class ConditionKind { spring, damper, inertia, friction };
struct Condition {
    ConditionKind kind;
    double offset, deadband;
    double positive_coefficient, negative_coefficient;
    double positive_limit, negative_limit;
};
struct Envelope { double attack_level, fade_level; Time attack, fade; };
struct Effect {
    std::variant<Constant, Ramp, Periodic, Condition> parameters = Constant{0};
    std::optional<Envelope> envelope;
    Time duration = forever;
    Time delay{};
    double gain = 1;
    double direction = 1; // Physical force direction for unconditioned effects.
};

struct Motion {
    double position = 0;
    double velocity = 0;
    double acceleration = 0;
    bool position_valid = false;
    bool derivatives_valid = false;
};

double evaluate(const Effect& effect, Time elapsed, const Motion& motion) noexcept;

class Mixer {
public:
    static constexpr unsigned capacity = 32;
    using Id = std::uint32_t;
    struct Sample {
        double force;
        bool active;
        bool uses_motion;
    };

    Id add(const Effect& effect) noexcept;
    bool update(Id id, const Effect& effect, Time now) noexcept;
    bool erase(Id id) noexcept;
    bool start(Id id, Time now, std::uint32_t iterations, bool solo = false) noexcept;
    bool stop(Id id) noexcept;
    std::optional<bool> playing(Id id, Time now) const noexcept;
    bool active(Time now) const noexcept;
    bool needs_motion() const noexcept;
    unsigned size() const noexcept;
    void stop_all() noexcept;
    void reset() noexcept;
    void pause(Time now) noexcept;
    void resume(Time now) noexcept;
    bool paused() const noexcept;
    void set_enabled(bool enabled) noexcept;
    bool enabled() const noexcept;
    void set_gain(double gain) noexcept;
    Sample sample(Time now, const Motion& motion) noexcept;

private:
    struct Slot {
        Id id = 0;
        Effect effect;
        Time started{};
        std::uint32_t iterations = 0;
    };
    Time clock_at(Time now) const noexcept;
    static bool playing_at(const Slot& slot, Time now) noexcept;
    std::uint32_t slot_bit(const Slot& slot) const noexcept;
    void stop_slot(Slot& slot) noexcept;
    Slot* find(Id id) noexcept;
    const Slot* find(Id id) const noexcept;

    static_assert(capacity <= 32);
    std::array<Slot, capacity> slots_{};
    std::uint32_t started_slots_ = 0;
    Id next_id_ = 1;
    double gain_ = 1;
    bool enabled_ = true;
    std::optional<Time> paused_at_;
    Time paused_total_{};
};

} // namespace lw
