#include "ni_ght_si/pinned_process.hpp"

#include <cerrno>
#include <array>
#include <atomic>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
using namespace std::chrono_literals;
using night::PinnedProcess;
using night::ProcessExitState;
using night::ProcessPinStatus;

static_assert(!std::is_copy_constructible_v<PinnedProcess>);
static_assert(!std::is_copy_assignable_v<PinnedProcess>);
static_assert(std::is_nothrow_move_constructible_v<PinnedProcess>);
static_assert(std::is_nothrow_move_assignable_v<PinnedProcess>);

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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
    if (!directory) throw RuntimeUnavailable("cannot inventory test descriptors");
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

void invalid_inputs() {
    for (const auto pid : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
        const auto result = PinnedProcess::bind_fresh(pid);
        require(!result.process && result.status == ProcessPinStatus::invalid_pid &&
                result.error_number == 0 && !result.operation.empty(), "invalid PID must fail explicitly");
    }
}

bool unavailable_runtime(ProcessPinStatus status) {
    return status == ProcessPinStatus::unavailable || status == ProcessPinStatus::permission_denied ||
           status == ProcessPinStatus::context_mismatch || status == ProcessPinStatus::resource_limit;
}

void self_and_moves(night::ProcessPinResult initial, std::size_t baseline) {
    require(initial.status == ProcessPinStatus::ready && initial.error_number == 0 && initial.process,
            "self acquisition must report ready with an owned handle");
    require(initial.process->valid(), "self handle invalid");
    const auto& metadata = initial.process->metadata();
    require(metadata.stat.pid == static_cast<std::uint64_t>(::getpid()) && metadata.uid == ::getuid() &&
            !metadata.boot_id.empty() && metadata.pid_namespace.starts_with("pid:[") &&
            metadata.observed_at <= night::Clock::now(), "self metadata does not describe fresh native context");
    require(initial.process->poll_exit().state == ProcessExitState::running, "self reported exited");
    const auto credentials = initial.process->observe_credentials();
    require(credentials.status == night::ProcessContextStatus::observed && credentials.context &&
            credentials.context->credentials.uid.real == ::getuid() &&
            credentials.context->credentials.uid.effective == ::geteuid() &&
            credentials.context->credentials.gid.real == ::getgid() &&
            credentials.context->credentials.gid.effective == ::getegid() &&
            credentials.context->begin <= credentials.context->end &&
            credentials.context->end <= night::Clock::now(), "fresh self credentials or timing incorrect");
    const auto executable = initial.process->observe_executable();
    if (executable.status == night::ProcessContextStatus::unavailable ||
        executable.status == night::ProcessContextStatus::permission_denied ||
        executable.status == night::ProcessContextStatus::resource_limit)
        throw RuntimeUnavailable("native self executable metadata unavailable");
    struct stat own_executable{};
    require(::stat("/proc/self/exe", &own_executable) == 0, "native self executable metadata unavailable");
    const night::ProcessExecutableMetadata expected_executable{
        static_cast<std::uint64_t>(own_executable.st_dev), static_cast<std::uint64_t>(own_executable.st_ino),
        static_cast<std::uint64_t>(own_executable.st_mode), static_cast<std::uint64_t>(own_executable.st_uid),
        static_cast<std::uint64_t>(own_executable.st_gid)};
    require(executable.status == night::ProcessContextStatus::observed && executable.context &&
            executable.context->stat.pid == metadata.stat.pid &&
            executable.context->executable == expected_executable &&
            executable.context->begin <= executable.context->end && executable.context->end <= night::Clock::now(),
            "fresh self numeric executable metadata or timing incorrect");
    const auto scope = initial.process->observe_application_scope();
    require(scope.stamp.begin <= scope.stamp.end && scope.stamp.end <= night::Clock::now(),
            "scope observation must have its own acquisition timestamps");
    require(scope.candidate.quality != night::Quality::valid || scope.candidate.value.has_value(),
            "valid naming observation requires an actual candidate");
    require(descriptor_count() == baseline + 2, "a retained identity must own exactly two descriptors");

    PinnedProcess moved{std::move(*initial.process)};
    require(!initial.process->valid() && moved.valid() &&
            initial.process->poll_exit().state == ProcessExitState::invalid_handle,
            "move construction left a usable source handle");
    require(descriptor_count() == baseline + 2, "move construction duplicated descriptors");
    require(initial.process->observe_application_scope().stamp.quality == night::Quality::error,
            "moved-from handle cannot observe a replacement path");
    const auto moved_credentials = initial.process->observe_credentials();
    require(moved_credentials.status == night::ProcessContextStatus::invalid_handle &&
            !moved_credentials.context && moved_credentials.error_number == EBADF,
            "moved-from handle cannot observe credentials");
    const auto moved_executable = initial.process->observe_executable();
    require(moved_executable.status == night::ProcessContextStatus::invalid_handle &&
            !moved_executable.context && moved_executable.error_number == EBADF,
            "moved-from handle cannot observe executable metadata");
    require(moved.observe_executable().status == night::ProcessContextStatus::observed,
            "moved handle lost its anchored executable observation");
    auto destination = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
    require(destination.process.has_value(), "second self acquisition failed");
    require(descriptor_count() == baseline + 4, "second identity did not own a descriptor pair");
    *destination.process = std::move(moved);
    require(!moved.valid() && destination.process->valid() &&
            destination.process->poll_exit().state == ProcessExitState::running,
            "move assignment did not transfer exact ownership");
    require(descriptor_count() == baseline + 2, "move assignment leaked the previous descriptor pair");
    auto& same = *destination.process;
    same = std::move(same);
    require(same.valid() && descriptor_count() == baseline + 2, "self move damaged ownership");
}

std::size_t retained_budget(std::size_t baseline) {
    std::vector<PinnedProcess> retained;
    retained.reserve(PinnedProcess::maximum_retained);
    for (std::size_t i = 0; i < PinnedProcess::maximum_retained; ++i) {
        auto candidate = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
        if (!candidate.process) {
            require(candidate.status == ProcessPinStatus::resource_limit,
                    "effective capacity must fail explicitly at a resource boundary");
            std::cout << "INFO descriptor reserve reduced retained capacity to " << retained.size() << '\n';
            break;
        }
        retained.push_back(std::move(*candidate.process));
    }
    require(!retained.empty(), "retained budget test admitted no identities");
    require(descriptor_count() == baseline + 2 * retained.size(), "retained budget descriptor count mismatch");
    const auto overflow = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
    require(!overflow.process && overflow.status == ProcessPinStatus::resource_limit,
            "global cap or effective descriptor reserve admitted another identity");
    rlimit limit{};
    require(::getrlimit(RLIMIT_NOFILE, &limit) == 0, "cannot observe actual descriptor limit");
    const auto current = descriptor_count();
    require(limit.rlim_cur == RLIM_INFINITY ||
            (limit.rlim_cur >= current && limit.rlim_cur - current >= PinnedProcess::descriptor_reserve),
            "admitted identities consumed the descriptor reserve");
    const auto capacity = retained.size();
    retained.clear();
    require(descriptor_count() == baseline, "destroyed identity handles leaked descriptors");
    auto recovered = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
    require(recovered.process.has_value(), "released identities did not restore admission capacity");
    return capacity;
}

void concurrent_admission(std::size_t baseline, std::size_t capacity) {
    std::array<std::vector<PinnedProcess>, 4> retained;
    std::atomic_bool start{false}, failed{false};
    std::vector<std::jthread> workers;
    workers.reserve(retained.size());
    for (auto& handles : retained) {
        workers.emplace_back([&start, &failed, &handles](std::stop_token stop) {
            while (!start && !stop.stop_requested()) std::this_thread::yield();
            if (stop.stop_requested()) return;
            try {
                for (int attempt = 0; attempt < 20; ++attempt) {
                    auto candidate = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
                    if (candidate.process && candidate.status == ProcessPinStatus::ready)
                        handles.push_back(std::move(*candidate.process));
                    else if (candidate.process || candidate.status != ProcessPinStatus::resource_limit)
                        failed = true;
                }
            } catch (...) { failed = true; }
        });
    }
    start = true;
    for (auto& worker : workers) worker.join();
    std::size_t admitted{};
    for (const auto& handles : retained) admitted += handles.size();
    require(!failed && admitted == capacity && admitted <= PinnedProcess::maximum_retained,
            "concurrent admissions changed the effective retained capacity");
    require(descriptor_count() == baseline + 2 * admitted, "concurrent handles lost exact descriptor ownership");
    for (auto& handles : retained) handles.clear();
    require(descriptor_count() == baseline, "concurrent handle cleanup leaked descriptors");
    require(retained_budget(baseline) == capacity, "concurrent admissions leaked global budget slots");
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
            // EOF from the parent is the normal exit trigger; a finite timeout
            // also ends this disposable child if the parent cannot finish.
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
        // Keep the readiness reader until the child has finished its write,
        // including cleanup after a readiness timeout.
        if (ready_ >= 0) ::close(ready_);
    }
    OwnedChild(const OwnedChild&) = delete;
    OwnedChild& operator=(const OwnedChild&) = delete;

    std::uint64_t pid() const { return static_cast<std::uint64_t>(pid_); }
    void await_ready() {
        pollfd descriptor{ready_, POLLIN, 0};
        char token{};
        require(::poll(&descriptor, 1, 1500) > 0 && ::read(ready_, &token, 1) == 1 && token == 'R',
                "owned child did not become ready");
        ::close(std::exchange(ready_, -1));
    }
    void release() noexcept {
        if (control_ >= 0) ::close(std::exchange(control_, -1));
    }
    void reap() {
        int status{};
        pid_t waited;
        do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "could not reap the owned child");
        pid_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "owned child did not exit naturally");
    }
private:
    pid_t pid_{-1};
    int control_{-1};
    int ready_{-1};
};

void owned_child_exit() {
    OwnedChild child;
    child.await_ready();
    auto candidate = PinnedProcess::bind_fresh(child.pid());
    require(candidate.process && candidate.status == ProcessPinStatus::ready &&
            candidate.process->metadata().stat.pid == child.pid(), "owned child acquisition failed");
    require(candidate.process->poll_exit().state == ProcessExitState::running,
            "live ready child reported exited");
    require(candidate.process->observe_credentials().status == night::ProcessContextStatus::observed,
            "live owned child credentials unavailable");
    const auto child_executable = candidate.process->observe_executable();
    require(child_executable.status == night::ProcessContextStatus::observed && child_executable.context &&
            child_executable.context->stat.pid == child.pid() &&
            S_ISREG(static_cast<mode_t>(child_executable.context->executable.mode)),
            "live owned child numeric executable metadata unavailable");
    child.release();
    const auto deadline = night::Clock::now() + 2s;
    bool observed{};
    do {
        const auto exit = candidate.process->poll_exit();
        require(exit.state == ProcessExitState::running || exit.state == ProcessExitState::exited,
                "owned child exit poll failed");
        if (exit.state == ProcessExitState::exited) { observed = true; break; }
        std::this_thread::sleep_for(2ms);
    } while (night::Clock::now() < deadline);
    require(observed, "pidfd did not observe the owned child's natural exit");
    // Until this parent reaps it, a zombie still owns its numeric PID.
    const auto zombie = PinnedProcess::bind_fresh(child.pid());
    require(!zombie.process && zombie.status == ProcessPinStatus::exited,
            "fresh acquisition accepted an exited unreaped child");
    child.reap();
    const auto after_exit = candidate.process->observe_application_scope();
    require(!after_exit.candidate.value, "dead anchored proc handle must not acquire another process's scope");
    const auto exited_credentials = candidate.process->observe_credentials();
    require(exited_credentials.status == night::ProcessContextStatus::exited && !exited_credentials.context,
            "exited child retained stale credentials");
    const auto exited_executable = candidate.process->observe_executable();
    require(exited_executable.status == night::ProcessContextStatus::exited && !exited_executable.context,
            "exited child retained stale executable metadata");
    require(candidate.process->poll_exit().state == ProcessExitState::exited,
            "the retained pidfd lost exit readiness after owned-child reaping");
}
} // namespace

int main() {
    try {
        invalid_inputs();
        std::cout << "PASS invalid PID contracts\n";
        const auto baseline = descriptor_count();
        auto probe = PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
        if (!probe.process && unavailable_runtime(probe.status)) {
            std::cout << "UNSUPPORTED native binding/exit checks: operation=" << probe.operation
                      << " status=" << static_cast<int>(probe.status) << " errno=" << probe.error_number << '\n';
            return 77;
        }
        require(probe.process.has_value(), "native self acquisition failed unexpectedly");
        self_and_moves(std::move(probe), baseline);
        require(descriptor_count() == baseline, "self/move lifetimes leaked descriptors");
        const auto capacity = retained_budget(baseline);
        require(descriptor_count() == baseline, "budget recovery lifetime leaked descriptors");
        concurrent_admission(baseline, capacity);
        require(descriptor_count() == baseline, "concurrent admission recovery leaked descriptors");
        owned_child_exit();
        require(descriptor_count() == baseline, "owned child observation leaked descriptors");
        std::cout << "PASS native self/move/cap/concurrent-admission/owned-child exit contracts\n";
        return 0;
    } catch (const RuntimeUnavailable& error) {
        std::cout << "UNSUPPORTED native runtime checks: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
