#include "ni_ght_si/io.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <cstdlib>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path root;
    std::string native_index{"2"};
    std::optional<night::Issue> native_issue;
    Fixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "ni-ght-io-XXXXXX").string();
        auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("cannot create test fixture");
        root = created;
        put("proc/sys/kernel/random/boot_id", "01234567-89ab-cdef-0123-456789abcdef\n");
        std::filesystem::create_directories(root / "proc/thread-self/ns");
        std::filesystem::create_symlink("net:[42]", root / "proc/thread-self/ns/net");
        // This mount deliberately exposes a different namespace's stable interface index.
        put("sys/class/net/eth0/ifindex", "99\n");
        put("sys/dev/block/8:0/diskseq", "7\n");
        counters(true, 100, 200);
        counters(false, 100, 200);
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    void put(const std::filesystem::path& name, std::string_view contents) {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream out(root / name); out << contents;
        if (!out) throw std::runtime_error("cannot write test fixture");
    }
    void counters(bool network, std::uint64_t read, std::uint64_t written) {
        if (network)
            put("proc/thread-self/net/dev", "eth0: " + std::to_string(read) + " 0 0 0 0 0 0 0 " +
                                std::to_string(written) + " 0 0 0 0 0 0 0\n");
        else
            put("proc/diskstats", "8 0 sda 1 0 " + std::to_string(read) + " 0 1 0 " +
                                  std::to_string(written) + " 0 0 0 0\n");
    }
    night::IoCollector::Readers readers() {
        night::IoCollector::Readers result;
        result.interface_index = [this](std::string_view name) {
            require(name == "eth0", "fixture native lookup uses the discovered interface name");
            return night::TextResult{native_index, native_issue};
        };
        return result;
    }
};
const night::Throughput& row(const night::IoSnapshot& sample, bool network) {
    const auto& rows = network ? sample.network : sample.disks;
    require(rows.size() == 1, "expected exactly one fixture counter row");
    return rows.front();
}
bool issue(const night::IoSnapshot& sample, const std::filesystem::path& source, night::Quality quality) {
    return std::any_of(sample.stamp.issues.begin(), sample.stamp.issues.end(), [&](const auto& value) {
        return value.source == source.string() && value.quality == quality;
    });
}
void unavailable(const night::Throughput& value, night::Quality quality) {
    require(!value.read_bytes_per_second.value && !value.written_bytes_per_second.value &&
            value.read_bytes_per_second.quality == quality && value.written_bytes_per_second.quality == quality,
            "unavailable I/O must preserve quality without fabricated values");
}
void ratio_two(const night::Throughput& value) {
    require(value.read_bytes_per_second.value && *value.read_bytes_per_second.value > 0 &&
            value.written_bytes_per_second.value &&
            std::abs(*value.written_bytes_per_second.value / *value.read_bytes_per_second.value - 2.0) < 1e-9,
            "baseline must use actual counters, with both rates from the same interval");
}
void replacement_test(bool network, bool during_discovery) {
    Fixture fixture;
    const auto counter_path = fixture.root / (network ? "proc/thread-self/net/dev" : "proc/diskstats");
    bool armed = false;
    unsigned reads = 0;
    unsigned sysfs_index_reads = 0;
    auto readers = fixture.readers();
    readers.file = [&](const auto& path, std::size_t limit) {
        if (path == fixture.root / "sys/class/net/eth0/ifindex") ++sysfs_index_reads;
        auto result = night::read_text(path, limit);
        if (armed && path == counter_path && ++reads == (during_discovery ? 1U : 2U)) {
            if (network) fixture.native_index = "9";
            else fixture.put("sys/dev/block/8:0/diskseq", "9\n");
            fixture.counters(network, 1000, 9000);
            armed = false;
        }
        return result;
    };
    night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
    collector.collect(network, !network);
    fixture.counters(network, 200, 400);
    armed = true;
    unavailable(row(collector.collect(network, !network), network), night::Quality::warming);
    if (!during_discovery) {
        // The mismatched read must not become the replacement's baseline.
        unavailable(row(collector.collect(network, !network), network), night::Quality::warming);
    }
    fixture.counters(network, 1100, 9200);
    ratio_two(row(collector.collect(network, !network), network));
    require(sysfs_index_reads == 0 && night::read_text(fixture.root / "sys/class/net/eth0/ifindex").text == "99\n",
            "local replacement must be detected while foreign sysfs identity stays unread and unchanged");
}
void disappearance_test(bool network) {
    Fixture fixture;
    const auto counter_path = fixture.root / (network ? "proc/thread-self/net/dev" : "proc/diskstats");
    bool armed = false;
    unsigned reads = 0;
    auto readers = fixture.readers();
    readers.file = [&](const auto& path, std::size_t limit) {
        auto result = night::read_text(path, limit);
        if (armed && path == counter_path && ++reads == 2) {
            if (network)
                fixture.native_issue = night::Issue{night::Quality::error, "if_nametoindex(eth0)", "No such device"};
            else std::filesystem::remove_all(fixture.root / "sys/dev/block/8:0");
            armed = false;
        }
        return result;
    };
    night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
    collector.collect(network, !network);
    armed = true;
    unavailable(row(collector.collect(network, !network), network), night::Quality::error);
    if (network) fixture.native_issue.reset();
    else fixture.put("sys/dev/block/8:0/diskseq", "7\n");
    fixture.counters(network, 1000, 2000);
    unavailable(row(collector.collect(network, !network), network), night::Quality::warming);
    fixture.counters(network, 1100, 2200);
    ratio_two(row(collector.collect(network, !network), network));
}
void identity_error_tests() {
    for (const auto quality : {night::Quality::permission_denied, night::Quality::error}) {
        for (const auto* name : {"proc/sys/kernel/random/boot_id", "if_nametoindex(eth0)", "sys/dev/block/8:0/diskseq"}) {
            Fixture fixture;
            const bool native = std::string_view{name} == "if_nametoindex(eth0)";
            const auto source = native ? std::filesystem::path{name} : fixture.root / name;
            const bool network = std::string_view{name} != "sys/dev/block/8:0/diskseq";
            bool blocked = false;
            auto readers = fixture.readers();
            readers.file = [&](const auto& path, std::size_t limit) {
                if (blocked && path == source)
                    return night::TextResult{{}, night::Issue{quality, path.string(), "injected identity read failure"}};
                return night::read_text(path, limit);
            };
            night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
            collector.collect(network, !network);
            blocked = true;
            if (native) fixture.native_issue = night::Issue{quality, source.string(), "injected native query failure"};
            const auto failed = collector.collect(network, !network);
            unavailable(row(failed, network), quality);
            require(issue(failed, source, quality), "identity failure retains source and error kind");
            blocked = false;
            fixture.native_issue.reset();
            unavailable(row(collector.collect(network, !network), network), night::Quality::warming);
        }
        Fixture fixture;
        const auto source = fixture.root / "proc/thread-self/ns/net";
        bool blocked = false;
        auto readers = fixture.readers();
        readers.link = [&](const auto& path) {
            if (blocked) return night::TextResult{{}, night::Issue{quality, path.string(), "injected namespace read failure"}};
            return night::TextResult{std::filesystem::read_symlink(path).string(), {}};
        };
        night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
        collector.collect(true, false);
        blocked = true;
        const auto failed = collector.collect(true, false);
        unavailable(row(failed, true), quality);
        require(issue(failed, source, quality), "namespace failure retains source and error kind");
        blocked = false;
        unavailable(row(collector.collect(true, false), true), night::Quality::warming);
    }
    for (const auto* name : {"proc/sys/kernel/random/boot_id", "if_nametoindex(eth0)", "sys/dev/block/8:0/diskseq"}) {
        const bool native = std::string_view{name} == "if_nametoindex(eth0)";
        for (const auto* malformed : {"", "invalid", "0"}) {
            Fixture fixture;
            const auto source = native ? std::filesystem::path{name} : fixture.root / name;
            const bool network = std::string_view{name} != "sys/dev/block/8:0/diskseq";
            if (native) fixture.native_index = malformed;
            else fixture.put(name, malformed);
            night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", fixture.readers()};
            const auto failed = collector.collect(network, !network);
            unavailable(row(failed, network), night::Quality::error);
            require(issue(failed, source, night::Quality::error), "malformed identity is a contextual error");
        }
        Fixture fixture;
        if (native) fixture.native_issue = night::Issue{night::Quality::error, name, "No such device"};
        else std::filesystem::remove(fixture.root / name);
        const bool network = std::string_view{name} != "sys/dev/block/8:0/diskseq";
        night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", fixture.readers()};
        unavailable(row(collector.collect(network, !network), network),
                    native ? night::Quality::error : night::Quality::unsupported);
    }
    for (const auto* malformed : {"", "net:[bad]", "net:[0]"}) {
        Fixture fixture;
        auto readers = fixture.readers();
        readers.link = [malformed](const auto&) { return night::TextResult{malformed, {}}; };
        night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
        const auto failed = collector.collect(true, false);
        unavailable(row(failed, true), night::Quality::error);
        require(issue(failed, fixture.root / "proc/thread-self/ns/net", night::Quality::error), "malformed namespace is contextual error");
    }
}
void global_change_tests() {
    for (bool change_boot : {true, false}) {
        Fixture fixture;
        bool armed = false;
        unsigned reads = 0;
        auto readers = fixture.readers();
        readers.file = [&](const auto& path, std::size_t limit) {
            auto result = night::read_text(path, limit);
            if (armed && path == fixture.root / "proc/thread-self/net/dev" && ++reads == 2) {
                if (change_boot) fixture.put("proc/sys/kernel/random/boot_id", "11234567-89ab-cdef-0123-456789abcdef\n");
                else {
                    std::filesystem::remove(fixture.root / "proc/thread-self/ns/net");
                    std::filesystem::create_symlink("net:[43]", fixture.root / "proc/thread-self/ns/net");
                }
                armed = false;
            }
            return result;
        };
        night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
        collector.collect(true, false);
        armed = true;
        unavailable(row(collector.collect(true, false), true), night::Quality::warming);
        unavailable(row(collector.collect(true, false), true), night::Quality::warming);
    }
}
void native_query_recovery_test() {
    Fixture fixture;
    unsigned sysfs_index_reads = 0;
    auto readers = fixture.readers();
    readers.file = [&](const auto& path, std::size_t limit) {
        if (path == fixture.root / "sys/class/net/eth0/ifindex") ++sysfs_index_reads;
        return night::read_text(path, limit);
    };
    night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", readers};
    collector.collect(true, false);
    require(row(collector.collect(true, false), true).read_bytes_per_second.value.has_value(),
            "native interface index establishes a stable baseline");
    fixture.native_issue = night::Issue{night::Quality::error, "if_nametoindex(eth0)", "No such device"};
    const auto failed = collector.collect(true, false);
    unavailable(row(failed, true), night::Quality::error);
    require(issue(failed, "if_nametoindex(eth0)", night::Quality::error),
            "native query failure retains its source while foreign sysfs index remains available");
    fixture.native_issue.reset();
    std::filesystem::remove(fixture.root / "proc/thread-self/ns/net");
    std::filesystem::create_symlink("net:[43]", fixture.root / "proc/thread-self/ns/net");
    fixture.counters(true, 1000, 9000);
    const auto recovered = collector.collect(true, false);
    unavailable(row(recovered, true), night::Quality::warming);
    require(row(recovered, true).identity.find("/net:[43]/2") != std::string::npos,
            "recovered native index is paired with the collector thread's current namespace");
    fixture.counters(true, 1100, 9200);
    ratio_two(row(collector.collect(true, false), true));
    require(sysfs_index_reads == 0 && night::read_text(fixture.root / "sys/class/net/eth0/ifindex").text == "99\n",
            "native query errors must never fall back to a foreign sysfs identity");
}
void partition_test() {
    Fixture fixture;
    fixture.put("proc/diskstats", "8 1 sda1 1 0 100 0 1 0 200 0 0 0 0\n");
    std::filesystem::create_directories(fixture.root / "sys/dev/block/8:1");
    fixture.put("sys/dev/block/diskseq", "7\n");
    fixture.put("sys/dev/block/8:1/partition", "1\n");
    night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", fixture.readers()};
    unavailable(row(collector.collect(false, true), false), night::Quality::unsupported);
    const auto sample = collector.collect(false, true);
    unavailable(row(sample, false), night::Quality::unsupported);
    // Recreated partition, same parent generation, replacement counters above prior.
    fixture.put("proc/diskstats", "8 1 sda1 1 0 10000 0 1 0 20000 0 0 0 0\n");
    unavailable(row(collector.collect(false, true), false), night::Quality::unsupported);
    fixture.put("sys/dev/block/8:1/diskseq", "8\n");
    unsigned fallback_reads = 0;
    auto readers = fixture.readers();
    readers.file = [&](const auto& path, std::size_t limit) {
        if (path == fixture.root / "sys/dev/block/8:1/partition")
            return night::TextResult{{}, night::Issue{night::Quality::permission_denied, path.string(), "denied"}};
        if (path == fixture.root / "sys/dev/block/8:1/../diskseq") ++fallback_reads;
        return night::read_text(path, limit);
    };
    night::IoCollector denied{fixture.root / "proc", fixture.root / "sys", readers};
    unavailable(row(denied.collect(false, true), false), night::Quality::permission_denied);
    require(fallback_reads == 0, "partition path must never substitute a parent generation");
}
void malformed_counter_tests() {
    std::vector<night::Issue> issues;
    const std::string net="eth0: 100 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0\n";
    require(night::parse_network(net+"bad: invalid\n", &issues, "fixture/net").size()==1 && !issues.empty() &&
            issues.front().source=="fixture/net", "mixed network parse retains contextual error");
    issues.clear();
    require(night::parse_network(net+net, &issues).empty() && !issues.empty(), "duplicate interface rows invalidate both");
    issues.clear();
    require(night::parse_network(net+"eth0: invalid\n", &issues).empty() && !issues.empty(), "malformed duplicate invalidates valid interface row");
    const std::string disk="8 0 sda 1 0 100 0 1 0 200 0 0 0 0\n";
    issues.clear();
    require(night::parse_disks(disk+"bad\n", &issues, "fixture/disks").size()==1 && !issues.empty() &&
            issues.front().source=="fixture/disks", "mixed disk parse retains contextual error");
    issues.clear();
    require(night::parse_disks(disk+"8 0 other-name 1 0 200 0 1 0 400 0 0 0 0\n", &issues).empty() &&
            !issues.empty(), "same block number cannot produce two valid counter rows");
    Fixture fixture;
    night::IoCollector collector{fixture.root/"proc", fixture.root/"sys", fixture.readers()};
    collector.collect(true,false);
    fixture.put("proc/thread-self/net/dev",net+net);
    const auto failed=collector.collect(true,false);
    require(failed.network.empty() && failed.stamp.quality==night::Quality::error, "all invalid source is a failed snapshot");
    fixture.put("proc/thread-self/net/dev",net);
    unavailable(row(collector.collect(true,false),true),night::Quality::warming);
}
void epoch_tests() {
    using namespace std::chrono_literals;
    Fixture fixture;
    std::optional<night::ClockObservation> clocks = night::ClockObservation{1s, 1s, 1s};
    night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", fixture.readers(), [&] { return clocks; }};
    collector.collect();
    require(row(collector.collect(), true).read_bytes_per_second.value.has_value(), "stable epoch preserves baseline");
    clocks = night::ClockObservation{2s, 4s, 2s};
    const auto resumed = collector.collect();
    unavailable(row(resumed, true), night::Quality::warming);
    unavailable(row(resumed, false), night::Quality::warming);
    require(row(collector.collect(), false).read_bytes_per_second.value.has_value(), "post-resume baseline becomes valid");
    clocks.reset();
    const auto unknown = collector.collect();
    unavailable(row(unknown, true), night::Quality::warming);
    require(issue(unknown, "clocks", night::Quality::error), "missing epoch clock is explicit");
    clocks = night::ClockObservation{3s, 5s, 3s};
    unavailable(row(collector.collect(), false), night::Quality::warming);
}
}
void io_tests() {
    const auto network = night::parse_network("Inter-| Receive | Transmit\n eth0: 1024 0 0 0 0 0 0 0 2048 0 0 0 0 0 0 0\n bad: invalid\n");
    require(network.size() == 1 && network[0].read == 1024 && network[0].written == 2048, "network parser field semantics");
    const auto disks = night::parse_disks("8 0 sda 10 0 100 5 20 0 200 7 0 1 1\nmalformed\n");
    require(disks.size() == 1 && disks[0].name == "8:0/sda" && disks[0].read == 100 && disks[0].written == 200, "disk sector field semantics");
    require(!night::rate(10, 5, 1) && !night::rate(1, 2, 0) && night::rate(10, 30, 2) == 10, "reset/elapsed rate semantics");
    Fixture fixture;
    night::IoCollector collector{fixture.root / "proc", fixture.root / "sys", fixture.readers()};
    auto first = collector.collect();
    require(first.network.size() == 1 && !first.network[0].read_bytes_per_second.value, "first network sample warms");
    fixture.put("proc/thread-self/net/dev", "eth0: 200 0 0 0 0 0 0 0 400 0 0 0 0 0 0 0\n");
    auto next = collector.collect();
    require(next.network[0].read_bytes_per_second.value && *next.network[0].read_bytes_per_second.value > 0, "network delta becomes valid");
    fixture.native_index = "3";
    require(!collector.collect().network[0].read_bytes_per_second.value, "interface replacement reseeds baseline");
    fixture.put("sys/dev/block/8:0/diskseq", "8\n");
    require(!collector.collect().disks[0].read_bytes_per_second.value, "disk replacement reseeds baseline");
    fixture.put("proc/thread-self/net/dev", "eth0: 1 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0\n");
    require(!collector.collect().network[0].read_bytes_per_second.value, "counter decrease does not underflow");
    collector.reset_baselines();
    unavailable(row(collector.collect(), true), night::Quality::warming);
    const auto skipped = collector.collect(true, false);
    require(skipped.disks.empty(), "skipped disk domain has no apparently fresh rows");
    require(row(collector.collect(false, true), false).read_bytes_per_second.value.has_value(), "skipped domain keeps baseline");
    fixture.put("proc/thread-self/net/dev", "");
    require(collector.collect(true, false).network.empty(), "disappearance removes absent records");
    fixture.counters(true, 100, 200);
    unavailable(row(collector.collect(true, false), true), night::Quality::warming);
    const auto capacity = night::collect_capacity({fixture.root, fixture.root / "missing"});
    require(capacity.mounts[0].total_bytes.value && capacity.mounts[0].available_bytes.value, "selected existing mount capacity available");
    require(!capacity.mounts[1].total_bytes.value && !capacity.stamp.issues.empty(), "missing mount is error, not zero");
    const auto limited = night::read_text(fixture.root / "proc/thread-self/net/dev", 1);
    require(limited.issue && limited.text.empty(), "oversized input rejected");
    for (bool network_domain : {true, false}) {
        replacement_test(network_domain, false);
        replacement_test(network_domain, true);
        disappearance_test(network_domain);
    }
    identity_error_tests();
    global_change_tests();
    native_query_recovery_test();
    partition_test();
    malformed_counter_tests();
    epoch_tests();
}
