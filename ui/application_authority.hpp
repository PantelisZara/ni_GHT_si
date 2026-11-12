#pragma once

#include "ni_ght_si/process_control.hpp"

namespace night::ui {
// Invoke only on a dedicated action worker, never the presentation thread.
// Supports direct-exec installed desktop applications in exact transient scopes.
// All D-Bus errors, unsupported launchers/wrappers and ambiguous metadata deny.
// Rejected evidence carries a static public failure stage, without unit IDs,
// paths or D-Bus error payloads; the resolver never logs automatically.
// User-manager provenance requires broker ProcessFDs and root-system-manager
// attestation of the exact active user service. It is a managed-role proof, not
// a live executable hash or an atomic promise against later exec/role changes.
// Requires the normal trusted native session broker in the monitor PID namespace;
// a substituted/private malicious broker is outside this provenance claim.
// The process-control core must independently enforce self/ancestor, namespace,
// credential/capability and fresh-instance exclusions before using this result.
ApplicationAuthorityResolver makeApplicationAuthorityResolver();
} // namespace night::ui
