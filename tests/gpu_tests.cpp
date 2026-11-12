#include "ni_ght_si/gpu.hpp"

#include <nvml.h>
#include <dlfcn.h>

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using night::Quality;
using namespace std::chrono_literals;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("GPU: " + message);
}
struct Device {
    nvmlReturn_t handle{NVML_SUCCESS}, uuid{NVML_SUCCESS}, name{NVML_SUCCESS};
    nvmlReturn_t utilization{NVML_SUCCESS}, memory{NVML_SUCCESS};
    nvmlReturn_t temperature{NVML_SUCCESS}, power{NVML_SUCCESS};
    unsigned int celsius{63}, milliwatts{125375};
    unsigned int percent{37};
    nvmlMemory_t bytes{1024, 824, 200};
    bool empty_uuid{}, unterminated_name{};
};
struct State {
    bool available{true}, initialized{}, lost{};
    std::string missing_symbol;
    nvmlReturn_t init_result{NVML_SUCCESS}, count_result{NVML_SUCCESS}, shutdown_result{NVML_SUCCESS};
    unsigned int device_count{1};
    unsigned int opens{}, symbols{}, closes{}, inits{}, shutdowns{}, counts{}, handles{}, queries{}, memories{}, errors{};
    unsigned int temperatures{}, powers{};
    std::array<Device, 32> devices;
    std::array<std::array<unsigned char, 32>, 8> tokens{};
    night::NvmlCollector* reentrant{};
    std::optional<night::GpuSnapshot> overlap;
} state;
int library_token;
const auto start = night::Clock::time_point{100s};

nvmlReturn_t status(nvmlReturn_t value) {
    if (value == NVML_ERROR_GPU_IS_LOST) state.lost = true;
    return value;
}
unsigned int index_of(nvmlDevice_t device) {
    require(state.initialized && !state.lost, "no queries after shutdown or GPU loss");
    for (unsigned int i = 0; i < 32; ++i)
        if (device == reinterpret_cast<nvmlDevice_t>(&state.tokens[state.inits][i])) return i;
    throw std::runtime_error("GPU: stale or foreign session handle");
}
nvmlReturn_t fake_init() {
    ++state.inits;
    require(!state.initialized && state.inits < state.tokens.size(), "one live initialization per session");
    state.initialized = state.init_result == NVML_SUCCESS;
    state.lost = false;
    return state.init_result;
}
nvmlReturn_t fake_shutdown() {
    require(state.initialized, "shutdown only balances successful init");
    ++state.shutdowns;
    if (state.shutdown_result != NVML_SUCCESS) return state.shutdown_result;
    state.initialized = false;
    state.lost = false;
    return NVML_SUCCESS;
}
nvmlReturn_t fake_count(unsigned int* count) {
    require(state.initialized && !state.lost, "count uses live session");
    ++state.counts;
    *count = state.device_count;
    return status(state.count_result);
}
nvmlReturn_t fake_handle(unsigned int index, nvmlDevice_t* device) {
    require(state.initialized && !state.lost && index < state.device_count && index < 32, "bounded handle lookup");
    ++state.handles;
    *device = reinterpret_cast<nvmlDevice_t>(&state.tokens[state.inits][index]);
    return status(state.devices[index].handle);
}
nvmlReturn_t fake_uuid(nvmlDevice_t device, char* buffer, unsigned int size) {
    const auto index = index_of(device);
    ++state.queries;
    if (state.devices[index].uuid != NVML_SUCCESS) return status(state.devices[index].uuid);
    const auto text = state.devices[index].empty_uuid ? std::string{} : "GPU-identity-" + std::to_string(index);
    require(text.size() < size, "UUID buffer size");
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return NVML_SUCCESS;
}
nvmlReturn_t fake_name(nvmlDevice_t device, char* buffer, unsigned int size) {
    const auto index = index_of(device);
    ++state.queries;
    if (state.devices[index].name != NVML_SUCCESS) return status(state.devices[index].name);
    if (state.devices[index].unterminated_name) std::memset(buffer, 'x', size);
    else {
        const auto text = "Fake NVIDIA " + std::to_string(index);
        require(text.size() < size, "name buffer size");
        std::memcpy(buffer, text.c_str(), text.size() + 1);
    }
    return NVML_SUCCESS;
}
nvmlReturn_t fake_utilization(nvmlDevice_t device, nvmlUtilization_t* value) {
    const auto index = index_of(device);
    ++state.queries;
    if (state.reentrant) {
        auto* collector = state.reentrant;
        state.reentrant = nullptr;
        state.overlap = collector->collect(start);
    }
    value->gpu = state.devices[index].percent;
    value->memory = 99; // Memory bandwidth activity must not become VRAM occupancy.
    return status(state.devices[index].utilization);
}
nvmlReturn_t fake_memory(nvmlDevice_t device, nvmlMemory_t* value) {
    const auto index = index_of(device);
    ++state.queries;
    ++state.memories;
    *value = state.devices[index].bytes;
    return status(state.devices[index].memory);
}
nvmlReturn_t fake_temperature(nvmlDevice_t device, nvmlTemperatureSensors_t sensor, unsigned int* value) {
    const auto index = index_of(device);
    require(sensor == NVML_TEMPERATURE_GPU, "GPU die temperature sensor only");
    ++state.queries;
    ++state.temperatures;
    *value = state.devices[index].celsius;
    return status(state.devices[index].temperature);
}
nvmlReturn_t fake_power(nvmlDevice_t device, unsigned int* value) {
    const auto index = index_of(device);
    ++state.queries;
    ++state.powers;
    *value = state.devices[index].milliwatts;
    return status(state.devices[index].power);
}
const char* fake_error(nvmlReturn_t value) {
    ++state.errors;
    switch (value) {
    case NVML_ERROR_NO_PERMISSION: return "permission denied";
    case NVML_ERROR_NOT_SUPPORTED: return "operation unsupported";
    case NVML_ERROR_GPU_IS_LOST: return "GPU lost";
    default: return "fake driver failure";
    }
}
static_assert(std::is_same_v<decltype(&fake_init), decltype(&::nvmlInit_v2)>);
static_assert(std::is_same_v<decltype(&fake_shutdown), decltype(&::nvmlShutdown)>);
static_assert(std::is_same_v<decltype(&fake_count), decltype(&::nvmlDeviceGetCount_v2)>);
static_assert(std::is_same_v<decltype(&fake_handle), decltype(&::nvmlDeviceGetHandleByIndex_v2)>);
static_assert(std::is_same_v<decltype(&fake_uuid), decltype(&::nvmlDeviceGetUUID)>);
static_assert(std::is_same_v<decltype(&fake_name), decltype(&::nvmlDeviceGetName)>);
static_assert(std::is_same_v<decltype(&fake_utilization), decltype(&::nvmlDeviceGetUtilizationRates)>);
static_assert(std::is_same_v<decltype(&fake_memory), decltype(&::nvmlDeviceGetMemoryInfo)>);
static_assert(std::is_same_v<decltype(&fake_temperature), decltype(&::nvmlDeviceGetTemperature)>);
static_assert(std::is_same_v<decltype(&fake_power), decltype(&::nvmlDeviceGetPowerUsage)>);
static_assert(std::is_same_v<decltype(&fake_error), decltype(&::nvmlErrorString)>);

template<class T> void absent(const night::Reading<T>& reading, Quality quality, const std::string& message) {
    require(!reading.value && reading.quality == quality, message);
}
void tests() {
    const night::GpuReading defaults;
    absent(defaults.temperature_celsius, Quality::unsupported, "temperature defaults unsupported");
    absent(defaults.power_watts, Quality::unsupported, "power defaults unsupported");
    state = State{};
    {
        night::NvmlCollector collector;
        state.reentrant = &collector;
        const auto first = collector.collect(start);
        const auto& gpu = first.devices.at(0);
        require(first.stamp.sequence == 1 && first.stamp.begin == start && first.stamp.end >= start, "stamp");
        require(gpu.uuid.value == "GPU-identity-0" && gpu.name.value == "Fake NVIDIA 0", "UUID identity and display name");
        require(gpu.utilization_percent.value == 37 && gpu.utilization_percent.quality == Quality::valid, "driver utilization");
        require(gpu.total_bytes.value == 1024 && gpu.used_bytes.value == 200 && gpu.free_bytes.value == 824, "memory bytes, not bandwidth activity");
        require(gpu.temperature_celsius.value == 63 && gpu.temperature_celsius.quality == Quality::valid,
                "GPU temperature in degrees Celsius");
        require(gpu.power_watts.value == 125.375 && gpu.power_watts.quality == Quality::valid,
                "power milliwatts converted to fractional watts");
        require(first.stamp.issues.empty() && state.overlap && state.overlap->devices.empty() &&
                    state.overlap->stamp.quality == Quality::error, "overlap rejected without driver reentry");
        require(night::GpuSnapshot::utilization_window.find("1/6-1 s") != std::string_view::npos, "driver averaging-window label");
        collector.collect(start + 1s);
        require(state.inits == 1 && state.handles == 1, "session and handles reused");
        state.device_count = 2;
        require(collector.collect(start + 2s).devices.size() == 2 && state.handles == 3, "growth reenumerates all handles");
        state.device_count = 1;
        require(collector.collect(start + 3s).devices.size() == 1 && state.handles == 4, "shrink discards old handles");
    }
    require(state.shutdowns == 1 && state.closes == 1, "balanced teardown");

    state = State{};
    state.available = false;
    {
        night::NvmlCollector collector;
        const auto missing = collector.collect(start);
        require(missing.stamp.quality == Quality::unsupported && missing.devices.empty() && !missing.stamp.issues.empty(), "missing library issue");
        state.available = true;
        collector.collect(start + 4999ms);
        require(state.opens == 1 && state.inits == 0, "missing library backoff");
        require(collector.collect(start + 5s).devices.size() == 1 && state.opens == 2, "missing library recovery at deadline");
    }
    for (const auto* symbol : {"nvmlInit_v2", "nvmlShutdown", "nvmlDeviceGetCount_v2", "nvmlDeviceGetHandleByIndex_v2",
                              "nvmlDeviceGetUUID", "nvmlDeviceGetName", "nvmlDeviceGetUtilizationRates", "nvmlDeviceGetMemoryInfo", "nvmlErrorString"}) {
        state = State{};
        state.missing_symbol = symbol;
        night::NvmlCollector collector;
        const auto snapshot = collector.collect(start);
        require(snapshot.stamp.quality == Quality::unsupported && snapshot.stamp.issues[0].source == symbol &&
                    state.inits == 0 && state.closes == 1 && state.shutdowns == 0, "missing required symbol closes unloaded session");
        collector.collect(start + 1s);
        require(state.opens == 1, "missing symbol backoff");
    }
    for (const auto& [result, expected] : {std::pair{NVML_ERROR_NO_PERMISSION, Quality::permission_denied},
                                        std::pair{NVML_ERROR_DRIVER_NOT_LOADED, Quality::unsupported},
                                        std::pair{NVML_ERROR_UNKNOWN, Quality::error}}) {
        state = State{};
        state.init_result = result;
        night::NvmlCollector collector;
        require(collector.collect(start).stamp.quality == expected && state.shutdowns == 0 && state.closes == 1, "init error quality and no false shutdown");
        state.init_result = NVML_SUCCESS;
        collector.collect(start + 4999ms);
        require(state.inits == 1, "init failure backoff");
        require(collector.collect(start + 5s).devices.size() == 1 && state.inits == 2, "init failure recovery");
    }
    for (const auto& [result, expected] : {std::pair{NVML_ERROR_NO_PERMISSION, Quality::permission_denied},
                                        std::pair{NVML_ERROR_GPU_IS_LOST, Quality::error},
                                        std::pair{NVML_ERROR_UNKNOWN, Quality::error}}) {
        state = State{};
        state.count_result = result;
        night::NvmlCollector collector;
        const auto failed = collector.collect(start);
        require(failed.devices.empty() && failed.stamp.quality == expected &&
                    failed.stamp.issues.at(0).source == "nvmlDeviceGetCount_v2" &&
                    state.handles == 0 && state.shutdowns == 1 && state.closes == 1,
                "count errors close the session before any handle use");
        state.count_result = NVML_SUCCESS;
        collector.collect(start + 4999ms);
        require(state.counts == 1 && state.inits == 1, "count error backoff");
        require(collector.collect(start + 5s).devices.size() == 1 && state.inits == 2,
                "count error recovers with a fresh session");
    }
    for (const unsigned int count : {0U, 33U, 0xffffffffU}) {
        state = State{};
        state.device_count = count;
        night::NvmlCollector collector;
        const auto snapshot = collector.collect(start);
        require(snapshot.devices.empty() && !snapshot.stamp.issues.empty() && state.handles == 0 &&
                    state.shutdowns == 1, "zero or oversized enumeration is bounded and explicit");
        require(snapshot.stamp.quality == (count == 0 ? Quality::unsupported : Quality::error), "enumeration failure quality");
    }
    state = State{};
    state.device_count = 32;
    { night::NvmlCollector collector; require(collector.collect(start).devices.size() == 32 && state.handles == 32, "maximum supported count"); }

    state = State{};
    state.device_count = 2;
    state.devices[0].uuid = NVML_ERROR_NO_PERMISSION;
    state.devices[0].utilization = NVML_ERROR_NOT_SUPPORTED;
    state.devices[1].memory = NVML_ERROR_UNKNOWN;
    {
        night::NvmlCollector collector;
        const auto snapshot = collector.collect(start);
        absent(snapshot.devices[0].uuid, Quality::permission_denied, "denied identity stays absent");
        absent(snapshot.devices[0].utilization_percent, Quality::unsupported, "unsupported utilization stays absent");
        require(snapshot.devices[0].name.value && snapshot.devices[0].used_bytes.value == 200, "independent healthy fields survive");
        absent(snapshot.devices[1].used_bytes, Quality::error, "unknown memory failure");
        require(snapshot.devices[1].utilization_percent.value == 37 && snapshot.stamp.issues.size() == 3, "other device remains healthy");
    }
    state = State{};
    state.device_count = 2;
    state.devices[0].handle = NVML_ERROR_NO_PERMISSION;
    {
        night::NvmlCollector collector;
        const auto snapshot = collector.collect(start);
        absent(snapshot.devices[0].total_bytes, Quality::permission_denied, "denied handle does not expose garbage");
        require(snapshot.devices[1].used_bytes.value == 200, "denied handle does not hide another device");
        state.devices[0].handle = NVML_SUCCESS;
        require(collector.collect(start + 1s).devices[0].uuid.value == "GPU-identity-0", "failed handle can recover");
    }
    state = State{};
    state.devices[0].percent = 101;
    state.devices[0].bytes = {1024, 825, 200};
    state.devices[0].empty_uuid = true;
    state.devices[0].unterminated_name = true;
    {
        night::NvmlCollector collector;
        const auto snapshot = collector.collect(start);
        absent(snapshot.devices[0].utilization_percent, Quality::error, "out-of-range percent");
        absent(snapshot.devices[0].used_bytes, Quality::error, "inconsistent memory bounds");
        absent(snapshot.devices[0].uuid, Quality::error, "empty UUID is not identity");
        absent(snapshot.devices[0].name, Quality::error, "unterminated name rejected");
        state.devices[0] = Device{};
        state.devices[0].percent = 0;
        state.devices[0].bytes = {1024, 1024, 0};
        const auto empty = collector.collect(start + 1s);
        require(empty.devices[0].utilization_percent.value == 0 && empty.devices[0].used_bytes.value == 0, "real zero values remain valid");
    }
    for (const auto* symbol : {"nvmlDeviceGetTemperature", "nvmlDeviceGetPowerUsage"}) {
        state = State{};
        state.missing_symbol = symbol;
        night::NvmlCollector collector;
        const auto snapshot = collector.collect(start);
        const auto& gpu = snapshot.devices.at(0);
        const bool missing_temperature = state.missing_symbol == "nvmlDeviceGetTemperature";
        absent(missing_temperature ? gpu.temperature_celsius : gpu.power_watts, Quality::unsupported,
               "missing optional symbol stays unsupported");
        require((missing_temperature ? gpu.power_watts.value == 125.375 : gpu.temperature_celsius.value == 63) &&
                    gpu.utilization_percent.value == 37 && gpu.used_bytes.value == 200 &&
                    snapshot.stamp.quality == Quality::valid && snapshot.stamp.issues.size() == 1 &&
                    state.shutdowns == 0 && state.inits == 1,
                "optional symbol absence preserves mandatory and other sensor readings");
        require((missing_temperature ? state.temperatures : state.powers) == 0,
                "missing optional symbol never called");
    }
    for (const bool temperature_failure : {false, true}) {
        for (const auto& [result, expected] : {std::pair{NVML_ERROR_NOT_SUPPORTED, Quality::unsupported},
                                            std::pair{NVML_ERROR_NO_PERMISSION, Quality::permission_denied},
                                            std::pair{NVML_ERROR_UNKNOWN, Quality::error}}) {
            state = State{};
            auto& failure = temperature_failure ? state.devices[0].temperature : state.devices[0].power;
            failure = result;
            night::NvmlCollector collector;
            const auto snapshot = collector.collect(start);
            const auto& gpu = snapshot.devices.at(0);
            absent(temperature_failure ? gpu.temperature_celsius : gpu.power_watts, expected,
                   "optional sensor errors remain absent with exact quality");
            require((temperature_failure ? gpu.power_watts.value == 125.375 : gpu.temperature_celsius.value == 63) &&
                        gpu.utilization_percent.value == 37 && gpu.used_bytes.value == 200 &&
                        snapshot.stamp.quality == Quality::valid && snapshot.stamp.issues.size() == 1 && state.shutdowns == 0,
                    "sensor errors do not disable independent readings or session");
            failure = NVML_SUCCESS;
            const auto recovered = collector.collect(start + 1s);
            require(recovered.devices[0].temperature_celsius.value == 63 &&
                        recovered.devices[0].power_watts.value == 125.375 && recovered.stamp.issues.empty() && state.inits == 1,
                    "sensor quality recovers without reinitialization");
        }
    }
    state = State{};
    state.devices[0].celsius = std::numeric_limits<unsigned int>::max();
    state.devices[0].milliwatts = std::numeric_limits<unsigned int>::max();
    {
        night::NvmlCollector collector;
        const auto invalid = collector.collect(start);
        absent(invalid.devices[0].temperature_celsius, Quality::error, "unavailable temperature is not a measurement");
        absent(invalid.devices[0].power_watts, Quality::error, "unavailable power is not a measurement");
        state.devices[0].celsius = 0;
        state.devices[0].milliwatts = 0;
        const auto zero = collector.collect(start + 1s);
        require(zero.devices[0].temperature_celsius.value == 0 &&
                    zero.devices[0].temperature_celsius.quality == Quality::valid &&
                    zero.devices[0].power_watts.value == 0 && zero.devices[0].power_watts.quality == Quality::valid &&
                    zero.stamp.issues.empty(), "real zero sensor readings remain valid and clear prior failures");
    }
    for (const bool temperature_loss : {false, true}) {
        state = State{};
        state.device_count = 2;
        auto& failure = temperature_loss ? state.devices[0].temperature : state.devices[0].power;
        failure = NVML_ERROR_GPU_IS_LOST;
        night::NvmlCollector collector;
        const auto lost = collector.collect(start);
        const auto& gpu = lost.devices.at(0);
        absent(temperature_loss ? gpu.temperature_celsius : gpu.power_watts, Quality::error,
               "sensor GPU loss is absent rather than zero");
        if (temperature_loss) absent(gpu.power_watts, Quality::error, "power not called after temperature GPU loss");
        else require(gpu.temperature_celsius.value == 63, "earlier healthy temperature preserved on later loss");
        absent(lost.devices[1].temperature_celsius, Quality::error, "later device sensors skipped after GPU loss");
        require(gpu.used_bytes.value == 200 && lost.stamp.quality == Quality::error &&
                    state.temperatures == 1 && state.powers == (temperature_loss ? 0U : 1U) &&
                    state.shutdowns == 1 && state.closes == 1, "sensor loss immediately invalidates session");
        const auto queries = state.queries;
        failure = NVML_SUCCESS;
        collector.collect(start + 4999ms);
        require(state.queries == queries && state.inits == 1, "sensor loss obeys existing backoff");
        const auto recovered = collector.collect(start + 5s);
        require(recovered.devices[1].power_watts.value == 125.375 && recovered.stamp.issues.empty() && state.inits == 2,
                "sensor loss recovers with fresh session handles");
    }
    state = State{};
    state.device_count = 3;
    state.devices[1].utilization = NVML_ERROR_GPU_IS_LOST;
    {
        night::NvmlCollector collector;
        const auto lost = collector.collect(start);
        require(lost.devices[0].used_bytes.value == 200 && state.memories == 1 && state.shutdowns == 1 && state.closes == 1, "loss preserves earlier healthy data and invalidates session immediately");
        absent(lost.devices[1].utilization_percent, Quality::error, "lost GPU is not zero");
        absent(lost.devices[2].uuid, Quality::error, "no later handle is used after loss");
        const auto queries = state.queries;
        state.devices[1].utilization = NVML_SUCCESS;
        collector.collect(start + 4999ms);
        require(state.queries == queries && state.inits == 1, "lost session backoff");
        require(collector.collect(start + 5s).devices[2].uuid.value == "GPU-identity-2" && state.inits == 2, "fresh session handles after loss");
    }
    state = State{};
    state.device_count = 0;
    state.shutdown_result = NVML_ERROR_UNKNOWN;
    {
        night::NvmlCollector collector;
        const auto failure = collector.collect(start);
        require(failure.stamp.quality == Quality::error && failure.stamp.issues.back().source == "nvmlShutdown" &&
                    failure.stamp.issues.back().message.find("permanently disabled") != std::string::npos,
                "collection-triggered shutdown failure reported immediately");
        require(state.shutdowns == 1 && state.closes == 0, "failed shutdown retains the existing loader reference");
        state.shutdown_result = NVML_SUCCESS;
        state.device_count = 1;
        const auto counts = std::array{state.opens, state.symbols, state.closes, state.inits, state.shutdowns,
                                       state.counts, state.handles, state.queries, state.memories, state.errors};
        for (const auto offset : {1s, 5s, 100s}) {
            const auto disabled = collector.collect(start + offset);
            require(disabled.devices.empty() && disabled.stamp.quality == Quality::error &&
                        disabled.stamp.issues[0].source == "nvmlShutdown", "disabled state remains explicit");
        }
        require(counts == std::array{state.opens, state.symbols, state.closes, state.inits, state.shutdowns,
                                     state.counts, state.handles, state.queries, state.memories, state.errors},
                "permanent disable prevents every further loader and driver call");
    }
    require(state.shutdowns == 1 && state.closes == 0, "destructor does not retry failed shutdown or unload");
    state = State{};
    {
        night::NvmlCollector collector;
        require(collector.collect(start).devices.size() == 1, "healthy session before destructor-only failure");
        state.shutdown_result = NVML_ERROR_UNKNOWN;
    }
    require(state.shutdowns == 1 && state.closes == 0, "destructor-only failure retains one reference without retry");
}
} // namespace

extern "C" void* __wrap_dlopen(const char* path, int flags) {
    require(std::string_view{path} == "/usr/lib/libnvidia-ml.so.1" && flags == (RTLD_NOW | RTLD_LOCAL), "fixed system library and flags");
    ++state.opens;
    return state.available ? &library_token : nullptr;
}
extern "C" void* __wrap_dlsym(void* library, const char* symbol) {
    require(library == &library_token, "symbols use loaded system handle");
    ++state.symbols;
    if (state.missing_symbol == symbol) return nullptr;
    const std::string_view name{symbol};
    if (name == "nvmlInit_v2") return reinterpret_cast<void*>(&fake_init);
    if (name == "nvmlShutdown") return reinterpret_cast<void*>(&fake_shutdown);
    if (name == "nvmlDeviceGetCount_v2") return reinterpret_cast<void*>(&fake_count);
    if (name == "nvmlDeviceGetHandleByIndex_v2") return reinterpret_cast<void*>(&fake_handle);
    if (name == "nvmlDeviceGetUUID") return reinterpret_cast<void*>(&fake_uuid);
    if (name == "nvmlDeviceGetName") return reinterpret_cast<void*>(&fake_name);
    if (name == "nvmlDeviceGetUtilizationRates") return reinterpret_cast<void*>(&fake_utilization);
    if (name == "nvmlDeviceGetMemoryInfo") return reinterpret_cast<void*>(&fake_memory);
    if (name == "nvmlDeviceGetTemperature") return reinterpret_cast<void*>(&fake_temperature);
    if (name == "nvmlDeviceGetPowerUsage") return reinterpret_cast<void*>(&fake_power);
    if (name == "nvmlErrorString") return reinterpret_cast<void*>(&fake_error);
    throw std::runtime_error("GPU: unexpected symbol");
}
extern "C" int __wrap_dlclose(void* library) {
    require(library == &library_token && !state.initialized, "library closes only after shutdown");
    ++state.closes;
    return 0;
}

int main() {
    try { tests(); std::cout << "GPU contracts passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
