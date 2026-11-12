#include "ni_ght_si/system.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace night {
namespace {
bool checked_add(std::uint64_t& sum, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - sum) return false;
    sum += value;
    return true;
}

std::optional<std::uint64_t> tick_total(const CpuTicks& ticks) {
    std::uint64_t sum{};
    for (std::size_t i = 0; i < 8; ++i) {
        if (!checked_add(sum, ticks[i])) return {};
    }
    return sum;
}

bool cpu_name(std::string_view name) {
    if (name == "cpu") return true;
    return name.starts_with("cpu") && name.size() > 3 &&
           std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

template<class Function> void lines(std::string_view text, Function function) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        function(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
}

void invalid_memory(SystemSnapshot& snapshot, Quality quality) {
    snapshot.total_bytes = {{}, quality};
    snapshot.available_bytes = {{}, quality};
    snapshot.used_bytes = {{}, quality};
    snapshot.swap_total_bytes = {{}, quality};
    snapshot.swap_used_bytes = {{}, quality};
}
} // namespace

std::vector<CpuSample> parse_cpu_stat(std::string_view text, std::vector<Issue>& issues,
                                      std::string_view source) {
    std::vector<CpuSample> result;
    std::map<std::string, std::size_t> positions;
    lines(text, [&](std::string_view line) {
        const auto fields = words(line);
        if (fields.empty() || !cpu_name(fields[0])) return;
        const std::string name{fields[0]};
        const auto [position, fresh] = positions.emplace(name, result.size());
        if (!fresh) {
            result[position->second].ticks.reset();
            issues.push_back({Quality::error, std::string{source}, "duplicate CPU row: " + name});
            return;
        }
        CpuTicks ticks{};
        bool valid = fields.size() >= 9;
        // Eight required counters, plus optional guest and guest_nice counters.
        for (std::size_t i = 1; valid && i < fields.size(); ++i) {
            const auto value = unsigned_number(fields[i]);
            if (!value) valid = false;
            else if (i <= ticks.size()) ticks[i - 1] = *value;
        }
        valid = valid && tick_total(ticks).has_value();
        result.push_back({name, valid ? std::optional<CpuTicks>{ticks} : std::nullopt});
        if (!valid) issues.push_back({Quality::error, std::string{source}, "invalid CPU counters: " + name});
    });
    if (!positions.contains("cpu")) {
        issues.push_back({Quality::error, std::string{source}, "aggregate CPU counters missing"});
        result.insert(result.begin(), CpuSample{"cpu", {}});
    }
    return result;
}

namespace {
Reading<double> cpu_fraction(const CpuTicks& before, const CpuTicks& after, double seconds, bool iowait) {
    if (!tick_total(before) || !tick_total(after)) return {{}, Quality::error};
    CpuTicks delta{};
    for (std::size_t i = 0; i < delta.size(); ++i) {
        if (after[i] < before[i]) return {{}, Quality::warming};
        delta[i] = after[i] - before[i];
    }
    const auto total = tick_total(delta);
    if (!total || *total == 0) return {{}, Quality::warming};
    const auto busy = iowait ? delta[4] : *total - delta[3] - delta[4];
    const auto total_rate = rate(0, *total, seconds);
    const auto busy_rate = rate(0, busy, seconds);
    if (!total_rate || !busy_rate) return {{}, Quality::warming};
    return {100.0 * (*busy_rate / *total_rate), Quality::valid};
}
}

Reading<double> cpu_busy(const CpuTicks& before, const CpuTicks& after, double seconds) {
    return cpu_fraction(before, after, seconds, false);
}
Reading<double> cpu_iowait(const CpuTicks& before, const CpuTicks& after, double seconds) {
    return cpu_fraction(before, after, seconds, true);
}

void parse_meminfo(std::string_view text, SystemSnapshot& snapshot, std::string_view source) {
    invalid_memory(snapshot, Quality::error);
    std::map<std::string, std::optional<std::uint64_t>> values;
    constexpr std::array keys{"MemTotal:", "MemAvailable:", "SwapTotal:", "SwapFree:"};
    lines(text, [&](std::string_view line) {
        const auto fields = words(line);
        if (fields.empty() || std::find(keys.begin(), keys.end(), fields[0]) == keys.end()) return;
        std::optional<std::uint64_t> value;
        if (fields.size() == 3 && fields[2] == "kB") {
            const auto kib = unsigned_number(fields[1]);
            if (kib && *kib <= std::numeric_limits<std::uint64_t>::max() / 1024) value = *kib * 1024;
        }
        const auto [position, fresh] = values.emplace(std::string{fields[0]}, value);
        if (!fresh) position->second.reset();
    });
    for (const auto key : keys) {
        if (!values.contains(key) || !values[key])
            snapshot.stamp.issues.push_back({Quality::error, std::string{source}, std::string{"missing or invalid "} + key});
    }
    const auto total = values["MemTotal:"];
    const auto available = values["MemAvailable:"];
    const auto swap_total = values["SwapTotal:"];
    const auto swap_free = values["SwapFree:"];
    if (total) snapshot.total_bytes = {total, Quality::valid};
    if (available) snapshot.available_bytes = {available, Quality::valid};
    if (total && available) {
        if (*available <= *total) snapshot.used_bytes = {*total - *available, Quality::valid};
        else {
            snapshot.available_bytes = {{}, Quality::error};
            snapshot.stamp.issues.push_back({Quality::error, std::string{source}, "MemAvailable exceeds MemTotal"});
        }
    }
    if (swap_total) snapshot.swap_total_bytes = {swap_total, Quality::valid};
    if (swap_total && swap_free) {
        if (*swap_free <= *swap_total) snapshot.swap_used_bytes = {*swap_total - *swap_free, Quality::valid};
        else snapshot.stamp.issues.push_back({Quality::error, std::string{source}, "SwapFree exceeds SwapTotal"});
    }
}

namespace {
std::optional<double> nonnegative_real(std::string_view text) {
    if (text.empty()) return {};
    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < 0) return {};
    return value;
}
}

void parse_uptime(std::string_view text, SystemSnapshot& snapshot) {
    snapshot.uptime_seconds = {{}, Quality::error};
    const auto fields = words(text);
    if (fields.size() == 2) {
        const auto uptime = nonnegative_real(fields[0]), idle = nonnegative_real(fields[1]);
        if (uptime && idle) snapshot.uptime_seconds = {*uptime, Quality::valid};
    }
    if (!snapshot.uptime_seconds.value) snapshot.stamp.issues.push_back({Quality::error, "proc uptime", "invalid uptime seconds"});
}

void parse_loadavg(std::string_view text, SystemSnapshot& snapshot) {
    snapshot.load_average = {{}, Quality::error};
    const auto fields = words(text);
    if (fields.size() >= 3) {
        const auto a = nonnegative_real(fields[0]), b = nonnegative_real(fields[1]), c = nonnegative_real(fields[2]);
        if (a && b && c) snapshot.load_average = {std::array{*a, *b, *c}, Quality::valid};
    }
    if (!snapshot.load_average.value) snapshot.stamp.issues.push_back({Quality::error, "proc loadavg", "invalid load windows"});
}

SystemCollector::SystemCollector(std::filesystem::path proc_root, ClockSource clock_source)
    : proc_root_(std::move(proc_root)), clock_source_(std::move(clock_source)) {}

void SystemCollector::reset_baselines() { previous_.clear(); previous_time_.reset(); }

SystemSnapshot SystemCollector::collect() try {
    SystemSnapshot snapshot;
    snapshot.stamp.sequence = ++sequence_;
    snapshot.stamp.begin = Clock::now();
    const auto clocks = clock_source_();
    if (epoch_.update(clocks)) reset_baselines();
    if (!clocks) snapshot.stamp.issues.push_back({Quality::error, "clocks", "cannot establish sampling epoch"});
    const auto stat_path = proc_root_ / "stat";
    const auto stat = read_text(stat_path);
    const auto sampled_at = Clock::now();
    if (stat.issue) {
        snapshot.stamp.issues.push_back(*stat.issue);
        snapshot.cpus.push_back({"cpu", {{}, stat.issue->quality}, {{}, stat.issue->quality}});
        previous_.clear();
        previous_time_.reset();
    } else {
        const auto samples = parse_cpu_stat(stat.text, snapshot.stamp.issues, stat_path.string());
        std::map<std::string, std::optional<CpuTicks>> current;
        for (const auto& sample : samples) current.emplace(sample.name, sample.ticks);
        const bool same_topology = current.size() == previous_.size() &&
            std::equal(current.begin(), current.end(), previous_.begin(),
                       [](const auto& a, const auto& b) { return a.first == b.first; });
        const double seconds = previous_time_ ? std::chrono::duration<double>(sampled_at - *previous_time_).count() : 0;
        for (const auto& sample : samples) {
            Reading<double> reading, iowait;
            if (!sample.ticks || !clocks) reading.quality = iowait.quality = Quality::error;
            else if (same_topology && previous_time_ && previous_.at(sample.name)) {
                reading = cpu_busy(*previous_.at(sample.name), *sample.ticks, seconds);
                iowait = cpu_iowait(*previous_.at(sample.name), *sample.ticks, seconds);
            }
            snapshot.cpus.push_back({sample.name, reading, iowait});
        }
        previous_ = std::move(current);
        previous_time_ = sampled_at;
    }
    const auto memory_path = proc_root_ / "meminfo";
    const auto memory = read_text(memory_path);
    if (memory.issue) {
        snapshot.stamp.issues.push_back(*memory.issue);
        invalid_memory(snapshot, memory.issue->quality);
    } else parse_meminfo(memory.text, snapshot, memory_path.string());
    for (bool uptime : {true, false}) {
        const auto path = proc_root_ / (uptime ? "uptime" : "loadavg");
        const auto text = read_text(path, 4096);
        if (text.issue) {
            snapshot.stamp.issues.push_back(*text.issue);
            if (uptime) snapshot.uptime_seconds = {{}, text.issue->quality};
            else snapshot.load_average = {{}, text.issue->quality};
        } else if (uptime) parse_uptime(text.text, snapshot);
        else parse_loadavg(text.text, snapshot);
    }
    if (!snapshot.total_bytes.value && !snapshot.uptime_seconds.value && !snapshot.load_average.value &&
        std::none_of(snapshot.cpus.begin(), snapshot.cpus.end(), [](const auto& cpu) { return cpu.busy_percent.value.has_value(); }) &&
        !snapshot.stamp.issues.empty()) snapshot.stamp.quality = Quality::error;
    snapshot.stamp.end = Clock::now();
    return snapshot;
} catch (...) {
    reset_baselines();
    throw;
}
} // namespace night
