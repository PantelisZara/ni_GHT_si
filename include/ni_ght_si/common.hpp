#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace night {
using Clock = std::chrono::steady_clock;
enum class Quality { valid, warming, stale, permission_denied, unsupported, error };

struct Issue {
    Quality quality{Quality::error};
    std::string source;
    std::string message;
};

template<class T> struct Reading {
    std::optional<T> value;
    Quality quality{Quality::warming};
};

struct Stamp {
    std::uint64_t sequence{};
    // Whole-collector failure; individual fields still carry their own quality.
    Quality quality{Quality::valid};
    Clock::time_point begin{};
    Clock::time_point end{};
    std::vector<Issue> issues;
};

struct TextResult {
    std::string text;
    std::optional<Issue> issue;
};

TextResult read_text(const std::filesystem::path& path, std::size_t limit = 1024 * 1024);
std::optional<std::uint64_t> unsigned_number(std::string_view text);
std::vector<std::string_view> words(std::string_view text);
std::optional<double> rate(std::uint64_t before, std::uint64_t after, double seconds);
Quality freshness(const Stamp& stamp, Clock::time_point now, std::chrono::milliseconds maximum_age);

struct ClockObservation {
    std::chrono::nanoseconds monotonic_before, boot_time, monotonic_after;
};
using ClockSource = std::function<std::optional<ClockObservation>()>;
std::optional<ClockObservation> observe_clocks();

// Bracketed offsets avoid interpreting a scheduling delay between clock reads
// as suspend. A >1 ms offset discontinuity invalidates interval baselines.
class CounterEpoch {
public:
    bool update(const std::optional<ClockObservation>& observation);
private:
    std::optional<ClockObservation> previous_;
};
} // namespace night
