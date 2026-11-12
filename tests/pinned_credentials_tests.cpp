#include "ni_ght_si/pinned_process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" int __real_poll(pollfd*, nfds_t, int);

namespace {
using Status = night::ProcessContextStatus;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::size_t descriptor_count() {
    auto* directory = ::opendir("/proc/self/fd");
    require(directory, "test descriptor inventory unavailable");
    std::size_t count{};
    while (const auto* entry = ::readdir(directory))
        if (night::unsigned_number(entry->d_name)) ++count;
    ::closedir(directory);
    return count;
}
struct Script {
    std::array<std::string, 4> text;
    std::size_t chunk{4096};
    int open_failure{-1}, read_failure{-1}, error_number{EIO};
    int poll_failure{-1}, poll_exited{-1}, poll_invalid{-1};
    int exit_before{-1}, exit_after{-1}, interrupted_read{-1};
    int opens{}, polls{}, current{-1}, descriptor{-1}, directory{-1}, pidfd{-1};
    std::size_t offset{};
    bool protocol_ok{true}, exited{}, interrupted{};
};
Script* active{};
std::string stat_text(const night::ProcessStat& stat) {
    std::array<std::string, 22> fields;
    fields.fill("0");
    fields[0] = stat.state;
    fields[1] = std::to_string(stat.parent_pid);
    fields[11] = std::to_string(stat.user_ticks);
    fields[12] = std::to_string(stat.system_ticks);
    fields[17] = std::to_string(stat.threads);
    fields[19] = std::to_string(stat.start_ticks);
    fields[21] = std::to_string(stat.rss_pages);
    std::string result = std::to_string(stat.pid) + " (" + stat.name + ")";
    for (const auto& field : fields) result += " " + field;
    return result + "\n";
}
std::string credential_text(const std::array<std::uint64_t, 8>& ids) {
    std::string text = "Uid:";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i == 4) text += "\nGid:";
        text += " " + std::to_string(ids[i]);
    }
    return text + "\n";
}
Script fixture(const night::PinnedProcess& pin) {
    const auto stat = stat_text(pin.metadata().stat);
    const auto status = credential_text({10, 11, 12, 13, 20, 21, 22, 23});
    Script script;
    script.text = {stat, status, status, stat};
    return script;
}
night::ProcessCredentialContextResult observe(const night::PinnedProcess& pin, Script& script) {
    struct Reset { ~Reset() { active = nullptr; } } reset;
    active = &script;
    return pin.observe_credentials();
}
void rejected(const night::PinnedProcess& pin, Script script, Status expected, int error = 0) {
    const auto descriptors = descriptor_count();
    const auto result = observe(pin, script);
    require(result.status == expected && !result.context && result.error_number == error &&
            !result.operation.empty(), "observation failure lost status/errno or retained a payload");
    require(script.protocol_ok, "observation left anchored fresh-read/nonblocking poll protocol");
    require(descriptor_count() == descriptors, "failed observation leaked a temporary descriptor");
}
void stable_and_changing(night::PinnedProcess& pin) {
    const auto cached = pin.metadata();
    auto script = fixture(pin);
    script.chunk = 7;
    script.interrupted_read = 2;
    auto later = pin.metadata().stat;
    ++later.user_ticks; ++later.system_ticks; ++later.rss_pages; ++later.threads;
    script.text[3] = stat_text(later);
    const auto count = descriptor_count();
    const auto result = observe(pin, script);
    require(result.status == Status::observed && result.context && result.error_number == 0 &&
            result.operation.empty() && result.context->stat.user_ticks == later.user_ticks &&
            result.context->stat.threads == later.threads && result.context->credentials.uid.filesystem == 13 &&
            result.context->credentials.gid.saved == 22 && result.context->begin <= result.context->end &&
            result.context->end <= night::Clock::now(), "fresh final values or observation status/timing incorrect");
    require(script.opens == 4 && script.polls == 2 && script.protocol_ok && script.interrupted,
            "fresh read sequence, partial reads or EINTR retry incorrect");
    require(pin.metadata().observed_at == cached.observed_at && pin.metadata().uid == cached.uid &&
            pin.metadata().stat.user_ticks == cached.stat.user_ticks && descriptor_count() == count,
            "observation mutated binding metadata or retained extra descriptors");
    for (std::size_t i = 0; i < 8; ++i) {
        auto ids = std::array<std::uint64_t, 8>{10, 11, 12, 13, 20, 21, 22, 23};
        ++ids[i]; script = fixture(pin); script.text[2] = credential_text(ids);
        rejected(pin, script, Status::context_changed);
    }
    for (const bool name : {false, true}) {
        auto changed = pin.metadata().stat;
        if (name) changed.name += " changed"; else ++changed.parent_pid;
        script = fixture(pin); script.text[3] = stat_text(changed);
        rejected(pin, script, Status::context_changed);
    }
}
void malformed_and_identity(const night::PinnedProcess& pin) {
    for (const int stage : {0, 3}) {
        for (int variant = 0; variant < 6; ++variant) {
            auto stat = pin.metadata().stat;
            if (variant == 0) ++stat.pid;
            if (variant == 1) ++stat.start_ticks;
            if (variant == 2) stat.state = 'Z';
            if (variant == 3) stat.state = '?';
            if (variant == 4) stat.threads = 0;
            auto script = fixture(pin);
            script.text[static_cast<std::size_t>(stage)] = variant == 5 ? "broken" : stat_text(stat);
            rejected(pin, script, variant < 2 ? Status::identity_changed : variant == 2 ? Status::unavailable : Status::malformed);
        }
    }
    for (const int stage : {1, 2}) {
        auto script = fixture(pin); script.text[static_cast<std::size_t>(stage)] = "Uid: 1 2 3 4\n";
        rejected(pin, script, Status::malformed);
    }
    for (const int stage : {0, 3}) {
        auto stat = pin.metadata().stat;
        stat.state = 'Z';
        auto script = fixture(pin);
        script.text[static_cast<std::size_t>(stage)] = stat_text(stat);
        script.poll_exited = 1;
        rejected(pin, script, Status::exited);
        script = fixture(pin);
        script.text[static_cast<std::size_t>(stage)] = stat_text(stat);
        script.poll_failure = 1;
        rejected(pin, script, Status::error, EIO);
    }
    for (int stage = 0; stage < 4; ++stage) {
        auto script = fixture(pin); script.text[static_cast<std::size_t>(stage)].resize(65537, ' ');
        rejected(pin, script, Status::malformed, EOVERFLOW);
    }
    auto script = fixture(pin);
    for (auto& text : script.text) text.resize(65536, ' ');
    require(observe(pin, script).status == Status::observed, "exactly64KiB valid records were rejected");
}
void failures_and_exit(const night::PinnedProcess& pin) {
    const std::array errors{EACCES, EPERM, ESRCH, ENOENT, ENOSYS, EOPNOTSUPP, EMFILE, EIO, EBADF};
    const std::array expected{Status::permission_denied, Status::permission_denied, Status::exited,
        Status::exited, Status::unavailable, Status::unavailable, Status::resource_limit, Status::error, Status::invalid_handle};
    for (int stage = 0; stage < 4; ++stage) {
        for (std::size_t i = 0; i < errors.size(); ++i) {
            for (const bool on_read : {false, true}) {
                auto script = fixture(pin); script.error_number = errors[i];
                (on_read ? script.read_failure : script.open_failure) = stage;
                rejected(pin, script, expected[i], errors[i]);
            }
        }
        for (const bool after : {false, true}) {
            auto script = fixture(pin); (after ? script.exit_after : script.exit_before) = stage;
            rejected(pin, script, Status::exited, after && stage == 3 ? 0 : ESRCH);
        }
    }
    for (int stage = 0; stage < 2; ++stage) {
        auto script = fixture(pin); script.poll_failure = stage;
        rejected(pin, script, Status::error, EIO);
        script = fixture(pin); script.poll_exited = stage;
        rejected(pin, script, Status::exited);
        script = fixture(pin); script.poll_invalid = stage;
        rejected(pin, script, Status::invalid_handle, EBADF);
    }
}
} // namespace

extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    if (!active) return __real_openat(directory, path, flags);
    auto& s = *active;
    const auto stage = s.opens++;
    constexpr std::array names{"stat", "status", "status", "stat"};
    if (s.directory < 0) s.directory = directory;
    if (stage >= 4 || std::strcmp(path, names[static_cast<std::size_t>(std::min(stage, 3))]) != 0 ||
        flags != (O_RDONLY | O_CLOEXEC | O_NOFOLLOW) || directory != s.directory) {
        s.protocol_ok = false; errno = EINVAL; return -1;
    }
    if (s.exit_before == stage) s.exited = true;
    if (s.exited || s.open_failure == stage) { errno = s.exited ? ESRCH : s.error_number; return -1; }
    s.current = stage; s.offset = 0;
    s.descriptor = __real_openat(directory, path, flags);
    return s.descriptor;
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, std::size_t count) {
    if (!active) return __real_read(fd, buffer, count);
    auto& s = *active;
    if (fd != s.descriptor || s.current < 0) { s.protocol_ok = false; errno = EBADF; return -1; }
    if (s.interrupted_read == s.current && !s.interrupted) { s.interrupted = true; errno = EINTR; return -1; }
    if (s.read_failure == s.current) { errno = s.error_number; return -1; }
    const auto& text = s.text[static_cast<std::size_t>(s.current)];
    const auto bytes = std::min({count, s.chunk, text.size() - s.offset});
    if (bytes) { std::memcpy(buffer, text.data() + s.offset, bytes); s.offset += bytes; }
    else if (s.exit_after == s.current) s.exited = true;
    return static_cast<ssize_t>(bytes);
}
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (!active) return __real_poll(fds, count, timeout);
    auto& s = *active;
    const auto stage = s.polls++;
    if (count != 1 || timeout != 0 || fds[0].events != POLLIN) {
        s.protocol_ok = false; errno = EINVAL; return -1;
    }
    if (s.pidfd < 0) s.pidfd = fds[0].fd;
    if (s.pidfd != fds[0].fd) { s.protocol_ok = false; errno = EBADF; return -1; }
    if (s.poll_failure == stage) { errno = EIO; return -1; }
    if (s.poll_invalid == stage) { fds[0].revents = POLLNVAL; return 1; }
    if (s.exited || s.poll_exited == stage) { fds[0].revents = POLLIN; return 1; }
    return __real_poll(fds, count, timeout);
}
int main() {
    try {
        const auto baseline = descriptor_count();
        {
            auto bound = night::PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
            if (!bound.process && (bound.status == night::ProcessPinStatus::unavailable ||
                bound.status == night::ProcessPinStatus::permission_denied ||
                bound.status == night::ProcessPinStatus::context_mismatch ||
                bound.status == night::ProcessPinStatus::resource_limit)) {
                std::cout << "UNSUPPORTED native credential boundary checks\n"; return 77;
            }
            require(bound.process.has_value(), "self binding failed unexpectedly");
            stable_and_changing(*bound.process);
            malformed_and_identity(*bound.process);
            failures_and_exit(*bound.process);
        }
        require(descriptor_count() == baseline, "credential test lifetime leaked descriptors");
        std::cout << "PASS anchored credential freshness, identity, read/poll failures, exit boundaries and FD cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
