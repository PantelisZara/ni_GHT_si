#include "ni_ght_si/engine.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

void system_tests();
void process_tests();
void io_tests();
void runtime_tests();
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void engine_tests() {
    night::Ring<int> ring{3};
    for (int value = 0; value < 8; ++value) ring.push(value);
    require(ring.copy() == std::vector<int>({5, 6, 7}), "ring must retain exactly latest entries in order");
    night::Latest<int> latest;
    require(!latest.read(), "initial latest slot absent");
    latest.publish(1); auto retained = latest.read(); latest.publish(2); latest.publish(3);
    require(*retained == 1 && *latest.read() == 3 && latest.overwrites() == 1, "immutable latest/coalescing contract");
    night::EngineConfig invalid; invalid.history_capacity = std::numeric_limits<std::size_t>::max();
    bool rejected = false;
    try { night::Engine engine{invalid}; } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "history limits checked before allocation");
    night::EngineConfig config;
    config.proc_root = "/path-that-does-not-exist/ni-ght-si";
    config.sys_root = config.proc_root;
    config.system_interval = config.process_interval = config.io_interval = std::chrono::milliseconds{10};
    config.history_capacity = 3;
    config.capacity_paths = {config.proc_root};
    const auto begin = night::Clock::now();
    {
        night::Engine engine{config};
        const auto deadline = night::Clock::now() + std::chrono::seconds{3};
        while (night::Clock::now() < deadline) {
            auto sample = engine.system();
            if (sample && sample->stamp.sequence >= 5 && engine.capacity()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        const auto sample = engine.system();
        require(sample && sample->stamp.sequence >= 5 && !sample->stamp.issues.empty(), "missing sources publish errors repeatedly");
        require(engine.processes() && engine.io() && engine.capacity(), "independent collectors publish under source errors");
        require(engine.capacity()->stamp.sequence > 0 && engine.io()->stamp.sequence > 0, "every lane publishes a sequence including capacity errors");
        require(engine.history().size() == 3, "history stays bounded while sampling");
    }
    require(night::Clock::now() - begin < std::chrono::seconds{5}, "stop must interrupt scheduled 30-second capacity sleep");
}
}

int main() {
    try {
        system_tests(); std::cout << "PASS system contracts\n";
        process_tests(); std::cout << "PASS process contracts\n";
        io_tests(); std::cout << "PASS I/O contracts\n";
        engine_tests(); std::cout << "PASS bounded engine contracts\n";
        runtime_tests(); std::cout << "PASS clocks, faults, isolation, owned child and resources\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
