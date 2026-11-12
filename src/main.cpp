#include "ni_ght_si/engine.hpp"
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    try {
        std::uint64_t count = 3, interval = 500;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const auto value = night::unsigned_number(argv[i + 1]);
            if (!value) throw std::invalid_argument("expected unsigned option value");
            const std::string_view option{argv[i]};
            if (option == "--samples") count = *value;
            else if (option == "--interval-ms") interval = *value;
            else throw std::invalid_argument("unknown option");
        }
        if (count == 0 || count > 1000 || interval < 10 || interval > 60000)
            throw std::invalid_argument("samples must be 1..1000 and interval 10..60000 ms");
        night::EngineConfig config;
        config.system_interval = config.process_interval = config.io_interval = std::chrono::milliseconds{interval};
        night::Engine engine{config};
        for (std::uint64_t i = 0; i < count; ++i) {
            std::this_thread::sleep_for(config.system_interval);
            const auto system = engine.system(); const auto processes = engine.processes();
            if (!system || !processes) throw std::runtime_error("initial collectors have not published");
            std::cout << "sequence=" << system->stamp.sequence << " processes=" << processes->processes.size();
            if (!system->cpus.empty() && system->cpus.front().busy_percent.value)
                std::cout << " cpu_busy_percent=" << *system->cpus.front().busy_percent.value;
            if (system->available_bytes.value) std::cout << " available_bytes=" << *system->available_bytes.value;
            std::cout << " issues=" << system->stamp.issues.size() + processes->stamp.issues.size() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "night-metrics: " << error.what() << '\n'; return 1;
    }
}
