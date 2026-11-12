#pragma once

#include "ni_ght_si/common.hpp"

#include <array>
#include <map>

namespace night {
struct CpuReading {
    std::string name;
    Reading<double> busy_percent;
    Reading<double> iowait_percent; // Kernel accounting estimate; may reset.
};

struct SystemSnapshot {
    Stamp stamp;
    std::vector<CpuReading> cpus;
    Reading<std::uint64_t> total_bytes, available_bytes, used_bytes;
    Reading<std::uint64_t> swap_total_bytes, swap_used_bytes;
    Reading<double> uptime_seconds;
    Reading<std::array<double, 3>> load_average; // 1, 5, 15 min; not CPU percent.
};

using CpuTicks = std::array<std::uint64_t, 10>;
struct CpuSample {
    std::string name;
    std::optional<CpuTicks> ticks;
};

// Linux guest counters are included in user/nice; only the first eight sum.
std::vector<CpuSample> parse_cpu_stat(std::string_view text, std::vector<Issue>& issues,
                                      std::string_view source = "/proc/stat");
Reading<double> cpu_busy(const CpuTicks& before, const CpuTicks& after, double seconds);
Reading<double> cpu_iowait(const CpuTicks& before, const CpuTicks& after, double seconds);
void parse_meminfo(std::string_view text, SystemSnapshot& snapshot,
                   std::string_view source = "/proc/meminfo");
void parse_uptime(std::string_view text, SystemSnapshot& snapshot);
void parse_loadavg(std::string_view text, SystemSnapshot& snapshot);

class SystemCollector {
public:
    explicit SystemCollector(std::filesystem::path proc_root = "/proc", ClockSource clock_source = observe_clocks);
    SystemSnapshot collect();
    void reset_baselines();

private:
    std::filesystem::path proc_root_;
    std::uint64_t sequence_{};
    std::map<std::string, std::optional<CpuTicks>> previous_;
    std::optional<Clock::time_point> previous_time_;
    ClockSource clock_source_;
    CounterEpoch epoch_;
};
} // namespace night
