#include "ni_ght_si/process_control.hpp"
#include "ni_ght_si/descriptor_budget.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace night {
namespace {
std::atomic<std::uint64_t> next_ticket{1};

class Descriptor {
public:
    explicit Descriptor(int fd) noexcept : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

ProcessActionStatus native_status(int error) {
    switch (error) {
    case EACCES: case EPERM: return ProcessActionStatus::permission_denied;
    case ENOENT: case ESRCH: return ProcessActionStatus::exited;
    case EMFILE: case ENFILE: case ENOMEM: case ENOSPC: return ProcessActionStatus::resource_limit;
    case ENOSYS: case EOPNOTSUPP: case ENODEV: return ProcessActionStatus::unavailable;
    default: return ProcessActionStatus::error;
    }
}
ProcessActionStatus context_status(ProcessContextStatus status) {
    switch (status) {
    case ProcessContextStatus::observed: return ProcessActionStatus::ready;
    case ProcessContextStatus::exited: return ProcessActionStatus::exited;
    case ProcessContextStatus::permission_denied: return ProcessActionStatus::permission_denied;
    case ProcessContextStatus::resource_limit: return ProcessActionStatus::resource_limit;
    case ProcessContextStatus::identity_changed: case ProcessContextStatus::context_changed:
        return ProcessActionStatus::context_changed;
    case ProcessContextStatus::unavailable: case ProcessContextStatus::malformed:
        return ProcessActionStatus::unavailable;
    default: return ProcessActionStatus::error;
    }
}

struct Text { std::string value; int error{}; };
Text anchored_text(int directory, const char* name) {
    Descriptor file{::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.get() < 0) return {{}, errno};
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {{}, errno};
        if (count == 0) return {std::move(text), 0};
        const auto size = static_cast<std::size_t>(count);
        if (size > 65536 - text.size()) return {{}, EOVERFLOW};
        text.append(buffer.data(), size);
    }
}
std::optional<std::string> namespace_link(int directory, const char* path) {
    std::array<char, 256> buffer{};
    const auto count = ::readlinkat(directory, path, buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size()) return {};
    std::string value{buffer.data(), static_cast<std::size_t>(count)};
    if (!value.starts_with("user:[") || !value.ends_with(']')) return {};
    const auto number = unsigned_number(std::string_view{value}.substr(6, value.size() - 7));
    if (!number || !*number) return {};
    return value;
}
std::optional<ProcessSecurityContext> security_status(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) return {};
    ProcessSecurityContext result;
    std::array<bool, 5> found{};
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        constexpr std::array names{"CapPrm:", "CapEff:", "CapAmb:", "Groups:", "Kthread:"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (!line.starts_with(names[i])) continue;
            if (found[i]) return {};
            found[i] = true;
            const auto fields = words(line.substr(std::string_view{names[i]}.size()));
            if (i < 3) {
                if (fields.size() != 1 || fields[0].empty() || fields[0].size() > 16 ||
                    fields[0].find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) return {};
                std::uint64_t value{};
                const auto [end, error] = std::from_chars(fields[0].data(), fields[0].data() + fields[0].size(), value, 16);
                if (error != std::errc{} || end != fields[0].data() + fields[0].size()) return {};
                if (i == 0) result.permitted_capabilities = value;
                if (i == 1) result.effective_capabilities = value;
                if (i == 2) result.ambient_capabilities = value;
            } else if (i == 3) {
                for (const auto field : fields) {
                    const auto value = unsigned_number(field);
                    if (!value || *value > std::numeric_limits<std::uint32_t>::max()) return {};
                    result.supplementary_groups.push_back(*value);
                }
                std::sort(result.supplementary_groups.begin(), result.supplementary_groups.end());
            } else if (fields.size() != 1 || fields[0] != "0") return {};
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    if (!std::all_of(found.begin(), found.end(), [](bool value) { return value; })) return {};
    return result;
}
bool stable_process(const ProcessStat& a, const ProcessStat& b) {
    return a.pid == b.pid && a.start_ticks == b.start_ticks && a.name == b.name && a.parent_pid == b.parent_pid;
}
bool same_context(const ProcessActionContext& a, const ProcessActionContext& b) {
    return stable_process(a.credentials.stat, b.credentials.stat) &&
        a.credentials.credentials == b.credentials.credentials && a.executable.executable == b.executable.executable &&
        a.scope.cgroup_path == b.scope.cgroup_path && a.scope.unit_name == b.scope.unit_name && a.security == b.security;
}
bool ordinary_ids(const ProcessCredentialIds& ids, std::uint64_t expected) {
    return ids.real == expected && ids.effective == expected && ids.saved == expected && ids.filesystem == expected;
}
// No target action is allowed when a complete current ancestor chain cannot be
// established. Names and an old bulk-row PPID never establish protection.
std::optional<bool> protected_ancestor(std::uint64_t target) {
    std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
    std::vector<std::uint64_t> seen;
    for (std::size_t depth = 0; depth < 128; ++depth) {
        if (pid == target || target == 1) return true;
        if (pid == 1) return false;
        if (!pid || std::find(seen.begin(), seen.end(), pid) != seen.end()) return {};
        seen.push_back(pid);
        const auto text = read_text(std::filesystem::path{"/proc"} / std::to_string(pid) / "stat", 65536);
        if (text.issue) return {};
        const auto stat = parse_process_stat(text.text);
        if (!stat || stat->pid != pid) return {};
        pid = stat->parent_pid;
    }
    return {};
}
} // namespace

ProcessActionCandidate::ProcessActionCandidate(PinnedProcess&& process, ApplicationAuthorityResolver resolver,
                                               bool owned_child)
    : process_(std::move(process)), resolver_(std::move(resolver)), owned_child_(owned_child) {}

ProcessActionResult ProcessActionCandidate::result(ProcessActionStatus status, std::string reason,
                                                   int error, std::uint64_t request) const {
    return {status, error, std::move(reason), generation_, request};
}
ProcessActionPreparation ProcessActionCandidate::prepare(std::uint64_t pid, ProcessAction action,
    ApplicationAuthorityResolver resolver, std::chrono::milliseconds lifetime) {
    return prepare_impl(pid, action, std::move(resolver), false, lifetime);
}
ProcessActionPreparation ProcessActionCandidate::prepare_owned_child(std::uint64_t pid, ProcessAction action,
    std::chrono::milliseconds lifetime) {
    return prepare_impl(pid, action, {}, true, lifetime);
}
ProcessActionPreparation ProcessActionCandidate::prepare_impl(std::uint64_t pid, ProcessAction action,
    ApplicationAuthorityResolver resolver, bool owned_child, std::chrono::milliseconds lifetime) {
    if (pid == 1 || pid == static_cast<std::uint64_t>(::getpid()))
        return {{}, {ProcessActionStatus::denied, 0, "PID 1 and this monitor are protected", 0, 0}};
    auto bound = PinnedProcess::bind_fresh(pid);
    if (!bound.process) {
        const auto status = bound.status == ProcessPinStatus::permission_denied ? ProcessActionStatus::permission_denied
            : bound.status == ProcessPinStatus::resource_limit ? ProcessActionStatus::resource_limit
            : bound.status == ProcessPinStatus::exited ? ProcessActionStatus::exited : ProcessActionStatus::unavailable;
        return {{}, {status, bound.error_number, bound.operation, 0, 0}};
    }
    ProcessActionCandidate candidate{std::move(*bound.process), std::move(resolver), owned_child};
    auto ready = candidate.renew(action, lifetime);
    if (ready.status != ProcessActionStatus::ready) return {{}, std::move(ready)};
    return {std::move(candidate), std::move(ready)};
}

ProcessActionResult ProcessActionCandidate::capture(ProcessActionContext& context) const {
    const auto credentials = process_.observe_credentials();
    if (!credentials.context) return result(context_status(credentials.status), credentials.operation, credentials.error_number);
    const auto executable = process_.observe_executable();
    if (!executable.context) return result(context_status(executable.status), executable.operation, executable.error_number);
    const auto scope_before = process_.observe_application_scope();
    if (!owned_child_ && (scope_before.stamp.quality != Quality::valid || !scope_before.candidate.value))
        return result(ProcessActionStatus::unavailable, "dedicated application scope unavailable");
    std::optional<ProcessSecurityContext> first, second;
    {
        auto reservation = DescriptorReservation::try_acquire(1);
        if (!reservation) return result(ProcessActionStatus::resource_limit, "temporary descriptor budget");
        const auto first_text = anchored_text(process_.proc_fd_, "status");
        if (first_text.error) return result(native_status(first_text.error), "initial security status", first_text.error);
        first = security_status(first_text.value);
        const auto first_namespace = namespace_link(process_.proc_fd_, "ns/user");
        const auto second_text = anchored_text(process_.proc_fd_, "status");
        if (second_text.error) return result(native_status(second_text.error), "final security status", second_text.error);
        second = security_status(second_text.value);
        const auto second_namespace = namespace_link(process_.proc_fd_, "ns/user");
        const auto own_namespace = namespace_link(AT_FDCWD, "/proc/self/ns/user");
        if (!first || !second || !first_namespace || !second_namespace || !own_namespace)
            return result(ProcessActionStatus::unavailable, "security or user namespace metadata unavailable");
        first->user_namespace = *first_namespace;
        second->user_namespace = *second_namespace;
        if (*first != *second) return result(ProcessActionStatus::context_changed, "security context changed while observing");
        if (*second_namespace != *own_namespace)
            return result(ProcessActionStatus::denied, "different user namespace");
    }
    const auto scope_after = process_.observe_application_scope();
    if (!owned_child_ && (scope_after.stamp.quality != Quality::valid || !scope_after.candidate.value))
        return result(ProcessActionStatus::unavailable, "dedicated application scope unavailable");
    if (scope_before.candidate.value.has_value() != scope_after.candidate.value.has_value() ||
        (scope_before.candidate.value && (scope_before.candidate.value->cgroup_path != scope_after.candidate.value->cgroup_path ||
         scope_before.candidate.value->unit_name != scope_after.candidate.value->unit_name)))
        return result(ProcessActionStatus::context_changed, "application scope changed while observing");
    const auto final_credentials = process_.observe_credentials();
    if (!final_credentials.context)
        return result(context_status(final_credentials.status), final_credentials.operation, final_credentials.error_number);
    if (credentials.context->credentials != final_credentials.context->credentials ||
        !stable_process(credentials.context->stat, executable.context->stat) ||
        !stable_process(credentials.context->stat, final_credentials.context->stat))
        return result(ProcessActionStatus::context_changed, "process context changed while observing");
    context.credentials = *final_credentials.context;
    context.executable = *executable.context;
    context.scope = scope_after.candidate.value.value_or(ApplicationScopeCandidate{});
    context.security = std::move(*second);
    return result(ProcessActionStatus::ready);
}

ProcessActionResult ProcessActionCandidate::evaluate(const ProcessActionContext& context, ApplicationAuthority& authority) const {
    const auto ancestor = protected_ancestor(context.credentials.stat.pid);
    if (!ancestor) return result(ProcessActionStatus::unavailable, "current ancestor protection unavailable");
    if (*ancestor) return result(ProcessActionStatus::denied, "monitor ancestor is protected");
    const auto uid = static_cast<std::uint64_t>(::getuid());
    const auto gid = static_cast<std::uint64_t>(::getgid());
    if (!uid || ::geteuid() != ::getuid() || ::getegid() != ::getgid() ||
        !ordinary_ids(context.credentials.credentials.uid, uid) || !ordinary_ids(context.credentials.credentials.gid, gid))
        return result(ProcessActionStatus::denied, "privileged or different-user credentials are protected");
    const auto group_count = ::getgroups(0, nullptr);
    if (group_count < 0 || group_count > 65536)
        return result(ProcessActionStatus::unavailable, "caller supplementary groups unavailable");
    std::vector<gid_t> native_groups(static_cast<std::size_t>(group_count));
    if (::getgroups(group_count, native_groups.data()) != group_count)
        return result(ProcessActionStatus::unavailable, "caller supplementary groups changed");
    std::vector<std::uint64_t> groups;
    groups.reserve(native_groups.size());
    for (const auto group : native_groups) groups.push_back(static_cast<std::uint64_t>(group));
    std::sort(groups.begin(), groups.end());
    if (groups != context.security.supplementary_groups)
        return result(ProcessActionStatus::denied, "different supplementary groups are protected");
    if (context.security.permitted_capabilities || context.security.effective_capabilities || context.security.ambient_capabilities)
        return result(ProcessActionStatus::denied, "capability-bearing processes are protected");
    const auto& file = context.executable.executable;
    if ((file.mode & (S_ISUID | S_ISGID | 0022)) || (file.owner_uid != 0 && file.owner_uid != uid))
        return result(ProcessActionStatus::denied, "privileged or writable executable context is protected");
    if (owned_child_) {
        struct stat own_executable{};
        if (context.credentials.stat.parent_pid != static_cast<std::uint64_t>(::getpid()) ||
            ::stat("/proc/self/exe", &own_executable) < 0 ||
            file.device != static_cast<std::uint64_t>(own_executable.st_dev) ||
            file.inode != static_cast<std::uint64_t>(own_executable.st_ino))
            return result(ProcessActionStatus::denied, "owned-child policy requires direct parent and monitor executable");
        authority = {};
        return result(ProcessActionStatus::ready);
    }
    if (!resolver_) return result(ProcessActionStatus::unavailable, "trusted application authority resolver unavailable");
    try { authority = resolver_(context); }
    catch (...) { return result(ProcessActionStatus::unavailable, "trusted application authority resolution failed"); }
    if (authority.classification == ApplicationAuthorityClass::protected_process)
        return result(ProcessActionStatus::denied, authority.reason.empty() ? "session infrastructure is protected" : authority.reason);
    if (authority.classification != ApplicationAuthorityClass::application ||
        authority.unit_name != context.scope.unit_name || authority.cgroup_path != context.scope.cgroup_path ||
        !authority.manager_owner.starts_with(':') || authority.manager_owner.size() < 2 || authority.invocation_id.empty())
        return result(ProcessActionStatus::unavailable, authority.reason.empty()
            ? "dedicated application authority is incomplete or ambiguous" : authority.reason);
    return result(ProcessActionStatus::ready);
}

ProcessActionResult ProcessActionCandidate::renew(ProcessAction action, std::chrono::milliseconds lifetime) {
    confirmation_nonce_ = 0;
    consumed_ = true;
    if (cancelled_) return result(ProcessActionStatus::cancelled, "candidate cancelled");
    if (!process_.valid()) return result(ProcessActionStatus::error, "invalid retained handle", EBADF);
    if ((action != ProcessAction::terminate && action != ProcessAction::force_kill) ||
        lifetime <= std::chrono::milliseconds::zero() || lifetime > std::chrono::seconds{60})
        return result(ProcessActionStatus::denied, "invalid action or confirmation lifetime");
    const auto begin = Clock::now();
    ProcessActionContext fresh;
    auto observed = capture(fresh);
    if (observed.status != ProcessActionStatus::ready) return observed;
    ApplicationAuthority authority;
    auto eligible = evaluate(fresh, authority);
    if (eligible.status != ProcessActionStatus::ready) return eligible;
    if (Clock::now() >= begin + lifetime) return result(ProcessActionStatus::expired, "fresh confirmation context expired");
    context_ = std::move(fresh);
    authority_ = std::move(authority);
    action_ = action;
    generation_ = next_ticket.fetch_add(1, std::memory_order_relaxed);
    if (!generation_) return result(ProcessActionStatus::error, "confirmation generation exhausted");
    expires_ = begin + lifetime;
    consumed_ = false;
    return result(ProcessActionStatus::ready);
}
std::optional<ProcessConfirmation> ProcessActionCandidate::confirm(std::uint64_t generation, ProcessAction action) {
    if (cancelled_ || consumed_ || !process_.valid() || generation != generation_ || action != action_ || Clock::now() >= expires_)
        return {};
    confirmation_nonce_ = next_ticket.fetch_add(1, std::memory_order_relaxed);
    if (!confirmation_nonce_) return {};
    return ProcessConfirmation{generation_, confirmation_nonce_, action_};
}
ProcessActionResult ProcessActionCandidate::dispatch(const ProcessConfirmation& confirmation, std::function<bool()> commit,
                                                    std::function<bool()> validate_context) {
    if (cancelled_) return result(ProcessActionStatus::cancelled, "candidate cancelled");
    if (consumed_ || !confirmation_nonce_ || confirmation.generation != generation_ ||
        confirmation.nonce != confirmation_nonce_ || confirmation.action != action_)
        return result(ProcessActionStatus::invalid_confirmation, "fresh action-specific confirmation required");
    consumed_ = true;
    confirmation_nonce_ = 0;
    const auto request = confirmation.nonce;
    if (Clock::now() >= expires_) return result(ProcessActionStatus::expired, "confirmation expired", 0, request);
    ProcessActionContext fresh;
    auto observed = capture(fresh);
    if (observed.status != ProcessActionStatus::ready) { observed.request_id = request; return observed; }
    ApplicationAuthority authority;
    auto eligible = evaluate(fresh, authority);
    if (eligible.status != ProcessActionStatus::ready) { eligible.request_id = request; return eligible; }
    if (!same_context(context_, fresh) || authority != authority_)
        return result(ProcessActionStatus::context_changed, "confirmed process or application authority changed", 0, request);
    if (validate_context && !validate_context())
        return result(ProcessActionStatus::context_changed, "application context validation rejected dispatch", 0, request);
    // The manager resolver may block. Read again after it returns, so a change
    // during authority resolution cannot reuse the earlier process observation.
    ProcessActionContext final_context;
    auto final_observation = capture(final_context);
    if (final_observation.status != ProcessActionStatus::ready) {
        final_observation.request_id = request;
        return final_observation;
    }
    if (!same_context(fresh, final_context) || !same_context(context_, final_context))
        return result(ProcessActionStatus::context_changed, "process changed during authority resolution", 0, request);
    if (Clock::now() >= expires_) return result(ProcessActionStatus::expired, "confirmation expired during revalidation", 0, request);
    if (commit && !commit()) return result(ProcessActionStatus::cancelled, "cancelled before signal commitment", 0, request);
    const auto error = process_.send_signal(action_ == ProcessAction::terminate ? SIGTERM : SIGKILL);
    if (error) return result(native_status(error), "pidfd signal submission failed", error, request);
    return result(ProcessActionStatus::submitted, "signal submitted; exit has not yet been observed", 0, request);
}
ProcessActionResult ProcessActionCandidate::cancel() {
    cancelled_ = true;
    consumed_ = true;
    confirmation_nonce_ = 0;
    process_.reset();
    return result(ProcessActionStatus::cancelled, "candidate cancelled; an already submitted signal cannot be withdrawn");
}
} // namespace night
