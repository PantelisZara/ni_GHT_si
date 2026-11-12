#include "ni_ght_si/process_control.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

extern "C" long __real_syscall(long, ...) noexcept;
extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __real_read(int, void*, std::size_t);

namespace {
using namespace std::chrono_literals;
using night::ProcessAction;
using night::ProcessActionCandidate;
using night::ProcessActionStatus;
static_assert(!std::is_copy_constructible_v<ProcessActionCandidate>);
static_assert(std::is_nothrow_move_constructible_v<ProcessActionCandidate>);

void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::size_t descriptor_count() {
    auto* directory = ::opendir("/proc/self/fd");
    require(directory, "descriptor inventory unavailable");
    std::size_t count{};
    while (const auto* entry = ::readdir(directory)) if (night::unsigned_number(entry->d_name)) ++count;
    ::closedir(directory);
    return count;
}
class OwnedChild {
public:
    explicit OwnedChild(bool ignore_term = false) {
        int control[2], ready[2];
        require(::pipe2(control, O_CLOEXEC) == 0, "control pipe failed");
        if (::pipe2(ready, O_CLOEXEC) < 0) {
            ::close(control[0]); ::close(control[1]); throw std::runtime_error("readiness pipe failed");
        }
        pid_ = ::fork();
        if (!pid_) {
            ::close(control[1]); ::close(ready[0]);
            struct sigaction action{};
            action.sa_handler = ignore_term ? SIG_IGN : SIG_DFL;
            ::sigemptyset(&action.sa_mask);
            if (::sigaction(SIGTERM, &action, nullptr) < 0) ::_exit(2);
            const char token = 'R';
            if (::write(ready[1], &token, 1) != 1) ::_exit(3);
            ::close(ready[1]);
            const auto deadline = night::Clock::now() + 6s;
            while (night::Clock::now() < deadline) {
                pollfd fd{control[0], POLLIN, 0};
                const auto count = ::poll(&fd, 1, 20);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 || (fd.revents & (POLLERR | POLLNVAL))) ::_exit(4);
                if (count > 0) { char byte{}; if (::read(control[0], &byte, 1) != 0) ::_exit(5); break; }
            }
            ::close(control[0]); ::_exit(0);
        }
        ::close(control[0]); ::close(ready[1]);
        if (pid_ < 0) { ::close(control[1]); ::close(ready[0]); throw std::runtime_error("fork failed"); }
        control_ = control[1]; ready_ = ready[0];
        pollfd fd{ready_, POLLIN, 0}; char token{};
        if (::poll(&fd, 1, 1500) <= 0 || ::read(ready_, &token, 1) != 1 || token != 'R') {
            ::close(control_); control_ = -1;
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            ::close(ready_); ready_ = -1; pid_ = -1;
            throw std::runtime_error("owned child not ready");
        }
        ::close(std::exchange(ready_, -1));
    }
    ~OwnedChild() {
        if (control_ >= 0) ::close(control_);
        if (pid_ > 0) while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        if (ready_ >= 0) ::close(ready_);
    }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;
    std::uint64_t pid() const { return static_cast<std::uint64_t>(pid_); }
    int reap() {
        int status{}; pid_t waited;
        do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "owned child reap failed"); pid_ = -1; return status;
    }
private:
    pid_t pid_{-1}; int control_{-1}, ready_{-1};
};

std::uint64_t expected_pid{};
int target_directory{-1}, read_descriptor{-1};
enum class ReadKind { none, status, cgroup };
ReadKind read_kind{ReadKind::none};
bool synthetic_scope{}, injected_capability{}, mutate_on_authority{};
int scope_generation{}, manager_generation{}, signal_error{};
std::size_t cgroup_offset{}, submissions{}, native_submissions{}, invalid_signatures{};
int last_signal{};

class Target {
public:
    explicit Target(std::uint64_t pid, bool scope = false) {
        expected_pid = pid; synthetic_scope = scope; scope_generation = manager_generation = 0;
        target_directory = read_descriptor = -1; read_kind = ReadKind::none;
        injected_capability = mutate_on_authority = false; signal_error = 0;
    }
    ~Target() { expected_pid = 0; synthetic_scope = injected_capability = false; signal_error = 0; }
};
night::ApplicationAuthority authority(const night::ProcessActionContext& context) {
    require(context.credentials.stat.pid == expected_pid, "authority callback received a different target");
    if (mutate_on_authority) injected_capability = true;
    return {night::ApplicationAuthorityClass::application, context.scope.unit_name, context.scope.cgroup_path,
            ":1.4242", "fixture-invocation-" + std::to_string(manager_generation)};
}
ProcessActionCandidate owned(OwnedChild& child, ProcessAction action = ProcessAction::terminate,
                             std::chrono::milliseconds lifetime = 30s) {
    auto prepared = ProcessActionCandidate::prepare_owned_child(child.pid(), action, lifetime);
    require(prepared.result.status == ProcessActionStatus::ready && prepared.candidate.has_value(),
            prepared.result.reason.c_str());
    return std::move(*prepared.candidate);
}
night::ProcessConfirmation consent(ProcessActionCandidate& candidate) {
    const auto token = candidate.confirm(candidate.generation(), candidate.action());
    require(token.has_value(), "fresh explicit confirmation unavailable"); return *token;
}
void await_exit(ProcessActionCandidate& candidate) {
    const auto deadline = night::Clock::now() + 1500ms;
    while (night::Clock::now() < deadline) {
        const auto exit = candidate.poll_exit();
        if (exit.state == night::ProcessExitState::exited) return;
        require(exit.state == night::ProcessExitState::running, "pidfd observation failed");
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("owned child exit was not observed");
}
void protection_and_confirmation() {
    require(ProcessActionCandidate::prepare_owned_child(1, ProcessAction::terminate).result.status == ProcessActionStatus::denied,
            "PID1 was not protected");
    require(ProcessActionCandidate::prepare_owned_child(static_cast<std::uint64_t>(::getpid()), ProcessAction::force_kill).result.status == ProcessActionStatus::denied,
            "monitor was not protected");
    OwnedChild child; Target target{child.pid()}; auto candidate = owned(child);
    const auto count = submissions;
    require(!candidate.confirm(candidate.generation() + 1, candidate.action()), "wrong generation confirmed");
    require(!candidate.confirm(candidate.generation(), ProcessAction::force_kill), "TERM consent also authorized KILL");
    auto token = consent(candidate); auto wrong = token; wrong.action = ProcessAction::force_kill;
    require(candidate.dispatch(wrong).status == ProcessActionStatus::invalid_confirmation, "cross-action token accepted");
    wrong = token; ++wrong.nonce;
    require(candidate.dispatch(wrong).status == ProcessActionStatus::invalid_confirmation, "forged nonce accepted");
    auto moved = std::move(candidate);
    require(candidate.dispatch(token).status != ProcessActionStatus::submitted, "moved-from candidate submitted");
    require(moved.cancel().status == ProcessActionStatus::cancelled && moved.dispatch(token).status == ProcessActionStatus::cancelled,
            "cancelled candidate dispatched");
    require(submissions == count, "invalid/cancelled consent reached signal syscall");
}
void expiry_and_budget() {
    OwnedChild child; Target target{child.pid()}; auto candidate = owned(child, ProcessAction::terminate, 40ms);
    const auto token = consent(candidate); const auto count = submissions;
    std::this_thread::sleep_for(50ms);
    require(candidate.dispatch(token).status == ProcessActionStatus::expired && submissions == count, "expired consent dispatched");
    require(candidate.renew(ProcessAction::terminate).status == ProcessActionStatus::ready, "confirmation renewal failed");
    const auto fresh = consent(candidate);
    {
        auto full = night::DescriptorReservation::try_acquire(8); require(full.has_value(), "test budget reservation failed");
        require(candidate.dispatch(fresh).status == ProcessActionStatus::resource_limit, "busy dispatch did not fail before signal");
    }
    require(candidate.dispatch(fresh).status == ProcessActionStatus::invalid_confirmation && submissions == count,
            "failed dispatch retained replayable consent");
}
void kernel_failures() {
    OwnedChild child; Target target{child.pid()}; auto candidate = owned(child);
    struct Case { int error; ProcessActionStatus status; };
    constexpr Case cases[]{{EPERM, ProcessActionStatus::permission_denied}, {ESRCH, ProcessActionStatus::exited},
        {ENOSYS, ProcessActionStatus::unavailable}, {EBADF, ProcessActionStatus::error}, {EIO, ProcessActionStatus::error}};
    for (const auto& test : cases) {
        require(candidate.renew(ProcessAction::terminate).status == ProcessActionStatus::ready, "error test renewal failed");
        signal_error = test.error; const auto token = consent(candidate); const auto count = native_submissions;
        const auto result = candidate.dispatch(token);
        require(result.status == test.status && result.error_number == test.error && result.request_id == token.nonce &&
                native_submissions == count, "kernel failure lost errno, identity or attempted a fallback");
        require(candidate.dispatch(token).status == ProcessActionStatus::invalid_confirmation, "failed signal was replayed");
    }
    signal_error = 0;
    require(candidate.poll_exit().state == night::ProcessExitState::running, "injected failure affected disposable child");
}
void authoritative_context_changes() {
    OwnedChild child; Target target{child.pid(), true};
    auto prepared = ProcessActionCandidate::prepare(child.pid(), ProcessAction::terminate, authority);
    require(prepared.candidate.has_value(), prepared.result.reason.c_str());
    auto& candidate = *prepared.candidate; auto token = consent(candidate); const auto count = submissions;
    ++manager_generation;
    require(candidate.dispatch(token).status == ProcessActionStatus::context_changed && submissions == count,
            "changed manager invocation retained consent");
    require(candidate.renew(ProcessAction::terminate).status == ProcessActionStatus::ready, "authority renewal failed");
    token = consent(candidate); ++scope_generation;
    require(candidate.dispatch(token).status == ProcessActionStatus::context_changed && submissions == count,
            "changed application membership retained consent");
    require(candidate.renew(ProcessAction::terminate).status == ProcessActionStatus::ready, "scope renewal failed");
    token = consent(candidate); injected_capability = true;
    require(candidate.dispatch(token).status == ProcessActionStatus::denied && submissions == count,
            "capability-bearing process passed final policy");
    injected_capability = false;
    require(candidate.renew(ProcessAction::terminate).status == ProcessActionStatus::ready, "final-capture renewal failed");
    token = consent(candidate); mutate_on_authority = true;
    require(candidate.dispatch(token).status == ProcessActionStatus::context_changed && submissions == count,
            "context changed during authority resolution was not recaptured");
    injected_capability = mutate_on_authority = false;
    require(candidate.renew(ProcessAction::terminate).status == ProcessActionStatus::ready, "commit-gate renewal failed");
    token = consent(candidate); std::size_t gate_calls{};
    require(candidate.dispatch(token, [&] { ++gate_calls; return false; }).status == ProcessActionStatus::cancelled &&
            gate_calls == 1 && submissions == count, "final cancellation gate did not prevent signal");
    require(candidate.dispatch(token).status == ProcessActionStatus::invalid_confirmation,
            "cancelled commit retained consent");
    auto absent = ProcessActionCandidate::prepare(child.pid(), ProcessAction::terminate, {});
    require(!absent.candidate && absent.result.status == ProcessActionStatus::unavailable, "missing manager authority was allowed");
    auto protected_role = ProcessActionCandidate::prepare(child.pid(), ProcessAction::terminate,
        [](const auto&) { return night::ApplicationAuthority{night::ApplicationAuthorityClass::protected_process, {}, {}, {}, {}}; });
    require(!protected_role.candidate && protected_role.result.status == ProcessActionStatus::denied,
            "protected infrastructure classification was allowed");
}
void actual_term() {
    OwnedChild child; Target target{child.pid()}; auto candidate = owned(child); const auto token = consent(candidate);
    const auto result = candidate.dispatch(token);
    require(result.status == ProcessActionStatus::submitted && last_signal == SIGTERM && result.request_id == token.nonce,
            "real owned-child TERM was not submitted");
    require(candidate.dispatch(token).status == ProcessActionStatus::invalid_confirmation, "successful TERM was replayed");
    await_exit(candidate); const auto status = child.reap();
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "owned child did not observe TERM");
    require(candidate.renew(ProcessAction::force_kill).status == ProcessActionStatus::exited,
            "exited instance was renewed for KILL");
}
void actual_independent_force_kill() {
    OwnedChild child{true}; Target target{child.pid()}; auto candidate = owned(child); const auto term = consent(candidate);
    require(candidate.dispatch(term).status == ProcessActionStatus::submitted, "ignored TERM submission failed");
    const auto count = submissions;
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(2ms);
        require(candidate.poll_exit().state == night::ProcessExitState::running && submissions == count,
                "ignored TERM escalated automatically");
    }
    const auto old_generation = candidate.generation();
    require(candidate.renew(ProcessAction::force_kill).status == ProcessActionStatus::ready &&
            candidate.generation() != old_generation && !candidate.confirm(old_generation, ProcessAction::force_kill),
            "Force Kill did not require fresh generation");
    require(candidate.dispatch(term).status == ProcessActionStatus::invalid_confirmation, "TERM consent reused for KILL");
    const auto force = consent(candidate);
    require(candidate.dispatch(force).status == ProcessActionStatus::submitted && last_signal == SIGKILL,
            "independently confirmed owned-child KILL was not submitted");
    await_exit(candidate); const auto status = child.reap();
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "owned child did not observe KILL");
}
} // namespace

extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) { errno = EINVAL; return -1; }
    const auto fd = __real_openat(directory, path, flags);
    if (expected_pid && fd >= 0) {
        char pid[32]{}; std::snprintf(pid, sizeof(pid), "%llu", static_cast<unsigned long long>(expected_pid));
        if (std::strcmp(path, pid) == 0) target_directory = fd;
        if (directory == target_directory) {
            read_descriptor = fd; read_kind = ReadKind::none;
            if (std::strcmp(path, "status") == 0) read_kind = ReadKind::status;
            if (std::strcmp(path, "cgroup") == 0) { read_kind = ReadKind::cgroup; cgroup_offset = 0; }
        }
    }
    return fd;
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, std::size_t count) {
    if (expected_pid && fd == read_descriptor && read_kind == ReadKind::cgroup && synthetic_scope) {
        const auto path = "0::/user.slice/user-" + std::to_string(::getuid()) + ".slice/user@" +
            std::to_string(::getuid()) + ".service/app.slice/app-disposable-" + std::to_string(scope_generation) + ".scope\n";
        const auto size = std::min(count, path.size() - cgroup_offset);
        std::memcpy(buffer, path.data() + cgroup_offset, size); cgroup_offset += size;
        return static_cast<ssize_t>(size);
    }
    const auto size = __real_read(fd, buffer, count);
    if (size > 0 && expected_pid && fd == read_descriptor && read_kind == ReadKind::status && injected_capability) {
        const std::string_view text{static_cast<char*>(buffer), static_cast<std::size_t>(size)};
        const auto cap = text.find("CapEff:\t");
        if (cap != std::string_view::npos && cap + 8 + 16 <= text.size())
            static_cast<char*>(buffer)[cap + 8 + 15] = '1';
    }
    return size;
}
extern "C" long __wrap_syscall(long number, ...) noexcept {
    va_list args; va_start(args, number);
#ifdef SYS_pidfd_open
    if (number == SYS_pidfd_open) {
        const auto pid = va_arg(args, pid_t); const auto flags = va_arg(args, unsigned int); va_end(args);
        return __real_syscall(number, pid, flags);
    }
#endif
#ifdef SYS_pidfd_send_signal
    if (number == SYS_pidfd_send_signal) {
        const auto fd = va_arg(args, int); const auto signal = va_arg(args, int);
        const auto* info = va_arg(args, siginfo_t*); const auto flags = va_arg(args, unsigned int); va_end(args);
        ++submissions; last_signal = signal;
        if (!expected_pid || fd < 0 || (signal != SIGTERM && signal != SIGKILL) || info || flags) {
            ++invalid_signatures; errno = EINVAL; return -1;
        }
        // Real signal delivery is restricted to the exact disposable child:
        // compare the kernel's pidfd identity before forwarding the syscall.
        char path[64]{}; std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
        const auto file = ::open(path, O_RDONLY | O_CLOEXEC);
        std::array<char, 1024> data{}; const auto count = file >= 0 ? __real_read(file, data.data(), data.size()) : -1;
        if (file >= 0) ::close(file);
        bool matched{};
        if (count > 0) {
            const std::string_view text{data.data(), static_cast<std::size_t>(count)};
            const auto offset = text.find("Pid:\t");
            if (offset != std::string_view::npos) {
                auto value = text.substr(offset + 5); value = value.substr(0, value.find('\n'));
                matched = night::unsigned_number(value) == expected_pid;
            }
        }
        if (!matched) { ++invalid_signatures; errno = EPERM; return -1; }
        if (signal_error) { errno = signal_error; return -1; }
        ++native_submissions;
        return __real_syscall(number, fd, signal, static_cast<siginfo_t*>(nullptr), flags);
    }
#endif
    va_end(args); ++invalid_signatures; errno = ENOSYS; return -1;
}
int main() {
    if (::getuid() == 0 || ::getuid() != ::geteuid()) {
        std::cout << "UNSUPPORTED requires ordinary unprivileged test account\n"; return 77;
    }
    try {
        const auto baseline = descriptor_count();
        protection_and_confirmation(); expiry_and_budget(); kernel_failures(); authoritative_context_changes();
        actual_term(); actual_independent_force_kill();
        require(invalid_signatures == 0 && native_submissions == 3, "unexpected syscall or non-owned signal delivery");
        require(descriptor_count() == baseline, "process-control lifetime leaked descriptors");
        require(night::DescriptorReservation::try_acquire(8).has_value(), "process-control leaked temporary budget");
        std::cout << "PASS owned TERM, independently confirmed KILL, cancellation, expiry, policy transitions, errno and exact pidfd ownership\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
