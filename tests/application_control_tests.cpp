#include "ni_ght_si/application_control.hpp"
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
#include <memory>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern "C" long __real_syscall(long, ...) noexcept;
extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __real_read(int, void*, std::size_t);
namespace {
using namespace std::chrono_literals;
using night::ApplicationActionCandidate;
using night::ProcessActionStatus;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::size_t descriptors() {
    auto* dir = ::opendir("/proc/self/fd"); require(dir, "descriptor inventory unavailable");
    std::size_t count{};
    while (const auto* entry = ::readdir(dir)) if (night::unsigned_number(entry->d_name)) ++count;
    ::closedir(dir); return count;
}
std::vector<int> inherited_controls;
class Child {
public:
    Child() {
        int pipe[2]; require(::pipe2(pipe, O_CLOEXEC) == 0, "owned control pipe unavailable");
        pid_ = ::fork();
        if (!pid_) {
            ::close(pipe[1]);
            for (const auto fd : inherited_controls) ::close(fd);
            struct sigaction action{}; action.sa_handler = SIG_DFL;
            ::sigemptyset(&action.sa_mask); if (::sigaction(SIGTERM, &action, nullptr) < 0) ::_exit(2);
            const auto deadline = night::Clock::now() + 6s;
            while (night::Clock::now() < deadline) {
                pollfd control{pipe[0], POLLIN, 0}; const auto result = ::poll(&control, 1, 20);
                if (result < 0 && errno == EINTR) continue;
                if (result < 0) ::_exit(3);
                if (result > 0) break;
            }
            ::close(pipe[0]); ::_exit(0);
        }
        ::close(pipe[0]);
        if (pid_ < 0) { ::close(pipe[1]); throw std::runtime_error("owned fork unavailable"); }
        control_ = pipe[1]; inherited_controls.push_back(control_);
    }
    ~Child() { release(); if (pid_ > 0) while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {} }
    std::uint64_t pid() const { return static_cast<std::uint64_t>(pid_); }
    void release() {
        if (control_ >= 0) { std::erase(inherited_controls, control_); ::close(std::exchange(control_, -1)); }
    }
    bool exited() const {
        siginfo_t info{};
        require(::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0,
                "owned child status unavailable");
        return info.si_pid == pid_;
    }
    void await_exit() const {
        const auto deadline = night::Clock::now() + 1500ms;
        while (night::Clock::now() < deadline) { if (exited()) return; std::this_thread::sleep_for(2ms); }
        throw std::runtime_error("owned child exit not observed");
    }
private:
    pid_t pid_{-1}; int control_{-1};
};
struct Fixture;
Fixture* active{};
int read_fd{-1}; bool read_cgroup{}; std::size_t read_offset{}, signals{}, native_signals{}, invalid_signals{};
struct Fixture {
    std::vector<std::unique_ptr<Child>> children;
    std::vector<std::uint64_t> members;
    std::size_t reader_calls{}, resolver_calls{}, change_on_read{}, change_on_resolve{}, error_on_signal{};
    std::uint64_t conflicting_authority{};
    Fixture() {
        children.push_back(std::make_unique<Child>()); children.push_back(std::make_unique<Child>());
        children.push_back(std::make_unique<Child>());
        members = {children[0]->pid(), children[1]->pid()};
        active = this; read_fd = -1; read_cgroup = false;
    }
    ~Fixture() { active = nullptr; }
    std::string path() const {
        return "/user.slice/user-" + std::to_string(::getuid()) + ".slice/user@" + std::to_string(::getuid()) +
            ".service/app.slice/app-owned-group-test.scope";
    }
    night::ApplicationAuthority authority(const night::ProcessActionContext& context) {
        ++resolver_calls;
        if (change_on_resolve && resolver_calls == change_on_resolve) members.push_back(children[2]->pid());
        return {night::ApplicationAuthorityClass::application, context.scope.unit_name, context.scope.cgroup_path,
            ":1.5151", context.credentials.stat.pid == conflicting_authority ? "different-invocation" : "owned-fixture-invocation"};
    }
    night::ApplicationMembershipResult membership(const night::ApplicationAuthority&, std::uint64_t) {
        ++reader_calls;
        if (change_on_read && reader_calls == change_on_read) members.push_back(children[2]->pid());
        auto live = members;
        std::erase_if(live, [&](auto pid) {
            const auto child = std::find_if(children.begin(), children.end(), [pid](const auto& item) { return item->pid() == pid; });
            return child != children.end() && (*child)->exited();
        });
        return {night::ApplicationMembership{std::move(live), 1, 2, 3, "cgroup:[owned-fixture]"},
                {ProcessActionStatus::ready, 0, {}, 0, 0}};
    }
    night::ApplicationActionPreparation prepare() {
        return ApplicationActionCandidate::prepare(children[0]->pid(),
            [this](const auto& context) { return authority(context); },
            [this](const auto& unit, auto seed) { return membership(unit, seed); });
    }
};
night::ApplicationConfirmation consent(ApplicationActionCandidate& candidate) {
    const auto token = candidate.confirm(candidate.generation()); require(token.has_value(), "group confirmation unavailable"); return *token;
}
void rejects_ambiguous_sets() {
    {
        Fixture fixture; fixture.conflicting_authority = fixture.children[1]->pid(); const auto result = fixture.prepare();
        require(!result.candidate && result.result.status == ProcessActionStatus::context_changed, "mixed invocation admitted");
    }
    {
        Fixture fixture; fixture.change_on_read = 2; const auto result = fixture.prepare();
        require(!result.candidate && result.result.status == ProcessActionStatus::context_changed, "changing pre-consent membership admitted");
    }
    {
        Fixture fixture; fixture.members.clear(); for (std::uint64_t i = 100; i < 165; ++i) fixture.members.push_back(i);
        const auto result = fixture.prepare();
        require(!result.candidate && result.result.status == ProcessActionStatus::resource_limit, "oversized set was truncated or admitted");
    }
    const auto unsupported = night::read_application_membership({}, 0);
    require(!unsupported.membership && unsupported.result.status == ProcessActionStatus::unavailable,
            "native reader accepted absent authority");
}
void cancellation_and_drift() {
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        const auto token = consent(*prepared.candidate); const auto before = signals;
        auto moved = std::move(*prepared.candidate);
        require(!prepared.candidate->confirm(token.generation) &&
                prepared.candidate->dispatch(token).result.status == ProcessActionStatus::invalid_confirmation && signals == before,
                "moved-from application candidate remained actionable");
        require(moved.cancel().status == ProcessActionStatus::cancelled &&
                moved.dispatch(token).result.status == ProcessActionStatus::cancelled && signals == before,
                "cancelled moved application candidate submitted");
    }
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        auto& candidate = *prepared.candidate; const auto token = consent(candidate); const auto before = signals;
        auto wrong = token; ++wrong.nonce;
        require(candidate.dispatch(wrong).result.status == ProcessActionStatus::invalid_confirmation, "wrong group nonce accepted");
        fixture.members.push_back(fixture.children[2]->pid());
        const auto result = candidate.dispatch(token);
        require(result.result.status == ProcessActionStatus::context_changed && signals == before &&
                std::none_of(result.members.begin(), result.members.end(), [](const auto& m) { return m.attempted; }),
                "entrant was added to consent or drift did not cancel all unsent members");
    }
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        auto& candidate = *prepared.candidate; const auto token = consent(candidate); const auto before = signals;
        std::size_t gates{};
        const auto report = candidate.dispatch(token, [&] { ++gates; return false; });
        require(report.result.status == ProcessActionStatus::cancelled && gates == 1 && signals == before,
                "group final cancellation gate sent a signal");
    }
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        auto& candidate = *prepared.candidate; const auto token = consent(candidate); const auto before = signals;
        fixture.change_on_resolve = fixture.resolver_calls + 3; // Two-member preflight, then first dispatch resolver.
        const auto report = candidate.dispatch(token);
        require(report.result.status == ProcessActionStatus::context_changed && signals == before,
                "membership change during manager resolution reached signal commitment");
    }
}
void actual_set_and_partial_failure() {
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        auto& candidate = *prepared.candidate; require(candidate.members().size() == 2, "displayed set incomplete");
        const auto token = consent(candidate); const auto before = native_signals;
        const auto report = candidate.dispatch(token);
        require(report.result.status == ProcessActionStatus::submitted && !report.partial && native_signals == before + 2 &&
                std::all_of(report.members.begin(), report.members.end(), [](const auto& m) { return m.signal_submitted; }),
                "confirmed fixed set did not submit exactly two owned TERM requests");
        require(candidate.dispatch(token).result.status == ProcessActionStatus::invalid_confirmation && native_signals == before + 2,
                "group consent was replayed");
        fixture.children[0]->await_exit(); fixture.children[1]->await_exit();
        require(!fixture.children[2]->exited(), "unconfirmed third child was signalled");
    }
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        const auto before = native_signals; fixture.error_on_signal = signals + 2;
        const auto report = prepared.candidate->dispatch(consent(*prepared.candidate));
        require(report.result.status == ProcessActionStatus::permission_denied && report.partial &&
                native_signals == before + 1 && report.members[0].signal_submitted &&
                report.members[1].attempted && !report.members[1].signal_submitted && report.members[1].result.error_number == EPERM,
                "partial submission lost exact per-member results");
    }
    {
        Fixture fixture; auto prepared = fixture.prepare(); require(prepared.candidate.has_value(), prepared.result.reason.c_str());
        fixture.children[0]->release(); fixture.children[0]->await_exit(); const auto before = native_signals;
        const auto report = prepared.candidate->dispatch(consent(*prepared.candidate));
        require(native_signals == before + 1 && report.members[0].result.status == ProcessActionStatus::exited &&
                !report.members[0].attempted && report.members[1].signal_submitted,
                "already-exited exact instance caused rebind or another target substitution");
    }
}
} // namespace

extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) { errno = EINVAL; return -1; }
    const auto fd = __real_openat(directory, path, flags);
    if (fd >= 0) { read_fd = fd; read_cgroup = active && std::strcmp(path, "cgroup") == 0; read_offset = 0; }
    return fd;
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, std::size_t count) {
    if (active && fd == read_fd && read_cgroup) {
        const auto value = "0::" + active->path() + "\n"; const auto size = std::min(count, value.size() - read_offset);
        std::memcpy(buffer, value.data() + read_offset, size); read_offset += size; return static_cast<ssize_t>(size);
    }
    return __real_read(fd, buffer, count);
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
        const auto* info = va_arg(args, siginfo_t*); const auto flags = va_arg(args, unsigned int); va_end(args); ++signals;
        char path[64]{}; std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
        const auto file = ::open(path, O_RDONLY | O_CLOEXEC); std::array<char, 1024> bytes{};
        const auto count = file >= 0 ? __real_read(file, bytes.data(), bytes.size()) : -1; if (file >= 0) ::close(file);
        std::optional<std::uint64_t> pid;
        if (count > 0) {
            const std::string_view text{bytes.data(), static_cast<std::size_t>(count)}; const auto start = text.find("Pid:\t");
            if (start != std::string_view::npos) { auto value = text.substr(start + 5); pid = night::unsigned_number(value.substr(0, value.find('\n'))); }
        }
        const bool owned = active && pid && std::any_of(active->children.begin(), active->children.end(),
            [&](const auto& child) { return child->pid() == *pid; });
        if (!owned || signal != SIGTERM || info || flags) { ++invalid_signals; errno = EPERM; return -1; }
        if (active->error_on_signal == signals) { errno = EPERM; return -1; }
        ++native_signals; return __real_syscall(number, fd, signal, static_cast<siginfo_t*>(nullptr), flags);
    }
#endif
    va_end(args); ++invalid_signals; errno = ENOSYS; return -1;
}
int main() {
    if (::getuid() == 0 || ::getuid() != ::geteuid()) { std::cout << "UNSUPPORTED ordinary account required\n"; return 77; }
    try {
        const auto baseline = descriptors();
        rejects_ambiguous_sets(); cancellation_and_drift(); actual_set_and_partial_failure();
        require(invalid_signals == 0 && native_signals == 4, "unexpected signal or non-owned process delivery");
        require(descriptors() == baseline && night::DescriptorReservation::try_acquire(8).has_value(), "fixed-set lifetime leaked descriptors or budget");
        std::cout << "PASS fixed owned TERM set, entrant/provenance/authority rejection, cancellation and exact partial/gone outcomes\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
