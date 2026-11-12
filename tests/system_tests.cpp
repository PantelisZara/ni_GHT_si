#include "ni_ght_si/system.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("system: " + message);
}

void expect_percent(const night::Reading<double>& reading, double expected, const std::string& message) {
    require(reading.quality == night::Quality::valid && reading.value &&
                std::abs(*reading.value - expected) < 1e-9, message);
}

void expect_absent(const night::Reading<double>& reading, night::Quality quality, const std::string& message) {
    require(!reading.value && reading.quality == quality, message);
}

const night::Reading<double>& cpu(const night::SystemSnapshot& snapshot, const std::string& name) {
    for (const auto& reading : snapshot.cpus) if (reading.name == name) return reading.busy_percent;
    throw std::runtime_error("system: missing CPU " + name);
}

struct Fixture {
    std::filesystem::path root;
    Fixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "ni-ght-system-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("system: cannot create fixture directory");
        root = directory;
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void put(const std::string& file, std::string_view text) const {
        std::ofstream output(root / file);
        output << text;
        if (!output) throw std::runtime_error("system: cannot write fixture " + file);
    }
};

constexpr std::string_view memory = "MemTotal: 1024 kB\nMemFree: 1 kB\nMemAvailable: 400 kB\nSwapTotal: 2048 kB\nSwapFree: 512 kB\n";
constexpr std::string_view first = "100 20 30 400 50 10 20 5 70 10\n";
constexpr std::string_view second = "120 30 40 440 60 15 25 5 85 15\n";
std::string stat(std::string_view ticks, bool extra = false) {
    std::string result = "cpu " + std::string{ticks} + "cpu0 " + std::string{ticks};
    if (extra) result += "cpu1 " + std::string{ticks};
    return result + "intr 100 1 2\n";
}
} // namespace

void system_tests() {
    using night::Quality;
    std::vector<night::Issue> issues;
    const auto before = night::parse_cpu_stat(stat(first), issues);
    const auto after = night::parse_cpu_stat(stat(second), issues);
    require(issues.empty() && before.size() == 2 && after.size() == 2 &&
                before[0].ticks && after[0].ticks, "parse aggregate and per-core counters");
    // Total delta = 100, idle+iowait = 50. Guest delta is already in user/nice.
    expect_percent(night::cpu_busy(*before[0].ticks, *after[0].ticks, 2.5), 50, "guest accounting and busy denominator");
    expect_percent(night::cpu_iowait(*before[0].ticks, *after[0].ticks, 2.5), 10, "iowait is separate from busy");
    expect_absent(night::cpu_busy(*before[0].ticks, *before[0].ticks, 1), Quality::warming, "zero ticks warm up");
    expect_absent(night::cpu_busy(*before[0].ticks, *after[0].ticks, 0), Quality::warming, "zero elapsed interval");
    expect_absent(night::cpu_busy(*before[0].ticks, *after[0].ticks, -1), Quality::warming, "negative elapsed interval");
    expect_absent(night::cpu_busy(*before[0].ticks, *after[0].ticks,
                                std::numeric_limits<double>::quiet_NaN()), Quality::warming, "non-finite interval");
    for (std::size_t index = 0; index < 10; ++index) {
        auto decreased = *after[0].ticks;
        decreased[index] = (*before[0].ticks)[index] - 1;
        expect_absent(night::cpu_busy(*before[0].ticks, decreased, 1), Quality::warming,
                      "counter reset at index " + std::to_string(index));
        expect_absent(night::cpu_iowait(*before[0].ticks, decreased, 1), Quality::warming, "iowait shares reset checks");
    }
    auto overflow = *after[0].ticks;
    overflow[0] = std::numeric_limits<std::uint64_t>::max();
    expect_absent(night::cpu_busy(*before[0].ticks, overflow, 1), Quality::error, "absolute total overflow");
    for (const auto input : {"cpu 1 2 3\n", "cpu 18446744073709551616 0 0 0 0 0 0 0\n",
                             "cpu 18446744073709551615 1 0 0 0 0 0 0\n", "cpu -1 0 0 0 0 0 0 0\n",
                             "cpu 1 2 3 4 5 6 7 8\ncpu 1 2 3 4 5 6 7 8\n"}) {
        issues.clear();
        const auto parsed = night::parse_cpu_stat(input, issues);
        require(parsed.size() == 1 && !parsed[0].ticks && !issues.empty(), "invalid CPU fields must not produce values");
    }
    issues.clear();
    const auto absent = night::parse_cpu_stat("intr 1\n", issues);
    require(absent.size() == 1 && !absent[0].ticks && !issues.empty(), "missing CPU aggregate");
    issues.clear();
    require(night::parse_cpu_stat("cpu 1 2 3 4 5 6 7 8\n", issues)[0].ticks.has_value(), "eight-counter kernel format");

    night::SystemSnapshot parsed_memory;
    night::parse_meminfo(memory, parsed_memory);
    require(parsed_memory.stamp.issues.empty(), "known memory fields parse cleanly");
    require(parsed_memory.total_bytes.value == 1024ULL * 1024 &&
                parsed_memory.available_bytes.value == 400ULL * 1024 &&
                parsed_memory.used_bytes.value == 624ULL * 1024 &&
                parsed_memory.swap_total_bytes.value == 2048ULL * 1024 &&
                parsed_memory.swap_used_bytes.value == 1536ULL * 1024, "RAM and swap KiB conversion");
    night::parse_meminfo("MemTotal: 20 kB\nMemFree: 10 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n", parsed_memory);
    require(parsed_memory.total_bytes.value == 20480 && !parsed_memory.available_bytes.value &&
                !parsed_memory.used_bytes.value && parsed_memory.swap_used_bytes.value == 0,
            "missing available RAM stays absent; valid empty swap is zero");
    for (const auto input : {"MemTotal: 18446744073709551615 kB\n", "MemTotal: 1 MB\n",
                             "MemTotal: -1 kB\n", "MemTotal: 1 kB\nMemTotal: 1 kB\n"}) {
        night::SystemSnapshot invalid;
        night::parse_meminfo(input, invalid);
        require(!invalid.total_bytes.value && invalid.total_bytes.quality == Quality::error &&
                    !invalid.used_bytes.value && !invalid.stamp.issues.empty(), "invalid memory units, range, duplicate or number");
    }
    night::parse_meminfo("MemTotal: 1 kB\nMemAvailable: 2 kB\nSwapTotal: 1 kB\nSwapFree: 2 kB\n", parsed_memory);
    require(!parsed_memory.available_bytes.value && !parsed_memory.used_bytes.value &&
                !parsed_memory.swap_used_bytes.value, "invalid memory ordering cannot underflow");

    Fixture fixture;
    fixture.put("meminfo", memory);
    fixture.put("uptime", "1234.50 4321.00\n");
    fixture.put("loadavg", "0.10 1.25 2.50 1/100 42\n");
    fixture.put("stat", stat(first));
    night::SystemCollector collector{fixture.root};
    const auto initial = collector.collect();
    require(initial.uptime_seconds.value == 1234.5 && initial.load_average.value &&
            (*initial.load_average.value)[1] == 1.25, "uptime seconds and load windows, not CPU percent");
    expect_absent(cpu(initial, "cpu"), Quality::warming, "initial aggregate is warming");
    expect_absent(cpu(initial, "cpu0"), Quality::warming, "initial core is warming");
    fixture.put("stat", stat(second));
    const auto next = collector.collect();
    expect_percent(cpu(next, "cpu"), 50, "collector aggregate delta");
    expect_percent(cpu(next, "cpu0"), 50, "collector per-core delta");
    expect_percent(next.cpus.front().iowait_percent,10,"collector exposes iowait separately");
    require(next.stamp.sequence == initial.stamp.sequence + 1 && next.stamp.begin <= next.stamp.end &&
                next.used_bytes.value == 624ULL * 1024, "collector sequence, interval and memory");
    fixture.put("stat", stat(second, true));
    const auto added = collector.collect();
    for (const auto& reading : added.cpus) expect_absent(reading.busy_percent, Quality::warming, "CPU addition resets topology baseline");
    fixture.put("stat", stat(first));
    const auto removed = collector.collect();
    require(removed.cpus.size() == 2, "removed CPU disappears");
    expect_absent(cpu(removed, "cpu"), Quality::warming, "CPU removal resets baseline");
    fixture.put("stat", stat(second));
    expect_percent(cpu(collector.collect(), "cpu"), 50, "hotplug baseline recovers");
    fixture.put("stat", "cpu 130 40 50 450 59 20 30 5 90 20\ncpu0 130 40 50 450 59 20 30 5 90 20\n");
    expect_absent(cpu(collector.collect(), "cpu"), Quality::warming, "decreasing iowait reseeds");
    fixture.put("stat", "cpu 140 40 50 460 60 20 30 5 90 20\ncpu0 140 40 50 460 60 20 30 5 90 20\n");
    expect_percent(cpu(collector.collect(), "cpu"), 1000.0 / 21, "iowait reset recovery uses new baseline");
    fixture.put("stat", "cpu bad\ncpu0 bad\n");
    expect_absent(cpu(collector.collect(), "cpu"), Quality::error, "invalid input invalidates current CPU reading");
    fixture.put("stat", stat(first));
    expect_absent(cpu(collector.collect(), "cpu"), Quality::warming, "invalid prior CPU input resets baseline");
    std::filesystem::remove(fixture.root / "stat");
    std::filesystem::remove(fixture.root / "meminfo");
    const auto missing = collector.collect();
    expect_absent(cpu(missing, "cpu"), Quality::error, "missing stat reports error");
    require(!missing.total_bytes.value && missing.total_bytes.quality == Quality::error &&
                missing.stamp.issues.size() >= 2, "missing sources preserve absent values and issues");
    fixture.put("stat", stat(second));
    fixture.put("meminfo", memory);
    expect_absent(cpu(collector.collect(), "cpu"), Quality::warming, "source failure resets CPU baseline");

    for (const auto bad : {"nan 10", "-1 10", "1", "1 bad", "1e999 10"}) {
        night::parse_uptime(bad, parsed_memory);
        require(!parsed_memory.uptime_seconds.value && parsed_memory.uptime_seconds.quality == Quality::error,
                "invalid uptime must remain absent");
    }
    for (const auto bad : {"1 2", "nan 1 2", "1 -2 3", "1 2 infinity"}) {
        night::parse_loadavg(bad, parsed_memory);
        require(!parsed_memory.load_average.value && parsed_memory.load_average.quality == Quality::error,
                "invalid load window must remain absent");
    }
    using namespace std::chrono_literals;
    night::ClockObservation clocks{1s, 1s, 1s};
    night::SystemCollector resumed{fixture.root, [&] { return std::optional{clocks}; }};
    fixture.put("stat", stat(first)); resumed.collect();
    clocks = {2s, 2s, 2s}; fixture.put("stat", stat(second));
    expect_percent(cpu(resumed.collect(), "cpu"), 50, "pre-suspend delta valid");
    clocks = {3s, 13s, 3s}; fixture.put("stat", stat(first));
    expect_absent(cpu(resumed.collect(), "cpu"), Quality::warming, "suspend invalidates CPU interval");
    clocks = {4s, 14s, 4s}; fixture.put("stat", stat(second));
    expect_percent(cpu(resumed.collect(), "cpu"), 50, "post-suspend new interval valid");
}
