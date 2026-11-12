#pragma once
#include "ni_ght_si/common.hpp"

namespace night {
// A naming observation only. It establishes neither manager ownership, complete
// membership, namespace equivalence, application identity nor action eligibility.
struct ApplicationScopeCandidate {
    std::string cgroup_path;
    std::string unit_name;
};

Reading<ApplicationScopeCandidate> parse_application_scope_candidate(std::string_view cgroup);

struct ApplicationScopeObservation {
    Stamp stamp;
    Reading<ApplicationScopeCandidate> candidate;
};
}
