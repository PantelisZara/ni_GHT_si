#include "ni_ght_si/application_scope.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void unsupported(std::string_view path) {
    const auto result = night::parse_application_scope_candidate(std::string{"0::"} + std::string{path} + "\n");
    require(!result.value && result.quality == night::Quality::unsupported, "ambiguous paths must not produce a scope candidate");
}
}
int main() {
    try {
        const std::string path = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-org.kde.konsole-42.scope";
        const auto parsed = night::parse_application_scope_candidate("0::" + path + "\n");
        require(parsed.quality == night::Quality::valid && parsed.value && parsed.value->cgroup_path == path &&
                parsed.value->unit_name == "app-org.kde.konsole-42.scope", "unified app-scope naming observation");
        for (const auto path_value : {"/", "/app.slice", "/app.slice/app-foo.service", "/session.slice/app-foo.scope",
                "/background.slice/app-foo.scope", "/system.slice/app.slice/app-foo.scope", "/app.slice/app-foo.scope/child",
                "/app.slice/app-foo.scope/", "/app.slice/app-foo.scope/../app-bar.scope", "/../app.slice/app-foo.scope",
                "/app.slice//app-foo.scope", "/app.slice/app-.scope", "/app.slice/app-foo\\oops.scope"}) unsupported(path_value);
        require(night::parse_application_scope_candidate("0::/app.slice/app-foo\\x2dbar.scope\n").value.has_value(),
                "systemd escape is kept literal, not decoded into an identity");
        require(night::parse_application_scope_candidate("0::/app.slice/app-foo.scope\n7:cpu:/old\n").value.has_value(),
                "hybrid input may have one unified record");
        require(night::parse_application_scope_candidate("7:cpu:/old\n").quality == night::Quality::unsupported,
                "v1-only has no supported unified candidate");
        require(night::parse_application_scope_candidate("00::/app.slice/app-foo.scope\n").value.has_value(),
                "hierarchy IDs are numeric: zero remains the unified hierarchy");
        for (const auto malformed : {"garbage", "0:cpu:/app.slice/app-foo.scope", "0::relative", "0::/app.slice/a\r.scope",
                "0::/app.slice/app-one.scope\n0::/app.slice/app-two.scope\n", "0::\n",
                "0::/app.slice/app-one.scope\n00::/app.slice/app-two.scope\n", "00:cpu:/app.slice/app-foo.scope"}) {
            const auto result = night::parse_application_scope_candidate(malformed);
            require(!result.value && result.quality == night::Quality::error, "malformed or duplicate input fails closed");
        }
        require(night::parse_application_scope_candidate(std::string(65537, 'x')).quality == night::Quality::error,
                "input bytes are bounded");
        std::cout << "PASS application scope candidates (naming only, no eligibility)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
