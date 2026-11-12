#pragma once

#include "ni_ght_si/common.hpp"
#include "ni_ght_si/application_scope.hpp"

#include <unordered_map>

namespace night {

// These keys identify observational cache entries only. Tick-granular start times
// and bulk rows do not confer permission or a safe handle for process actions.
struct ProcessInfo {
    std::uint64_t pid{}, parent_pid{}, start_ticks{}, uid{}, threads{};
    std::string name; // Raw comm bytes; presentation must escape untrusted text.
    char state{};
    Reading<double> cpu_percent; // 100% is one logical core; may exceed 100%.
    Reading<std::uint64_t> rss_bytes; // Approximate resident bytes; shared pages overlap.
    Reading<ApplicationScopeCandidate> application_scope{{}, Quality::unsupported};
};

struct ProcessSnapshot {
    Stamp stamp;
    std::vector<ProcessInfo> processes;
    std::string boot_id;
    std::string pid_namespace;
    std::string cgroup_namespace;
};

struct ProcessStat {
    std::uint64_t pid{}, parent_pid{}, start_ticks{}, threads{};
    std::uint64_t user_ticks{}, system_ticks{};
    std::int64_t rss_pages{};
    std::string name;
    char state{};
};

// Pure adapters exposed for deterministic fixture and arithmetic tests.
std::optional<ProcessStat> parse_process_stat(std::string_view text);
std::optional<std::uint64_t> parse_process_uid(std::string_view status);
Reading<std::uint64_t> process_rss_bytes(std::int64_t pages, std::uint64_t page_size);
Reading<double> process_cpu_percent(const std::optional<ProcessStat>& before,
                                    const ProcessStat& after, double seconds,
                                    std::uint64_t clock_ticks,
                                    bool same_context = true);

// Single collector-lane ownership: collect() and its mutable cache are not
// concurrently callable. No process descriptors survive a collection cycle.
// Fresh stat/status/stat reads share an anchored proc directory. This avoids
// replacement-PID metadata, but is not an atomic snapshot of exec/credential
// changes and makes no action-safety claim about tick-granular cache keys.
// Optional cgroup naming observations share the same directory and are bracketed
// by collector-thread cgroup namespace observations. Migration can still occur
// between reads; neither a path nor a namespace ID authorizes process actions.
class ProcessCollector {
public:
    explicit ProcessCollector(std::filesystem::path proc_root = "/proc", ClockSource clock_source = observe_clocks);
    ProcessSnapshot collect(bool include_application_scopes = false);
    void reset_baselines() { previous_.clear(); }

private:
    struct Previous {
        ProcessStat stat;
        Clock::time_point observed;
    };
    std::filesystem::path proc_root_;
    std::uint64_t sequence_{};
    std::uint64_t clock_ticks_{};
    std::uint64_t page_size_{};
    std::string boot_id_;
    std::string pid_namespace_;
    std::unordered_map<std::uint64_t, Previous> previous_;
    ClockSource clock_source_;
    CounterEpoch epoch_;
};
} // namespace night
