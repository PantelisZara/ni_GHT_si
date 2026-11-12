#include "ni_ght_si/pinned_process.hpp"

#include <cerrno>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern "C" long __real_syscall(long number, ...) noexcept;

namespace {
using namespace std::chrono_literals;
using night::PinnedProcess;
using night::ProcessPinStatus;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class RuntimeUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CloseDirectory {
    void operator()(DIR* directory) const noexcept { ::closedir(directory); }
};

std::size_t descriptor_count() {
    std::unique_ptr<DIR, CloseDirectory> directory{::opendir("/proc/self/fd")};
    if (!directory) throw RuntimeUnavailable("test descriptor inventory unavailable");
    std::size_t count{};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory.get());
        if (!entry) {
            require(errno == 0, "test descriptor enumeration failed");
            return count;
        }
        if (night::unsigned_number(entry->d_name)) ++count;
    }
}

class OwnedChild {
public:
    OwnedChild() {
        int control[2], ready[2];
        if (::pipe2(control, O_CLOEXEC) < 0) throw RuntimeUnavailable("owned control pipe unavailable");
        if (::pipe2(ready, O_CLOEXEC) < 0) {
            ::close(control[0]); ::close(control[1]);
            throw RuntimeUnavailable("owned readiness pipe unavailable");
        }
        pid_ = ::fork();
        if (pid_ == 0) {
            ::close(control[1]); ::close(ready[0]);
            const char token = 'R';
            if (::write(ready[1], &token, 1) != 1) ::_exit(2);
            ::close(ready[1]);
            const auto deadline = night::Clock::now() + 4s;
            for (;;) {
                const auto now = night::Clock::now();
                if (now >= deadline) break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                pollfd descriptor{control[0], POLLIN, 0};
                const auto result = ::poll(&descriptor, 1, static_cast<int>(remaining.count() + 1));
                if (result < 0 && errno == EINTR) continue;
                if (result < 0 || (descriptor.revents & (POLLERR | POLLNVAL))) ::_exit(3);
                if (result > 0) {
                    char byte{};
                    if (::read(control[0], &byte, 1) != 0) ::_exit(4);
                    break;
                }
            }
            ::close(control[0]); ::_exit(0);
        }
        ::close(control[0]); ::close(ready[1]);
        if (pid_ < 0) {
            ::close(control[1]); ::close(ready[0]);
            throw RuntimeUnavailable("owned fork unavailable");
        }
        control_ = control[1];
        ready_ = ready[0];
    }
    ~OwnedChild() {
        release();
        if (pid_ > 0) {
            int status{};
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
        // Preserve the reader through a possible delayed readiness write.
        if (ready_ >= 0) ::close(ready_);
    }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;

    pid_t pid() const noexcept { return pid_; }
    void await_ready() {
        pollfd descriptor{ready_, POLLIN, 0};
        char token{};
        require(::poll(&descriptor, 1, 1500) > 0 && ::read(ready_, &token, 1) == 1 && token == 'R',
                "owned child did not become ready");
        ::close(std::exchange(ready_, -1));
    }
    // Called inside the test wrapper. No exceptions cross the syscall boundary.
    bool exit_and_reap() noexcept {
        release();
        const auto deadline = night::Clock::now() + 2s;
        for (;;) {
            int status{};
            const auto waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (waited < 0 && errno != EINTR) return false;
            if (night::Clock::now() >= deadline) return false;
            static_cast<void>(::poll(nullptr, 0, 2));
        }
    }
private:
    void release() noexcept {
        if (control_ >= 0) ::close(std::exchange(control_, -1));
    }
    pid_t pid_{-1};
    int control_{-1};
    int ready_{-1};
};

enum class InterceptionMode { forward, fail, exit_before, exit_after };
struct InterceptionState {
    InterceptionMode mode{InterceptionMode::forward};
    int injected_error{};
    pid_t expected_pid{};
    OwnedChild* child{};
    std::size_t calls{}, unexpected_calls{}, invalid_arguments{};
    bool native_called{}, acquired{}, reaped{};
    int native_error{};
};
InterceptionState interception;

class Intercept {
public:
    Intercept(InterceptionMode mode, pid_t pid, int error = 0, OwnedChild* child = nullptr) {
        interception.mode = mode;
        interception.injected_error = error;
        interception.expected_pid = pid;
        interception.child = child;
        interception.native_called = interception.acquired = interception.reaped = false;
        interception.native_error = 0;
    }
    ~Intercept() {
        interception.mode = InterceptionMode::forward;
        interception.expected_pid = 0;
        interception.child = nullptr;
    }
    Intercept(const Intercept&) = delete;
    Intercept& operator=(const Intercept&) = delete;
};

bool unavailable_runtime(ProcessPinStatus status) {
    return status == ProcessPinStatus::unavailable || status == ProcessPinStatus::permission_denied ||
           status == ProcessPinStatus::context_mismatch || status == ProcessPinStatus::resource_limit;
}

void admission_recovery(std::size_t baseline) {
    {
        auto candidate = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
        require(candidate.process && candidate.status == ProcessPinStatus::ready &&
                candidate.process->poll_exit().state == night::ProcessExitState::running,
                "failed acquisition prevented subsequent self admission");
        require(descriptor_count() == baseline + 2, "recovered admission did not retain exactly one pair");
    }
    require(descriptor_count() == baseline, "recovered admission leaked descriptors");
}

std::size_t admission_capacity(std::size_t baseline) {
    std::vector<PinnedProcess> retained;
    retained.reserve(PinnedProcess::maximum_retained);
    for (std::size_t i = 0; i < PinnedProcess::maximum_retained; ++i) {
        auto candidate = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
        if (!candidate.process) {
            require(candidate.status == ProcessPinStatus::resource_limit,
                    "capacity admission failed outside the resource boundary");
            break;
        }
        retained.push_back(std::move(*candidate.process));
    }
    require(!retained.empty(), "no native admission capacity");
    const auto full = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
    require(!full.process && full.status == ProcessPinStatus::resource_limit,
            "capacity probe did not reach the global cap or descriptor reserve");
    const auto capacity = retained.size();
    retained.clear();
    require(descriptor_count() == baseline, "capacity probe leaked descriptors");
    return capacity;
}

void syscall_errors(std::size_t baseline) {
    struct Case { int error; ProcessPinStatus status; };
    constexpr Case cases[]{{ENOSYS, ProcessPinStatus::unavailable},
                           {EPERM, ProcessPinStatus::permission_denied},
                           {ESRCH, ProcessPinStatus::exited},
                           {EMFILE, ProcessPinStatus::resource_limit}};
    for (const auto& test : cases) {
        const auto calls = interception.calls;
        {
            Intercept hook{InterceptionMode::fail, ::getpid(), test.error};
            const auto result = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
            require(interception.calls == calls + 1 && !interception.native_called,
                    "injected error did not intercept the actual acquisition call");
            require(!result.process && result.status == test.status && result.error_number == test.error &&
                    result.operation == "pidfd_open", "pidfd_open error classification lost status or errno");
        }
        require(descriptor_count() == baseline, "injected acquisition failure leaked descriptors");
        admission_recovery(baseline);
    }
}

void exited_boundary(InterceptionMode mode, std::size_t baseline) {
    OwnedChild child;
    child.await_ready();
    const auto pid = child.pid();
    const auto calls = interception.calls;
    {
        Intercept hook{mode, pid, 0, &child};
        const auto result = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(pid));
        require(interception.calls == calls + 1 && interception.native_called && interception.reaped,
                "owned child did not exit and reap at the requested syscall boundary");
        require(!result.process && result.status == ProcessPinStatus::exited,
                "binding retained a handle after boundary exit");
        if (mode == InterceptionMode::exit_after || interception.acquired) {
            require(interception.acquired && result.operation == "process proc directory" &&
                    (result.error_number == ESRCH || result.error_number == ENOENT),
                    "post-pidfd proc open did not reject the reaped original process");
        } else {
            require(interception.native_error == ESRCH && result.operation == "pidfd_open" &&
                    result.error_number == ESRCH, "pre-acquisition exit lost its native ESRCH result");
        }
    }
    require(child.pid() == -1 && descriptor_count() == baseline,
            "boundary exit left an owned child or descriptor behind");
    admission_recovery(baseline);
}
} // namespace

// This binding-only executable exercises pidfd_open(pid_t, 0U) only.
// Reject every other variadic syscall signature, including signal submission.
extern "C" long __wrap_syscall(long number, ...) noexcept {
#ifdef SYS_pidfd_open
    if (number == SYS_pidfd_open) {
        va_list arguments;
        va_start(arguments, number);
        const auto pid = va_arg(arguments, pid_t);
        const auto flags = va_arg(arguments, unsigned int);
        va_end(arguments);
        ++interception.calls;
        if (flags != 0U || pid <= 0 || (interception.expected_pid && pid != interception.expected_pid)) {
            ++interception.invalid_arguments;
            errno = EINVAL;
            return -1;
        }
        if (interception.mode == InterceptionMode::fail) {
            errno = interception.injected_error;
            return -1;
        }
        if (interception.mode == InterceptionMode::exit_before) {
            interception.reaped = interception.child && interception.child->exit_and_reap();
            if (!interception.reaped) { errno = ETIMEDOUT; return -1; }
        }
        const auto result = __real_syscall(number, pid, flags);
        const auto error = result < 0 ? errno : 0;
        interception.native_called = true;
        interception.acquired = result >= 0;
        interception.native_error = error;
        if (interception.mode == InterceptionMode::exit_after && result >= 0) {
            interception.reaped = interception.child && interception.child->exit_and_reap();
            if (!interception.reaped) {
                ::close(static_cast<int>(result));
                errno = ETIMEDOUT;
                return -1;
            }
        }
        errno = error;
        return result;
    }
#else
    static_cast<void>(number);
#endif
    ++interception.unexpected_calls;
    errno = ENOSYS;
    return -1;
}

int main() {
#ifndef SYS_pidfd_open
    std::cout << "UNSUPPORTED pidfd_open syscall number absent from this toolchain\n";
    return 77;
#else
    try {
        const auto baseline = descriptor_count();
        {
            auto probe = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
            if (!probe.process && unavailable_runtime(probe.status)) {
                std::cout << "UNSUPPORTED native boundary checks: operation=" << probe.operation
                          << " status=" << static_cast<int>(probe.status) << " errno=" << probe.error_number << '\n';
                return 77;
            }
            require(probe.process && interception.calls == 1 && interception.native_called,
                    "linker wrapper did not observe the production acquisition");
        }
        require(descriptor_count() == baseline, "native preflight leaked descriptors");
        const auto capacity = admission_capacity(baseline);
        syscall_errors(baseline);
        exited_boundary(InterceptionMode::exit_before, baseline);
        exited_boundary(InterceptionMode::exit_after, baseline);
        require(admission_capacity(baseline) == capacity, "boundary failures consumed retained admission slots");
        require(interception.unexpected_calls == 0 && interception.invalid_arguments == 0,
                "wrapper observed an unsupported syscall signature");
        std::cout << "PASS pidfd errno classification, acquisition-boundary exit/reap and descriptor/admission recovery\n"
                  << "UNVERIFIED same-tick PID reuse/ABA; no PID churn or signaling performed\n";
        return 0;
    } catch (const RuntimeUnavailable& error) {
        std::cout << "UNSUPPORTED native boundary checks: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
#endif
}
