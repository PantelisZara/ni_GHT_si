#include "ni_ght_si/process.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <unistd.h>
#include <utility>

namespace night {
namespace {
class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

struct CloseDirectory {
    void operator()(DIR* directory) const { ::closedir(directory); }
};

Quality error_quality(int error) {
    return error == EACCES || error == EPERM ? Quality::permission_denied : Quality::error;
}

void read_relative_into(TextResult& result, int directory, const char* name,
                        std::string_view source_base, std::string_view suffix = {}) {
    result.text.clear();
    result.issue.reset();
    const auto fail = [&](Quality quality, const char* message) {
        std::string source{source_base};
        source.append(suffix);
        result.issue = Issue{quality, std::move(source), message};
        result.text.clear();
    };
    auto reservation = DescriptorReservation::try_acquire(1);
    if (!reservation) {
        fail(Quality::error, "temporary descriptor budget unavailable");
        return;
    }
    Descriptor fd{::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.get() < 0) {
        const auto error = errno;
        fail(error_quality(error), std::strerror(error));
        return;
    }
    constexpr std::size_t limit = 64 * 1024;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(fd.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            const auto error = errno;
            fail(error_quality(error), std::strerror(error));
            break;
        }
        if (count == 0) break;
        const auto size = static_cast<std::size_t>(count);
        if (size > limit - result.text.size()) {
            fail(Quality::error, "process input exceeds byte limit");
            break;
        }
        result.text.append(buffer.data(), size);
    }
}

TextResult read_relative(int directory, const char* name, std::string_view source) {
    TextResult result;
    read_relative_into(result, directory, name, source);
    return result;
}

TextResult read_cgroup_namespace(int directory) {
    TextResult result;
    constexpr const char* source = "proc thread cgroup namespace";
    std::array<char, 256> name{};
    const auto length = ::readlinkat(directory, "thread-self/ns/cgroup", name.data(), name.size());
    if (length < 0) {
        const auto error = errno;
        result.issue = Issue{error_quality(error), source, std::strerror(error)};
        return result;
    }
    if (static_cast<std::size_t>(length) >= name.size()) {
        result.issue = Issue{Quality::error, source, "namespace identity exceeds byte limit"};
        return result;
    }
    const std::string_view identity{name.data(), static_cast<std::size_t>(length)};
    constexpr std::string_view prefix = "cgroup:[";
    std::optional<std::uint64_t> number;
    if (identity.starts_with(prefix) && identity.ends_with(']'))
        number = unsigned_number(identity.substr(prefix.size(), identity.size() - prefix.size() - 1));
    if (!number || *number == 0) {
        result.issue = Issue{Quality::error, source, "invalid cgroup namespace identity"};
        return result;
    }
    result.text = identity;
    return result;
}

std::string_view trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

std::optional<std::uint64_t> cpu_ticks(const ProcessStat& stat) {
    if (stat.system_ticks > std::numeric_limits<std::uint64_t>::max() - stat.user_ticks) return {};
    return stat.user_ticks + stat.system_ticks;
}
} // namespace

std::optional<ProcessStat> parse_process_stat(std::string_view text) {
    const auto left = text.find('(');
    const auto right = text.rfind(')');
    if (left == std::string_view::npos || right == std::string_view::npos || right <= left ||
        right + 1 >= text.size() || text[right + 1] != ' ') return {};
    const auto pid = unsigned_number(trim(text.substr(0, left)));
    std::array<std::string_view, 22> fields;
    auto remaining = text.substr(right + 1);
    for (auto& field : fields) {
        const auto start = remaining.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) return {};
        remaining.remove_prefix(start);
        const auto end = remaining.find_first_of(" \t\r\n");
        field = remaining.substr(0, end);
        if (end == std::string_view::npos) remaining = {};
        else remaining.remove_prefix(end);
    }
    // fields[0] is stat field 3 (state), fields[21] is field 24 (RSS).
    if (!pid || *pid == 0 || fields[0].size() != 1) return {};
    const auto parent = unsigned_number(fields[1]);
    const auto user = unsigned_number(fields[11]);
    const auto system = unsigned_number(fields[12]);
    const auto threads = unsigned_number(fields[17]);
    const auto start = unsigned_number(fields[19]);
    std::int64_t rss{};
    const auto [end, error] = std::from_chars(fields[21].data(), fields[21].data() + fields[21].size(), rss);
    if (!parent || !user || !system || !threads || !start || error != std::errc{} ||
        end != fields[21].data() + fields[21].size()) return {};
    return ProcessStat{*pid, *parent, *start, *threads, *user, *system, rss,
                       std::string{text.substr(left + 1, right - left - 1)}, fields[0][0]};
}

std::optional<std::uint64_t> parse_process_uid(std::string_view status) {
    while (!status.empty()) {
        const auto newline = status.find('\n');
        const auto line = status.substr(0, newline);
        if (line.starts_with("Uid:")) {
            const auto fields = words(line.substr(4));
            if (fields.size() != 4) return {};
            std::optional<std::uint64_t> real;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const auto id = unsigned_number(fields[i]);
                if (!id) return {};
                if (i == 0) real = id;
            }
            return real;
        }
        if (newline == std::string_view::npos) break;
        status.remove_prefix(newline + 1);
    }
    return {};
}

Reading<std::uint64_t> process_rss_bytes(std::int64_t pages, std::uint64_t page_size) {
    if (pages < 0 || page_size == 0 ||
        static_cast<std::uint64_t>(pages) > std::numeric_limits<std::uint64_t>::max() / page_size)
        return {{}, Quality::error};
    return {static_cast<std::uint64_t>(pages) * page_size, Quality::valid};
}

Reading<double> process_cpu_percent(const std::optional<ProcessStat>& before,
                                    const ProcessStat& after, double seconds,
                                    std::uint64_t clock_ticks, bool same_context) {
    if (clock_ticks == 0 || !cpu_ticks(after)) return {{}, Quality::error};
    if (!before || !same_context || before->pid != after.pid || before->start_ticks != after.start_ticks)
        return {{}, Quality::warming};
    if (!cpu_ticks(*before)) return {{}, Quality::warming};
    // Check each component as well as the sum: a reset must not be concealed
    // by growth in the other counter.
    if (after.user_ticks < before->user_ticks || after.system_ticks < before->system_ticks)
        return {{}, Quality::warming};
    if (!std::isfinite(seconds) || seconds <= 0) return {{}, Quality::error};
    const auto delta = *cpu_ticks(after) - *cpu_ticks(*before);
    const double percent = (static_cast<double>(delta) / static_cast<double>(clock_ticks)) / seconds * 100.0;
    if (!std::isfinite(percent)) return {{}, Quality::error};
    return {percent, Quality::valid};
}

ProcessCollector::ProcessCollector(std::filesystem::path proc_root, ClockSource clock_source)
    : proc_root_(std::move(proc_root)), clock_source_(std::move(clock_source)) {
    const auto ticks = ::sysconf(_SC_CLK_TCK);
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    if (ticks > 0) clock_ticks_ = static_cast<std::uint64_t>(ticks);
    if (page_size > 0) page_size_ = static_cast<std::uint64_t>(page_size);
}

ProcessSnapshot ProcessCollector::collect(bool include_application_scopes) try {
    ProcessSnapshot snapshot;
    snapshot.stamp.sequence = ++sequence_;
    snapshot.stamp.begin = Clock::now();
    const auto clocks = clock_source_();
    if (epoch_.update(clocks)) reset_baselines();
    if (!clocks) snapshot.stamp.issues.push_back({Quality::error, "clocks", "cannot establish sampling epoch"});
    const auto finish = [&]() {
        if (snapshot.processes.empty() && !snapshot.stamp.issues.empty()) {
            snapshot.stamp.quality = Quality::error;
            for (const auto& issue : snapshot.stamp.issues)
                if (issue.quality == Quality::permission_denied) snapshot.stamp.quality = Quality::permission_denied;
        }
        snapshot.stamp.end = Clock::now();
        return std::move(snapshot);
    };
    // Cover root, enumeration and one anchored process directory. Each leaf
    // read separately reserves its descriptor before opening it.
    auto directories = DescriptorReservation::try_acquire(3);
    if (!directories) {
        snapshot.stamp.issues.push_back({Quality::error, "proc temporary descriptor budget",
                                        "temporary descriptor budget unavailable"});
        previous_.clear();
        return finish();
    }
    Descriptor root{::open(proc_root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (root.get() < 0) {
        const auto error = errno;
        snapshot.stamp.issues.push_back({error_quality(error), proc_root_.string(), std::strerror(error)});
        previous_.clear();
        return finish();
    }
    auto boot = read_relative(root.get(), "sys/kernel/random/boot_id", "proc boot identity");
    if (boot.issue) snapshot.stamp.issues.push_back(*boot.issue);
    else snapshot.boot_id = std::string{trim(boot.text)};
    if (!boot.issue && snapshot.boot_id.empty())
        snapshot.stamp.issues.push_back({Quality::error, "proc boot identity", "empty boot identity"});
    std::array<char, 256> ns{};
    const auto ns_length = ::readlinkat(root.get(), "self/ns/pid", ns.data(), ns.size());
    if (ns_length < 0) {
        const auto error = errno;
        snapshot.stamp.issues.push_back({error_quality(error), "proc PID namespace", std::strerror(error)});
    } else if (static_cast<std::size_t>(ns_length) >= ns.size()) {
        snapshot.stamp.issues.push_back({Quality::error, "proc PID namespace", "namespace identity exceeds byte limit"});
    } else {
        snapshot.pid_namespace.assign(ns.data(), static_cast<std::size_t>(ns_length));
        if (snapshot.pid_namespace.empty())
            snapshot.stamp.issues.push_back({Quality::error, "proc PID namespace", "empty namespace identity"});
    }
    const bool context_available = !snapshot.boot_id.empty() && !snapshot.pid_namespace.empty();
    if (!context_available || snapshot.boot_id != boot_id_ || snapshot.pid_namespace != pid_namespace_)
        previous_.clear();
    boot_id_ = snapshot.boot_id;
    pid_namespace_ = snapshot.pid_namespace;
    if (!clock_ticks_ || !page_size_)
        snapshot.stamp.issues.push_back({Quality::error, "sysconf", "clock tick or page size unavailable"});
    TextResult cgroup_before;
    if (include_application_scopes) {
        cgroup_before = read_cgroup_namespace(root.get());
        if (cgroup_before.issue) snapshot.stamp.issues.push_back(*cgroup_before.issue);
    }

    // The duplicate shares directory position; root is opened afresh each cycle.
    const int enumeration_fd = ::fcntl(root.get(), F_DUPFD_CLOEXEC, 0);
    std::unique_ptr<DIR, CloseDirectory> directory{enumeration_fd < 0 ? nullptr : ::fdopendir(enumeration_fd)};
    if (!directory) {
        const auto error = errno;
        if (enumeration_fd >= 0) ::close(enumeration_fd);
        snapshot.stamp.issues.push_back({error_quality(error), "proc enumeration", std::strerror(error)});
        previous_.clear();
        return finish();
    }
    std::unordered_map<std::uint64_t, Previous> next;
    TextResult first, status, cgroup, last;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            const auto error = errno;
            if (error) {
                snapshot.stamp.quality = error_quality(error);
                snapshot.stamp.issues.push_back({error_quality(error), "proc enumeration", std::strerror(error)});
            }
            break;
        }
        const auto pid = unsigned_number(entry->d_name);
        if (!pid || *pid == 0) continue;
        const std::string source = "proc/" + std::to_string(*pid);
        Descriptor process{::openat(root.get(), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (process.get() < 0) {
            const auto error = errno;
            if (error != ENOENT && error != ESRCH)
                snapshot.stamp.issues.push_back({error_quality(error), source, std::strerror(error)});
            continue;
        }
        // Fresh relative opens bind every read to this same proc directory;
        // never reopen a cached numeric PID path for status metadata.
        read_relative_into(first, process.get(), "stat", source, "/stat");
        read_relative_into(status, process.get(), "status", source, "/status");
        if (include_application_scopes) read_relative_into(cgroup, process.get(), "cgroup", source, "/cgroup");
        else { cgroup.text.clear(); cgroup.issue.reset(); }
        read_relative_into(last, process.get(), "stat", source, "/stat");
        const auto observed = Clock::now();
        bool failed = false;
        for (const auto* read : {&first, &status, &last}) {
            if (read->issue) {
                snapshot.stamp.issues.push_back(*read->issue);
                failed = true;
            }
        }
        if (failed) continue;
        const auto initial = parse_process_stat(first.text);
        const auto current = parse_process_stat(last.text);
        const auto uid = parse_process_uid(status.text);
        if (!initial || !current || !uid) {
            snapshot.stamp.issues.push_back({Quality::error, source, "invalid process stat or numeric UID"});
            continue;
        }
        if (initial->pid != *pid || current->pid != *pid || initial->start_ticks != current->start_ticks) {
            snapshot.stamp.issues.push_back({Quality::stale, source, "process identity changed during sample"});
            continue;
        }
        Reading<ApplicationScopeCandidate> application_scope{{}, Quality::unsupported};
        if (include_application_scopes) {
            if (cgroup.issue) {
                application_scope.quality = cgroup.issue->quality;
                snapshot.stamp.issues.push_back(*cgroup.issue);
            } else {
                application_scope = parse_application_scope_candidate(cgroup.text);
                if (application_scope.quality == Quality::error)
                    snapshot.stamp.issues.push_back({Quality::error, source + "/cgroup", "invalid cgroup membership records"});
            }
        }
        Reading<double> cpu{{}, Quality::warming};
        const auto prior = previous_.find(*pid);
        if (prior != previous_.end()) {
            const double seconds = std::chrono::duration<double>(observed - prior->second.observed).count();
            cpu = process_cpu_percent(prior->second.stat, *current, seconds, clock_ticks_);
        } else {
            cpu = process_cpu_percent({}, *current, 0.0, clock_ticks_);
        }
        if (!context_available || !clocks) cpu = {{}, Quality::error};
        const auto rss = process_rss_bytes(current->rss_pages, page_size_);
        if (cpu.quality == Quality::error)
            snapshot.stamp.issues.push_back({Quality::error, source, "process CPU interval unavailable"});
        if (rss.quality == Quality::error)
            snapshot.stamp.issues.push_back({Quality::error, source, "invalid resident page count or page size"});
        snapshot.processes.push_back({current->pid, current->parent_pid, current->start_ticks, *uid,
                                      current->threads, current->name, current->state, cpu, rss,
                                      std::move(application_scope)});
        if (context_available && cpu_ticks(*current)) next.emplace(*pid, Previous{*current, observed});
    }
    // Replacing the map prunes disappeared, denied and malformed instances.
    previous_ = std::move(next);
    if (include_application_scopes) {
        const auto cgroup_after = read_cgroup_namespace(root.get());
        if (cgroup_after.issue) snapshot.stamp.issues.push_back(*cgroup_after.issue);
        if (!cgroup_before.issue && !cgroup_after.issue && cgroup_before.text == cgroup_after.text) {
            snapshot.cgroup_namespace = cgroup_before.text;
        } else {
            Quality quality = Quality::stale;
            if (cgroup_before.issue) quality = cgroup_before.issue->quality;
            if (cgroup_after.issue && quality != Quality::permission_denied) quality = cgroup_after.issue->quality;
            if (!cgroup_before.issue && !cgroup_after.issue)
                snapshot.stamp.issues.push_back({quality, "proc thread cgroup namespace", "namespace changed during collection"});
            for (auto& process : snapshot.processes) {
                // Preserve field-level read/parser failures while discarding all
                // candidates whose namespace context could not be established.
                if (process.application_scope.quality == Quality::valid || process.application_scope.quality == Quality::unsupported)
                    process.application_scope = {{}, quality};
            }
        }
    }
    return finish();
} catch (...) {
    reset_baselines();
    throw;
}
} // namespace night
