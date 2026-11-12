#include "ni_ght_si/pinned_process.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/magic.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
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
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
private:
    int fd_;
};

struct CloseDirectory {
    void operator()(DIR* directory) const noexcept { ::closedir(directory); }
};

// Serializes admissions and accounts for every retained pair, including moves
// into queues or observation owners. Bulk collection never enters this budget.
std::mutex admission_mutex;
std::size_t retained_processes{};

ProcessPinStatus error_status(int error, bool target_process) {
    switch (error) {
    case EACCES: case EPERM: return ProcessPinStatus::permission_denied;
    case ENOENT: case ESRCH:
        return target_process ? ProcessPinStatus::exited : ProcessPinStatus::unavailable;
    case ENOSYS: case ENODEV: case EOPNOTSUPP: return ProcessPinStatus::unavailable;
    case EMFILE: case ENFILE: case ENOMEM: case ENOSPC: return ProcessPinStatus::resource_limit;
    case EOVERFLOW: return ProcessPinStatus::malformed;
    default: return ProcessPinStatus::error;
    }
}

ProcessPinResult failure(ProcessPinStatus status, const char* operation, int error = 0) {
    return {{}, status, error, operation};
}

ProcessPinResult native_failure(const char* operation, int error, bool target_process = false) {
    return failure(error_status(error, target_process), operation, error);
}

struct ReadResult {
    std::string text;
    int error{};
};

ReadResult read_relative(int directory, const char* name) {
    Descriptor file{::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.get() < 0) return {{}, errno};
    ReadResult result;
    constexpr std::size_t byte_limit = 64 * 1024;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {{}, errno};
        if (count == 0) return result;
        const auto size = static_cast<std::size_t>(count);
        if (size > byte_limit - result.text.size()) return {{}, EOVERFLOW};
        result.text.append(buffer.data(), size);
    }
}

bool native_namespace_status(std::string_view text, std::uint64_t own_pid) {
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        if (line.starts_with("NSpid:")) {
            const auto ids = words(line.substr(6));
            // An ancestor proc mount reports additional namespace-local IDs.
            return ids.size() == 1 && unsigned_number(ids.front()) == own_pid;
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    return false;
}

bool valid_namespace(std::string_view identity) {
    if (!identity.starts_with("pid:[") || !identity.ends_with(']')) return false;
    const auto number = unsigned_number(identity.substr(5, identity.size() - 6));
    return number && *number != 0;
}

// The enumeration FD itself is included. Failure to obtain a bounded, current
// inventory disables admission instead of guessing at the available headroom.
int descriptor_count(int proc_root, std::uint64_t& count) {
    Descriptor enumeration{::openat(proc_root, "self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (enumeration.get() < 0) return errno;
    std::unique_ptr<DIR, CloseDirectory> directory{::fdopendir(enumeration.get())};
    if (!directory) return errno;
    enumeration.release();
    constexpr std::uint64_t scan_limit = 1024 * 1024;
    count = 0;
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) return errno;
        if (!unsigned_number(entry->d_name)) continue;
        if (++count > scan_limit) return EMFILE;
    }
}

ProcessPinResult check_headroom(int proc_root, std::uint64_t additional) {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) < 0) return native_failure("RLIMIT_NOFILE", errno);
    std::uint64_t count{};
    const auto inventory_error = descriptor_count(proc_root, count);
    if (inventory_error) return native_failure("proc/self/fd inventory", inventory_error);
    constexpr auto reserve = static_cast<std::uint64_t>(PinnedProcess::descriptor_reserve +
                                                      PinnedProcess::temporary_descriptor_allowance);
    if (limit.rlim_cur != RLIM_INFINITY &&
        (limit.rlim_cur < count || limit.rlim_cur - count < reserve + additional))
        return failure(ProcessPinStatus::resource_limit, "descriptor reserve");
    return {{}, ProcessPinStatus::ready, 0, {}};
}

ProcessExitObservation observe_exit(int fd) noexcept {
    if (fd < 0) return {ProcessExitState::invalid_handle, EBADF};
    pollfd observation{fd, POLLIN, 0};
    const auto result = ::poll(&observation, 1, 0);
    if (result < 0) return {ProcessExitState::error, errno};
    if (observation.revents & POLLNVAL) return {ProcessExitState::invalid_handle, EBADF};
    if (observation.revents & POLLERR) return {ProcessExitState::error, EIO};
    if (observation.revents & (POLLIN | POLLHUP)) return {ProcessExitState::exited, 0};
    if (result == 0) return {ProcessExitState::running, 0};
    return {ProcessExitState::error, EIO};
}

ProcessPinStatus stat_status(const ProcessStat& stat, std::uint64_t pid) {
    if (stat.pid != pid) return ProcessPinStatus::identity_changed;
    // A dead leader may still have live workers. Only the process pidfd can
    // establish whole-thread-group exit; unusable leader metadata is unavailable.
    if (stat.state == 'Z' || stat.state == 'X' || stat.state == 'x') return ProcessPinStatus::unavailable;
    if (!stat.threads || std::string_view{"RSDTtKWPI"}.find(stat.state) == std::string_view::npos)
        return ProcessPinStatus::malformed;
    return ProcessPinStatus::ready;
}

ProcessCredentialContextResult context_failure(ProcessContextStatus status, const char* operation, int error = 0) {
    return {{}, status, error, operation};
}

ProcessContextStatus context_error_status(int error) {
    switch (error) {
    case EBADF: return ProcessContextStatus::invalid_handle;
    case EACCES: case EPERM: return ProcessContextStatus::permission_denied;
    case ENOENT: case ESRCH: return ProcessContextStatus::exited;
    case ENOSYS: case ENODEV: case EOPNOTSUPP: return ProcessContextStatus::unavailable;
    case EMFILE: case ENFILE: case ENOMEM: case ENOSPC: return ProcessContextStatus::resource_limit;
    case EOVERFLOW: return ProcessContextStatus::malformed;
    default: return ProcessContextStatus::error;
    }
}

ProcessCredentialContextResult context_native_failure(const char* operation, int error) {
    return context_failure(context_error_status(error), operation, error);
}

ProcessExecutableContextResult executable_failure(ProcessContextStatus status, const char* operation, int error = 0) {
    return {{}, status, error, operation};
}

ProcessExecutableMetadata executable_metadata(const struct stat& file) {
    return {static_cast<std::uint64_t>(file.st_dev), static_cast<std::uint64_t>(file.st_ino),
            static_cast<std::uint64_t>(file.st_mode), static_cast<std::uint64_t>(file.st_uid),
            static_cast<std::uint64_t>(file.st_gid)};
}

ProcessContextStatus context_stat_status(const ProcessStat& stat, const ProcessStat& bound) {
    if (stat.pid != bound.pid || stat.start_ticks != bound.start_ticks)
        return ProcessContextStatus::identity_changed;
    const auto status = stat_status(stat, bound.pid);
    if (status == ProcessPinStatus::unavailable) return ProcessContextStatus::unavailable;
    if (status != ProcessPinStatus::ready) return ProcessContextStatus::malformed;
    return ProcessContextStatus::observed;
}

std::optional<ProcessCredentialIds> parse_credential_ids(std::string_view text) {
    const auto fields = words(text);
    if (fields.size() != 4) return {};
    std::array<std::uint64_t, 4> ids{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].find_first_not_of("0123456789") != std::string_view::npos) return {};
        const auto id = unsigned_number(fields[i]);
        if (!id || *id > std::numeric_limits<std::uint32_t>::max()) return {};
        ids[i] = *id;
    }
    return ProcessCredentialIds{ids[0], ids[1], ids[2], ids[3]};
}
} // namespace

std::optional<ProcessCredentials> parse_process_credentials(std::string_view status) {
    if (status.find('\0') != std::string_view::npos) return {};
    std::optional<ProcessCredentialIds> uid, gid;
    while (!status.empty()) {
        const auto newline = status.find('\n');
        const auto line = status.substr(0, newline);
        if (line.starts_with("Uid:") || line.starts_with("Gid:")) {
            auto& ids = line.starts_with("Uid:") ? uid : gid;
            if (ids) return {};
            ids = parse_credential_ids(line.substr(4));
            if (!ids) return {};
        }
        if (newline == std::string_view::npos) break;
        status.remove_prefix(newline + 1);
    }
    if (!uid || !gid) return {};
    return ProcessCredentials{*uid, *gid};
}

PinnedProcess::PinnedProcess(int proc_fd, int pid_fd, PinnedProcessMetadata&& metadata) noexcept
    : proc_fd_(proc_fd), pid_fd_(pid_fd), budget_slot_(true), metadata_(std::move(metadata)) {}

PinnedProcess::~PinnedProcess() { reset(); }

PinnedProcess::PinnedProcess(PinnedProcess&& other) noexcept
    : proc_fd_(std::exchange(other.proc_fd_, -1)), pid_fd_(std::exchange(other.pid_fd_, -1)),
      budget_slot_(std::exchange(other.budget_slot_, false)), metadata_(std::move(other.metadata_)) {}

PinnedProcess& PinnedProcess::operator=(PinnedProcess&& other) noexcept {
    if (this != &other) {
        reset();
        proc_fd_ = std::exchange(other.proc_fd_, -1);
        pid_fd_ = std::exchange(other.pid_fd_, -1);
        budget_slot_ = std::exchange(other.budget_slot_, false);
        metadata_ = std::move(other.metadata_);
    }
    return *this;
}

void PinnedProcess::reset() noexcept {
    if (proc_fd_ >= 0) ::close(std::exchange(proc_fd_, -1));
    if (pid_fd_ >= 0) ::close(std::exchange(pid_fd_, -1));
    if (std::exchange(budget_slot_, false)) {
        std::lock_guard lock{admission_mutex};
        --retained_processes;
    }
}

ProcessExitObservation PinnedProcess::poll_exit() const noexcept { return observe_exit(pid_fd_); }

ProcessCredentialContextResult PinnedProcess::observe_credentials() const {
    const auto begin = Clock::now();
    if (!valid()) return context_failure(ProcessContextStatus::invalid_handle, "credential handle", EBADF);
    const auto before_exit = observe_exit(pid_fd_);
    if (before_exit.state == ProcessExitState::exited)
        return context_failure(ProcessContextStatus::exited, "initial credential pidfd readiness");
    if (before_exit.state != ProcessExitState::running)
        return context_native_failure("initial credential pidfd readiness", before_exit.error_number);
    auto reservation = DescriptorReservation::try_acquire(1);
    if (!reservation) return context_failure(ProcessContextStatus::resource_limit, "temporary descriptor budget");
    const auto check_stat = [&](const ProcessStat& stat, const char* operation)
        -> std::optional<ProcessCredentialContextResult> {
        const auto status = context_stat_status(stat, metadata_.stat);
        if (status == ProcessContextStatus::observed) return {};
        if (status == ProcessContextStatus::unavailable) {
            const auto exit = observe_exit(pid_fd_);
            if (exit.state == ProcessExitState::exited)
                return context_failure(ProcessContextStatus::exited, operation);
            if (exit.state != ProcessExitState::running)
                return context_native_failure("credential leader pidfd readiness", exit.error_number);
        }
        return context_failure(status, operation);
    };

    const auto before_text = read_relative(proc_fd_, "stat");
    if (before_text.error) return context_native_failure("initial credential stat", before_text.error);
    const auto before = parse_process_stat(before_text.text);
    if (!before) return context_failure(ProcessContextStatus::malformed, "initial credential stat");
    if (auto failure = check_stat(*before, "initial credential stat")) return *failure;

    const auto first_text = read_relative(proc_fd_, "status");
    if (first_text.error) return context_native_failure("initial credential status", first_text.error);
    const auto first = parse_process_credentials(first_text.text);
    if (!first) return context_failure(ProcessContextStatus::malformed, "initial credential status");
    const auto second_text = read_relative(proc_fd_, "status");
    if (second_text.error) return context_native_failure("final credential status", second_text.error);
    const auto second = parse_process_credentials(second_text.text);
    if (!second) return context_failure(ProcessContextStatus::malformed, "final credential status");

    const auto after_text = read_relative(proc_fd_, "stat");
    if (after_text.error) return context_native_failure("final credential stat", after_text.error);
    const auto after = parse_process_stat(after_text.text);
    if (!after) return context_failure(ProcessContextStatus::malformed, "final credential stat");
    if (auto failure = check_stat(*after, "final credential stat")) return *failure;
    const auto after_exit = observe_exit(pid_fd_);
    if (after_exit.state == ProcessExitState::exited)
        return context_failure(ProcessContextStatus::exited, "final credential pidfd readiness");
    if (after_exit.state != ProcessExitState::running)
        return context_native_failure("final credential pidfd readiness", after_exit.error_number);

    if (*first != *second || before->name != after->name || before->parent_pid != after->parent_pid)
        return context_failure(ProcessContextStatus::context_changed, "credential context comparison");
    return {ProcessCredentialContext{*after, *second, begin, Clock::now()},
            ProcessContextStatus::observed, 0, {}};
}

ProcessExecutableContextResult PinnedProcess::observe_executable() const {
    const auto begin = Clock::now();
    if (!valid()) return executable_failure(ProcessContextStatus::invalid_handle, "executable handle", EBADF);
    const auto before_exit = observe_exit(pid_fd_);
    if (before_exit.state == ProcessExitState::exited)
        return executable_failure(ProcessContextStatus::exited, "initial executable pidfd readiness");
    if (before_exit.state != ProcessExitState::running)
        return executable_failure(context_error_status(before_exit.error_number),
                                  "initial executable pidfd readiness", before_exit.error_number);
    auto reservation = DescriptorReservation::try_acquire(3);
    if (!reservation) return executable_failure(ProcessContextStatus::resource_limit, "temporary descriptor budget");
    const auto metadata_failure = [&](const char* operation, int error) {
        if (error == ENOENT || error == ESRCH) {
            const auto exit = observe_exit(pid_fd_);
            if (exit.state == ProcessExitState::exited)
                return executable_failure(ProcessContextStatus::exited, operation, error);
            if (exit.state == ProcessExitState::running)
                return executable_failure(ProcessContextStatus::unavailable, operation, error);
            return executable_failure(context_error_status(exit.error_number),
                                      "missing executable pidfd readiness", exit.error_number);
        }
        return executable_failure(context_error_status(error), operation, error);
    };
    const auto check_stat = [&](const ProcessStat& stat, const char* operation)
        -> std::optional<ProcessExecutableContextResult> {
        const auto status = context_stat_status(stat, metadata_.stat);
        if (status == ProcessContextStatus::observed) return {};
        if (status == ProcessContextStatus::unavailable) {
            const auto exit = observe_exit(pid_fd_);
            if (exit.state == ProcessExitState::exited)
                return executable_failure(ProcessContextStatus::exited, operation);
            if (exit.state != ProcessExitState::running)
                return executable_failure(context_error_status(exit.error_number),
                                          "executable leader pidfd readiness", exit.error_number);
        }
        return executable_failure(status, operation);
    };

    const auto before_text = read_relative(proc_fd_, "stat");
    if (before_text.error) return metadata_failure("initial executable stat", before_text.error);
    const auto before = parse_process_stat(before_text.text);
    if (!before) return executable_failure(ProcessContextStatus::malformed, "initial executable stat");
    if (auto failure = check_stat(*before, "initial executable stat")) return *failure;

    // O_PATH follows the proc magic link without reading the image contents.
    // O_NOFOLLOW would instead open the link itself. Hold A through B and the
    // final checks; the two file handles plus a transient stat read peak at 3.
    Descriptor first{::openat(proc_fd_, "exe", O_PATH | O_CLOEXEC)};
    if (first.get() < 0) return metadata_failure("initial executable open", errno);
    struct stat first_file{};
    if (::fstat(first.get(), &first_file) < 0) return metadata_failure("initial executable fstat", errno);
    if (!S_ISREG(first_file.st_mode))
        return executable_failure(ProcessContextStatus::malformed, "initial executable file type");
    Descriptor second{::openat(proc_fd_, "exe", O_PATH | O_CLOEXEC)};
    if (second.get() < 0) return metadata_failure("final executable open", errno);
    struct stat second_file{};
    if (::fstat(second.get(), &second_file) < 0) return metadata_failure("final executable fstat", errno);
    if (!S_ISREG(second_file.st_mode))
        return executable_failure(ProcessContextStatus::malformed, "final executable file type");

    const auto after_text = read_relative(proc_fd_, "stat");
    if (after_text.error) return metadata_failure("final executable stat", after_text.error);
    const auto after = parse_process_stat(after_text.text);
    if (!after) return executable_failure(ProcessContextStatus::malformed, "final executable stat");
    if (auto failure = check_stat(*after, "final executable stat")) return *failure;
    const auto after_exit = observe_exit(pid_fd_);
    if (after_exit.state == ProcessExitState::exited)
        return executable_failure(ProcessContextStatus::exited, "final executable pidfd readiness");
    if (after_exit.state != ProcessExitState::running)
        return executable_failure(context_error_status(after_exit.error_number),
                                  "final executable pidfd readiness", after_exit.error_number);

    const auto executable = executable_metadata(second_file);
    if (executable_metadata(first_file) != executable || before->name != after->name ||
        before->parent_pid != after->parent_pid)
        return executable_failure(ProcessContextStatus::context_changed, "executable context comparison");
    return {ProcessExecutableContext{*after, executable, begin, Clock::now()},
            ProcessContextStatus::observed, 0, {}};
}

int PinnedProcess::send_signal(int signal) const noexcept {
    if (!valid()) return EBADF;
    if (signal != SIGTERM && signal != SIGKILL) return EINVAL;
#ifdef SYS_pidfd_send_signal
    if (::syscall(SYS_pidfd_send_signal, pid_fd_, signal, static_cast<siginfo_t*>(nullptr), 0U) < 0)
        return errno;
    return 0;
#else
    return ENOSYS;
#endif
}

ApplicationScopeObservation PinnedProcess::observe_application_scope() const {
    ApplicationScopeObservation result;
    result.stamp.begin = Clock::now();
    if (!valid()) result.candidate.quality = Quality::error;
    else {
        auto reservation = DescriptorReservation::try_acquire(1);
        if (!reservation) {
            result.candidate.quality = Quality::error;
            result.stamp.issues.push_back({Quality::error, "process cgroup", "temporary descriptor budget unavailable"});
        } else {
            const auto text = read_relative(proc_fd_, "cgroup");
            if (text.error) {
                result.candidate.quality = text.error == EACCES || text.error == EPERM ? Quality::permission_denied
                    : text.error == ENOENT || text.error == ENOSYS ? Quality::unsupported : Quality::error;
            } else result.candidate = parse_application_scope_candidate(text.text);
        }
    }
    if (result.candidate.quality == Quality::error || result.candidate.quality == Quality::permission_denied)
        result.stamp.quality = result.candidate.quality;
    result.stamp.end = Clock::now();
    return result;
}

ProcessPinResult PinnedProcess::bind_fresh(std::uint64_t pid) {
    if (!pid || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
        return failure(ProcessPinStatus::invalid_pid, "PID validation");
    std::lock_guard lock{admission_mutex};
    if (retained_processes >= maximum_retained)
        return failure(ProcessPinStatus::resource_limit, "retained process cap");
    auto reservation = DescriptorReservation::try_acquire(4);
    if (!reservation) return failure(ProcessPinStatus::resource_limit, "temporary descriptor budget");

    Descriptor root{::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (root.get() < 0) return native_failure("native proc root", errno);
    struct statfs filesystem{};
    if (::fstatfs(root.get(), &filesystem) < 0) return native_failure("proc filesystem", errno);
    if (filesystem.f_type != PROC_SUPER_MAGIC)
        return failure(ProcessPinStatus::context_mismatch, "proc filesystem");
    auto budget = check_headroom(root.get(), 2);
    if (budget.status != ProcessPinStatus::ready) return budget;

    const auto own_pid = static_cast<std::uint64_t>(::getpid());
    const auto own_stat_text = read_relative(root.get(), "self/stat");
    if (own_stat_text.error) return native_failure("proc/self/stat", own_stat_text.error);
    const auto own_stat = parse_process_stat(own_stat_text.text);
    if (!own_stat) return failure(ProcessPinStatus::malformed, "proc/self/stat");
    if (own_stat->pid != own_pid)
        return failure(ProcessPinStatus::context_mismatch, "proc/syscall PID");
    const auto own_status = read_relative(root.get(), "self/status");
    if (own_status.error) return native_failure("proc/self/status", own_status.error);
    if (!native_namespace_status(own_status.text, own_pid))
        return failure(ProcessPinStatus::context_mismatch, "proc/syscall PID namespace");
    std::array<char, 256> namespace_buffer{};
    const auto namespace_size = ::readlinkat(root.get(), "self/ns/pid", namespace_buffer.data(), namespace_buffer.size());
    if (namespace_size < 0) return native_failure("proc/self/ns/pid", errno);
    if (static_cast<std::size_t>(namespace_size) >= namespace_buffer.size())
        return failure(ProcessPinStatus::malformed, "proc/self/ns/pid");
    std::string pid_namespace{namespace_buffer.data(), static_cast<std::size_t>(namespace_size)};
    if (!valid_namespace(pid_namespace)) return failure(ProcessPinStatus::malformed, "proc/self/ns/pid");
    const auto boot = read_relative(root.get(), "sys/kernel/random/boot_id");
    if (boot.error) return native_failure("proc boot identity", boot.error);
    const auto boot_fields = words(boot.text);
    if (boot_fields.size() != 1) return failure(ProcessPinStatus::malformed, "proc boot identity");
    std::string boot_id{boot_fields.front()};

    // Acquire the lasting instance before opening its numeric proc directory.
    // If that instance exits before/during proc reads, final pidfd readiness
    // rejects the candidate even if the PID and start tick have been reused.
#ifdef SYS_pidfd_open
    const auto acquired = ::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0U);
    if (acquired < 0) return native_failure("pidfd_open", errno, true);
    Descriptor pidfd{static_cast<int>(acquired)};
#else
    return failure(ProcessPinStatus::unavailable, "pidfd_open", ENOSYS);
    Descriptor pidfd{-1};
#endif
    const auto pid_name = std::to_string(pid);
    Descriptor proc{::openat(root.get(), pid_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (proc.get() < 0) return native_failure("process proc directory", errno, true);
    const auto before_text = read_relative(proc.get(), "stat");
    if (before_text.error) return native_failure("initial anchored stat", before_text.error, true);
    const auto before = parse_process_stat(before_text.text);
    if (!before) return failure(ProcessPinStatus::malformed, "initial anchored stat");
    const auto before_status = stat_status(*before, pid);
    if (before_status != ProcessPinStatus::ready && before_status != ProcessPinStatus::unavailable)
        return failure(before_status, "initial anchored stat");
    const auto status_text = read_relative(proc.get(), "status");
    if (status_text.error) return native_failure("anchored status", status_text.error, true);
    const auto uid = parse_process_uid(status_text.text);
    if (!uid) return failure(ProcessPinStatus::malformed, "anchored UID");

    // A NEW stat open through the SAME directory verifies the sampled context.
    // The earlier pidfd must still report running after these anchored reads.
    const auto after_text = read_relative(proc.get(), "stat");
    if (after_text.error) return native_failure("final anchored stat", after_text.error, true);
    const auto after = parse_process_stat(after_text.text);
    if (!after) return failure(ProcessPinStatus::malformed, "final anchored stat");
    const auto after_status = stat_status(*after, pid);
    if (after_status != ProcessPinStatus::ready && after_status != ProcessPinStatus::unavailable)
        return failure(after_status, "final anchored stat");
    if (before->start_ticks != after->start_ticks)
        return failure(ProcessPinStatus::identity_changed, "anchored start ticks");
    budget = check_headroom(root.get(), 0);
    if (budget.status != ProcessPinStatus::ready) return budget;
    const auto exit = observe_exit(pidfd.get());
    if (exit.state == ProcessExitState::exited) return failure(ProcessPinStatus::exited, "pidfd readiness");
    if (exit.state != ProcessExitState::running) return native_failure("pidfd readiness", exit.error_number);
    if (before_status == ProcessPinStatus::unavailable || after_status == ProcessPinStatus::unavailable)
        return failure(ProcessPinStatus::unavailable, "anchored process leader");

    PinnedProcessMetadata metadata{*after, *uid, std::move(boot_id), std::move(pid_namespace), Clock::now()};
    ++retained_processes;
    return {PinnedProcess{proc.release(), pidfd.release(), std::move(metadata)}, ProcessPinStatus::ready, 0, {}};
}
} // namespace night
