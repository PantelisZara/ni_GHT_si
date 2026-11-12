#include "ni_ght_si/descriptor_budget.hpp"
#include "ni_ght_si/pinned_process.hpp"
#include "ni_ght_si/system.hpp"
#include "ni_ght_si/io.hpp"
#include <array>
#include <cerrno>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <unistd.h>

extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_openat(int, const char*, int, ...);
extern "C" ssize_t __real_read(int, void*, std::size_t);
namespace {
bool tracking{}, throw_read{};
std::size_t opens{}, reads_before_throw{};
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::size_t descriptors() {
    auto* directory = ::opendir("/proc/self/fd");
    require(directory, "descriptor inventory unavailable");
    std::size_t count{};
    int error{};
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) { error = errno; break; }
        if (night::unsigned_number(entry->d_name)) ++count;
    }
    ::closedir(directory);
    require(error == 0, "descriptor inventory failed");
    return count;
}
template<class Function> auto without_opens(Function function) {
    struct Reset { ~Reset() { tracking = false; } } reset;
    opens = 0; tracking = true;
    auto result = function();
    require(opens == 0, "budget rejection opened a native descriptor");
    return result;
}
class Fixture {
public:
    std::filesystem::path root;
    Fixture() {
        std::string pattern = "/tmp/night-descriptor-XXXXXX";
        const auto* directory = ::mkdtemp(pattern.data());
        require(directory, "cannot create owned fixture");
        root = directory;
        put("sys/kernel/random/boot_id", "12345678-1234-1234-1234-123456789abc\n");
        std::filesystem::create_directories(root / "self/ns");
        std::filesystem::create_symlink("pid:[123]", root / "self/ns/pid");
        std::filesystem::create_directories(root / "thread-self/ns");
        std::filesystem::create_symlink("net:[123]", root / "thread-self/ns/net");
        put("42/status", "Uid: 1000 1000 1000 1000\n");
        put("meminfo", "MemTotal: 1024 kB\nMemAvailable: 400 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
        put("uptime", "1.0 2.0\n"); put("loadavg", "0.1 0.2 0.3 1/2 42\n");
        advance(100);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void put(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream output(root / path); output << text;
        require(output.good(), "cannot write owned fixture");
    }
    void advance(std::uint64_t ticks) {
        put("stat", "cpu " + std::to_string(ticks) + " 0 0 100 0 0 0 0 0 0\n");
        std::array<std::string, 22> fields;
        fields.fill("0"); fields[0] = "S"; fields[1] = "1"; fields[11] = std::to_string(ticks);
        fields[17] = "1"; fields[19] = "100"; fields[21] = "1";
        std::string stat = "42 (owned fixture)";
        for (const auto& field : fields) stat += " " + field;
        put("42/stat", stat + "\n");
        put("thread-self/net/dev", "Inter-| Receive | Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\nlo: " +
            std::to_string(ticks) + " 1 0 0 0 0 0 0 " + std::to_string(ticks * 2) + " 1 0 0 0 0 0 0\n");
    }
};
void file_exception_and_admission(Fixture& fixture) {
    const auto baseline = descriptors();
    {
        auto full = night::DescriptorReservation::try_acquire(8);
        require(full.has_value(), "cannot reserve test budget");
        const auto failed = without_opens([&] { return night::read_text(fixture.root / "42/stat"); });
        require(failed.text.empty() && failed.issue && failed.issue->quality == night::Quality::error &&
                failed.issue->message == "temporary descriptor budget unavailable", "busy text read returned usable data");
    }
    bool caught{};
    throw_read = true;
    try { static_cast<void>(night::read_text(fixture.root / "42/stat")); }
    catch (const std::bad_alloc&) { caught = true; }
    throw_read = false;
    require(caught && descriptors() == baseline, "exception after open leaked its native descriptor");
    auto all = night::DescriptorReservation::try_acquire(8);
    require(all.has_value(), "exception after open leaked its budget reservation");
    all.reset();
    require(!night::read_text(fixture.root / "42/stat").issue, "text read did not recover after exhaustion/exception");
}
void collector_admission_and_recovery(Fixture& fixture) {
    night::SystemCollector system{fixture.root};
    night::ProcessCollector process{fixture.root};
    night::IoCollector::Readers readers;
    readers.interface_index = [](std::string_view) { return night::TextResult{"1", {}}; };
    night::IoCollector io{fixture.root, fixture.root / "unused-sys", std::move(readers)};
    static_cast<void>(system.collect()); static_cast<void>(process.collect()); static_cast<void>(io.collect(true, false));
    fixture.advance(200);
    const auto warm_system = system.collect(); const auto warm_process = process.collect(); const auto warm_io = io.collect(true, false);
    require(!warm_system.cpus.empty() && warm_system.cpus.front().busy_percent.quality == night::Quality::valid &&
            warm_process.processes.size() == 1 && warm_process.processes.front().cpu_percent.quality == night::Quality::valid &&
            warm_io.network.size() == 1 && warm_io.network.front().read_bytes_per_second.quality == night::Quality::valid,
            "owned collector baselines never became valid");
    {
        auto full = night::DescriptorReservation::try_acquire(8);
        require(full.has_value(), "cannot exhaust budget for collectors");
        const auto failed_system = without_opens([&] { return system.collect(); });
        const auto failed_process = without_opens([&] { return process.collect(); });
        const auto failed_io = without_opens([&] { return io.collect(true, false); });
        require(failed_system.stamp.quality == night::Quality::error && !failed_system.cpus.empty() &&
                !failed_system.cpus.front().busy_percent.value &&
                !failed_system.available_bytes.value && failed_process.stamp.quality == night::Quality::error &&
                failed_process.processes.empty() && failed_io.stamp.quality == night::Quality::error &&
                failed_io.network.empty(), "exhausted collectors retained usable readings");
    }
    fixture.advance(300);
    const auto recovered_system = system.collect(); const auto recovered_process = process.collect(); const auto recovered_io = io.collect(true, false);
    require(!recovered_system.cpus.empty() && recovered_system.cpus.front().busy_percent.quality == night::Quality::warming &&
            recovered_process.processes.size() == 1 && recovered_process.processes.front().cpu_percent.quality == night::Quality::warming &&
            recovered_io.network.size() == 1 && recovered_io.network.front().read_bytes_per_second.quality == night::Quality::warming,
            "collector recovery reused a pre-failure rate baseline");
    {
        // Outer three-directory admission succeeds, but no leaf file slot remains.
        auto held = night::DescriptorReservation::try_acquire(5);
        require(held.has_value(), "cannot reserve leaf exhaustion case");
        const auto partial = process.collect();
        require(partial.stamp.quality == night::Quality::error && partial.processes.empty() && !partial.stamp.issues.empty(),
                "process leaf exhaustion produced partial identity metadata");
    }
    const auto leaf_recovery = process.collect();
    require(leaf_recovery.processes.size() == 1 && leaf_recovery.processes.front().cpu_percent.quality == night::Quality::warming,
            "leaf exhaustion did not prune process baseline");
}

template<class Function> bool read_exception_cleanup(Function function, std::size_t countdown = 0) {
    const auto baseline = descriptors();
    bool caught{};
    reads_before_throw = countdown;
    throw_read = true;
    try { function(); }
    catch (const std::bad_alloc&) { caught = true; }
    throw_read = false;
    reads_before_throw = 0;
    const auto all = night::DescriptorReservation::try_acquire(8);
    return caught && descriptors() == baseline && all.has_value();
}

const char* quality_label(night::Quality quality) {
    switch (quality) {
    case night::Quality::valid: return "valid";
    case night::Quality::warming: return "warming";
    case night::Quality::stale: return "stale";
    case night::Quality::permission_denied: return "permission_denied";
    case night::Quality::unsupported: return "unsupported";
    case night::Quality::error: return "error";
    }
    return "unknown";
}

void collector_exception_recovery(Fixture& fixture) {
    night::SystemCollector system{fixture.root};
    night::ProcessCollector process{fixture.root};
    night::IoCollector::Readers readers;
    readers.interface_index = [](std::string_view) { return night::TextResult{"1", {}}; };
    night::IoCollector io{fixture.root, fixture.root / "unused-sys", std::move(readers)};
    fixture.advance(1000);
    static_cast<void>(system.collect());
    static_cast<void>(process.collect());
    static_cast<void>(io.collect(true, false));
    fixture.advance(1100);
    const auto seeded_system = system.collect();
    const auto seeded_process = process.collect();
    const auto seeded_io = io.collect(true, false);
    require(!seeded_system.cpus.empty() && seeded_system.cpus.front().busy_percent.quality == night::Quality::valid &&
            seeded_process.processes.size() == 1 && seeded_process.processes.front().cpu_percent.quality == night::Quality::valid &&
            seeded_io.network.size() == 1 && seeded_io.network.front().read_bytes_per_second.quality == night::Quality::valid,
            "exception fixtures require established valid collector rate baselines");

    fixture.advance(1200);
    // The small stat file takes one data read and one EOF read. Throwing on the
    // next read reaches meminfo after SystemCollector has updated its CPU state.
    const bool system_clean = read_exception_cleanup([&] { static_cast<void>(system.collect()); }, 2);
    const bool process_clean = read_exception_cleanup([&] { static_cast<void>(process.collect()); });
    const bool io_clean = read_exception_cleanup([&] { static_cast<void>(io.collect(true, false)); });
    fixture.advance(1300);
    const auto recovered_system = system.collect();
    const auto recovered_process = process.collect();
    const auto recovered_io = io.collect(true, false);
    fixture.advance(1400);
    const auto next_system = system.collect();
    const auto next_process = process.collect();
    const auto next_io = io.collect(true, false);

    const auto system_quality = recovered_system.cpus.empty() ? night::Quality::error
        : recovered_system.cpus.front().busy_percent.quality;
    const auto process_quality = recovered_process.processes.empty() ? night::Quality::error
        : recovered_process.processes.front().cpu_percent.quality;
    const auto io_quality = recovered_io.network.empty() ? night::Quality::error
        : recovered_io.network.front().read_bytes_per_second.quality;
    std::cout << "INFO exception recovery system=" << quality_label(system_quality)
              << " process=" << quality_label(process_quality) << " network=" << quality_label(io_quality)
              << " expected=warming cleanup=" << system_clean << ',' << process_clean << ',' << io_clean << '\n';
    std::cout << "INFO exception sequences seed/recovery/next system=" << seeded_system.stamp.sequence << '/'
              << recovered_system.stamp.sequence << '/' << next_system.stamp.sequence
              << " process=" << seeded_process.stamp.sequence << '/' << recovered_process.stamp.sequence << '/'
              << next_process.stamp.sequence << " network=" << seeded_io.stamp.sequence << '/'
              << recovered_io.stamp.sequence << '/' << next_io.stamp.sequence << '\n';

    const bool warming = system_quality == night::Quality::warming &&
        !recovered_system.cpus.front().busy_percent.value &&
        recovered_system.cpus.front().iowait_percent.quality == night::Quality::warming &&
        !recovered_system.cpus.front().iowait_percent.value &&
        recovered_process.processes.size() == 1 && process_quality == night::Quality::warming &&
        !recovered_process.processes.front().cpu_percent.value && recovered_io.network.size() == 1 &&
        io_quality == night::Quality::warming && !recovered_io.network.front().read_bytes_per_second.value &&
        recovered_io.network.front().written_bytes_per_second.quality == night::Quality::warming &&
        !recovered_io.network.front().written_bytes_per_second.value;
    const bool valid = !next_system.cpus.empty() && next_system.cpus.front().busy_percent.quality == night::Quality::valid &&
        next_system.cpus.front().busy_percent.value && next_system.cpus.front().iowait_percent.quality == night::Quality::valid &&
        next_system.cpus.front().iowait_percent.value && next_process.processes.size() == 1 &&
        next_process.processes.front().cpu_percent.quality == night::Quality::valid && next_process.processes.front().cpu_percent.value &&
        next_io.network.size() == 1 && next_io.network.front().read_bytes_per_second.quality == night::Quality::valid &&
        next_io.network.front().read_bytes_per_second.value &&
        next_io.network.front().written_bytes_per_second.quality == night::Quality::valid &&
        next_io.network.front().written_bytes_per_second.value;
    const bool sequences = recovered_system.stamp.sequence == seeded_system.stamp.sequence + 2 &&
        next_system.stamp.sequence == recovered_system.stamp.sequence + 1 &&
        recovered_process.stamp.sequence == seeded_process.stamp.sequence + 2 &&
        next_process.stamp.sequence == recovered_process.stamp.sequence + 1 &&
        recovered_io.stamp.sequence == seeded_io.stamp.sequence + 2 &&
        next_io.stamp.sequence == recovered_io.stamp.sequence + 1;
    require(system_clean && process_clean && io_clean && warming && valid && sequences,
            "thrown collector read must propagate bad_alloc, release resources and restart rate baselines without reusing failed sequence");
}

bool handle_admission_and_recovery() {
    auto pin = night::PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid()));
    if (!pin.process && (pin.status == night::ProcessPinStatus::unavailable ||
        pin.status == night::ProcessPinStatus::permission_denied || pin.status == night::ProcessPinStatus::context_mismatch ||
        pin.status == night::ProcessPinStatus::resource_limit)) return false;
    require(pin.process.has_value(), "native self binding failed unexpectedly");
    const auto cached = pin.process->metadata(); const auto baseline = descriptors();
    {
        auto full = night::DescriptorReservation::try_acquire(8);
        require(full.has_value(), "cannot exhaust budget for handles");
        const auto binding = without_opens([] { return night::PinnedProcess::bind_fresh(static_cast<std::uint64_t>(::getpid())); });
        const auto credentials = without_opens([&] { return pin.process->observe_credentials(); });
        const auto executable = without_opens([&] { return pin.process->observe_executable(); });
        const auto scope = without_opens([&] { return pin.process->observe_application_scope(); });
        require(!binding.process && binding.status == night::ProcessPinStatus::resource_limit && binding.error_number == 0 &&
                binding.operation == "temporary descriptor budget", "binding exhaustion lost explicit resource result");
        require(!credentials.context && credentials.status == night::ProcessContextStatus::resource_limit && credentials.error_number == 0 &&
                credentials.operation == "temporary descriptor budget", "credential exhaustion returned stale payload");
        require(!executable.context && executable.status == night::ProcessContextStatus::resource_limit && executable.error_number == 0 &&
                executable.operation == "temporary descriptor budget" && scope.stamp.quality == night::Quality::error &&
                !scope.candidate.value && !scope.stamp.issues.empty(), "executable/scope exhaustion lost empty resource result");
        require(descriptors() == baseline && pin.process->metadata().observed_at == cached.observed_at,
                "budget exhaustion changed retained ownership or cached metadata");
    }
    require(pin.process->observe_credentials().status == night::ProcessContextStatus::observed &&
            pin.process->observe_executable().status == night::ProcessContextStatus::observed,
            "retained observations did not recover after budget release");
    return true;
}
} // namespace
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (tracking) ++opens;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); const auto mode = va_arg(args, mode_t); va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}
extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    if (tracking) ++opens;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); const auto mode = va_arg(args, mode_t); va_end(args);
        return __real_openat(directory, path, flags, mode);
    }
    return __real_openat(directory, path, flags);
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, std::size_t count) {
    if (throw_read) {
        if (reads_before_throw) --reads_before_throw;
        else { throw_read = false; throw std::bad_alloc{}; }
    }
    return __real_read(fd, buffer, count);
}
int main() {
    try {
        const auto baseline = descriptors();
        {
            Fixture fixture;
            file_exception_and_admission(fixture);
            collector_admission_and_recovery(fixture);
            collector_exception_recovery(fixture);
        }
        if (!handle_admission_and_recovery()) {
            std::cout << "UNSUPPORTED native managed descriptor integration\n"; return 77;
        }
        require(descriptors() == baseline, "managed integration leaked native descriptors");
        require(night::DescriptorReservation::try_acquire(8).has_value(), "managed integration leaked temporary reservations");
        std::cout << "PASS managed budget rejection, no native opens, exception cleanup and baseline/handle recovery\n";
        return 0;
    } catch (const std::exception& error) {
        tracking = throw_read = false;
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
