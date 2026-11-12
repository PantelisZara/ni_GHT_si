#pragma once
#include "ni_ght_si/process_control.hpp"

namespace night {
struct ApplicationMembership {
    std::vector<std::uint64_t> pids;
    std::uint64_t device{}, directory_inode{}, mount_id{};
    std::string cgroup_namespace;
    bool operator==(const ApplicationMembership&) const = default;
};
struct ApplicationMembershipResult {
    std::optional<ApplicationMembership> membership;
    ProcessActionResult result;
};
// The native reader accepts only a matching cgroup-v2 mount/namespace and a
// dedicated scope without nested groups. Injection is for bounded owned tests.
using ApplicationMembershipReader = std::function<ApplicationMembershipResult(const ApplicationAuthority&, std::uint64_t)>;
ApplicationMembershipResult read_application_membership(const ApplicationAuthority& authority, std::uint64_t seed_pid);

struct ApplicationMember {
    std::uint64_t pid{}, start_ticks{}, uid{};
    std::string name;
    ProcessExecutableMetadata executable;
};
struct ApplicationMemberResult {
    ApplicationMember member;
    ProcessActionResult result;
    bool attempted{}, signal_submitted{};
};
struct ApplicationConfirmation { std::uint64_t generation{}, nonce{}; };
struct ApplicationDispatchResult {
    ProcessActionResult result;
    std::vector<ApplicationMemberResult> members;
    bool partial{};
};
struct ApplicationActionPreparation;

// An immutable, freshly displayed fixed set, never a dynamic cgroup action.
// End Application submits TERM only; all candidates keep their exact pidfds.
// Owner serializes calls; commit may provide a per-signal cancellation gate.
class ApplicationActionCandidate {
public:
    static ApplicationActionPreparation prepare(std::uint64_t seed_pid, ApplicationAuthorityResolver resolver,
        ApplicationMembershipReader reader = {}, std::chrono::milliseconds lifetime = std::chrono::seconds{30});
    ApplicationActionCandidate(const ApplicationActionCandidate&) = delete;
    ApplicationActionCandidate& operator=(const ApplicationActionCandidate&) = delete;
    ApplicationActionCandidate(ApplicationActionCandidate&&) noexcept = default;
    ApplicationActionCandidate& operator=(ApplicationActionCandidate&&) noexcept = default;
    const std::vector<ApplicationMember>& members() const noexcept { return displayed_; }
    const ApplicationAuthority& authority() const noexcept { return authority_; }
    std::uint64_t generation() const noexcept { return generation_; }
    Clock::time_point expires_at() const noexcept { return expires_; }
    std::optional<ApplicationConfirmation> confirm(std::uint64_t generation);
    ApplicationDispatchResult dispatch(const ApplicationConfirmation& confirmation, std::function<bool()> commit = {});
    ProcessActionResult cancel();
private:
    ApplicationActionCandidate(std::vector<ProcessActionCandidate>&& candidates, ApplicationAuthority authority,
        ApplicationMembership membership, ApplicationMembershipReader reader);
    ProcessActionResult check_membership();
    ProcessActionResult result(ProcessActionStatus status, std::string reason = {}, int error = 0) const;
    std::vector<ProcessActionCandidate> candidates_;
    std::vector<ApplicationMember> displayed_;
    std::vector<bool> exited_;
    ApplicationAuthority authority_;
    ApplicationMembership membership_;
    ApplicationMembershipReader reader_;
    std::uint64_t generation_{}, nonce_{}, seed_pid_{};
    Clock::time_point expires_{};
    bool cancelled_{}, consumed_{};
};
struct ApplicationActionPreparation {
    std::optional<ApplicationActionCandidate> candidate;
    ProcessActionResult result;
};
} // namespace night
