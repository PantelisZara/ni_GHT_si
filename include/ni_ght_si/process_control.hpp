#pragma once

#include "ni_ght_si/pinned_process.hpp"

#include <chrono>
#include <functional>
#include <vector>

namespace night {

enum class ProcessAction { terminate, force_kill };
enum class ProcessActionStatus {
    ready, denied, unavailable, invalid_confirmation, expired, cancelled,
    context_changed, submitted, exited, permission_denied, resource_limit, error
};

struct ProcessActionResult {
    ProcessActionStatus status{ProcessActionStatus::error};
    int error_number{};
    std::string reason;
    std::uint64_t generation{}, request_id{};
};

struct ProcessSecurityContext {
    std::string user_namespace;
    std::vector<std::uint64_t> supplementary_groups;
    std::uint64_t permitted_capabilities{}, effective_capabilities{}, ambient_capabilities{};
    bool operator==(const ProcessSecurityContext&) const = default;
};

struct ProcessActionContext {
    ProcessCredentialContext credentials;
    ProcessExecutableContext executable;
    ApplicationScopeCandidate scope;
    ProcessSecurityContext security;
};

enum class ApplicationAuthorityClass { unknown, protected_process, application };
struct ApplicationAuthority {
    ApplicationAuthorityClass classification{ApplicationAuthorityClass::unknown};
    std::string unit_name, cgroup_path, manager_owner, invocation_id;
    std::string reason{}; // Static public failure stage; never include unit IDs or paths.
    bool operator==(const ApplicationAuthority&) const = default;
};
// Trusted integration boundary: resolve live user-manager ownership and exact
// dedicated application scope, never infer application authority from a name.
// Called independently before confirmation and dispatch. No cached attestation.
using ApplicationAuthorityResolver = std::function<ApplicationAuthority(const ProcessActionContext&)>;

struct ProcessConfirmation {
    std::uint64_t generation{}, nonce{};
    ProcessAction action{ProcessAction::terminate};
};
struct ProcessActionPreparation;

// Serialized single-owner API. The UI must display context() and ask explicitly
// for action() before calling confirm(). Force Kill requires a separate confirm.
// Matching prechecks are not atomic with exec, credentials or role changes.
class ProcessActionCandidate {
public:
    static ProcessActionPreparation prepare(std::uint64_t pid, ProcessAction action,
        ApplicationAuthorityResolver resolver, std::chrono::milliseconds lifetime = std::chrono::seconds{30});
    // Only for monitor-created children whose direct parent and executable match
    // this monitor. Never route arbitrary UI selections through this entry point.
    static ProcessActionPreparation prepare_owned_child(std::uint64_t pid, ProcessAction action,
        std::chrono::milliseconds lifetime = std::chrono::seconds{30});

    ProcessActionCandidate(const ProcessActionCandidate&) = delete;
    ProcessActionCandidate& operator=(const ProcessActionCandidate&) = delete;
    ProcessActionCandidate(ProcessActionCandidate&&) noexcept = default;
    ProcessActionCandidate& operator=(ProcessActionCandidate&&) noexcept = default;

    const ProcessActionContext& context() const noexcept { return context_; }
    const ApplicationAuthority& authority() const noexcept { return authority_; }
    const PinnedProcessMetadata& binding() const noexcept { return process_.metadata(); }
    ProcessAction action() const noexcept { return action_; }
    std::uint64_t generation() const noexcept { return generation_; }
    Clock::time_point expires_at() const noexcept { return expires_; }
    std::optional<ProcessConfirmation> confirm(std::uint64_t generation, ProcessAction action);
    // commit is an optional final nonblocking cancellation/commit handshake.
    // Once it accepts, a submitted signal cannot be retracted.
    // Optional group validation runs before the final process recapture. A
    // rejected validation consumes this consent; the caller must not retry it.
    ProcessActionResult dispatch(const ProcessConfirmation& confirmation, std::function<bool()> commit = {},
                                 std::function<bool()> validate_context = {});
    // Renew context/expiry/consent on the SAME retained instance, including a
    // separately requested Force Kill after an unsuccessful termination request.
    ProcessActionResult renew(ProcessAction action,
        std::chrono::milliseconds lifetime = std::chrono::seconds{30});
    ProcessActionResult cancel();
    ProcessExitObservation poll_exit() const noexcept { return process_.poll_exit(); }

private:
    ProcessActionCandidate(PinnedProcess&& process, ApplicationAuthorityResolver resolver, bool owned_child);
    static ProcessActionPreparation prepare_impl(std::uint64_t pid, ProcessAction action,
        ApplicationAuthorityResolver resolver, bool owned_child, std::chrono::milliseconds lifetime);
    ProcessActionResult capture(ProcessActionContext& context) const;
    ProcessActionResult evaluate(const ProcessActionContext& context, ApplicationAuthority& authority) const;
    ProcessActionResult result(ProcessActionStatus status, std::string reason = {}, int error = 0,
                               std::uint64_t request = 0) const;

    PinnedProcess process_;
    ApplicationAuthorityResolver resolver_;
    ProcessActionContext context_;
    ApplicationAuthority authority_;
    ProcessAction action_{ProcessAction::terminate};
    std::uint64_t generation_{}, confirmation_nonce_{};
    Clock::time_point expires_{};
    bool owned_child_{}, cancelled_{}, consumed_{};
};

struct ProcessActionPreparation {
    std::optional<ProcessActionCandidate> candidate;
    ProcessActionResult result;
};
} // namespace night
