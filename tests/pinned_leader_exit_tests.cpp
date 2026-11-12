#include "ni_ght_si/pinned_process.hpp"

#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace std::chrono_literals;

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

// Both child threads have finite lifetimes even if the parent abandons a check.
// EOF is the only command; neither cleanup nor the test sends a signal.
bool await_eof(int fd, night::Clock::time_point deadline) {
    for (;;) {
        const auto now = night::Clock::now();
        if (now >= deadline) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd descriptor{fd, POLLIN, 0};
        const auto result = ::poll(&descriptor, 1, static_cast<int>(remaining.count() + 1));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (descriptor.revents & (POLLERR | POLLNVAL))) return false;
        if (result > 0) {
            char byte{};
            return ::read(fd, &byte, 1) == 0;
        }
    }
}

struct WorkerState {
    int control;
    int ready;
    night::Clock::time_point deadline;
};

void* worker_main(void* argument) {
    const auto& state = *static_cast<const WorkerState*>(argument);
    const char token = 'R';
    if (::write(state.ready, &token, 1) != 1) ::_exit(2);
    ::close(state.ready);
    if (!await_eof(state.control, state.deadline)) ::_exit(3);
    ::close(state.control);
    return nullptr;
}

[[noreturn]] void child_main(int leader, int worker, int ready) {
    // The leader exits its pthread stack while worker_main still reads these
    // arguments, so their lifetime must extend until the whole process exits.
    static WorkerState state;
    state = {worker, ready, night::Clock::now() + 6s};
    pthread_t thread{};
    if (::pthread_create(&thread, nullptr, worker_main, &state) != 0) ::_exit(4);
    if (!await_eof(leader, state.deadline)) ::_exit(5);
    ::close(leader);
    ::pthread_exit(nullptr);
}

class OwnedThreadedChild {
public:
    OwnedThreadedChild() {
        std::array<std::array<int, 2>, 3> pipes{{{{-1, -1}}, {{-1, -1}}, {{-1, -1}}}};
        for (auto& pipe : pipes) {
            if (::pipe2(pipe.data(), O_CLOEXEC) < 0) {
                for (const auto& opened : pipes)
                    for (const auto fd : opened) if (fd >= 0) ::close(fd);
                throw RuntimeUnavailable("owned child control pipes unavailable");
            }
        }
        pid_ = ::fork();
        if (pid_ == 0) {
            ::close(pipes[0][1]);
            ::close(pipes[1][1]);
            ::close(pipes[2][0]);
            child_main(pipes[0][0], pipes[1][0], pipes[2][1]);
        }
        ::close(pipes[0][0]);
        ::close(pipes[1][0]);
        ::close(pipes[2][1]);
        if (pid_ < 0) {
            ::close(pipes[0][1]);
            ::close(pipes[1][1]);
            ::close(pipes[2][0]);
            throw RuntimeUnavailable("owned threaded child fork unavailable");
        }
        leader_ = pipes[0][1];
        worker_ = pipes[1][1];
        ready_ = pipes[2][0];
    }

    ~OwnedThreadedChild() {
        release_leader();
        release_worker();
        if (pid_ > 0) {
            int status{};
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
        // Preserve the readiness reader through the child's one-byte write,
        // including exceptional cleanup, to avoid generating SIGPIPE.
        if (ready_ >= 0) ::close(ready_);
    }
    OwnedThreadedChild(const OwnedThreadedChild&) = delete;
    OwnedThreadedChild& operator=(const OwnedThreadedChild&) = delete;

    std::uint64_t pid() const { return static_cast<std::uint64_t>(pid_); }

    void await_ready() {
        pollfd descriptor{ready_, POLLIN, 0};
        char token{};
        if (::poll(&descriptor, 1, 1500) <= 0 || ::read(ready_, &token, 1) != 1 || token != 'R')
            throw RuntimeUnavailable("owned child worker thread did not become ready");
        ::close(std::exchange(ready_, -1));
    }

    void release_leader() noexcept {
        if (leader_ >= 0) ::close(std::exchange(leader_, -1));
    }

    void release_worker() noexcept {
        if (worker_ >= 0) ::close(std::exchange(worker_, -1));
    }

    void reap() {
        int status{};
        pid_t waited;
        do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "could not reap owned threaded child");
        pid_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "owned threaded child did not exit naturally with status zero");
    }

private:
    pid_t pid_{-1};
    int leader_{-1}, worker_{-1}, ready_{-1};
};

void await_zombie_leader(std::uint64_t pid) {
    const auto path = std::filesystem::path{"/proc"} / std::to_string(pid) / "stat";
    const auto deadline = night::Clock::now() + 1500ms;
    do {
        const auto text = night::read_text(path);
        if (text.issue) throw RuntimeUnavailable("owned-child leader stat unavailable");
        const auto stat = night::parse_process_stat(text.text);
        require(stat && stat->pid == pid, "owned-child fresh leader stat malformed");
        if (stat->state == 'Z') return;
        std::this_thread::sleep_for(2ms);
    } while (night::Clock::now() < deadline);
    throw std::runtime_error("owned leader did not reach zombie state while worker remained alive");
}

void leader_exit_contract() {
    OwnedThreadedChild child;
    child.await_ready();
    auto bound = night::PinnedProcess::bind_fresh(child.pid());
    if (!bound.process && (bound.status == night::ProcessPinStatus::unavailable ||
            bound.status == night::ProcessPinStatus::permission_denied ||
            bound.status == night::ProcessPinStatus::context_mismatch ||
            bound.status == night::ProcessPinStatus::resource_limit))
        throw RuntimeUnavailable("native owned-child binding unavailable");
    require(bound.process && bound.status == night::ProcessPinStatus::ready,
            "owned threaded child failed fresh binding before leader exit");
    require(bound.process->poll_exit().state == night::ProcessExitState::running,
            "owned threaded child reported exited before leader exit");

    child.release_leader();
    await_zombie_leader(child.pid());
    require(bound.process->poll_exit().state == night::ProcessExitState::running,
            "leader zombie must not report whole-process exit while worker survives");
    const auto observation = bound.process->observe_credentials();
    const auto executable = bound.process->observe_executable();
    const auto fresh = night::PinnedProcess::bind_fresh(child.pid());
    std::cout << "INFO leader=Z retained_pidfd=running credential_status="
              << static_cast<int>(observation.status) << " credential_payload=" << observation.context.has_value()
              << " fresh_pin_status=" << static_cast<int>(fresh.status)
              << " fresh_handle=" << fresh.process.has_value()
              << " expected_unavailable_statuses=" << static_cast<int>(night::ProcessContextStatus::unavailable)
              << ',' << static_cast<int>(night::ProcessPinStatus::unavailable) << '\n';
    require(!observation.context && observation.status == night::ProcessContextStatus::unavailable,
            "leader-dead credential observation must be unavailable while process pidfd is running");
    require(!executable.context && executable.status == night::ProcessContextStatus::unavailable,
            "leader-dead executable observation must be unavailable while process pidfd is running");
    require(!fresh.process && fresh.status == night::ProcessPinStatus::unavailable,
            "fresh binding must be unavailable, not exited or ready, for leader-dead running process");

    child.release_worker();
    const auto deadline = night::Clock::now() + 1500ms;
    bool exited{};
    do {
        const auto observation_exit = bound.process->poll_exit();
        require(observation_exit.state == night::ProcessExitState::running ||
                observation_exit.state == night::ProcessExitState::exited,
                "owned threaded child exit observation failed");
        if (observation_exit.state == night::ProcessExitState::exited) { exited = true; break; }
        std::this_thread::sleep_for(2ms);
    } while (night::Clock::now() < deadline);
    require(exited, "whole-process exit was not observed after last worker returned");
    child.reap();
    require(bound.process->poll_exit().state == night::ProcessExitState::exited,
            "retained pidfd lost whole-process exit readiness after reaping");
    const auto after_exit = bound.process->observe_credentials();
    require(!after_exit.context && after_exit.status == night::ProcessContextStatus::exited,
            "credential observation after last-thread exit must report exited");
}
} // namespace

int main() {
    try {
        const auto baseline = descriptor_count();
        leader_exit_contract();
        require(descriptor_count() == baseline, "leader-exit fixture leaked descriptors");
        std::cout << "PASS leader exit with live worker and final whole-process exit\n";
        return 0;
    } catch (const RuntimeUnavailable& error) {
        std::cout << "UNSUPPORTED leader-exit native runtime checks: " << error.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
