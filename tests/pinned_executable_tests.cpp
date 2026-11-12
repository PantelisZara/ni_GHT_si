#include "ni_ght_si/pinned_process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" int __real_fstat(int, struct stat*);
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
    int error{};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) { error = errno; break; }
        if (night::unsigned_number(entry->d_name)) ++count;
    }
    ::closedir(directory);
    require(error == 0, "test descriptor enumeration failed");
    return count;
}

struct Script {
    std::array<std::string, 2> text;
    std::array<struct stat, 2> files{};
    std::array<int, 2> executable_fds{-1, -1};
    std::size_t chunk{4096}, offset{}, baseline{}, peak{};
    int opens{}, fstats{}, polls{}, current{-1}, stat_fd{-1}, directory{-1}, pidfd{-1};
    int open_failure{-1}, read_failure{-1}, fstat_failure{-1}, error_number{EIO};
    int interrupted_read{-1}, poll_failure{-1}, poll_error{EIO}, poll_invalid{-1}, poll_bad_event{-1}, poll_exited{-1};
    int exit_before_open{-1}, exit_after_read{-1}, exit_after_fstat{-1};
    bool protocol_ok{true}, interrupted{}, exited{};
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

Script fixture(const night::PinnedProcess& pin) {
    Script script;
    script.text.fill(stat_text(pin.metadata().stat));
    for (auto& file : script.files) {
        file.st_dev = 7;
        file.st_ino = 41;
        file.st_mode = S_IFREG | 0644; // Execute bits are not an observation requirement.
        file.st_uid = 5000;
        file.st_gid = 6000;
    }
    return script;
}

night::ProcessExecutableContextResult observe(const night::PinnedProcess& pin, Script& script) {
    struct Reset { ~Reset() { active = nullptr; } } reset;
    script.baseline = descriptor_count();
    active = &script;
    return pin.observe_executable();
}

void rejected(const night::PinnedProcess& pin, Script script, Status expected, int error = 0) {
    const auto descriptors = descriptor_count();
    const auto result = observe(pin, script);
    require(result.status == expected && !result.context && result.error_number == error &&
            !result.operation.empty(), "executable failure lost status/errno or retained a payload");
    require(script.protocol_ok && script.peak <= 3, "executable observation violated anchored descriptor protocol");
    require(descriptor_count() == descriptors, "failed executable observation leaked a temporary descriptor");
}

void stable_and_changing(night::PinnedProcess& pin) {
    const auto cached = pin.metadata();
    auto script = fixture(pin);
    script.chunk = 7;
    script.interrupted_read = 3;
    auto later = cached.stat;
    ++later.user_ticks; ++later.system_ticks; ++later.rss_pages; ++later.threads;
    script.text[1] = stat_text(later);
    const auto descriptors = descriptor_count();
    const auto result = observe(pin, script);
    const night::ProcessExecutableMetadata expected{7, 41, S_IFREG | 0644, 5000, 6000};
    require(result.status == Status::observed && result.context && result.error_number == 0 &&
            result.operation.empty() && result.context->executable == expected &&
            result.context->stat.user_ticks == later.user_ticks && result.context->stat.threads == later.threads &&
            result.context->begin <= result.context->end && result.context->end <= night::Clock::now(),
            "final numeric executable metadata, stat values or timing incorrect");
    require(script.protocol_ok && script.opens == 4 && script.fstats == 2 && script.polls == 2 &&
            script.interrupted && script.peak == 3 && descriptor_count() == descriptors,
            "fresh executable descriptors, partial/EINTR reads, temporary peak or cleanup incorrect");
    require(pin.metadata().observed_at == cached.observed_at && pin.metadata().uid == cached.uid &&
            pin.metadata().stat.user_ticks == cached.stat.user_ticks,
            "executable observation mutated binding metadata");
    for (int field = 0; field < 5; ++field) {
        script = fixture(pin);
        if (field == 0) ++script.files[1].st_dev;
        if (field == 1) ++script.files[1].st_ino;
        if (field == 2) script.files[1].st_mode ^= 0100;
        if (field == 3) ++script.files[1].st_uid;
        if (field == 4) ++script.files[1].st_gid;
        rejected(pin, script, Status::context_changed);
    }
    for (const bool name : {false, true}) {
        auto stat = cached.stat;
        if (name) stat.name += " changed"; else ++stat.parent_pid;
        script = fixture(pin); script.text[1] = stat_text(stat);
        rejected(pin, script, Status::context_changed);
    }
}

void malformed_and_identity(const night::PinnedProcess& pin) {
    for (std::size_t stage = 0; stage < 2; ++stage) {
        for (int variant = 0; variant < 6; ++variant) {
            auto stat = pin.metadata().stat;
            if (variant == 0) ++stat.pid;
            if (variant == 1) ++stat.start_ticks;
            if (variant == 2) stat.state = 'Z';
            if (variant == 3) stat.state = '?';
            if (variant == 4) stat.threads = 0;
            auto script = fixture(pin);
            script.text[stage] = variant == 5 ? "broken" : stat_text(stat);
            rejected(pin, script, variant < 2 ? Status::identity_changed :
                     variant == 2 ? Status::unavailable : Status::malformed);
        }
        for (const char dead : {'Z', 'X', 'x'}) {
            auto stat = pin.metadata().stat; stat.state = dead;
            auto script = fixture(pin); script.text[stage] = stat_text(stat); script.poll_exited = 1;
            rejected(pin, script, Status::exited);
            script = fixture(pin); script.text[stage] = stat_text(stat); script.poll_failure = 1;
            rejected(pin, script, Status::error, EIO);
            script = fixture(pin); script.text[stage] = stat_text(stat); script.poll_invalid = 1;
            rejected(pin, script, Status::invalid_handle, EBADF);
        }
        auto script = fixture(pin); script.text[stage].resize(65537, ' ');
        rejected(pin, script, Status::malformed, EOVERFLOW);
        for (const mode_t type : {mode_t{0}, mode_t{S_IFDIR}, mode_t{S_IFLNK}, mode_t{S_IFIFO},
                mode_t{S_IFCHR}, mode_t{S_IFBLK}, mode_t{S_IFSOCK}}) {
            script = fixture(pin); script.files[stage].st_mode = type | 0644;
            rejected(pin, script, Status::malformed);
        }
    }
    auto script = fixture(pin);
    for (auto& text : script.text) text.resize(65536, ' ');
    require(observe(pin, script).status == Status::observed, "exactly 64KiB stat records rejected");
}

void failures_and_exit(const night::PinnedProcess& pin) {
    const std::array errors{EACCES, EPERM, ENOENT, ESRCH, ENOSYS, EOPNOTSUPP, EMFILE, ENFILE, ENOMEM,
                           ENOSPC, EOVERFLOW, EBADF, EIO};
    const std::array expected{Status::permission_denied, Status::permission_denied, Status::unavailable,
        Status::unavailable, Status::unavailable, Status::unavailable, Status::resource_limit,
        Status::resource_limit, Status::resource_limit, Status::resource_limit, Status::malformed,
        Status::invalid_handle, Status::error};
    for (int stage = 0; stage < 4; ++stage) {
        for (std::size_t i = 0; i < errors.size(); ++i) {
            auto script = fixture(pin); script.open_failure = stage; script.error_number = errors[i];
            rejected(pin, script, expected[i], errors[i]);
            if (stage == 0 || stage == 3) {
                script = fixture(pin); script.read_failure = stage; script.error_number = errors[i];
                rejected(pin, script, expected[i], errors[i]);
            }
        }
        auto script = fixture(pin); script.exit_before_open = stage;
        rejected(pin, script, Status::exited, ESRCH);
    }
    for (int stage = 0; stage < 2; ++stage) {
        for (std::size_t i = 0; i < errors.size(); ++i) {
            auto script = fixture(pin); script.fstat_failure = stage; script.error_number = errors[i];
            rejected(pin, script, expected[i], errors[i]);
        }
        auto script = fixture(pin); script.exit_after_fstat = stage;
        rejected(pin, script, Status::exited, ESRCH);
        script = fixture(pin); script.exit_after_read = stage == 0 ? 0 : 3;
        rejected(pin, script, Status::exited, stage == 0 ? ESRCH : 0);
    }
    for (int stage = 0; stage < 2; ++stage) {
        auto script = fixture(pin); script.poll_failure = stage;
        rejected(pin, script, Status::error, EIO);
        script = fixture(pin); script.poll_exited = stage;
        rejected(pin, script, Status::exited);
        script = fixture(pin); script.poll_invalid = stage;
        rejected(pin, script, Status::invalid_handle, EBADF);
        script = fixture(pin); script.poll_bad_event = stage;
        rejected(pin, script, Status::error, EIO);
        script = fixture(pin); script.poll_failure = stage; script.poll_error = EINTR;
        rejected(pin, script, Status::error, EINTR);
    }
    // An absent exe link can also describe a running process. Both fresh opens
    // require a follow-up pidfd observation, preserving ENOENT when it is usable.
    for (const int stage : {1, 2}) {
        auto script = fixture(pin); script.open_failure = stage; script.error_number = ENOENT;
        rejected(pin, script, Status::unavailable, ENOENT);
        script = fixture(pin); script.open_failure = stage; script.error_number = ENOENT; script.poll_exited = 1;
        rejected(pin, script, Status::exited, ENOENT);
        script = fixture(pin); script.open_failure = stage; script.error_number = ENOENT; script.poll_failure = 1;
        rejected(pin, script, Status::error, EIO);
        script = fixture(pin); script.open_failure = stage; script.error_number = ENOENT; script.poll_invalid = 1;
        rejected(pin, script, Status::invalid_handle, EBADF);
    }
}
} // namespace

extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    if (!active) return __real_openat(directory, path, flags);
    auto& s = *active;
    const auto stage = s.opens++;
    constexpr std::array names{"stat", "exe", "exe", "stat"};
    if (s.directory < 0) s.directory = directory;
    const bool executable = stage == 1 || stage == 2;
    const auto expected_flags = executable ? O_PATH | O_CLOEXEC : O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    if (stage >= 4 || directory < 0 || directory != s.directory ||
        std::strcmp(path, names[static_cast<std::size_t>(std::min(stage, 3))]) != 0 || flags != expected_flags ||
        (stage >= 2 && ::fcntl(s.executable_fds[0], F_GETFD) < 0) ||
        (stage == 3 && ::fcntl(s.executable_fds[1], F_GETFD) < 0)) {
        s.protocol_ok = false; errno = EINVAL; return -1;
    }
    if (s.exit_before_open == stage) s.exited = true;
    if (s.exited || s.open_failure == stage) { errno = s.exited ? ESRCH : s.error_number; return -1; }
    const auto descriptor = __real_openat(directory, path, flags);
    if (descriptor >= 0) {
        if (executable) {
            s.executable_fds[static_cast<std::size_t>(stage - 1)] = descriptor;
            if (stage == 2 && s.executable_fds[0] == descriptor) s.protocol_ok = false;
        } else {
            s.stat_fd = descriptor; s.offset = 0;
        }
        s.current = stage;
        const auto count = descriptor_count();
        if (count < s.baseline) s.protocol_ok = false;
        else s.peak = std::max(s.peak, count - s.baseline);
    }
    return descriptor;
}

extern "C" ssize_t __wrap_read(int fd, void* buffer, std::size_t count) {
    if (!active) return __real_read(fd, buffer, count);
    auto& s = *active;
    if ((s.current != 0 && s.current != 3) || fd != s.stat_fd) {
        s.protocol_ok = false; errno = EBADF; return -1;
    }
    if (s.interrupted_read == s.current && !s.interrupted) { s.interrupted = true; errno = EINTR; return -1; }
    if (s.read_failure == s.current) { errno = s.error_number; return -1; }
    const auto& text = s.text[s.current == 0 ? 0U : 1U];
    const auto bytes = std::min({count, s.chunk, text.size() - s.offset});
    if (bytes) { std::memcpy(buffer, text.data() + s.offset, bytes); s.offset += bytes; }
    else if (s.exit_after_read == s.current) s.exited = true;
    return static_cast<ssize_t>(bytes);
}

extern "C" int __wrap_fstat(int fd, struct stat* file) {
    if (!active) return __real_fstat(fd, file);
    auto& s = *active;
    const auto stage = s.fstats++;
    if (stage >= 2 || s.current != stage + 1 || fd != s.executable_fds[static_cast<std::size_t>(std::min(stage, 1))] ||
        (stage == 1 && (::fcntl(s.executable_fds[0], F_GETFD) < 0 || s.executable_fds[0] == fd))) {
        s.protocol_ok = false; errno = EBADF; return -1;
    }
    if (s.fstat_failure == stage) { errno = s.error_number; return -1; }
    *file = s.files[static_cast<std::size_t>(stage)];
    if (s.exit_after_fstat == stage) s.exited = true;
    return 0;
}

extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (!active) return __real_poll(fds, count, timeout);
    auto& s = *active;
    const auto stage = s.polls++;
    if (count != 1 || timeout != 0 || fds[0].events != POLLIN) {
        s.protocol_ok = false; errno = EINVAL; return -1;
    }
    if (s.pidfd < 0) s.pidfd = fds[0].fd;
    if (s.pidfd != fds[0].fd || fds[0].fd == s.directory ||
        (s.executable_fds[0] >= 0 && ::fcntl(s.executable_fds[0], F_GETFD) < 0) ||
        (s.executable_fds[1] >= 0 && ::fcntl(s.executable_fds[1], F_GETFD) < 0)) {
        s.protocol_ok = false; errno = EBADF; return -1;
    }
    if (s.poll_failure == stage) { errno = s.poll_error; return -1; }
    if (s.poll_invalid == stage) { fds[0].revents = POLLNVAL; return 1; }
    if (s.poll_bad_event == stage) { fds[0].revents = POLLERR; return 1; }
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
                std::cout << "UNSUPPORTED native executable boundary checks\n"; return 77;
            }
            require(bound.process.has_value(), "self binding failed unexpectedly");
            const auto native = bound.process->observe_executable();
            if (native.status == Status::unavailable || native.status == Status::permission_denied ||
                native.status == Status::resource_limit) {
                std::cout << "UNSUPPORTED native executable metadata reads\n"; return 77;
            }
            require(native.status == Status::observed, "native self executable observation failed unexpectedly");
            stable_and_changing(*bound.process);
            malformed_and_identity(*bound.process);
            failures_and_exit(*bound.process);
        }
        require(descriptor_count() == baseline, "executable test lifetime leaked descriptors");
        std::cout << "PASS anchored numeric executable sampling, metadata changes, errors, exit checks and FD cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
