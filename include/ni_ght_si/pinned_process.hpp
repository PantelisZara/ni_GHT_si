#pragma once

#include "ni_ght_si/process.hpp"
#include "ni_ght_si/application_scope.hpp"

#include <cstddef>

namespace night {

enum class ProcessPinStatus {
    ready, invalid_pid, unavailable, permission_denied, exited, identity_changed,
    resource_limit, malformed, context_mismatch, error
};

enum class ProcessExitState { running, exited, invalid_handle, error };

struct ProcessExitObservation {
    ProcessExitState state{ProcessExitState::error};
    int error_number{};
};

struct PinnedProcessMetadata {
    ProcessStat stat;
    std::uint64_t uid{};
    std::string boot_id;
    std::string pid_namespace;
    Clock::time_point observed_at{};
};

struct ProcessCredentialIds {
    std::uint64_t real{}, effective{}, saved{}, filesystem{};
    bool operator==(const ProcessCredentialIds&) const = default;
};

struct ProcessCredentials {
    ProcessCredentialIds uid, gid;
    bool operator==(const ProcessCredentials&) const = default;
};

// Requires exactly one Uid and Gid record, each containing four unsigned
// decimal IDs representable in 32 bits. Other status records are ignored.
std::optional<ProcessCredentials> parse_process_credentials(std::string_view status);

enum class ProcessContextStatus {
    observed, invalid_handle, exited, identity_changed, context_changed, malformed,
    permission_denied, unavailable, resource_limit, error
};

struct ProcessCredentialContext {
    ProcessStat stat;
    ProcessCredentials credentials;
    Clock::time_point begin{}, end{};
};

struct ProcessCredentialContextResult {
    std::optional<ProcessCredentialContext> context;
    ProcessContextStatus status{ProcessContextStatus::error};
    int error_number{};
    std::string operation;
};

struct ProcessExecutableMetadata {
    std::uint64_t device{}, inode{}, mode{}, owner_uid{}, owner_gid{};
    bool operator==(const ProcessExecutableMetadata&) const = default;
};

struct ProcessExecutableContext {
    ProcessStat stat;
    ProcessExecutableMetadata executable;
    Clock::time_point begin{}, end{};
};

struct ProcessExecutableContextResult {
    std::optional<ProcessExecutableContext> context;
    ProcessContextStatus status{ProcessContextStatus::error};
    int error_number{};
    std::string operation;
};

struct ProcessPinResult;

// A fresh observational candidate, never permission to act on a bulk row.
// PID/start_ticks remain tick-granular metadata: a successful bind does not
// establish continuity with an earlier sample or make credentials/exec atomic.
// The instance may exit immediately after binding. Signaling is private to the
// independently confirmed process-control boundary.
// One owner must serialize operations and moves of each individual handle.
class PinnedProcess {
public:
    static constexpr std::size_t maximum_retained = 64;
    static constexpr std::size_t descriptor_reserve = 64;
    static constexpr std::size_t temporary_descriptor_allowance = 8;

    // Only the native /proc is accepted. Admission uses the current descriptor
    // inventory and soft RLIMIT_NOFILE; unrelated opens can race that snapshot.
    static ProcessPinResult bind_fresh(std::uint64_t pid);

    ~PinnedProcess();
    PinnedProcess(const PinnedProcess&) = delete;
    PinnedProcess& operator=(const PinnedProcess&) = delete;
    PinnedProcess(PinnedProcess&& other) noexcept;
    PinnedProcess& operator=(PinnedProcess&& other) noexcept;

    bool valid() const noexcept { return proc_fd_ >= 0 && pid_fd_ >= 0; }
    const PinnedProcessMetadata& metadata() const noexcept { return metadata_; }
    // Nonblocking readiness only; this neither reaps nor reveals exit status.
    ProcessExitObservation poll_exit() const noexcept;
    // Fresh anchored stat/status/status/stat reads with pidfd exit checks.
    // Unusable dead-leader metadata is unavailable while its process pidfd runs.
    // Matching samples establish neither complete policy nor action eligibility:
    // credentials/exec can change between reads or after the final observation.
    ProcessCredentialContextResult observe_credentials() const;
    // Numeric file metadata only; file owners are distinct from process credentials.
    // Equal samples do not prove unchanged file contents, no intervening exec,
    // durable image identity or action eligibility. No executable path is read.
    ProcessExecutableContextResult observe_executable() const;
    // A fresh bounded read through the retained proc directory. Cgroup naming
    // and membership can change; this is never action authority or a group handle.
    ApplicationScopeObservation observe_application_scope() const;

private:
    friend class ProcessActionCandidate;
    int send_signal(int signal) const noexcept;
    PinnedProcess(int proc_fd, int pid_fd, PinnedProcessMetadata&& metadata) noexcept;
    void reset() noexcept;

    int proc_fd_{-1};
    int pid_fd_{-1};
    bool budget_slot_{};
    PinnedProcessMetadata metadata_;
};

struct ProcessPinResult {
    std::optional<PinnedProcess> process;
    ProcessPinStatus status{ProcessPinStatus::error};
    int error_number{}; // Native errno when available; zero for validation failures.
    std::string operation;
};
} // namespace night
