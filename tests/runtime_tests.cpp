#include "ni_ght_si/engine.hpp"
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <poll.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::size_t entries(const char* path) {
    DIR* dir = ::opendir(path);
    if (!dir) throw std::runtime_error("cannot inspect test process resources");
    std::size_t count{};
    while (const auto* entry = ::readdir(dir)) if (entry->d_name[0] != '.') ++count;
    ::closedir(dir); return count;
}
template<class Predicate> bool eventually(Predicate predicate) {
    const auto end = night::Clock::now() + 2s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (night::Clock::now() < end);
    return false;
}
night::EngineConfig quick_config() {
    night::EngineConfig config;
    config.system_interval = config.process_interval = config.io_interval = config.capacity_interval = 10ms;
    config.gpu_interval = 10ms;
    config.history_capacity = 4;
    return config;
}
void sampling_presets() {
    struct Preset {
        night::RefreshMode mode;
        std::chrono::milliseconds system, processes, io, capacity, gpu;
    };
    const std::array presets{
        Preset{night::RefreshMode::fast, 250ms, 250ms, 500ms, 30000ms, 500ms},
        Preset{night::RefreshMode::balanced, 500ms, 500ms, 500ms, 30000ms, 1000ms},
        Preset{night::RefreshMode::low_power, 2000ms, 2000ms, 2000ms, 60000ms, 2000ms}};
    const night::EngineConfig defaults;
    for (const auto& preset : presets) {
        const auto config = night::sampling_configuration(preset.mode);
        require(config.system_interval == preset.system && config.process_interval == preset.processes &&
                    config.io_interval == preset.io && config.capacity_interval == preset.capacity &&
                    config.gpu_interval == preset.gpu, "refresh preset maps each collector polling cadence");
        require(config.proc_root == defaults.proc_root && config.sys_root == defaults.sys_root &&
                    config.history_capacity == defaults.history_capacity && config.history_capacity == 1200 &&
                    config.capacity_paths == defaults.capacity_paths &&
                    config.include_application_scopes == defaults.include_application_scopes,
                "refresh presets retain ordinary paths, history budget and scope defaults");
    }
    const auto balanced = night::sampling_configuration(night::RefreshMode::balanced);
    require(defaults.system_interval == balanced.system_interval &&
                defaults.process_interval == balanced.process_interval && defaults.io_interval == balanced.io_interval &&
                defaults.capacity_interval == balanced.capacity_interval && defaults.gpu_interval == balanced.gpu_interval,
            "default engine sampling is exactly the balanced preset");
    for (const auto invalid : {-1, 100}) {
        bool rejected{};
        try { static_cast<void>(night::sampling_configuration(static_cast<night::RefreshMode>(invalid))); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "unknown refresh mode is rejected instead of selecting a preset");
    }
}
void live_sampling_presets() {
    auto config = night::sampling_configuration(night::RefreshMode::low_power);
    config.history_capacity = 7;
    config.capacity_paths = {"/fixture"};
    config.include_application_scopes = true;
    std::array<std::atomic_uint, 5> calls{};
    std::array<std::thread::id, 5> threads{};
    std::mutex threads_mutex;
    std::atomic_bool replaced_worker{false}, incoherent_config{false};
    std::atomic_uint config_reads{0};
    std::promise<void> release;
    const auto released = release.get_future().share();
    const auto observed = [&](std::size_t lane) {
        std::lock_guard lock(threads_mutex);
        if (calls[lane] == 0) threads[lane] = std::this_thread::get_id();
        else if (threads[lane] != std::this_thread::get_id()) replaced_worker = true;
        ++calls[lane];
    };
    const auto stamp = [] {
        night::Stamp value;
        value.begin = value.end = night::Clock::now();
        return value;
    };
    night::EngineSources sources;
    sources.system = [&] { observed(0); night::SystemSnapshot value; value.stamp = stamp(); return value; };
    sources.processes = [&] { observed(1); night::ProcessSnapshot value; value.stamp = stamp(); return value; };
    sources.io = [&] { observed(2); night::IoSnapshot value; value.stamp = stamp(); return value; };
    sources.capacity = [&] { observed(3); night::CapacitySnapshot value; value.stamp = stamp(); return value; };
    sources.gpu = [&] { observed(4); released.wait(); night::GpuSnapshot value; value.stamp = stamp(); return value; };
    night::Engine engine{config, sources};
    const bool started = eventually([&] {
        return std::all_of(calls.begin(), calls.end(), [](const auto& count) { return count.load() == 1; });
    });
    auto change = std::async(std::launch::async, [&] { return engine.setRefreshMode(night::RefreshMode::fast); });
    const auto changed_without_collection = change.wait_for(200ms) == std::future_status::ready;
    release.set_value(); // Release before assertions, including the failure path.
    const bool changed = change.get();
    require(started && changed_without_collection && changed, "mode change must not wait for a collector");
    const auto snapshot = engine.configuration();
    require(snapshot.system_interval == 250ms && snapshot.process_interval == 250ms && snapshot.io_interval == 500ms
            && snapshot.gpu_interval == 500ms && snapshot.capacity_interval == 30000ms,
            "running engine switches every cadence to Fast");
    require(snapshot.history_capacity == 7 && snapshot.capacity_paths == config.capacity_paths
            && snapshot.include_application_scopes && engine.gpuEnabled(), "mode switch preserves engine scope and storage");
    require(!engine.setRefreshMode(night::RefreshMode::fast), "selecting the active mode is a no-op");
    const auto resumed = night::Clock::now();
    require(eventually([&] { return calls[0] >= 2 && calls[1] >= 2 && calls[2] >= 2 && calls[4] >= 2; })
            && night::Clock::now() - resumed < 1500ms, "sleeping lanes wake for the new shorter polling cadence");
    require(!replaced_worker && calls[3] == 1 && engine.history().size() >= 2,
            "existing workers and history survive a cadence change");
    const auto retained = engine.system();
    std::jthread reader([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const auto current = engine.configuration();
            bool matches = false;
            for (const auto mode : {night::RefreshMode::fast, night::RefreshMode::balanced, night::RefreshMode::low_power}) {
                const auto preset = night::sampling_configuration(mode);
                matches = matches || (current.system_interval == preset.system_interval
                    && current.process_interval == preset.process_interval && current.io_interval == preset.io_interval
                    && current.capacity_interval == preset.capacity_interval && current.gpu_interval == preset.gpu_interval);
            }
            if (!matches) incoherent_config = true;
            ++config_reads;
        }
    });
    const bool reader_started = eventually([&] { return config_reads.load() != 0; });
    for (int i = 0; i < 20; ++i) {
        engine.setRefreshMode(night::RefreshMode::balanced);
        engine.setRefreshMode(night::RefreshMode::fast);
        engine.setRefreshMode(night::RefreshMode::low_power);
    }
    reader.request_stop();
    reader.join();
    require(reader_started && !incoherent_config, "configuration readers see coherent presets while modes change");
    bool rejected = false;
    try { engine.setRefreshMode(static_cast<night::RefreshMode>(-1)); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && engine.configuration().system_interval == 2000ms, "invalid live mode preserves current configuration");
    const auto quiet = calls[0].load();
    std::this_thread::sleep_for(100ms);
    require(calls[0] <= quiet + 1 && !replaced_worker && retained && !engine.history().empty(),
            "slower mode avoids catch-up bursts and retains previously published snapshots");
}
void clocks_and_freshness() {
    night::CounterEpoch epoch;
    const auto v = [](auto a, auto b, auto c) { return std::optional{night::ClockObservation{a,b,c}}; };
    require(epoch.update(v(1s,1s,1s)), "first clock epoch reseeds");
    require(!epoch.update(v(2s,2s,2s)), "regular clock progress retains baseline");
    require(!epoch.update(v(3s,3500ms,4s)), "bracket uncertainty is not false suspend");
    require(epoch.update(v(5s,15s,5s)), "suspend offset jump reseeds");
    require(!epoch.update(v(6s,16s,6s)), "resumed stable offset retains baseline");
    require(epoch.update(std::nullopt) && epoch.update(v(7s,17s,7s)), "missing clock invalidates both sides of interval");
    require(epoch.update(v(6s,16s,6s)), "backward clocks reseed");
    require(epoch.update(v(8s,18s,7s)), "invalid observation bracket rejected");
    require(night::observe_clocks().has_value(), "Linux monotonic/boottime clocks available");
    night::Stamp stamp; stamp.begin = night::Clock::now(); stamp.end = stamp.begin + 10ms;
    require(night::freshness(stamp, stamp.end + 100ms, 100ms) == night::Quality::valid, "fresh boundary inclusive");
    require(night::freshness(stamp, stamp.end + 101ms, 100ms) == night::Quality::stale, "age explicitly marks stale");
    stamp.quality = night::Quality::error;
    require(night::freshness(stamp, stamp.end + 1s, 100ms) == night::Quality::error, "failure outranks sample age");
}

void failing_and_slow_sources() {
    auto config = quick_config();
    night::EngineSources failures;
    failures.system = []() -> night::SystemSnapshot { throw std::runtime_error("injected system failure"); };
    failures.capacity = []() -> night::CapacitySnapshot { throw std::runtime_error("injected capacity failure"); };
    {
        night::Engine engine{config, failures};
        require(eventually([&] { auto s=engine.system(); auto c=engine.capacity(); return s && c && s->stamp.sequence>=3 && c->stamp.sequence>=3; }), "failure snapshots continue advancing");
        const auto system=engine.system(); const auto capacity=engine.capacity();
        require(system->stamp.quality==night::Quality::error && capacity->stamp.quality==night::Quality::error &&
                !system->stamp.issues.empty() && !capacity->stamp.issues.empty(), "exceptions publish authoritative errors");
        require(!engine.history().empty() && engine.history().back().quality==night::Quality::error, "failed sample is a history gap");
    }
    std::atomic<bool> entered{}, release{};
    std::atomic<unsigned> calls{};
    night::EngineSources slow;
    slow.capacity = [&] {
        ++calls; entered=true;
        while (!release.load()) std::this_thread::sleep_for(1ms);
        night::CapacitySnapshot s; s.stamp.begin=s.stamp.end=night::Clock::now(); return s;
    };
    bool entered_ok{}, others_ok{}, one_call{};
    {
        night::Engine engine{config, slow};
        entered_ok = eventually([&] { return entered.load(); });
        others_ok = eventually([&] { auto s=engine.system(); auto p=engine.processes(); return s && p && s->stamp.sequence>=4 && p->stamp.sequence>=4; });
        one_call = calls==1;
        release=true; // Always release before assertions/destruction, including failure.
    }
    require(entered_ok && others_ok && one_call, "slow capacity stays isolated; no replacement-worker creation");
}

template<class T> T idle_sample() {
    T sample; sample.stamp.begin = sample.stamp.end = night::Clock::now(); return sample;
}
night::EngineSources idle_sources() {
    night::EngineSources sources;
    sources.system = idle_sample<night::SystemSnapshot>;
    sources.processes = idle_sample<night::ProcessSnapshot>;
    sources.io = idle_sample<night::IoSnapshot>;
    sources.capacity = idle_sample<night::CapacitySnapshot>;
    return sources;
}
void optional_gpu_lane() {
    auto config = quick_config();
    auto sources = idle_sources();
    require(night::EngineConfig{}.gpu_interval == 1000ms, "default GPU interval is one second");
    for (const auto invalid : {9ms, 3600001ms}) {
        auto invalid_config = config; invalid_config.gpu_interval = invalid;
        bool rejected{};
        try { night::Engine engine{invalid_config, sources}; }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "GPU interval uses engine sampling bounds");
    }
    std::mutex observed_mutex;
    std::array<std::thread::id, 4> worker_ids;
    const auto record = [&](std::size_t lane) {
        std::lock_guard lock(observed_mutex);
        worker_ids[lane] = std::this_thread::get_id();
    };
    sources.system = [&] { record(0); return idle_sample<night::SystemSnapshot>(); };
    sources.processes = [&] { record(1); return idle_sample<night::ProcessSnapshot>(); };
    sources.io = [&] { record(2); return idle_sample<night::IoSnapshot>(); };
    sources.capacity = [&] { record(3); return idle_sample<night::CapacitySnapshot>(); };
    {
        night::Engine engine{config, sources};
        require(!engine.gpuEnabled() && !engine.gpu(), "absent GPU source disables publication");
        require(eventually([&] { return engine.system() && engine.processes() && engine.io() && engine.capacity(); }),
                "default four lanes publish without GPU");
        // Observe our workers directly: sanitizer/runtime threads and recently
        // joined kernel task entries are outside this engine's ownership.
        std::lock_guard lock(observed_mutex);
        for (std::size_t lane = 0; lane < worker_ids.size(); ++lane) {
            require(worker_ids[lane] != std::thread::id{} && worker_ids[lane] != std::this_thread::get_id(),
                    "default lane runs on its worker");
            for (std::size_t other = 0; other < lane; ++other)
                require(worker_ids[lane] != worker_ids[other], "four default lanes have distinct workers");
        }
    }
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> on_caller{};
    sources.gpu = [&] {
        if (std::this_thread::get_id() == caller) on_caller = true;
        auto sample = idle_sample<night::GpuSnapshot>();
        sample.stamp.sequence = 999999; // Engine sequence ownership must override the source.
        night::GpuReading device;
        device.utilization_percent = {25, night::Quality::valid};
        device.total_bytes = {1024, night::Quality::valid};
        device.used_bytes = {200, night::Quality::valid};
        device.free_bytes = {824, night::Quality::valid};
        sample.devices.push_back(std::move(device));
        return sample;
    };
    {
        night::Engine engine{config, sources};
        require(engine.gpuEnabled(), "provided GPU source enables its lane");
        require(eventually([&] { auto gpu = engine.gpu(); return gpu && gpu->stamp.sequence >= 3; }),
                "GPU source advances on its worker");
        const auto first = engine.gpu();
        const auto sequence = first->stamp.sequence;
        require(sequence < 999999 && !on_caller && first->devices[0].utilization_percent.value == 25 &&
                    first->devices[0].used_bytes.value == 200, "engine sequences preserve valid GPU data off caller thread");
        require(eventually([&] { return engine.gpu()->stamp.sequence > sequence; }) && first->stamp.sequence == sequence,
                "GPU publication is monotonic and earlier immutable snapshots remain intact");
    }
    sources.gpu = []() -> night::GpuSnapshot { throw std::runtime_error("injected GPU failure"); };
    {
        night::Engine engine{config, sources};
        require(eventually([&] { auto gpu = engine.gpu(); return gpu && gpu->stamp.sequence >= 3; }),
                "throwing GPU source continues publishing");
        const auto gpu = engine.gpu();
        require(gpu->stamp.quality == night::Quality::error && gpu->devices.empty() &&
                    !gpu->stamp.issues.empty() && gpu->stamp.issues[0].message == "injected GPU failure",
                "GPU exceptions publish explicit error snapshots");
    }
    std::atomic<bool> entered{}, release{};
    std::atomic<unsigned int> calls{};
    sources.gpu = [&] {
        ++calls; entered = true;
        while (!release.load()) std::this_thread::sleep_for(1ms);
        return idle_sample<night::GpuSnapshot>();
    };
    {
        night::Engine engine{config, sources};
        struct Release {
            std::atomic<bool>& flag;
            ~Release() { flag = true; }
        } unblock_before_engine_destruction{release};
        require(eventually([&] { return entered.load(); }), "blocked GPU source entered");
        require(eventually([&] {
            const auto system = engine.system(); const auto processes = engine.processes();
            const auto io = engine.io(); const auto capacity = engine.capacity();
            return system && processes && io && capacity && system->stamp.sequence >= 4 &&
                   processes->stamp.sequence >= 4 && io->stamp.sequence >= 4 && capacity->stamp.sequence >= 4;
        }), "all other lanes advance while GPU source blocks");
        require(calls == 1 && !engine.gpu(), "blocked GPU lane has one worker and no fabricated sample");
    }
}

void owned_child_collection() {
    int control[2], ready[2];
    if (::pipe2(control,O_CLOEXEC)!=0) throw std::runtime_error("test control pipe failed");
    if (::pipe2(ready,O_CLOEXEC)!=0) { ::close(control[0]);::close(control[1]);throw std::runtime_error("test ready pipe failed"); }
    const auto child=::fork();
    if (child==0) {
        ::close(control[1]);::close(ready[0]);
        if (::prctl(PR_SET_NAME,"test ) child",0,0,0)!=0) ::_exit(2);
        const char token='R'; if (::write(ready[1],&token,1)!=1) ::_exit(3);
        ::close(ready[1]);
        pollfd descriptor{control[0],POLLIN,0};
        // Natural EOF cleanup with a finite fallback; no signals to any process.
        if (::poll(&descriptor,1,5000)>0) { char byte; static_cast<void>(::read(control[0],&byte,1)); }
        ::close(control[0]);::_exit(0);
    }
    ::close(control[0]);::close(ready[1]);
    if (child<0) { ::close(control[1]);::close(ready[0]);throw std::runtime_error("test fork failed"); }
    struct Child {
        pid_t pid; int pipe; int ready_fd;
        ~Child() {
            if (pipe>=0) ::close(pipe);
            if (ready_fd>=0) ::close(ready_fd);
            if (pid>0) { int status{}; while (::waitpid(pid,&status,0)<0 && errno==EINTR) {} }
        }
    } cleanup{child,control[1],ready[0]};
    pollfd descriptor{ready[0],POLLIN,0}; char token{};
    require(::poll(&descriptor,1,2000)>0 && ::read(ready[0],&token,1)==1 && token=='R', "owned child ready");
    night::ProcessCollector collector;
    auto first=collector.collect();
    auto find=[child](const auto& sample) {
        return std::find_if(sample.processes.begin(),sample.processes.end(),[child](const auto& p) {return p.pid==static_cast<std::uint64_t>(child);});
    };
    const auto process=find(first);
    require(process!=first.processes.end() && process->name=="test ) child" && process->uid==::getuid() &&
            process->rss_bytes.value && process->cpu_percent.quality==night::Quality::warming, "live owned child coherent metadata and warming baseline");
    std::this_thread::sleep_for(20ms);
    auto second=collector.collect(); const auto next=find(second);
    require(next!=second.processes.end() && next->start_ticks==process->start_ticks && next->cpu_percent.value &&
            *next->cpu_percent.value>=0, "live child interval accounting");
    ::close(cleanup.pipe);cleanup.pipe=-1;
    int status{}; pid_t waited;
    do { waited=::waitpid(child,&status,0); } while (waited<0 && errno==EINTR);
    require(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0, "only owned child exited naturally and was reaped");
    cleanup.pid=-1;
    auto final=collector.collect(); require(find(final)==final.processes.end(), "exited child removed from current generation");
}

void resource_lifetimes() {
    const auto fds=entries("/proc/self/fd"), threads=entries("/proc/self/task");
    auto config=quick_config();
    // Only bounded test fixture callbacks; exercise 20 worker lifetimes cheaply.
    night::EngineSources sources;
    sources.system=[] { night::SystemSnapshot s; s.stamp.begin=s.stamp.end=night::Clock::now();return s; };
    sources.processes=[] { night::ProcessSnapshot s; s.stamp.begin=s.stamp.end=night::Clock::now();return s; };
    sources.io=[] { night::IoSnapshot s; s.stamp.begin=s.stamp.end=night::Clock::now();return s; };
    sources.capacity=[] { night::CapacitySnapshot s; s.stamp.begin=s.stamp.end=night::Clock::now();return s; };
    for (int i=0;i<20;++i) {
        night::Engine engine{config,sources};
        require(eventually([&] { return engine.system() && engine.processes() && engine.io() && engine.capacity(); }), "all four test lanes started");
    }
    require(eventually([&] { return entries("/proc/self/fd")==fds && entries("/proc/self/task")==threads; }), "worker destruction restores descriptor/thread counts");
}
void enumeration_failure() {
    rlimit original{};
    require(::getrlimit(RLIMIT_NOFILE,&original)==0,"test can observe its own FD limit");
    struct Limit {
        rlimit original;
        std::vector<int> files;
        ~Limit() { for (const auto fd:files) ::close(fd); ::setrlimit(RLIMIT_NOFILE,&original); }
    } limit{original,{}};
    limit.files.reserve(64);
    auto bounded=original; bounded.rlim_cur=std::min<rlim_t>(original.rlim_cur,64);
    require(::setrlimit(RLIMIT_NOFILE,&bounded)==0,"bound only this test process FD budget");
    for (;;) {
        const int fd=::open("/dev/null",O_RDONLY|O_CLOEXEC);
        if (fd<0) break;
        limit.files.push_back(fd);
    }
    require(errno==EMFILE && !limit.files.empty(),"owned FD budget exhausted deterministically");
    ::close(limit.files.back());limit.files.pop_back(); // One free FD for proc root, none for enumeration duplicate.
    night::ProcessCollector collector;
    const auto result=collector.collect();
    require(result.stamp.quality==night::Quality::error &&
            std::any_of(result.stamp.issues.begin(),result.stamp.issues.end(),[](const auto& i) {return i.source=="proc enumeration";}),
            "failed enumeration is not a healthy process snapshot");
}
}
void runtime_tests() {
    sampling_presets(); live_sampling_presets(); clocks_and_freshness(); failing_and_slow_sources(); optional_gpu_lane();
    owned_child_collection(); resource_lifetimes(); enumeration_failure();
}
