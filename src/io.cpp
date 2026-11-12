#include "ni_ght_si/io.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <net/if.h>
#include <sstream>
#include <set>
#include <cctype>
#include <sys/statvfs.h>

namespace night {
namespace {
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string{text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1)};
}
std::optional<std::uint64_t> multiply(std::uint64_t a, std::uint64_t b) {
    if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b) return {};
    return a * b;
}
bool absent(const std::filesystem::path& path) {
    std::error_code ec;
    const auto present = std::filesystem::exists(path, ec);
    return !present && (!ec || ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory);
}
TextResult read_io_file(const std::filesystem::path& path, std::size_t limit) {
    if (absent(path)) return {{}, Issue{Quality::unsupported, path.string(), "source is absent"}};
    return read_text(path, limit);
}
TextResult read_io_link(const std::filesystem::path& path) {
    TextResult result;
    std::error_code ec;
    result.text = std::filesystem::read_symlink(path, ec).string();
    if (ec) {
        auto quality = Quality::error;
        if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted)
            quality = Quality::permission_denied;
        else if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory)
            quality = Quality::unsupported;
        result.issue = Issue{quality, path.string(), ec.message()};
    }
    return result;
}
TextResult read_interface_index(std::string_view name) {
    TextResult result;
    const auto source = "if_nametoindex(" + std::string{name} + ")";
    if (name.empty() || name.size() >= IF_NAMESIZE || name.find('\0') != std::string_view::npos) {
        result.issue = Issue{Quality::error, source, "invalid interface name"};
        return result;
    }
    const std::string terminated{name};
    errno = 0;
    const auto index = ::if_nametoindex(terminated.c_str());
    const auto error = errno;
    if (index == 0) {
        const auto quality = error == EACCES || error == EPERM ? Quality::permission_denied : Quality::error;
        result.issue = Issue{quality, source, error ? std::strerror(error) : "interface index query returned zero"};
    } else result.text = std::to_string(index);
    return result;
}
bool boot_id(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') ||
                     (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
}
TextResult checked_identity(TextResult result, const std::filesystem::path& source,
                            std::string_view kind) {
    if (result.issue) {
        result.text.clear();
        result.issue->source = source.string();
        return result;
    }
    result.text = trim(result.text);
    bool valid = false;
    if (kind == "boot ID") valid = boot_id(result.text);
    else if (kind == "network namespace") {
        const auto value = std::string_view{result.text};
        if (value.starts_with("net:[") && value.ends_with(']')) {
            const auto id = unsigned_number(value.substr(5, value.size() - 6));
            valid = id && *id != 0;
        }
    } else {
        const auto value = unsigned_number(result.text);
        valid = value && *value != 0 && (kind != "interface index" || *value <= INT_MAX);
        if (valid) result.text = std::to_string(*value);
    }
    if (!valid) {
        result.text.clear();
        result.issue = Issue{Quality::error, source.string(), "empty or malformed " + std::string{kind}};
    }
    return result;
}
Quality identity_quality(std::initializer_list<const TextResult*> identities) {
    auto quality = Quality::warming;
    for (const auto* identity : identities) {
        if (!identity->issue) continue;
        const auto failure = identity->issue->quality;
        if (failure == Quality::permission_denied) return failure;
        if (failure == Quality::error || quality == Quality::warming) quality = failure;
    }
    return quality;
}
}

namespace {
void parser_issue(std::vector<Issue>* issues, std::string_view source, const std::string& message) {
    if (issues) issues->push_back({Quality::error, std::string{source}, message});
}
void add_counter(std::map<std::string, IoCounters>& rows, std::set<std::string>& duplicates,
                 std::string key, IoCounters counter, std::vector<Issue>* issues, std::string_view source) {
    if (duplicates.contains(key) || rows.contains(key)) {
        rows.erase(key); duplicates.insert(key);
        parser_issue(issues, source, "duplicate counter identity: " + key);
    } else rows.emplace(std::move(key), std::move(counter));
}
std::vector<IoCounters> counter_rows(std::map<std::string, IoCounters> rows) {
    std::vector<IoCounters> result;
    for (auto& [key, row] : rows) result.push_back(std::move(row));
    return result;
}
}

std::vector<IoCounters> parse_network(std::string_view text, std::vector<Issue>* issues, std::string_view source) {
    std::istringstream stream{std::string{text}};
    std::string line;
    std::map<std::string, IoCounters> rows;
    std::set<std::string> duplicates;
    while (std::getline(stream, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            if (!trim(line).empty() && line.find('|') == std::string::npos)
                parser_issue(issues, source, "malformed interface counter row");
            continue;
        }
        const auto fields = words(std::string_view{line}.substr(colon + 1));
        const auto name = trim(std::string_view{line}.substr(0, colon));
        bool valid = fields.size() >= 16 && !name.empty() && name.size() < 16 && name != "." && name != ".." &&
                     name.find('/') == std::string::npos &&
                     std::none_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); });
        for (const auto field : fields) if (!unsigned_number(field)) valid = false;
        if (!valid) {
            rows.erase(name); duplicates.insert(name);
            parser_issue(issues, source, "malformed interface counters: " + name); continue;
        }
        const auto rx = unsigned_number(fields[0]), tx = unsigned_number(fields[8]);
        if (rx && tx) add_counter(rows, duplicates, name, {name, *rx, *tx}, issues, source);
    }
    return counter_rows(std::move(rows));
}

std::vector<IoCounters> parse_disks(std::string_view text, std::vector<Issue>* issues, std::string_view source) {
    std::istringstream stream{std::string{text}};
    std::string line;
    std::map<std::string, IoCounters> rows;
    std::set<std::string> duplicates;
    while (std::getline(stream, line)) {
        const auto fields = words(line);
        if (fields.empty()) continue;
        if (fields.size() < 14) {
            if (fields.size() >= 2) {
                const auto a=unsigned_number(fields[0]), b=unsigned_number(fields[1]);
                if (a && b) { const auto key=std::to_string(*a)+":"+std::to_string(*b); rows.erase(key);duplicates.insert(key); }
            }
            parser_issue(issues, source, "malformed block counter row"); continue;
        }
        const auto major = unsigned_number(fields[0]), minor = unsigned_number(fields[1]);
        const auto read = unsigned_number(fields[5]), written = unsigned_number(fields[9]);
        bool valid = major && minor && read && written;
        for (std::size_t i=3; i<fields.size(); ++i) if (!unsigned_number(fields[i])) valid=false;
        if (!valid) {
            if (major && minor) { const auto key=std::to_string(*major)+":"+std::to_string(*minor); rows.erase(key);duplicates.insert(key); }
            parser_issue(issues, source, "malformed block counters"); continue;
        }
        const auto key = std::to_string(*major) + ":" + std::to_string(*minor);
        add_counter(rows, duplicates, key, {key + "/" + std::string{fields[2]}, *read, *written}, issues, source);
    }
    return counter_rows(std::move(rows));
}

IoCollector::IoCollector(std::filesystem::path proc_root, std::filesystem::path sys_root, Readers readers,
                         ClockSource clock_source)
    : proc_(std::move(proc_root)), sys_(std::move(sys_root)), readers_(std::move(readers)),
      clock_source_(std::move(clock_source)) {
    if (!readers_.file) readers_.file = read_io_file;
    if (!readers_.link) readers_.link = read_io_link;
    if (!readers_.interface_index) readers_.interface_index = read_interface_index;
}

void IoCollector::reset_baselines() { previous_.clear(); }

IoSnapshot IoCollector::collect(bool network_enabled, bool disks_enabled) try {
    IoSnapshot snapshot;
    snapshot.stamp.begin = Clock::now();
    snapshot.stamp.sequence = ++sequence_;
    const auto clocks = clock_source_();
    if (epoch_.update(clocks)) reset_baselines();
    if (!clocks) snapshot.stamp.issues.push_back({Quality::error, "clocks", "cannot establish sampling epoch"});
    auto next = previous_;
    const auto record_issue = [&snapshot](const TextResult& result) {
        if (result.issue) snapshot.stamp.issues.push_back(*result.issue);
    };
    const auto read_boot = [this] {
        const auto path = proc_ / "sys/kernel/random/boot_id";
        return checked_identity(readers_.file(path, 256), path, "boot ID");
    };
    const auto read_namespace = [this] {
        const auto path = proc_ / "thread-self/ns/net";
        return checked_identity(readers_.link(path), path, "network namespace");
    };
    for (bool network : {true, false}) {
        if (!(network ? network_enabled : disks_enabled)) continue;
        const std::string prefix = network ? "net/" : "disk/";
        std::erase_if(next, [&prefix](const auto& entry) { return entry.first.starts_with(prefix); });
        const auto file = proc_ / (network ? "thread-self/net/dev" : "diskstats");
        const auto discovery = readers_.file(file, 1024 * 1024);
        if (discovery.issue) { record_issue(discovery); continue; }
        const auto discovered = network ? parse_network(discovery.text, &snapshot.stamp.issues, file.string())
                                        : parse_disks(discovery.text, &snapshot.stamp.issues, file.string());
        if (discovered.empty()) {
            snapshot.stamp.issues.push_back({Quality::error, file.string(), "no valid counter records"});
            continue;
        }
        const auto read_device = [this, network](const IoCounters& counter) {
            if (network) {
                // Native name lookup and thread-self proc sources share the calling thread's namespace.
                // A sysfs mount can expose another namespace and must not supply a fallback identity.
                const auto source = "if_nametoindex(" + counter.name + ")";
                return checked_identity(readers_.interface_index(counter.name), source, "interface index");
            }
            const auto slash = counter.name.find('/');
            const auto device = sys_ / "dev/block" / counter.name.substr(0, slash);
            if (!absent(device / "partition")) {
                const auto partition = readers_.file(device / "partition", 64);
                if (partition.issue) return partition;
                return TextResult{{}, Issue{Quality::unsupported, (device / "partition").string(),
                                           "partition lifetime has no generation-safe rate identity"}};
            }
            const auto path = device / "diskseq";
            auto result = readers_.file(path, 64);
            if (result.issue && result.issue->quality == Quality::unsupported && absent(device))
                result.issue = Issue{Quality::error, path.string(), "block device disappeared during sample"};
            return checked_identity(std::move(result), path, "disk sequence");
        };
        const auto boot_before = read_boot();
        const auto ns_before = network ? read_namespace() : TextResult{};
        record_issue(boot_before);
        if (network) record_issue(ns_before);
        std::map<std::string, TextResult> before;
        for (const auto& counter : discovered) {
            auto identity = read_device(counter);
            record_issue(identity);
            before.emplace(counter.name, std::move(identity));
        }

        // Discovery supplies names only. The actual counter read is bracketed by identities.
        // Native same-index ABA reuse remains observational; these are not atomic netlink records.
        const auto text = readers_.file(file, 1024 * 1024);
        const auto time = Clock::now();
        if (text.issue) { record_issue(text); continue; }
        const auto counters = network ? parse_network(text.text, &snapshot.stamp.issues, file.string())
                                      : parse_disks(text.text, &snapshot.stamp.issues, file.string());
        std::map<std::string, TextResult> after;
        for (const auto& counter : counters) {
            auto identity = read_device(counter);
            record_issue(identity);
            after.emplace(counter.name, std::move(identity));
        }
        const auto ns_after = network ? read_namespace() : TextResult{};
        const auto boot_after = read_boot();
        if (network) record_issue(ns_after);
        record_issue(boot_after);
        if (counters.empty()) snapshot.stamp.issues.push_back({Quality::error, file.string(), "no valid counter records"});
        auto& rows = network ? snapshot.network : snapshot.disks;
        for (const auto& counter : counters) {
            Throughput row;
            row.name = counter.name;
            const auto first = before.find(counter.name);
            const auto& last = after.at(counter.name);
            const auto quality = identity_quality({&boot_before, &boot_after, &ns_before, &ns_after,
                                                   first == before.end() ? &last : &first->second, &last});
            const bool failed = boot_before.issue || boot_after.issue || ns_before.issue || ns_after.issue ||
                                last.issue || (first != before.end() && first->second.issue);
            if (failed) {
                row.read_bytes_per_second.quality = row.written_bytes_per_second.quality = quality;
            } else if (first == before.end() || first->second.text != last.text ||
                       boot_before.text != boot_after.text || ns_before.text != ns_after.text) {
                snapshot.stamp.issues.push_back({Quality::warming, file.string() + "/" + counter.name,
                                                "identity changed during sample; baseline discarded"});
            } else {
                const auto slash = counter.name.find('/');
                row.identity = prefix + boot_after.text + "/" +
                    (network ? ns_after.text : counter.name.substr(0, slash)) + "/" + last.text;
                const auto old = previous_.find(row.identity);
                if (old != previous_.end()) {
                    const double elapsed = std::chrono::duration<double>(time - old->second.time).count();
                    auto rx = rate(old->second.read, counter.read, elapsed);
                    auto tx = rate(old->second.written, counter.written, elapsed);
                    const double scale = network ? 1.0 : 512.0;
                    if (rx && tx) {
                        row.read_bytes_per_second = {*rx * scale, Quality::valid};
                        row.written_bytes_per_second = {*tx * scale, Quality::valid};
                    }
                }
                next[row.identity] = {counter.read, counter.written, time};
            }
            rows.push_back(std::move(row));
        }
    }
    previous_ = std::move(next);
    if ((network_enabled || disks_enabled) && snapshot.network.empty() && snapshot.disks.empty() &&
        !snapshot.stamp.issues.empty()) snapshot.stamp.quality = Quality::error;
    snapshot.stamp.end = Clock::now();
    return snapshot;
} catch (...) {
    reset_baselines();
    throw;
}

CapacitySnapshot collect_capacity(const std::vector<std::filesystem::path>& paths) {
    CapacitySnapshot snapshot;
    snapshot.stamp.begin = Clock::now();
    for (const auto& path : paths) {
        Capacity row; row.path = path.string();
        struct statvfs values{};
        if (::statvfs(path.c_str(), &values) != 0) {
            const auto quality = errno == EACCES || errno == EPERM ? Quality::permission_denied : Quality::error;
            row.total_bytes.quality = row.used_bytes.quality = row.available_bytes.quality = quality;
            snapshot.stamp.issues.push_back({quality, row.path, std::strerror(errno)});
        } else if (values.f_bfree > values.f_blocks || values.f_bavail > values.f_blocks) {
            row.total_bytes.quality = row.used_bytes.quality = row.available_bytes.quality = Quality::error;
            snapshot.stamp.issues.push_back({Quality::error, row.path, "inconsistent filesystem counters"});
        } else {
            const auto total = multiply(values.f_blocks, values.f_frsize);
            const auto used = multiply(values.f_blocks - values.f_bfree, values.f_frsize);
            const auto available = multiply(values.f_bavail, values.f_frsize);
            if (total && used && available) {
                row.total_bytes = {*total, Quality::valid}; row.used_bytes = {*used, Quality::valid};
                row.available_bytes = {*available, Quality::valid};
            } else {
                row.total_bytes.quality = row.used_bytes.quality = row.available_bytes.quality = Quality::error;
                snapshot.stamp.issues.push_back({Quality::error, row.path, "filesystem size overflow"});
            }
        }
        snapshot.mounts.push_back(std::move(row));
    }
    snapshot.stamp.end = Clock::now();
    return snapshot;
}
} // namespace night
