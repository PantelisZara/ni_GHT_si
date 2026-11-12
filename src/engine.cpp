#include "ni_ght_si/engine.hpp"
#include <algorithm>
#include <stdexcept>
#include <pthread.h>

namespace night {
namespace {
EngineConfig validate(EngineConfig config) {
    for (auto interval : {config.system_interval, config.process_interval, config.io_interval,
                          config.capacity_interval, config.gpu_interval})
        if (interval < std::chrono::milliseconds{10} || interval > std::chrono::hours{1})
            throw std::invalid_argument("sampling intervals must be 10 ms..1 hour");
    if (!config.history_capacity || config.history_capacity > 2400 || config.capacity_paths.size() > 32)
        throw std::invalid_argument("history or selected mount budget exceeded");
    return config;
}
template<class T, class F> T guarded(F collect) {
    const auto begin = Clock::now();
    try { return collect(); }
    catch (const std::exception& error) {
        T sample;
        sample.stamp.begin = begin; sample.stamp.end = Clock::now();
        sample.stamp.quality = Quality::error;
        sample.stamp.issues.push_back({Quality::error, "collector", error.what()});
        return sample;
    }
}
}
EngineConfig sampling_configuration(RefreshMode mode) {
    EngineConfig config;
    switch (mode) {
    case RefreshMode::fast:
        config.system_interval = config.process_interval = std::chrono::milliseconds{250};
        config.gpu_interval = std::chrono::milliseconds{500};
        return config;
    case RefreshMode::balanced:
        return config;
    case RefreshMode::low_power:
        config.system_interval = config.process_interval = config.io_interval = config.gpu_interval
            = std::chrono::milliseconds{2000};
        config.capacity_interval = std::chrono::milliseconds{60000};
        return config;
    }
    throw std::invalid_argument("unknown refresh mode");
}

Engine::Engine(EngineConfig config, EngineSources sources)
    : config_(validate(std::move(config))), sources_(std::move(sources)), system_collector_(config_.proc_root),
      process_collector_(config_.proc_root), io_collector_(config_.proc_root, config_.sys_root),
      history_(config_.history_capacity) {
    start(&EngineConfig::system_interval, "night-system", [this] {
        auto sample = guarded<SystemSnapshot>([this] { return sources_.system ? sources_.system() : system_collector_.collect(); });
        sample.stamp.sequence = ++sequences_[0];
        { std::lock_guard lock(history_mutex_); history_.push({sample.stamp.end, sample.cpus, sample.available_bytes, sample.stamp.quality}); }
        system_.publish(std::move(sample));
    });
    start(&EngineConfig::process_interval, "night-process", [this, include_scopes = config_.include_application_scopes] {
        auto sample = guarded<ProcessSnapshot>([this, include_scopes] { return sources_.processes ? sources_.processes() : process_collector_.collect(include_scopes); });
        sample.stamp.sequence = ++sequences_[1]; processes_.publish(std::move(sample));
    });
    start(&EngineConfig::io_interval, "night-io", [this] {
        auto sample = guarded<IoSnapshot>([this] { return sources_.io ? sources_.io() : io_collector_.collect(); });
        sample.stamp.sequence = ++sequences_[2]; io_.publish(std::move(sample));
    });
    start(&EngineConfig::capacity_interval, "night-capacity", [this, paths = config_.capacity_paths] {
        auto sample = guarded<CapacitySnapshot>([this, &paths] { return sources_.capacity ? sources_.capacity() : collect_capacity(paths); });
        sample.stamp.sequence = ++sequences_[3]; capacity_.publish(std::move(sample));
    });
    if (sources_.gpu) start(&EngineConfig::gpu_interval, "night-gpu", [this] {
        auto sample = guarded<GpuSnapshot>([this] { return sources_.gpu(); });
        sample.stamp.sequence = ++sequences_[4]; gpu_.publish(std::move(sample));
    });
}

Engine::~Engine() {
    for (auto& worker : workers_) worker.request_stop();
    // Join before any collector, slot, or history member can be destroyed.
    workers_.clear();
}

EngineConfig Engine::configuration() const {
    std::lock_guard lock(schedule_mutex_);
    return config_;
}

bool Engine::setRefreshMode(RefreshMode mode) {
    const auto preset = sampling_configuration(mode); // Reject invalid values before any mutation.
    {
        std::lock_guard lock(schedule_mutex_);
        if (config_.system_interval == preset.system_interval && config_.process_interval == preset.process_interval
            && config_.io_interval == preset.io_interval && config_.capacity_interval == preset.capacity_interval
            && config_.gpu_interval == preset.gpu_interval) return false;
        config_.system_interval = preset.system_interval;
        config_.process_interval = preset.process_interval;
        config_.io_interval = preset.io_interval;
        config_.capacity_interval = preset.capacity_interval;
        config_.gpu_interval = preset.gpu_interval;
        schedule_changed_at_ = Clock::now();
        ++schedule_generation_;
    }
    schedule_wake_.notify_all();
    return true;
}

void Engine::start(std::chrono::milliseconds EngineConfig::* interval, const char* name, std::function<void()> task) {
    workers_.emplace_back([this, interval, name, task = std::move(task)](std::stop_token stop) {
        static_cast<void>(::pthread_setname_np(::pthread_self(), name));
        std::unique_lock lock(schedule_mutex_);
        auto generation = schedule_generation_;
        auto cadence = config_.*interval;
        auto deadline = Clock::now();
        const auto reschedule = [&] {
            generation = schedule_generation_;
            const auto next_cadence = config_.*interval;
            if (next_cadence == cadence) return; // Other lanes may have changed independently.
            cadence = next_cadence;
            const auto now = Clock::now();
            deadline = schedule_changed_at_ + cadence;
            // A change during a long collection never causes a catch-up call.
            if (deadline <= now) deadline = now + cadence;
        };
        while (!stop.stop_requested()) {
            lock.unlock();
            task(); // Never hold configuration/scheduling locks while collecting.
            lock.lock();
            const auto now = Clock::now();
            deadline += cadence;
            if (deadline <= now) deadline += cadence * ((now - deadline) / cadence + 1);
            if (generation != schedule_generation_) reschedule();
            while (!stop.stop_requested()
                   && schedule_wake_.wait_until(lock, stop, deadline, [&] { return generation != schedule_generation_; }))
                reschedule();
        }
    });
}

std::vector<HistoryPoint> Engine::history() const {
    std::lock_guard lock(history_mutex_); return history_.copy();
}
} // namespace night
