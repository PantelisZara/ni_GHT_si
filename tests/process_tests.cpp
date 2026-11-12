#include "ni_ght_si/process.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string stat_text(std::string_view name = "worker", std::uint64_t start = 100,
                      std::uint64_t user = 20, std::uint64_t system = 5,
                      std::string_view rss = "7") {
    std::array<std::string, 22> fields;
    fields.fill("0");
    fields[0] = "S";
    fields[1] = "1";
    fields[11] = std::to_string(user);
    fields[12] = std::to_string(system);
    fields[17] = "3";
    fields[19] = std::to_string(start);
    fields[20] = "999999";
    fields[21] = rss;
    std::string result = "42 (" + std::string{name} + ")";
    for (const auto& field : fields) result += " " + field;
    return result + "\n";
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output{path, std::ios::binary};
    output << text;
    output.close();
    require(output.good(), "fixture file write failed");
}

class ProcFixture {
public:
    ProcFixture() {
        std::string pattern = (std::filesystem::temp_directory_path() / "night-process-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        require(directory != nullptr, "temporary proc directory creation failed");
        root = directory;
        try {
            std::filesystem::create_directories(root / "sys/kernel/random");
            std::filesystem::create_directories(root / "self/ns");
            write_file(root / "sys/kernel/random/boot_id", "boot-A\n");
            std::filesystem::create_symlink("pid:[fixture-A]", root / "self/ns/pid");
            process();
        } catch (...) {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            throw;
        }
    }
    ~ProcFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void process(std::uint64_t start = 100, std::uint64_t user = 20, std::uint64_t system = 5) const {
        std::filesystem::create_directories(root / "42");
        write_file(root / "42/stat", stat_text("worker ) (pool)", start, user, system));
        write_file(root / "42/status", "Name:\tworker\nUid:\t1000\t1001\t1002\t1003\n");
    }
    std::filesystem::path root;
};

const night::ProcessInfo& only_process(const night::ProcessSnapshot& snapshot) {
    require(snapshot.processes.size() == 1, "expected one process");
    return snapshot.processes.front();
}

bool has_issue_source(const night::ProcessSnapshot& snapshot, std::string_view source) {
    return std::any_of(snapshot.stamp.issues.begin(), snapshot.stamp.issues.end(),
                       [source](const auto& issue) { return issue.source == source; });
}

void process_read_scratch_tests() {
    using namespace night;
    ProcFixture fixture;
    std::filesystem::create_directories(fixture.root / "thread-self/ns");
    std::filesystem::create_symlink("cgroup:[123]", fixture.root / "thread-self/ns/cgroup");
    const std::array<std::uint64_t, 3> pids{42, 43, 44};
    const std::array<std::uint64_t, 3> uids{1000, 2, 4000000000ULL};
    const std::array<std::string, 3> names{std::string(8192, 'L'), "s", std::string(233, 'm')};
    const std::array<std::string, 3> units{
        "app-" + std::string(220, 'a') + ".scope", "app-s.scope", "app-medium.scope"};
    const auto status_text = [&](std::size_t i) {
        const auto uid = std::to_string(uids[i]);
        return "Name:\t" + names[i] + "\nUid:\t" + uid + '\t' + uid + '\t' + uid + '\t' + uid + '\n';
    };
    const auto scope_text = [&](std::size_t i) {
        return (i == 0 ? "2:cpu:/" + std::string(6000, 'x') + '\n' : std::string{})
            + "0::/user.slice/app.slice/" + units[i] + '\n';
    };
    for (std::size_t i = 0; i < pids.size(); ++i) {
        const auto directory = fixture.root / std::to_string(pids[i]);
        std::filesystem::create_directories(directory);
        auto stat = stat_text(names[i]);
        stat.replace(0, 2, std::to_string(pids[i]));
        write_file(directory / "stat", stat);
        write_file(directory / "status", status_text(i));
        write_file(directory / "cgroup", scope_text(i));
    }
    const auto check_fields = [&](const ProcessSnapshot& sample) {
        for (const auto& value : sample.processes) {
            const auto found = std::find(pids.begin(), pids.end(), value.pid);
            require(found != pids.end(), "scratch reuse fabricated a process identity");
            const auto i = static_cast<std::size_t>(found - pids.begin());
            require(value.name == names[i] && value.uid == uids[i],
                    "long or short stat/status content leaked between processes");
            if (value.application_scope.quality == Quality::valid)
                require(value.application_scope.value && value.application_scope.value->unit_name == units[i],
                        "long or short cgroup content leaked between processes");
        }
    };
    const auto check_valid = [&](const ProcessSnapshot& sample) {
        require(sample.processes.size() == pids.size() && sample.stamp.issues.empty(),
                "scratch contents or read issues leaked into a valid process");
        check_fields(sample);
        for (const auto& value : sample.processes)
            require(value.application_scope.quality == Quality::valid,
                    "valid per-process scope was lost after scratch reuse");
    };
    ProcessCollector collector{fixture.root};
    const auto original = collector.collect(true);
    check_valid(original);
    for (std::size_t i = 0; i < pids.size(); ++i) {
        const auto path = fixture.root / std::to_string(pids[i]) / "status";
        std::filesystem::remove(path);
        const auto failed = collector.collect(true);
        require(failed.processes.size() == pids.size() - 1 && failed.stamp.issues.size() == 1 &&
                    failed.stamp.issues.front().source == "proc/" + std::to_string(pids[i]) + "/status" &&
                    std::none_of(failed.processes.begin(), failed.processes.end(),
                                 [&](const auto& value) { return value.pid == pids[i]; }),
                "missing status reused previous text or contaminated another process read");
        check_fields(failed);
        write_file(path, status_text(i));
        check_valid(collector.collect(true));
    }
    const auto unreadable = fixture.root / "43/status";
    std::filesystem::remove(unreadable);
    std::filesystem::create_directory(unreadable);
    const auto read_failed = collector.collect(true);
    require(read_failed.processes.size() == pids.size() - 1 && read_failed.stamp.issues.size() == 1 &&
                has_issue_source(read_failed, "proc/43/status"),
            "read failure reused stale status text or leaked its issue to another process");
    check_fields(read_failed);
    std::filesystem::remove(unreadable);
    write_file(unreadable, status_text(1));
    check_valid(collector.collect(true));
    std::filesystem::remove(fixture.root / "43/cgroup");
    const auto scope_failed = collector.collect(true);
    require(scope_failed.processes.size() == pids.size() && scope_failed.stamp.issues.size() == 1 &&
                has_issue_source(scope_failed, "proc/43/cgroup"), "scope read failure escaped its own field");
    check_fields(scope_failed);
    for (const auto& value : scope_failed.processes)
        require(value.application_scope.quality == (value.pid == 43 ? Quality::error : Quality::valid),
                "scope issue leaked between process scratch reads");
    const auto disabled = collector.collect();
    require(disabled.processes.size() == pids.size() && disabled.stamp.issues.empty(),
            "disabled scopes retained a previous cgroup read failure");
    check_fields(disabled);
    for (const auto& value : disabled.processes)
        require(value.application_scope.quality == Quality::unsupported && !value.application_scope.value,
                "disabled scopes retained previous cgroup text");
    write_file(fixture.root / "43/cgroup", scope_text(1));
    check_valid(collector.collect(true));
    check_valid(original);
}

void process_read_limit_tests() {
    using namespace night;
    ProcFixture fixture;
    std::filesystem::create_directories(fixture.root / "thread-self/ns");
    std::filesystem::create_symlink("cgroup:[123]", fixture.root / "thread-self/ns/cgroup");
    std::filesystem::create_directories(fixture.root / "43");
    auto other_stat = stat_text("other");
    other_stat.replace(0, 2, "43");
    write_file(fixture.root / "43/stat", other_stat);
    write_file(fixture.root / "43/status", "Uid: 2000 2000 2000 2000\n");
    write_file(fixture.root / "43/cgroup", "0::/app.slice/app-other.scope\n");
    constexpr std::string_view membership = "0::/app.slice/app-worker.scope\n";
    write_file(fixture.root / "42/cgroup", membership);
    const std::array<std::string, 3> files{"stat", "status", "cgroup"};
    const std::array<std::string, 3> contents{stat_text("worker ) (pool)"),
        "Uid: 1000 1001 1002 1003\n", std::string{membership}};
    ProcessCollector collector{fixture.root};
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto path = fixture.root / "42" / files[i];
        auto exact = contents[i];
        exact.resize(65536, '\n');
        write_file(path, exact);
        const auto at_limit = collector.collect(true);
        require(at_limit.processes.size() == 2 && at_limit.stamp.issues.empty(),
                "exactly 64 KiB process field was rejected or contaminated a neighboring read");
        write_file(path, exact + 'x');
        const auto oversized = collector.collect(true);
        require(oversized.processes.size() == (files[i] == "cgroup" ? 2U : 1U) &&
                    oversized.stamp.issues.size() == (files[i] == "stat" ? 2U : 1U),
                "over-limit process field retained usable partial text or affected another process");
        for (const auto& issue : oversized.stamp.issues)
            require(issue.source == "proc/42/" + files[i] && issue.quality == Quality::error &&
                        issue.message == "process input exceeds byte limit", "over-limit read diagnostics changed");
        for (const auto& value : oversized.processes) {
            if (value.pid == 42)
                require(value.application_scope.quality == Quality::error && !value.application_scope.value,
                        "over-limit cgroup retained a partial scope candidate");
            else
                require(value.pid == 43 && value.name == "other" && value.uid == 2000 &&
                            value.application_scope.quality == Quality::valid && value.application_scope.value &&
                            value.application_scope.value->unit_name == "app-other.scope",
                        "over-limit text or issue leaked into the neighboring process");
        }
        write_file(path, contents[i]);
        const auto recovered = collector.collect(true);
        require(recovered.processes.size() == 2 && recovered.stamp.issues.empty(),
                "short process field did not recover after an over-limit read");
    }
}

void stat_tokenization_tests() {
    using night::parse_process_stat;
    auto without_rss = stat_text();
    without_rss.erase(without_rss.rfind(' '));
    require(!parse_process_stat(without_rss) && !parse_process_stat(without_rss + " \t\r\n"),
            "missing 22nd stat token accepted with or without trailing delimiters");

    auto at_eof = stat_text();
    at_eof.pop_back();
    const auto eof = parse_process_stat(at_eof);
    require(eof && eof->rss_pages == 7, "complete RSS token at EOF rejected");
    const auto delimited = parse_process_stat(at_eof + " \t\r\n");
    require(delimited && delimited->rss_pages == 7,
            "RSS followed by trailing delimiters rejected");
    const auto extended = parse_process_stat(at_eof + " \tignored +garbage -1\v\f");
    require(extended && extended->rss_pages == 7,
            "ignored trailing stat tokens were validated");
    for (const auto rss : {"7junk", "+7", "7\v", "7\f", "-9223372036854775809"})
        require(!parse_process_stat(stat_text("worker", 100, 20, 5, rss)),
                "RSS accepted a numeric prefix or an invalid full signed token");
    const auto negative = parse_process_stat(stat_text("worker", 100, 20, 5, "-7"));
    require(negative && negative->rss_pages == -7, "signed RSS parsing changed before byte validation");

    constexpr std::string_view name = " worker (\n) ((pool)) ";
    const auto named = parse_process_stat(stat_text(name));
    require(named && named->name == name && named->state == 'S' && named->rss_pages == 7,
            "spaces, newline or multiple parentheses changed comm or numeric field offsets");

    const auto base = stat_text();
    const auto right = base.rfind(')');
    for (const auto separator : {" ", "\t", "\r", "\n", " \t\r\n"}) {
        std::string separated = " \t\r\n" + base.substr(0, right + 2);
        for (const auto character : std::string_view{base}.substr(right + 2)) {
            if (character == ' ') separated += separator;
            else separated += character;
        }
        const auto parsed = parse_process_stat(separated);
        require(parsed && parsed->pid == 42 && parsed->parent_pid == 1 && parsed->rss_pages == 7,
                "stat token delimiters or leading PID trim changed");
    }
    for (const auto separator : {'\t', '\r', '\n', '\v', '\f'}) {
        auto replaced = base;
        replaced[right + 1] = separator;
        require(!parse_process_stat(replaced), "non-space immediately after comm was accepted");
    }
    auto joined = base;
    joined.erase(2, 1);
    require(parse_process_stat(joined).has_value(), "PID immediately followed by comm was rejected");
    require(!parse_process_stat(base + " ignored) junk"),
            "closing parenthesis in trailing fields no longer determines comm boundary");
    const auto repeated = parse_process_stat(base + " ignored)" + base.substr(right + 1));
    require(repeated && repeated->name == base.substr(base.find('(') + 1) + " ignored"
                && repeated->rss_pages == 7,
            "last closing parenthesis behavior changed when a complete field tail follows it");
}

void application_scope_collection_tests() {
    using namespace night;
    ProcFixture fixture;
    ProcessCollector collector{fixture.root};
    // Default collection must not inspect either the absent namespace or the
    // malformed optional field, and must not retain a previous scope candidate.
    write_file(fixture.root / "42/cgroup", "malformed optional field\n");
    const auto disabled = collector.collect();
    require(only_process(disabled).application_scope.quality == Quality::unsupported &&
            !only_process(disabled).application_scope.value && disabled.cgroup_namespace.empty() &&
            !has_issue_source(disabled, "proc/42/cgroup") &&
            !has_issue_source(disabled, "proc thread cgroup namespace"),
            "disabled scope collection inspected optional sources");

    std::filesystem::create_directories(fixture.root / "thread-self/ns");
    const auto namespace_path = fixture.root / "thread-self/ns/cgroup";
    std::filesystem::create_symlink("cgroup:[123]", namespace_path);
    constexpr std::string_view membership = "0::/user.slice/user-1000.slice/app.slice/app-worker.scope\n";
    write_file(fixture.root / "42/cgroup", membership);
    const auto enabled = collector.collect(true);
    const auto& candidate = only_process(enabled).application_scope;
    require(enabled.cgroup_namespace == "cgroup:[123]" && candidate.quality == Quality::valid && candidate.value &&
            candidate.value->unit_name == "app-worker.scope" &&
            candidate.value->cgroup_path == "/user.slice/user-1000.slice/app.slice/app-worker.scope",
            "anchored cgroup naming observation or namespace missing");
    require(only_process(enabled).uid == 1000 && only_process(enabled).cpu_percent.quality == Quality::valid &&
            only_process(enabled).rss_bytes.quality == Quality::valid && enabled.stamp.quality == Quality::valid,
            "optional scope metadata changed existing process fields");
    require(only_process(collector.collect()).application_scope.quality == Quality::unsupported,
            "disabled scope collection retained a previous candidate");

    write_file(fixture.root / "42/cgroup", "2:cpu:/legacy\n");
    const auto unsupported = collector.collect(true);
    require(only_process(unsupported).application_scope.quality == Quality::unsupported &&
            !only_process(unsupported).application_scope.value && unsupported.cgroup_namespace == "cgroup:[123]",
            "unsupported cgroup hierarchy fabricated a scope");
    for (const auto& records : {std::string{"broken\n"}, std::string{membership} + std::string{membership},
                                std::string(65537, 'x')}) {
        write_file(fixture.root / "42/cgroup", records);
        const auto invalid = collector.collect(true);
        require(only_process(invalid).application_scope.quality == Quality::error &&
                !only_process(invalid).application_scope.value && has_issue_source(invalid, "proc/42/cgroup") &&
                only_process(invalid).rss_bytes.quality == Quality::valid && invalid.stamp.quality == Quality::valid,
                "invalid, duplicate or oversized cgroup metadata damaged process fields or fabricated a scope");
    }
    std::filesystem::remove(fixture.root / "42/cgroup");
    const auto missing_field = collector.collect(true);
    require(only_process(missing_field).application_scope.quality == Quality::error &&
            has_issue_source(missing_field, "proc/42/cgroup") && missing_field.cgroup_namespace == "cgroup:[123]",
            "missing optional cgroup field not reported");
    std::filesystem::create_symlink(fixture.root / "42/status", fixture.root / "42/cgroup");
    const auto symlink_field = collector.collect(true);
    require(only_process(symlink_field).application_scope.quality == Quality::error &&
            has_issue_source(symlink_field, "proc/42/cgroup"), "cgroup field followed a replacement symlink");
    std::filesystem::remove(fixture.root / "42/cgroup");
    write_file(fixture.root / "42/cgroup", membership);
    std::filesystem::remove(namespace_path);
    const auto missing_namespace = collector.collect(true);
    require(missing_namespace.cgroup_namespace.empty() &&
            only_process(missing_namespace).application_scope.quality == Quality::error &&
            !only_process(missing_namespace).application_scope.value &&
            has_issue_source(missing_namespace, "proc thread cgroup namespace") &&
            only_process(missing_namespace).cpu_percent.quality == Quality::valid &&
            missing_namespace.stamp.quality == Quality::valid,
            "missing cgroup namespace fabricated a candidate or invalidated process fields");
    write_file(fixture.root / "42/cgroup", "0::/\n");
    const auto unsupported_without_namespace = collector.collect(true);
    require(only_process(unsupported_without_namespace).application_scope.quality == Quality::error,
            "missing context must not masquerade as a successfully unclassified process");
    write_file(fixture.root / "42/cgroup", membership);
    for (const auto invalid_namespace : {"cgroup:[0]", "cgroup:[18446744073709551616]", "cgroup:[-2]",
                                         "pid:[123]", "cgroup:[123]extra"}) {
        std::filesystem::create_symlink(invalid_namespace, namespace_path);
        const auto invalid = collector.collect(true);
        require(invalid.cgroup_namespace.empty() && !only_process(invalid).application_scope.value &&
                only_process(invalid).application_scope.quality == Quality::error &&
                has_issue_source(invalid, "proc thread cgroup namespace"),
                "malformed cgroup namespace identity accepted");
        std::filesystem::remove(namespace_path);
    }
    std::filesystem::create_symlink("cgroup:[456]", namespace_path);
    write_file(fixture.root / "42/cgroup", "0::/user.slice/app.slice/app-replacement.scope\n");
    const auto replacement = collector.collect(true);
    require(replacement.cgroup_namespace == "cgroup:[456]" &&
            only_process(replacement).application_scope.value &&
            only_process(replacement).application_scope.value->unit_name == "app-replacement.scope" &&
            only_process(replacement).cpu_percent.quality == Quality::valid,
            "scope or namespace replacement retained stale metadata or reset unrelated CPU baseline");
}
} // namespace

void process_tests() {
    using namespace night;
    process_read_scratch_tests();
    process_read_limit_tests();
    stat_tokenization_tests();
    application_scope_collection_tests();
    const auto parsed = parse_process_stat(stat_text("worker ) (pool)"));
    require(parsed.has_value(), "parenthesized stat name rejected");
    require(parsed->name == "worker ) (pool)" && parsed->pid == 42 && parsed->parent_pid == 1,
            "stat name or PID fields shifted");
    require(parsed->start_ticks == 100 && parsed->threads == 3 && parsed->state == 'S' &&
            parsed->user_ticks == 20 && parsed->system_ticks == 5 && parsed->rss_pages == 7,
            "stat numeric field offsets incorrect");
    require(parse_process_stat(stat_text("spaces inside name") + " 123 456").has_value(),
            "future stat fields rejected");
    require(parse_process_stat(stat_text(std::string{"bad\xffname", 8})).has_value(),
            "non-UTF8 comm should remain observational bytes");
    require(!parse_process_stat("42 (truncated) S 1"), "truncated stat accepted");
    require(!parse_process_stat("42 missing-parentheses S 1"), "malformed comm accepted");
    auto overflowing_pid = stat_text();
    overflowing_pid.replace(0, 2, "18446744073709551616");
    require(!parse_process_stat(overflowing_pid), "overflowing PID accepted");
    require(!parse_process_stat(stat_text("worker", 100, 20, 5, "not-a-page-count")), "malformed RSS accepted");
    require(!parse_process_stat(stat_text("worker", 100, 20, 5, "9223372036854775808")),
            "overflowing signed RSS accepted");
    require(parse_process_uid("Name:\ttest\nUid:\t1000\t1001\t1002\t1003\n") == 1000,
            "real numeric UID not parsed");
    require(!parse_process_uid("Name: Uid: 1000\n"), "embedded UID label accepted");
    require(!parse_process_uid("Uid: 1000 broken 1000 1000\n"), "malformed UID accepted");
    require(!parse_process_uid("Uid: -1 0 0 0\n"), "negative UID accepted");
    require(!parse_process_uid("Uid: 1000 1000\n"), "truncated UID accepted");

    const auto rss = process_rss_bytes(7, 16384);
    require(rss.value == 114688 && rss.quality == Quality::valid, "RSS uses wrong page units");
    require(process_rss_bytes(0, 4096).value == 0, "valid zero RSS rejected");
    require(!process_rss_bytes(-1, 4096).value, "negative RSS fabricated a value");
    require(!process_rss_bytes(1, 0).value, "zero page size accepted");
    require(!process_rss_bytes(std::numeric_limits<std::int64_t>::max(), 4096).value,
            "RSS multiplication overflow accepted");

    const auto before = *parsed;
    auto after = before;
    after.user_ticks += 150;
    after.system_ticks += 50;
    const auto cpu = process_cpu_percent(before, after, 0.5, 100);
    require(cpu.quality == Quality::valid && cpu.value && std::abs(*cpu.value - 400.0) < 1e-9,
            "CPU must use one-core denominator and allow over 100 percent");
    require(process_cpu_percent(before, before, 1, 100).value == 0.0, "real zero CPU delta rejected");
    require(process_cpu_percent({}, after, 1, 100).quality == Quality::warming,
            "missing baseline should warm");
    require(!process_cpu_percent({}, after, 1, 100).value, "missing CPU baseline invented zero");
    require(!process_cpu_percent(before, after, 0, 100).value, "zero time accepted");
    require(!process_cpu_percent(before, after, -1, 100).value, "negative time accepted");
    require(!process_cpu_percent(before, after, std::numeric_limits<double>::infinity(), 100).value,
            "infinite time accepted");
    require(!process_cpu_percent(before, after, 1, 0).value, "zero clock tick rate accepted");
    require(process_cpu_percent(before, after, 1, 100, false).quality == Quality::warming,
            "changed boot or namespace context reused a CPU baseline");
    after.start_ticks++;
    require(!process_cpu_percent(before, after, 1, 100).value, "recycled PID reused a CPU baseline");
    after = before;
    after.pid++;
    require(!process_cpu_percent(before, after, 1, 100).value, "different PID reused a CPU baseline");
    after = before;
    after.user_ticks--;
    after.system_ticks += 100;
    require(!process_cpu_percent(before, after, 1, 100).value,
            "counter reset hidden by growth in another counter");
    after = before;
    after.user_ticks = std::numeric_limits<std::uint64_t>::max();
    after.system_ticks = 1;
    require(process_cpu_percent(before, after, 1, 100).quality == Quality::error,
            "CPU tick sum overflow accepted");

    ProcFixture fixture;
    // Non-numeric entries and numeric symlinks must not create extra records.
    std::filesystem::create_directory(fixture.root / "not-a-pid");
    std::filesystem::create_directory_symlink(fixture.root / "42", fixture.root / "99");
    ProcessCollector collector{fixture.root};
    const auto first = collector.collect();
    const auto& initial = only_process(first);
    require(first.stamp.sequence == 1 && first.stamp.end >= first.stamp.begin, "invalid collection stamp");
    require(first.boot_id == "boot-A" && first.pid_namespace == "pid:[fixture-A]", "missing proc identity context");
    require(initial.uid == 1000 && initial.name == "worker ) (pool)" && initial.threads == 3,
            "status and stat metadata were not joined");
    require(!initial.cpu_percent.value && initial.cpu_percent.quality == Quality::warming,
            "initial collector CPU must warm");
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    require(page_size > 0 && initial.rss_bytes.value == static_cast<std::uint64_t>(page_size) * 7,
            "collector ignored runtime page size");
    fixture.process(100, 25, 10);
    const auto second = collector.collect();
    require(second.stamp.sequence == 2 && only_process(second).cpu_percent.quality == Quality::valid,
            "second sample failed to calculate CPU");
    fixture.process(101, 25, 10);
    require(only_process(collector.collect()).cpu_percent.quality == Quality::warming,
            "reused PID did not reset collector baseline");
    fixture.process(101, 1, 1);
    require(only_process(collector.collect()).cpu_percent.quality == Quality::warming,
            "counter reset did not reseed collector");
    require(only_process(collector.collect()).cpu_percent.quality == Quality::valid,
            "counter reset did not recover on next sample");
    std::filesystem::remove_all(fixture.root / "42");
    require(collector.collect().processes.empty(), "disappeared process retained in snapshot");
    fixture.process(101, 1, 1);
    require(only_process(collector.collect()).cpu_percent.quality == Quality::warming,
            "disappeared process retained in baseline cache");
    write_file(fixture.root / "sys/kernel/random/boot_id", "boot-B\n");
    require(only_process(collector.collect()).cpu_percent.quality == Quality::warming,
            "boot change retained CPU baseline");
    std::filesystem::remove(fixture.root / "self/ns/pid");
    std::filesystem::create_symlink("pid:[fixture-B]", fixture.root / "self/ns/pid");
    require(only_process(collector.collect()).cpu_percent.quality == Quality::warming,
            "namespace change retained CPU baseline");
    write_file(fixture.root / "42/status", "Name: no owner\n");
    const auto denied_metadata = collector.collect();
    require(denied_metadata.processes.empty() && !denied_metadata.stamp.issues.empty(),
            "missing UID fabricated metadata");
    fixture.process(101, 1, 1);
    require(only_process(collector.collect()).cpu_percent.quality == Quality::warming,
            "invalid metadata did not prune baseline");
    write_file(fixture.root / "42/stat", "42 (broken) S\n");
    const auto malformed = collector.collect();
    require(malformed.processes.empty() && !malformed.stamp.issues.empty(), "malformed stat not reported");
    fixture.process();
    std::filesystem::remove(fixture.root / "self/ns/pid");
    const auto missing_context = collector.collect();
    require(only_process(missing_context).cpu_percent.quality == Quality::error &&
            !only_process(missing_context).cpu_percent.value && !missing_context.stamp.issues.empty(),
            "missing identity context fabricated CPU");
    ProcessCollector unavailable{fixture.root / "does-not-exist"};
    const auto missing_root = unavailable.collect();
    require(missing_root.processes.empty() && !missing_root.stamp.issues.empty() &&
            missing_root.stamp.end >= missing_root.stamp.begin && missing_root.stamp.quality==Quality::error &&
            freshness(missing_root.stamp,Clock::now(),std::chrono::seconds{1})==Quality::error,
            "unavailable proc root must not be a healthy empty snapshot");
    using namespace std::chrono_literals;
    ClockObservation clocks{1s,1s,1s};
    std::filesystem::create_symlink("pid:[fixture-B]",fixture.root/"self/ns/pid");
    ProcessCollector resumed{fixture.root,[&] { return std::optional{clocks}; }};
    resumed.collect(); clocks={2s,2s,2s};
    require(only_process(resumed.collect()).cpu_percent.quality==Quality::valid,"process interval before suspend");
    clocks={3s,13s,3s};
    require(only_process(resumed.collect()).cpu_percent.quality==Quality::warming,"process suspend invalidates baseline");
    clocks={4s,14s,4s};
    require(only_process(resumed.collect()).cpu_percent.quality==Quality::valid,"process new interval after suspend");
}
