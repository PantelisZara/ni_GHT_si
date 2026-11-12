#pragma once
#include "common.hpp"
#include <functional>
#include <map>

namespace night {
struct IoCounters { std::string name; std::uint64_t read{}, written{}; };
struct Throughput {
    std::string name;
    std::string identity;
    Reading<double> read_bytes_per_second, written_bytes_per_second;
};
struct IoSnapshot { Stamp stamp; std::vector<Throughput> network, disks; };
struct Capacity {
    std::string path;
    Reading<std::uint64_t> total_bytes, used_bytes, available_bytes;
};
struct CapacitySnapshot { Stamp stamp; std::vector<Capacity> mounts; };
std::vector<IoCounters> parse_network(std::string_view text, std::vector<Issue>* issues = nullptr,
                                      std::string_view source = "/proc/thread-self/net/dev");
std::vector<IoCounters> parse_disks(std::string_view text, std::vector<Issue>* issues = nullptr,
                                  std::string_view source = "/proc/diskstats");

class IoCollector {
public:
    struct Readers {
        std::function<TextResult(const std::filesystem::path&, std::size_t)> file;
        std::function<TextResult(const std::filesystem::path&)> link;
        std::function<TextResult(std::string_view)> interface_index;
    };
    explicit IoCollector(std::filesystem::path proc_root = "/proc", std::filesystem::path sys_root = "/sys",
                         Readers readers = {}, ClockSource clock_source = observe_clocks);
    IoSnapshot collect(bool network = true, bool disks = true);
    void reset_baselines();
private:
    struct Previous { std::uint64_t read, written; Clock::time_point time; };
    std::filesystem::path proc_, sys_;
    Readers readers_;
    ClockSource clock_source_;
    CounterEpoch epoch_;
    std::map<std::string, Previous> previous_;
    std::uint64_t sequence_{};
};
CapacitySnapshot collect_capacity(const std::vector<std::filesystem::path>& paths);
} // namespace night
