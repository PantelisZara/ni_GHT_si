#include "ni_ght_si/common.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <time.h>
#include <unistd.h>

namespace night {
namespace {
class TextDescriptor {
public:
    explicit TextDescriptor(int fd) noexcept : fd_(fd) {}
    ~TextDescriptor() { if (fd_ >= 0) ::close(fd_); }
    TextDescriptor(const TextDescriptor&) = delete;
    TextDescriptor& operator=(const TextDescriptor&) = delete;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};
}

TextResult read_text(const std::filesystem::path& path, std::size_t limit) {
    TextResult result;
    auto reservation = DescriptorReservation::try_acquire(1);
    if (!reservation) {
        result.issue = Issue{Quality::error, path.string(), "temporary descriptor budget unavailable"};
        return result;
    }
    // Declare the FD after its reservation so exception unwinding closes it
    // before making its capacity available to another managed operation.
    TextDescriptor file{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.get() < 0) {
        const auto error = errno;
        result.issue = Issue{error == EACCES || error == EPERM ? Quality::permission_denied : Quality::error,
                             path.string(), std::strerror(error)};
        return result;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            const auto error = errno;
            result.issue = Issue{error == EACCES || error == EPERM ? Quality::permission_denied : Quality::error,
                                 path.string(), std::strerror(error)};
            result.text.clear();
            break;
        }
        if (count == 0) break;
        const auto size = static_cast<std::size_t>(count);
        if (size > limit - result.text.size()) {
            result.issue = Issue{Quality::error, path.string(), "input exceeds configured byte limit"};
            result.text.clear();
            break;
        }
        result.text.append(buffer.data(), size);
    }
    return result;
}

std::optional<std::uint64_t> unsigned_number(std::string_view text) {
    if (text.empty()) return {};
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> result;
    while (!text.empty()) {
        const auto start = text.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) break;
        text.remove_prefix(start);
        const auto end = text.find_first_of(" \t\r\n");
        result.push_back(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end);
    }
    return result;
}

std::optional<double> rate(std::uint64_t before, std::uint64_t after, double seconds) {
    if (after < before || !std::isfinite(seconds) || seconds <= 0) return {};
    const double result = static_cast<double>(after - before) / seconds;
    return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

Quality freshness(const Stamp& stamp, Clock::time_point now, std::chrono::milliseconds maximum_age) {
    if (stamp.quality != Quality::valid) return stamp.quality;
    if (maximum_age.count() < 0 || stamp.end < stamp.begin || now < stamp.end) return Quality::error;
    if (stamp.end == Clock::time_point{}) return Quality::warming;
    return now - stamp.end > maximum_age ? Quality::stale : Quality::valid;
}

std::optional<ClockObservation> observe_clocks() {
    auto read = [](clockid_t id) -> std::optional<std::chrono::nanoseconds> {
        timespec time{};
        if (::clock_gettime(id, &time) != 0 || time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1000000000)
            return {};
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        if (time.tv_sec > maximum / 1000000000 ||
            time.tv_nsec > maximum - time.tv_sec * 1000000000) return {};
        return std::chrono::nanoseconds{time.tv_sec * 1000000000 + time.tv_nsec};
    };
    const auto before = read(CLOCK_MONOTONIC);
    const auto boot = read(CLOCK_BOOTTIME);
    const auto after = read(CLOCK_MONOTONIC);
    if (!before || !boot || !after) return {};
    return ClockObservation{*before, *boot, *after};
}

bool CounterEpoch::update(const std::optional<ClockObservation>& observation) {
    if (!observation || observation->monotonic_before.count() < 0 || observation->boot_time.count() < 0 ||
        observation->monotonic_after < observation->monotonic_before) {
        previous_.reset(); return true;
    }
    bool reset = !previous_;
    if (previous_) {
        const auto lower = [](const ClockObservation& v) {
            return static_cast<long double>(v.boot_time.count()) - static_cast<long double>(v.monotonic_after.count());
        };
        const auto upper = [](const ClockObservation& v) {
            return static_cast<long double>(v.boot_time.count()) - static_cast<long double>(v.monotonic_before.count());
        };
        constexpr long double tolerance_ns = 1000000;
        reset = observation->monotonic_before < previous_->monotonic_before || observation->boot_time < previous_->boot_time ||
                lower(*observation) - upper(*previous_) > tolerance_ns || lower(*previous_) - upper(*observation) > tolerance_ns;
    }
    previous_ = observation;
    return reset;
}
} // namespace night
