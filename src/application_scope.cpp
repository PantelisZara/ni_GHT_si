#include "ni_ght_si/application_scope.hpp"

namespace night {
namespace {
bool scope_unit(std::string_view unit) {
    if (unit.size() > 255 || !unit.starts_with("app-") || !unit.ends_with(".scope") || unit.size() <= 10)
        return false;
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    for (std::size_t i = 0; i < unit.size(); ++i) {
        const char c = unit[i];
        if (c == '\\') {
            if (i + 3 >= unit.size() || unit[i + 1] != 'x' || !hex(unit[i + 2]) || !hex(unit[i + 3])) return false;
            i += 3;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == ':' || c == '@')) return false;
    }
    return true;
}
}
Reading<ApplicationScopeCandidate> parse_application_scope_candidate(std::string_view text) {
    if (text.size() > 65536) return {{}, Quality::error};
    std::optional<std::string_view> unified;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        if (!line.empty()) {
            const auto first = line.find(':');
            const auto second = first == std::string_view::npos ? first : line.find(':', first + 1);
            if (first == std::string_view::npos || second == std::string_view::npos) return {{}, Quality::error};
            const auto hierarchy = unsigned_number(line.substr(0, first));
            if (!hierarchy) return {{}, Quality::error};
            if (*hierarchy == 0) {
                if (second != first + 1 || unified) return {{}, Quality::error};
                unified = line.substr(second + 1);
            }
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
    if (!unified) return {{}, Quality::unsupported};
    const auto path = *unified;
    if (path.empty() || path.front() != '/' || path.size() > 4096) return {{}, Quality::error};
    for (const auto c : path) if (static_cast<unsigned char>(c) < 32 || c == 127) return {{}, Quality::error};
    if (path == "/") return {{}, Quality::unsupported};
    std::string_view remaining = path.substr(1), previous;
    while (!remaining.empty()) {
        const auto slash = remaining.find('/');
        const auto component = remaining.substr(0, slash);
        if (component.empty() || component == "." || component == "..") return {{}, Quality::unsupported};
        if (component == "session.slice" || component == "background.slice" || component == "system.slice")
            return {{}, Quality::unsupported};
        if (slash == std::string_view::npos) {
            if (previous == "app.slice" && scope_unit(component))
                return {ApplicationScopeCandidate{std::string{path}, std::string{component}}, Quality::valid};
            return {{}, Quality::unsupported};
        }
        previous = component;
        remaining.remove_prefix(slash + 1);
    }
    return {{}, Quality::unsupported};
}
}
