#pragma once
#include "gpu.hpp"
#include "io.hpp"
#include "process.hpp"
#include "system.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace night {
template<class T> class Latest {
public:
    void publish(T value) {
        auto next = std::make_shared<const T>(std::move(value));
        std::lock_guard lock(mutex_);
        if (unread_) ++overwrites_;
        value_ = std::move(next); unread_ = true;
    }
    std::shared_ptr<const T> read() const {
        std::lock_guard lock(mutex_); unread_ = false; return value_;
    }
    std::uint64_t overwrites() const { std::lock_guard lock(mutex_); return overwrites_; }
private:
    mutable std::mutex mutex_;
    mutable bool unread_{};
    std::shared_ptr<const T> value_;
    std::uint64_t overwrites_{};
};

template<class T> class Ring {
public:
    explicit Ring(std::size_t capacity) : values_(capacity) {
        if (!capacity) throw std::invalid_argument("history capacity must be positive");
    }
    void push(T value) {
        values_[next_] = std::move(value); next_ = (next_ + 1) % values_.size();
        size_ = std::min(size_ + 1, values_.size());
    }
    std::vector<T> copy() const {
        std::vector<T> result; result.reserve(size_);
        const auto start = (next_ + values_.size() - size_) % values_.size();
        for (std::size_t i = 0; i < size_; ++i) result.push_back(values_[(start + i) % values_.size()]);
        return result;
    }
private:
    std::vector<T> values_;
    std::size_t next_{}, size_{};
};

enum class RefreshMode { fast, balanced, low_power };

struct EngineConfig {
    std::filesystem::path proc_root{"/proc"}, sys_root{"/sys"};
    std::chrono::milliseconds system_interval{500}, process_interval{500}, io_interval{500}, capacity_interval{30000};
    std::chrono::milliseconds gpu_interval{1000};
    std::size_t history_capacity{1200};
    std::vector<std::filesystem::path> capacity_paths{"/"};
    bool include_application_scopes{false};
};

// Polling cadences, not counter averaging windows or achieved CPU budgets.
EngineConfig sampling_configuration(RefreshMode mode);

struct HistoryPoint {
    Clock::time_point time;
    std::vector<CpuReading> cpus;
    Reading<std::uint64_t> available_bytes;
    Quality quality{Quality::valid};
};

// Each optional source is owned and invoked by its one collector lane. Sources
// must eventually return; no C++ timeout can cancel uninterruptible kernel I/O.
struct EngineSources {
    std::function<SystemSnapshot()> system;
    std::function<ProcessSnapshot()> processes;
    std::function<IoSnapshot()> io;
    std::function<CapacitySnapshot()> capacity;
    std::function<GpuSnapshot()> gpu;
};

class Engine {
public:
    explicit Engine(EngineConfig config = {}, EngineSources sources = {});
    ~Engine();
    EngineConfig configuration() const;
    // Updates polling cadences without replacing collectors or resetting their baselines.
    bool setRefreshMode(RefreshMode mode);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    std::shared_ptr<const SystemSnapshot> system() const { return system_.read(); }
    std::shared_ptr<const ProcessSnapshot> processes() const { return processes_.read(); }
    std::shared_ptr<const IoSnapshot> io() const { return io_.read(); }
    std::shared_ptr<const CapacitySnapshot> capacity() const { return capacity_.read(); }
    std::shared_ptr<const GpuSnapshot> gpu() const { return gpu_.read(); }
    bool gpuEnabled() const { return static_cast<bool>(sources_.gpu); }
    std::vector<HistoryPoint> history() const;
private:
    void start(std::chrono::milliseconds EngineConfig::* interval, const char* name, std::function<void()> task);
    mutable std::mutex schedule_mutex_;
    std::condition_variable_any schedule_wake_;
    std::uint64_t schedule_generation_{};
    Clock::time_point schedule_changed_at_{};
    EngineConfig config_;
    EngineSources sources_;
    SystemCollector system_collector_;
    ProcessCollector process_collector_;
    IoCollector io_collector_;
    Latest<SystemSnapshot> system_;
    Latest<ProcessSnapshot> processes_;
    Latest<IoSnapshot> io_;
    Latest<CapacitySnapshot> capacity_;
    Latest<GpuSnapshot> gpu_;
    mutable std::mutex history_mutex_;
    Ring<HistoryPoint> history_;
    std::array<std::uint64_t, 5> sequences_{};
    std::vector<std::jthread> workers_;
};
} // namespace night
