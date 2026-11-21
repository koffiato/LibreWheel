// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver.hpp"

#include "effects.hpp"
#include "ffb_trace.hpp"
#include "hid.hpp"
#include "settings.hpp"
#ifdef LIBREWHEEL_BENCH_OUTPUT
#include "output_lease.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

namespace lw::win {
namespace {

using namespace std::chrono_literals;

std::atomic_long objects = 0;
std::atomic_long server_locks = 0;

constexpr DWORD nominal = DI_FFNOMINALMAX;
constexpr auto sample_period = 4ms;
constexpr auto maximum_motion_age = 50ms;
constexpr auto lut_poll_interval = 250ms;

Time now() noexcept { return trace_time(); }
double level(LONG value) noexcept { return std::clamp(double(value) / nominal, -1.0, 1.0); }
double magnitude(DWORD value) noexcept { return double(std::min(value, nominal)) / nominal; }

template<class T>
const T* parameters(const DIEFFECT& effect) noexcept {
    if (effect.cbTypeSpecificParams != sizeof(T)) {
        return nullptr;
    }
    return static_cast<const T*>(effect.lpvTypeSpecificParams);
}

} // namespace

HRESULT translate_effect(DWORD type, const DIEFFECT* input, Effect& effect) noexcept {
    if (!input || (input->dwSize != sizeof(DIEFFECT) && input->dwSize != sizeof(DIEFFECT_DX5))) {
        return DIERR_INVALIDPARAM;
    }
    if (input->cAxes != 1 || !input->rgdwAxes || !input->rglDirection) {
        return DIERR_INCOMPLETEEFFECT;
    }
    const DWORD axis = *input->rgdwAxes;
    if (DIDFT_GETINSTANCE(axis) != 0 || (DIDFT_GETTYPE(axis) & DIDFT_AXIS) == 0) {
        return DIERR_INVALIDPARAM;
    }
    const DWORD coordinates = input->dwFlags & (DIEFF_CARTESIAN | DIEFF_SPHERICAL | DIEFF_POLAR);
    if (coordinates != DIEFF_CARTESIAN && coordinates != DIEFF_SPHERICAL) {
        return DIERR_UNSUPPORTED;
    }
    if (input->dwDuration == 0) {
        return DIERR_INVALIDPARAM;
    }
    effect.duration = input->dwDuration == INFINITE ? forever : Time(input->dwDuration);
    if (input->dwSize == sizeof(DIEFFECT)) {
        effect.delay = Time(input->dwStartDelay);
    }
    effect.gain = magnitude(input->dwGain);
    // DirectInput describes the source of the force. On one axis the zero
    // direction uses +X as the source, and a negative magnitude reverses it.
    effect.direction = coordinates == DIEFF_CARTESIAN && *input->rglDirection < 0 ? 1.0 : -1.0;

    if (input->lpEnvelope && type <= 6) {
        const auto& envelope = *input->lpEnvelope;
        if (envelope.dwSize != sizeof(DIENVELOPE)) {
            return DIERR_INVALIDPARAM;
        }
        effect.envelope = Envelope{magnitude(envelope.dwAttackLevel), magnitude(envelope.dwFadeLevel),
                                   Time(envelope.dwAttackTime), Time(envelope.dwFadeTime)};
    }
    switch (type) {
    case 0:
        if (const auto* value = parameters<DICONSTANTFORCE>(*input)) {
            effect.parameters = Constant{level(value->lMagnitude)};
            return DI_OK;
        }
        break;
    case 1:
        if (const auto* value = parameters<DIRAMPFORCE>(*input)) {
            if (effect.duration == forever) {
                return DIERR_INVALIDPARAM;
            }
            effect.parameters = Ramp{level(value->lStart), level(value->lEnd)};
            return DI_OK;
        }
        break;
    case 2: case 3: case 4: case 5: case 6:
        if (const auto* value = parameters<DIPERIODIC>(*input); value && value->dwPeriod != 0) {
            effect.parameters = Periodic{static_cast<Wave>(type - 2), magnitude(value->dwMagnitude),
                level(value->lOffset), double(value->dwPhase % 36000) / 36000,
                Time(value->dwPeriod)};
            return DI_OK;
        }
        break;
    case 7: case 8: case 9: case 10:
        if (const auto* value = parameters<DICONDITION>(*input)) {
            effect.parameters = Condition{static_cast<ConditionKind>(type - 7), level(value->lOffset),
                std::max(0.0, level(value->lDeadBand)), level(value->lPositiveCoefficient),
                level(value->lNegativeCoefficient), magnitude(value->dwPositiveSaturation),
                magnitude(value->dwNegativeSaturation)};
            return DI_OK;
        }
        break;
    default:
        return DIERR_UNSUPPORTED;
    }
    return DIERR_INVALIDPARAM;
}

namespace {

struct CommandRow {
    Time time{};
    const char* command;
    HRESULT status = DI_OK;
    DWORD handle = 0;
    DWORD type = 0;
    DWORD flags = 0;
    DWORD value = 0;
    DWORD effect_flags = 0;
    DWORD duration = 0;
    DWORD sample_period = 0;
    DWORD gain = 0;
    DWORD trigger = 0;
    DWORD trigger_repeat = 0;
    DWORD start_delay = 0;
    DWORD axis = 0;
    LONG direction = 0;
    DIENVELOPE envelope{};
    std::array<std::uint8_t, sizeof(DICONDITION)> parameters{};
    DWORD parameter_size = 0;

    void set_effect(const DIEFFECT& input) noexcept {
        effect_flags = input.dwFlags;
        duration = input.dwDuration;
        sample_period = input.dwSamplePeriod;
        gain = input.dwGain;
        trigger = input.dwTriggerButton;
        trigger_repeat = input.dwTriggerRepeatInterval;
        start_delay = input.dwSize == sizeof(DIEFFECT) ? input.dwStartDelay : 0;
        axis = *input.rgdwAxes;
        direction = *input.rglDirection;
        if (input.lpEnvelope && type <= 6) {
            envelope = *input.lpEnvelope;
        }
        parameter_size = input.cbTypeSpecificParams;
        std::memcpy(parameters.data(), input.lpvTypeSpecificParams, parameter_size);
    }
};

std::ostream& operator<<(std::ostream& out, const CommandRow& row) {
    out << row.time.count() << ',' << row.command << ',' << row.status << ',' << row.handle << ','
        << row.type << ',' << row.flags << ',' << row.value << ',' << row.effect_flags << ','
        << row.duration << ',' << row.sample_period << ',' << row.gain << ',' << row.trigger << ','
        << row.trigger_repeat << ',' << row.start_delay << ',' << row.axis << ',' << row.direction << ','
        << (row.envelope.dwSize != 0) << ',' << row.envelope.dwAttackLevel << ',' << row.envelope.dwAttackTime << ','
        << row.envelope.dwFadeLevel << ',' << row.envelope.dwFadeTime << ',';
    write_trace_bytes(out, row.parameters, row.parameter_size);
    return out;
}

struct MixRow {
    Time time;
    int steering;
    bool new_report;
    Motion motion;
    bool active;
    double mixed;
    double corrected;
    std::uint8_t force;
};

std::ostream& operator<<(std::ostream& out, const MixRow& row) {
    return out << row.time.count() << ',' << row.steering << ',' << row.new_report << ','
               << row.motion.position << ',' << row.motion.velocity << ',' << row.motion.acceleration << ','
               << row.motion.derivatives_valid << ',' << row.active << ',' << row.mixed << ','
               << row.corrected << ',' << unsigned(row.force);
}

class FfbTrace {
public:
    explicit FfbTrace(const Settings& settings) {
        const auto dir = settings_path().parent_path() / L"logs";
        std::filesystem::create_directories(dir);
        prefix_ = dir / (L"ffb-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(now().count()));
        commands.add({now(), "session_start", DI_OK, 0, 0, 0, settings.lut.has_value()});
    }

    ~FfbTrace() {
        try {
            commands.write(file(L"-commands.csv"),
                "time_us,command,hresult,handle,type,flags,value,effect_flags,duration_us,sample_period_us,"
                "effect_gain,trigger_button,trigger_repeat_us,start_delay_us,axis,direction,"
                "has_envelope,attack_level,attack_us,fade_level,fade_us,parameters_hex_le");
            mix.write(file(L"-mix.csv"),
                "time_us,steering_raw,new_report,position,velocity,acceleration,derivatives_valid,"
                "active,mixed,corrected,force_byte");
        } catch (...) {
            OutputDebugStringW(L"LibreWheel: unable to write the force-feedback session trace\n");
        }
    }

    std::filesystem::path next_usb_log() noexcept {
        try {
            return file(L"-usb-" + std::to_wstring(++leases_) + L".csv");
        } catch (...) {
            return {};
        }
    }

    TraceBuffer<CommandRow> commands;
    TraceBuffer<MixRow> mix;

private:
    std::filesystem::path file(const std::wstring& suffix) const {
        auto path = prefix_;
        path += suffix;
        return path;
    }

    std::filesystem::path prefix_;
    unsigned leases_ = 0;
};

class Session {
public:
    explicit Session(const std::wstring& path) {
#ifdef LIBREWHEEL_TEST_DRIVER
        if (path == L"fixture") {
            return;
        }
#endif
        input_ = std::make_unique<HidInput>(path);
        if (!input_->info().classic_output) {
            throw std::runtime_error("The G29 vendor output field is missing");
        }
        const auto config = settings_path();
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(config, error);
        settings_ = load_settings(config);
#ifdef LIBREWHEEL_BENCH_OUTPUT
        wchar_t enable[2]{};
        const auto length = GetEnvironmentVariableW(L"LIBREWHEEL_BENCH_OUTPUT", enable, 2);
#ifdef LIBREWHEEL_FULL_FORCE
        output_enabled_ = length == 0;
#endif
        output_enabled_ |= length == 1 && enable[0] == L'1';
#endif
        if (settings_.log_ffb) {
            try {
                trace_ = std::make_unique<FfbTrace>(settings_);
            } catch (...) {
                OutputDebugStringW(L"LibreWheel: unable to start the force-feedback trace\n");
            }
        }
        try {
            lut_thread_ = std::jthread([this, config, modified](std::stop_token stop) {
                watch_lut(stop, config, modified);
            });
        } catch (const std::system_error&) {
            OutputDebugStringW(L"LibreWheel: unable to start the LUT watcher\n");
        }
        thread_ = std::thread([this] { run(); });
    }

    ~Session() {
        lut_thread_.request_stop();
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (lut_thread_.joinable()) {
            lut_thread_.join();
        }
        if (trace_) {
            trace_->commands.add({now(), "session_end"});
        }
    }

    template<class Call> HRESULT access(Call&& call) {
        std::lock_guard lock(mutex_);
        if (lost_) {
            return DIERR_INPUTLOST;
        }
        const auto result = call(mixer_, now());
        wake_.notify_one();
        return result;
    }

    void record(CommandRow row, const DIEFFECT* effect = nullptr) noexcept {
        if (trace_) {
            if (effect) {
                row.set_effect(*effect);
            }
            trace_->commands.add(row);
        }
    }

private:
    void watch_lut(std::stop_token stop, const std::filesystem::path& config,
                   std::filesystem::file_time_type modified) noexcept {
        try {
            std::condition_variable_any wake;
            std::mutex wait_mutex;
            std::unique_lock wait_lock(wait_mutex);
            while (!stop.stop_requested()) {
                std::optional<Lut> next;
                if (reload_lut(config, modified, next)) {
                    std::lock_guard lock(mutex_);
                    if (closing_ || lost_) {
                        return;
                    }
                    settings_.lut.swap(next);
                    record({now(), "lut_reload", DI_OK, 0, 0, 0, settings_.lut.has_value()});
                }
                wake.wait_for(wait_lock, stop, lut_poll_interval, [] { return false; });
            }
        } catch (...) {
            OutputDebugStringW(L"LibreWheel: LUT watcher stopped\n");
        }
    }

    void track(unsigned steering, Time time) noexcept {
        const double position = steering >= 32768 ? (double(steering) - 32768) / 32767
                                                  : (double(steering) - 32768) / 32768;
        const auto interval = time - input_time_;
        if (motion_.position_valid && interval >= 1ms && interval <= maximum_motion_age) {
            const double seconds = std::chrono::duration<double>(interval).count();
            const double velocity = (position - motion_.position) / seconds;
            const double acceleration = motion_.derivatives_valid ? (velocity - motion_.velocity) / seconds : 0;
            motion_.velocity = velocity;
            motion_.acceleration = acceleration;
            motion_.derivatives_valid = true;
        } else {
            motion_.derivatives_valid = false;
        }
        motion_.position = position;
        motion_.position_valid = true;
        input_time_ = time;
    }

    void run() noexcept {
#ifdef LIBREWHEEL_BENCH_OUTPUT
        std::unique_ptr<OutputLease> output;
        Time last_tick{};
#endif
        std::unique_lock lock(mutex_);
        try {
            std::uint8_t previous = 0x80;
            int steering_raw = -1;
            input_->begin();
            while (!closing_) {
                const auto time = now();
                std::optional<unsigned> steering;
                // Drain already completed input reports so the force evaluator sees recent steering.
                // Bound the work in case the device fills the queue faster than we can read it.
                for (unsigned count = 0; count < 64; ++count) {
                    const auto input = input_->take_steering();
                    if (!input) {
                        break;
                    }
                    steering = *input;
                    input_->begin();
                }
                if (steering) {
                    track(*steering, time);
                    steering_raw = static_cast<int>(*steering);
                }
                if (time - input_time_ > maximum_motion_age) {
                    // A stationary G29 can stop sending reports. Its last position
                    // still applies while the outstanding read watches for changes.
                    motion_.derivatives_valid = false;
                }
                const auto [mixed, active] = mixer_.sample(time, motion_);
                const auto corrected = corrected_force(mixed, settings_);
                const auto force = g29::force_byte(corrected);
                if (trace_) {
                    trace_->mix.add({time, steering_raw, steering.has_value(), motion_, active, mixed, corrected, force});
                }
#ifdef LIBREWHEEL_BENCH_OUTPUT
                if (output && time - last_tick > std::chrono::milliseconds(lease_timeout_ms)) {
                    throw std::runtime_error("Mixer deadline missed");
                }
                if (output && !active) {
                    output.reset();
                }
                if (output_enabled_ && active && !output && force != 0x80) {
                    const auto log = trace_ ? trace_->next_usb_log() : std::filesystem::path{};
                    output = std::make_unique<OutputLease>(input_->info().path, settings_.range.value_or(0), log);
                    last_tick = now();
                    // Recompute after process startup, which may outlast the effect.
                    continue;
                }
                if (output && !output->publish(force)) {
                    throw std::runtime_error("Output process stopped");
                }
                last_tick = time;
#endif
                if (force != previous && settings_.diagnostics) {
                    // Report transitions between idle and active without logging the update stream.
                    if (force == 0x80 || previous == 0x80) {
                        OutputDebugStringW(force == 0x80 ? L"LibreWheel: preview torque returned to zero\n"
                                                          : L"LibreWheel: preview torque started\n");
                    }
                }
                previous = force;
                if (active) {
                    wake_.wait_for(lock, sample_period, [&] {
                        return closing_ || mixer_.paused() || !mixer_.enabled() || !mixer_.active(now());
                    });
                } else {
                    wake_.wait(lock);
                }
            }
        } catch (...) {
            mixer_.stop_all();
            lost_ = true;
            record({now(), "session_lost", DIERR_INPUTLOST});
            OutputDebugStringW(L"LibreWheel: session ended after a G29 I/O or timing error\n");
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    Mixer mixer_;
    Motion motion_;
    Time input_time_{};
    Settings settings_;
    std::unique_ptr<FfbTrace> trace_;
    std::unique_ptr<HidInput> input_;
    bool closing_ = false;
    bool lost_ = false;
#ifdef LIBREWHEEL_BENCH_OUTPUT
    bool output_enabled_ = false;
#endif
    std::jthread lut_thread_;
    std::thread thread_;
};

class Driver final : public IDirectInputEffectDriver {
public:
    Driver() noexcept { ++objects; }
    ~Driver() {
        session_.reset();
        --objects;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDirectInputEffectDriver) {
            return E_NOINTERFACE;
        }
        *result = static_cast<IDirectInputEffectDriver*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --refs_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DeviceID(DWORD, DWORD id, DWORD begin, DWORD, void* raw) override {
        std::lock_guard lock(mutex_);
        if (!begin) {
            if (session_ && id_ == id) {
                session_.reset();
            }
            return DI_OK;
        }
        const auto* init = static_cast<const DIHIDFFINITINFO*>(raw);
        if (!init || init->dwSize != sizeof(*init) || !init->pwszDeviceInterface || !*init->pwszDeviceInterface) {
            return DIERR_INVALIDPARAM;
        }
        if (session_ && id_ != id) {
            return DIERR_OTHERAPPHASPRIO;
        }
        try {
            if (session_) {
                const auto state = session_->access([](Mixer&, Time) { return DI_OK; });
                if (SUCCEEDED(state)) {
                    return DI_OK;
                }
                session_.reset();
            }
            session_ = std::make_unique<Session>(init->pwszDeviceInterface);
            id_ = id;
            return DI_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (const std::exception& error) {
            OutputDebugStringA(error.what());
            OutputDebugStringA("\n");
            return DIERR_DEVICENOTREG;
        }
    }

    HRESULT STDMETHODCALLTYPE GetVersions(LPDIDRIVERVERSIONS versions) override {
        if (!versions || versions->dwSize != sizeof(*versions)) {
            return DIERR_INVALIDPARAM;
        }
        versions->dwFirmwareRevision = 0;
        versions->dwHardwareRevision = 0;
        versions->dwFFDriverVersion = 0x00010000;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Escape(DWORD id, DWORD effect, LPDIEFFESCAPE) override {
        return reject_command(id, {{}, "escape", DIERR_UNSUPPORTED, effect});
    }

    HRESULT STDMETHODCALLTYPE SetGain(DWORD id, DWORD gain) override {
        return with_command(id, {{}, "set_gain", DI_OK, 0, 0, 0, gain}, [&](Mixer& mixer, Time) {
            mixer.set_gain(magnitude(gain));
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD id, DWORD command) override {
        return with_command(id, {{}, "device_command", DI_OK, 0, 0, 0, command},
                            [&](Mixer& mixer, Time time) -> HRESULT {
            switch (command) {
            case DISFFC_RESET: mixer.reset(); break;
            case DISFFC_STOPALL: mixer.stop_all(); break;
            case DISFFC_PAUSE: mixer.pause(time); break;
            case DISFFC_CONTINUE: mixer.resume(time); break;
            case DISFFC_SETACTUATORSON: mixer.set_enabled(true); break;
            case DISFFC_SETACTUATORSOFF: mixer.set_enabled(false); break;
            default: return DIERR_INVALIDPARAM;
            }
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(DWORD id, LPDIDEVICESTATE state) override {
        if (!state || state->dwSize != sizeof(*state)) {
            return DIERR_INVALIDPARAM;
        }
        return with(id, [&](Mixer& mixer, Time time) {
            state->dwLoad = mixer.size() * 100 / Mixer::capacity;
            state->dwState = mixer.enabled() ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF;
            if (!mixer.size()) state->dwState |= DIGFFS_EMPTY;
            if (!mixer.active(time)) state->dwState |= DIGFFS_STOPPED;
            if (mixer.paused()) state->dwState |= DIGFFS_PAUSED;
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE DownloadEffect(DWORD id, DWORD type, DWORD* handle,
                                             const DIEFFECT* input, DWORD flags) override {
        CommandRow row{{}, "download", DI_OK, 0, type, flags};
        if (!handle) {
            row.status = DIERR_INVALIDPARAM;
            return reject_command(id, row);
        }
        Effect effect;
        const auto status = translate_effect(type, input, effect);
        if (FAILED(status)) {
            row.status = status;
            return reject_command(id, row);
        }
        return with_command(id, row, [&](Mixer& mixer, Time time) -> HRESULT {
            if (flags & DIEP_NODOWNLOAD) {
                return DI_OK;
            }
            if (*handle && !mixer.update(*handle, effect, time)) {
                *handle = 0;
            }
            if (!*handle) {
                *handle = mixer.add(effect);
                if (!*handle) {
                    return DIERR_DEVICEFULL;
                }
            }
            if (flags & DIEP_START) {
                mixer.start(*handle, time, 1);
            }
            return DI_OK;
        }, input, handle);
    }

    HRESULT STDMETHODCALLTYPE DestroyEffect(DWORD id, DWORD effect) override {
        return with_command(id, {{}, "destroy", DI_OK, effect}, [&](Mixer& mixer, Time) -> HRESULT {
            return mixer.erase(effect) ? DI_OK : DIERR_NOTDOWNLOADED;
        });
    }

    HRESULT STDMETHODCALLTYPE StartEffect(DWORD id, DWORD effect, DWORD flags, DWORD iterations) override {
        CommandRow row{{}, "start", DI_OK, effect, 0, flags, iterations};
        if (!iterations || (flags & ~(DIES_SOLO | DIES_NODOWNLOAD))) {
            row.status = DIERR_INVALIDPARAM;
            return reject_command(id, row);
        }
        return with_command(id, row, [&](Mixer& mixer, Time time) -> HRESULT {
            return mixer.start(effect, time, iterations, (flags & DIES_SOLO) != 0) ?
                DI_OK : DIERR_NOTDOWNLOADED;
        });
    }

    HRESULT STDMETHODCALLTYPE StopEffect(DWORD id, DWORD effect) override {
        return with_command(id, {{}, "stop", DI_OK, effect}, [&](Mixer& mixer, Time) -> HRESULT {
            return mixer.stop(effect) ? DI_OK : DIERR_NOTDOWNLOADED;
        });
    }

    HRESULT STDMETHODCALLTYPE GetEffectStatus(DWORD id, DWORD effect, DWORD* status) override {
        if (!status) {
            return DIERR_INVALIDPARAM;
        }
        *status = 0;
        return with(id, [&](Mixer& mixer, Time time) -> HRESULT {
            if (!mixer.contains(effect)) {
                return DIERR_NOTDOWNLOADED;
            }
            *status = mixer.playing(effect, time) ? DIEGES_PLAYING : 0;
            return DI_OK;
        });
    }

private:
    template<class Call> HRESULT with(DWORD id, Call&& call) {
        std::lock_guard lock(mutex_);
        return session_ && id == id_ ? session_->access(call) : DIERR_NOTEXCLUSIVEACQUIRED;
    }

    template<class Call>
    HRESULT with_command(DWORD id, CommandRow row, Call&& call, const DIEFFECT* input = nullptr,
                         const DWORD* handle = nullptr) {
        return with(id, [&](Mixer& mixer, Time time) {
            row.time = time;
            row.status = call(mixer, time);
            if (handle) {
                row.handle = *handle;
            }
            session_->record(row, input);
            return row.status;
        });
    }

    HRESULT reject_command(DWORD id, CommandRow row) {
        with_command(id, row, [&](Mixer&, Time) { return row.status; });
        return row.status;
    }

    std::atomic_ulong refs_ = 1;
    std::mutex mutex_;
    std::unique_ptr<Session> session_;
    DWORD id_ = 0;
};

class Factory final : public IClassFactory {
public:
    Factory() noexcept { ++objects; }
    ~Factory() { --objects; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) {
            return E_NOINTERFACE;
        }
        *result = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --refs_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        auto* driver = new (std::nothrow) Driver;
        if (!driver) {
            return E_OUTOFMEMORY;
        }
        const auto status = driver->QueryInterface(iid, result);
        driver->Release();
        return status;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            ++server_locks;
        } else {
            auto count = server_locks.load();
            while (count > 0 && !server_locks.compare_exchange_weak(count, count - 1)) {}
        }
        return S_OK;
    }

private:
    std::atomic_ulong refs_ = 1;
};

} // namespace
} // namespace lw::win

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (!result) {
        return E_POINTER;
    }
    *result = nullptr;
    if (clsid != lw::win::driver_class) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) lw::win::Factory;
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const auto status = factory->QueryInterface(iid, result);
    factory->Release();
    return status;
}

HRESULT WINAPI DllCanUnloadNow() {
    return lw::win::objects == 0 && lw::win::server_locks == 0 ? S_OK : S_FALSE;
}
