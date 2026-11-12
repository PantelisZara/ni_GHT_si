#include "ni_ght_si/application_control.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <memory>
#include <limits>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>

namespace night {
namespace {
std::atomic<std::uint64_t> tickets{1};
class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) { if (fd_ >= 0) ::close(fd_); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
private:
    int fd_{-1};
};
struct CloseDirectory { void operator()(DIR* value) const noexcept { ::closedir(value); } };
ApplicationMembershipResult failure(ProcessActionStatus status, const char* reason, int error = 0) {
    return {{}, {status, error, reason, 0, 0}};
}
ApplicationMembershipResult native_failure(const char* reason, int error) {
    const auto status = error == EMFILE || error == ENFILE || error == ENOMEM ? ProcessActionStatus::resource_limit
        : error == EACCES || error == EPERM ? ProcessActionStatus::permission_denied : ProcessActionStatus::unavailable;
    return failure(status, reason, error);
}
std::optional<std::string> cgroup_namespace(std::uint64_t pid) {
    std::array<char, 256> buffer{};
    const auto path = pid ? "/proc/" + std::to_string(pid) + "/ns/cgroup" : "/proc/self/ns/cgroup";
    const auto count = ::readlink(path.c_str(), buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size()) return {};
    std::string result{buffer.data(), static_cast<std::size_t>(count)};
    if (!result.starts_with("cgroup:[") || !result.ends_with(']')) return {};
    return result;
}
struct Mount { std::uint64_t id{}, major{}, minor{}; bool operator==(const Mount&) const = default; };
std::optional<Mount> mount_identity(std::string_view text) {
    std::optional<Mount> result;
    while (!text.empty()) {
        const auto newline = text.find('\n'); const auto line = text.substr(0, newline); const auto fields = words(line);
        if (fields.size() >= 10 && fields[4] == "/sys/fs/cgroup") {
            if (result || fields[3] != "/") return {};
            const auto separator = std::find(fields.begin() + 6, fields.end(), "-");
            if (separator == fields.end() || separator + 1 == fields.end() || *(separator + 1) != "cgroup2") return {};
            const auto colon = fields[2].find(':');
            if (colon == std::string_view::npos) return {};
            const auto id = unsigned_number(fields[0]); const auto major = unsigned_number(fields[2].substr(0, colon));
            const auto minor = unsigned_number(fields[2].substr(colon + 1));
            if (!id || !major || !minor) return {};
            result = Mount{*id, *major, *minor};
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    return result;
}
std::optional<std::string> unified_path(std::string_view text) {
    std::optional<std::string> path;
    while (!text.empty()) {
        const auto newline = text.find('\n'); const auto line = text.substr(0, newline);
        if (line.starts_with("0::")) { if (path) return {}; path = std::string{line.substr(3)}; }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    return path;
}
Descriptor open_group(int root, std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > 4096) { errno = EINVAL; return {}; }
    Descriptor directory{::fcntl(root, F_DUPFD_CLOEXEC, 0)};
    if (directory.get() < 0) return directory;
    path.remove_prefix(1);
    while (!path.empty()) {
        const auto slash = path.find('/'); const auto component = path.substr(0, slash);
        if (component.empty() || component == "." || component == ".." ||
            std::any_of(component.begin(), component.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
            errno = EINVAL; return {};
        }
        const std::string name{component};
        Descriptor next{::openat(directory.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (next.get() < 0) return next;
        directory = std::move(next);
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return directory;
}
struct Pids { std::vector<std::uint64_t> values; int error{}; };
Pids read_members(int directory, std::size_t maximum) {
    Descriptor file{::openat(directory, "cgroup.procs", O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.get() < 0) return {{}, errno};
    std::array<char, 4096> buffer{}; std::string text;
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {{}, errno};
        if (!count) break;
        const auto size = static_cast<std::size_t>(count);
        if (size > 65536 - text.size()) return {{}, EOVERFLOW};
        text.append(buffer.data(), size);
    }
    Pids result;
    for (const auto word : words(text)) {
        const auto pid = unsigned_number(word);
        if (!pid || !*pid || *pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return {{}, EINVAL};
        result.values.push_back(*pid);
        if (result.values.size() > maximum) return {{}, E2BIG};
    }
    std::sort(result.values.begin(), result.values.end());
    if (std::adjacent_find(result.values.begin(), result.values.end()) != result.values.end()) return {{}, EINVAL};
    return result;
}
bool valid_membership(ApplicationMembership& value) {
    if (value.pids.size() > PinnedProcess::maximum_retained || !value.device || !value.directory_inode ||
        !value.mount_id || value.cgroup_namespace.empty()) return false;
    std::sort(value.pids.begin(), value.pids.end());
    return std::all_of(value.pids.begin(), value.pids.end(), [](auto pid) { return pid > 1; }) &&
        std::adjacent_find(value.pids.begin(), value.pids.end()) == value.pids.end();
}
bool same_provenance(const ApplicationMembership& a, const ApplicationMembership& b) {
    return a.device == b.device && a.directory_inode == b.directory_inode && a.mount_id == b.mount_id &&
        a.cgroup_namespace == b.cgroup_namespace;
}
bool same_context(const ProcessActionContext& a, const ProcessActionContext& b) {
    return a.credentials.credentials == b.credentials.credentials && a.security == b.security &&
        a.executable.executable == b.executable.executable && a.credentials.stat.pid == b.credentials.stat.pid &&
        a.credentials.stat.start_ticks == b.credentials.stat.start_ticks && a.credentials.stat.name == b.credentials.stat.name &&
        a.credentials.stat.parent_pid == b.credentials.stat.parent_pid && a.scope.unit_name == b.scope.unit_name &&
        a.scope.cgroup_path == b.scope.cgroup_path;
}
} // namespace

ApplicationMembershipResult read_application_membership(const ApplicationAuthority& authority, std::uint64_t seed_pid) {
    if (authority.classification != ApplicationAuthorityClass::application || authority.manager_owner.empty() ||
        authority.invocation_id.empty()) return failure(ProcessActionStatus::unavailable, "trusted scope authority unavailable");
    const auto checked = parse_application_scope_candidate("0::" + authority.cgroup_path + "\n");
    if (!checked.value || checked.value->unit_name != authority.unit_name)
        return failure(ProcessActionStatus::unavailable, "dedicated scope path unavailable");
    const auto own_namespace = cgroup_namespace(0);
    const auto target_namespace = seed_pid ? cgroup_namespace(seed_pid) : own_namespace;
    if (!own_namespace || !target_namespace || *own_namespace != *target_namespace)
        return failure(ProcessActionStatus::unavailable, "cgroup namespace mismatch");
    const auto mounts = read_text("/proc/self/mountinfo"); const auto own_cgroup = read_text("/proc/self/cgroup", 65536);
    if (mounts.issue || own_cgroup.issue) return failure(ProcessActionStatus::unavailable, "cgroup mount evidence unavailable");
    const auto mount = mount_identity(mounts.text); const auto own_path = unified_path(own_cgroup.text);
    if (!mount || !own_path) return failure(ProcessActionStatus::unavailable, "unambiguous native cgroup2 root unavailable");
    auto reservation = DescriptorReservation::try_acquire(3);
    if (!reservation) return failure(ProcessActionStatus::resource_limit, "temporary descriptor budget");
    Descriptor root{::open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (root.get() < 0) return native_failure("cgroup root open", errno);
    struct statfs fs{}; struct stat root_stat{};
    if (::fstatfs(root.get(), &fs) < 0 || ::fstat(root.get(), &root_stat) < 0) return native_failure("cgroup mount metadata", errno);
    if (fs.f_type != CGROUP2_SUPER_MAGIC || static_cast<std::uint64_t>(major(root_stat.st_dev)) != mount->major ||
        static_cast<std::uint64_t>(minor(root_stat.st_dev)) != mount->minor)
        return failure(ProcessActionStatus::unavailable, "cgroup mount identity mismatch");
    {
        auto own = open_group(root.get(), *own_path);
        if (own.get() < 0) return native_failure("own cgroup anchor", errno);
        const auto pids = read_members(own.get(), 65536);
        if (pids.error || !std::binary_search(pids.values.begin(), pids.values.end(), static_cast<std::uint64_t>(::getpid())))
            return failure(ProcessActionStatus::unavailable, "proc and cgroup mount membership disagree", pids.error);
    }
    auto group = open_group(root.get(), authority.cgroup_path);
    if (group.get() < 0) return native_failure("application cgroup anchor", errno);
    struct stat group_stat{};
    if (::fstat(group.get(), &group_stat) < 0) return native_failure("application cgroup metadata", errno);
    {
        Descriptor enumeration{::fcntl(group.get(), F_DUPFD_CLOEXEC, 0)};
        if (enumeration.get() < 0) return native_failure("application subgroup inventory", errno);
        std::unique_ptr<DIR, CloseDirectory> directory{::fdopendir(enumeration.get())};
        if (!directory) return native_failure("application subgroup inventory", errno);
        enumeration.release(); std::size_t count{};
        for (;;) {
            errno = 0; const auto* entry = ::readdir(directory.get());
            if (!entry) { if (errno) return native_failure("application subgroup inventory", errno); break; }
            const std::string_view name{entry->d_name};
            if (name == "." || name == "..") continue;
            if (++count > 1024) return failure(ProcessActionStatus::resource_limit, "subgroup inventory exceeded bound");
            struct stat child{};
            if (::fstatat(group.get(), entry->d_name, &child, AT_SYMLINK_NOFOLLOW) < 0)
                return native_failure("application subgroup metadata", errno);
            if (S_ISDIR(child.st_mode) || S_ISLNK(child.st_mode))
                return failure(ProcessActionStatus::unavailable, "nested application cgroups are unsupported");
        }
    }
    const auto pids = read_members(group.get(), PinnedProcess::maximum_retained);
    if (pids.error) return failure(pids.error == E2BIG ? ProcessActionStatus::resource_limit : ProcessActionStatus::unavailable,
                                  "application membership unavailable or oversized", pids.error);
    const auto final_mounts = read_text("/proc/self/mountinfo");
    const auto final_mount = final_mounts.issue ? std::optional<Mount>{} : mount_identity(final_mounts.text);
    if (!final_mount || *final_mount != *mount || cgroup_namespace(0) != own_namespace ||
        (seed_pid && cgroup_namespace(seed_pid) != target_namespace))
        return failure(ProcessActionStatus::context_changed, "cgroup namespace or mount changed during enumeration");
    return {ApplicationMembership{pids.values, static_cast<std::uint64_t>(group_stat.st_dev),
        static_cast<std::uint64_t>(group_stat.st_ino), mount->id, *own_namespace},
        {ProcessActionStatus::ready, 0, {}, 0, 0}};
}

ApplicationActionCandidate::ApplicationActionCandidate(std::vector<ProcessActionCandidate>&& candidates,
    ApplicationAuthority authority, ApplicationMembership membership, ApplicationMembershipReader reader)
    : candidates_(std::move(candidates)), exited_(candidates_.size(), false), authority_(std::move(authority)),
      membership_(std::move(membership)), reader_(std::move(reader)) {
    displayed_.reserve(candidates_.size());
    for (const auto& candidate : candidates_) {
        const auto& context = candidate.context(); const auto& stat = context.credentials.stat;
        displayed_.push_back({stat.pid, stat.start_ticks, context.credentials.credentials.uid.real, stat.name, context.executable.executable});
    }
}
ProcessActionResult ApplicationActionCandidate::result(ProcessActionStatus status, std::string reason, int error) const {
    return {status, error, std::move(reason), generation_, nonce_};
}
ApplicationActionPreparation ApplicationActionCandidate::prepare(std::uint64_t seed_pid, ApplicationAuthorityResolver resolver,
    ApplicationMembershipReader reader, std::chrono::milliseconds lifetime) {
    if (lifetime <= std::chrono::milliseconds::zero() || lifetime > std::chrono::seconds{60})
        return {{}, {ProcessActionStatus::denied, 0, "invalid confirmation lifetime", 0, 0}};
    if (!reader) reader = read_application_membership;
    const auto begin = Clock::now();
    const auto expired = [&] { return Clock::now() >= begin + lifetime; };
    const auto expired_result = [] {
        return ApplicationActionPreparation{{}, {ProcessActionStatus::expired, 0, "application preparation deadline exceeded", 0, 0}};
    };
    auto seed = ProcessActionCandidate::prepare(seed_pid, ProcessAction::terminate, resolver, lifetime);
    if (!seed.candidate) return {{}, seed.result};
    if (expired()) return expired_result();
    const auto authority = seed.candidate->authority();
    auto initial = reader(authority, seed_pid);
    if (expired()) return expired_result();
    if (!initial.membership) return {{}, initial.result};
    if (initial.membership->pids.size() > PinnedProcess::maximum_retained)
        return {{}, {ProcessActionStatus::resource_limit, 0, "application exceeds retained identity budget", 0, 0}};
    if (!valid_membership(*initial.membership) || initial.membership->pids.empty() ||
        !std::binary_search(initial.membership->pids.begin(), initial.membership->pids.end(), seed_pid))
        return {{}, {ProcessActionStatus::unavailable, 0, "ambiguous or oversized fixed application set", 0, 0}};
    std::vector<ProcessActionCandidate> members; members.reserve(initial.membership->pids.size());
    for (const auto pid : initial.membership->pids) {
        if (expired()) return expired_result();
        if (pid == seed_pid) members.push_back(std::move(*seed.candidate));
        else {
            auto prepared = ProcessActionCandidate::prepare(pid, ProcessAction::terminate, resolver, lifetime);
            if (expired()) return expired_result();
            if (!prepared.candidate) return {{}, prepared.result};
            members.push_back(std::move(*prepared.candidate));
        }
        if (members.back().authority() != authority)
            return {{}, {ProcessActionStatus::context_changed, 0, "members do not share exact application authority", 0, 0}};
    }
    if (expired()) return expired_result();
    auto final = reader(authority, seed_pid);
    if (expired()) return expired_result();
    if (!final.membership) return {{}, final.result};
    if (!valid_membership(*final.membership) || *final.membership != *initial.membership)
        return {{}, {ProcessActionStatus::context_changed, 0, "membership changed before confirmation", 0, 0}};
    ApplicationActionCandidate candidate{std::move(members), authority, std::move(*final.membership), std::move(reader)};
    candidate.seed_pid_ = seed_pid;
    candidate.expires_ = begin + lifetime;
    for (const auto& member : candidate.candidates_) candidate.expires_ = std::min(candidate.expires_, member.expires_at());
    if (Clock::now() >= candidate.expires_) return {{}, {ProcessActionStatus::expired, 0, "application confirmation expired", 0, 0}};
    const auto checked = candidate.check_membership();
    if (expired()) return expired_result();
    if (checked.status != ProcessActionStatus::ready) return {{}, checked};
    candidate.generation_ = tickets.fetch_add(1, std::memory_order_relaxed);
    if (!candidate.generation_) return {{}, {ProcessActionStatus::error, 0, "application generation exhausted", 0, 0}};
    auto ready = candidate.result(ProcessActionStatus::ready);
    return {std::move(candidate), std::move(ready)};
}
ProcessActionResult ApplicationActionCandidate::check_membership() {
    std::uint64_t live_seed{};
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        const auto exit = candidates_[i].poll_exit();
        if (exit.state == ProcessExitState::exited) exited_[i] = true;
        else if (exit.state != ProcessExitState::running) return result(ProcessActionStatus::error, "member pidfd observation failed", exit.error_number);
        else if (!live_seed) live_seed = displayed_[i].pid;
    }
    auto observed = reader_(authority_, live_seed);
    if (!observed.membership) return observed.result;
    if (!valid_membership(*observed.membership) || !same_provenance(membership_, *observed.membership))
        return result(ProcessActionStatus::context_changed, "application scope provenance changed");
    std::vector<std::uint64_t> expected;
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        const auto pid = displayed_[i].pid;
        if (!exited_[i] && !std::binary_search(observed.membership->pids.begin(), observed.membership->pids.end(), pid)) {
            const auto exit = candidates_[i].poll_exit();
            if (exit.state == ProcessExitState::exited) exited_[i] = true;
        }
        if (!exited_[i]) expected.push_back(pid);
    }
    std::sort(expected.begin(), expected.end());
    if (observed.membership->pids != expected)
        return result(ProcessActionStatus::context_changed, "fixed application membership changed; remaining signals cancelled");
    return result(ProcessActionStatus::ready);
}
std::optional<ApplicationConfirmation> ApplicationActionCandidate::confirm(std::uint64_t generation) {
    if (!reader_ || candidates_.empty() || cancelled_ || consumed_ || generation != generation_ || Clock::now() >= expires_) return {};
    nonce_ = tickets.fetch_add(1, std::memory_order_relaxed);
    if (!nonce_) return {};
    return ApplicationConfirmation{generation_, nonce_};
}
ApplicationDispatchResult ApplicationActionCandidate::dispatch(const ApplicationConfirmation& confirmation, std::function<bool()> commit) {
    ApplicationDispatchResult report;
    for (const auto& member : displayed_) report.members.push_back({member, result(ProcessActionStatus::cancelled, "not attempted"), false, false});
    const auto finish = [&](ProcessActionResult summary) {
        summary.generation = generation_; summary.request_id = confirmation.nonce;
        report.result = std::move(summary);
        const auto sent = std::count_if(report.members.begin(), report.members.end(), [](const auto& member) { return member.signal_submitted; });
        report.partial = sent > 0 && sent < static_cast<std::ptrdiff_t>(report.members.size());
        return report;
    };
    if (!reader_ || candidates_.empty())
        return finish(result(ProcessActionStatus::invalid_confirmation, "invalid or moved application candidate"));
    if (cancelled_) return finish(result(ProcessActionStatus::cancelled, "application candidate cancelled"));
    if (consumed_ || !nonce_ || confirmation.generation != generation_ || confirmation.nonce != nonce_)
        return finish(result(ProcessActionStatus::invalid_confirmation, "fresh fixed-set application confirmation required"));
    consumed_ = true;
    nonce_ = 0;
    if (Clock::now() >= expires_) return finish(result(ProcessActionStatus::expired, "application confirmation expired"));
    auto membership = check_membership();
    if (membership.status != ProcessActionStatus::ready) return finish(membership);
    // Validate the entire still-live fixed set before submitting any member.
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        if (exited_[i]) { report.members[i].result = result(ProcessActionStatus::exited, "confirmed member already exited"); continue; }
        auto& member = candidates_[i]; const auto before = member.context();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(expires_ - Clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) return finish(result(ProcessActionStatus::expired, "application confirmation expired"));
        auto fresh = member.renew(ProcessAction::terminate, remaining);
        if (fresh.status != ProcessActionStatus::ready) { report.members[i].result = fresh; return finish(fresh); }
        if (!same_context(before, member.context()) || member.authority() != authority_)
            return finish(result(ProcessActionStatus::context_changed, "a confirmed member context changed"));
    }
    for (std::size_t i = 0; i < candidates_.size(); ++i) {
        membership = check_membership();
        if (membership.status != ProcessActionStatus::ready) return finish(membership);
        if (exited_[i]) { report.members[i].result = result(ProcessActionStatus::exited, "confirmed member exited"); continue; }
        if (Clock::now() >= expires_) return finish(result(ProcessActionStatus::expired, "application confirmation expired"));
        const auto token = candidates_[i].confirm(candidates_[i].generation(), ProcessAction::terminate);
        if (!token) return finish(result(ProcessActionStatus::expired, "member confirmation unavailable"));
        report.members[i].attempted = true;
        auto validation = result(ProcessActionStatus::ready);
        report.members[i].result = candidates_[i].dispatch(*token, commit, [&] {
            // Check after manager resolution but before the final process read;
            // the final commit gate remains nonblocking and contains no I/O.
            validation = check_membership();
            return validation.status == ProcessActionStatus::ready;
        });
        if (validation.status != ProcessActionStatus::ready) {
            validation.request_id = token->nonce;
            report.members[i].result = std::move(validation);
        }
        report.members[i].signal_submitted = report.members[i].result.status == ProcessActionStatus::submitted;
        if (!report.members[i].signal_submitted && report.members[i].result.status != ProcessActionStatus::exited)
            return finish(report.members[i].result);
    }
    const bool any_submitted = std::any_of(report.members.begin(), report.members.end(), [](const auto& member) { return member.signal_submitted; });
    return finish(result(any_submitted ? ProcessActionStatus::submitted : ProcessActionStatus::exited,
        any_submitted ? "fixed-set TERM requests submitted; no dynamic application closure is guaranteed"
                      : "all confirmed members exited; no signals submitted"));
}
ProcessActionResult ApplicationActionCandidate::cancel() {
    cancelled_ = true; consumed_ = true; nonce_ = 0;
    for (auto& member : candidates_) member.cancel();
    return result(ProcessActionStatus::cancelled, "remaining application requests cancelled; submitted signals cannot be withdrawn");
}
} // namespace night
