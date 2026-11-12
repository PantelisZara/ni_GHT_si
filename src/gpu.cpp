// SDK-required NVIDIA notice reproduced from the installed nvml.h header.
/*
 * Copyright 1993-2026 NVIDIA Corporation.  All rights reserved.
 *
 * NOTICE TO USER:
 *
 * This source code is subject to NVIDIA ownership rights under U.S. and
 * international Copyright laws.  Users and possessors of this source code
 * are hereby granted a nonexclusive, royalty-free license to use this code
 * in individual and commercial software.
 *
 * NVIDIA MAKES NO REPRESENTATION ABOUT THE SUITABILITY OF THIS SOURCE
 * CODE FOR ANY PURPOSE.  IT IS PROVIDED "AS IS" WITHOUT EXPRESS OR
 * IMPLIED WARRANTY OF ANY KIND.  NVIDIA DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOURCE CODE, INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
 * IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL,
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS,  WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION,  ARISING OUT OF OR IN CONNECTION WITH THE USE
 * OR PERFORMANCE OF THIS SOURCE CODE.
 *
 * U.S. Government End Users.   This source code is a "commercial item" as
 * that term is defined at  48 C.F.R. 2.101 (OCT 1995), consisting  of
 * "commercial computer  software"  and "commercial computer software
 * documentation" as such terms are  used in 48 C.F.R. 12.212 (SEPT 1995)
 * and is provided to the U.S. Government only as a commercial end item.
 * Consistent with 48 C.F.R.12.212 and 48 C.F.R. 227.7202-1 through
 * 227.7202-4 (JUNE 1995), all U.S. Government End Users acquire the
 * source code with only those rights set forth herein.
 *
 * Any use of this source code in individual and commercial software must
 * include, in the user documentation and internal comments to the code,
 * the above Disclaimer and U.S. Government End Users Notice.
 */
#include "ni_ght_si/gpu.hpp"

#include <nvml.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace night {
namespace {
constexpr unsigned int maximum_devices = 32;
constexpr auto retry_delay = std::chrono::seconds{5};

Quality quality(nvmlReturn_t result) {
    switch (result) {
    case NVML_ERROR_NO_PERMISSION: case NVML_ERROR_OPERATING_SYSTEM:
        return Quality::permission_denied;
    case NVML_ERROR_NOT_SUPPORTED: case NVML_ERROR_DRIVER_NOT_LOADED:
    case NVML_ERROR_GPU_NOT_FOUND: case NVML_ERROR_LIBRARY_NOT_FOUND:
    case NVML_ERROR_FUNCTION_NOT_FOUND: return Quality::unsupported;
    default: return Quality::error;
    }
}

GpuReading absent_device(Quality state) {
    return {{{}, state}, {{}, state}, {{}, state}, {{}, state}, {{}, state}, {{}, state},
            {{}, state}, {{}, state}};
}
} // namespace

struct NvmlCollector::Impl {
    void* library{};
    bool initialized{};
    nvmlReturn_t shutdown_failure{NVML_SUCCESS};
    std::vector<nvmlDevice_t> handles;
    std::optional<Clock::time_point> retry_at;
    Issue unavailable{Quality::unsupported, "NVML", "NVIDIA telemetry unavailable"};
    std::atomic<std::uint64_t> sequence{};
    std::atomic_flag collecting = ATOMIC_FLAG_INIT;
    decltype(&::nvmlInit_v2) init{};
    decltype(&::nvmlShutdown) shutdown{};
    decltype(&::nvmlDeviceGetCount_v2) count{};
    decltype(&::nvmlDeviceGetHandleByIndex_v2) handle{};
    decltype(&::nvmlDeviceGetUUID) uuid{};
    decltype(&::nvmlDeviceGetName) name{};
    decltype(&::nvmlDeviceGetUtilizationRates) utilization{};
    decltype(&::nvmlDeviceGetMemoryInfo) memory{};
    decltype(&::nvmlDeviceGetTemperature) temperature{};
    decltype(&::nvmlDeviceGetPowerUsage) power{};
    decltype(&::nvmlErrorString) error_string{};

    ~Impl() { close(); }
    void close() {
        handles.clear();
        if (shutdown_failure != NVML_SUCCESS) return;
        if (initialized) shutdown_failure = shutdown();
        initialized = false;
        // On failure, cleanup is unconfirmed. Retain this one loader reference
        // until process exit, without retrying shutdown or reinitializing.
        if (library && shutdown_failure == NVML_SUCCESS) ::dlclose(library);
        library = nullptr;
        init = nullptr; shutdown = nullptr; count = nullptr; handle = nullptr;
        uuid = nullptr; name = nullptr; utilization = nullptr; memory = nullptr;
        temperature = nullptr; power = nullptr; error_string = nullptr;
    }
    Issue error(nvmlReturn_t result, std::string source) const {
        const auto message = error_string ? error_string(result) : nullptr;
        return {quality(result), std::move(source), message ? message : "NVML error " + std::to_string(result)};
    }
    void suspend(Issue issue, Clock::time_point now, GpuSnapshot* snapshot = nullptr) {
        unavailable = std::move(issue);
        close();
        if (shutdown_failure == NVML_SUCCESS) retry_at = now + retry_delay;
        else {
            retry_at.reset();
            unavailable = {Quality::error, "nvmlShutdown",
                "shutdown failed (NVML status " + std::to_string(shutdown_failure) +
                "); collector permanently disabled; driver resources unknown; library retained until process exit"};
            if (snapshot) {
                snapshot->stamp.quality = Quality::error;
                snapshot->stamp.issues.push_back(unavailable);
            }
        }
    }
    template<class Function> bool bind(Function& function, const char* symbol) {
        function = reinterpret_cast<Function>(::dlsym(library, symbol));
        if (!function) unavailable = {Quality::unsupported, symbol, "required NVML symbol missing"};
        return function != nullptr;
    }
    bool open(Clock::time_point now) {
        if (shutdown_failure != NVML_SUCCESS) return false;
        if (initialized) return true;
        if (retry_at && now < *retry_at) return false;
        library = ::dlopen("/usr/lib/libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            suspend({Quality::unsupported, "/usr/lib/libnvidia-ml.so.1", "NVML library missing or cannot be loaded"}, now);
            return false;
        }
        if (!bind(init, "nvmlInit_v2") || !bind(shutdown, "nvmlShutdown") ||
            !bind(count, "nvmlDeviceGetCount_v2") || !bind(handle, "nvmlDeviceGetHandleByIndex_v2") ||
            !bind(uuid, "nvmlDeviceGetUUID") || !bind(name, "nvmlDeviceGetName") ||
            !bind(utilization, "nvmlDeviceGetUtilizationRates") || !bind(memory, "nvmlDeviceGetMemoryInfo") ||
            !bind(error_string, "nvmlErrorString")) {
            suspend(unavailable, now);
            return false;
        }
        // Optional sensors must not disable utilization or memory on older drivers.
        temperature = reinterpret_cast<decltype(temperature)>(::dlsym(library, "nvmlDeviceGetTemperature"));
        power = reinterpret_cast<decltype(power)>(::dlsym(library, "nvmlDeviceGetPowerUsage"));
        const auto result = init();
        if (result != NVML_SUCCESS) {
            suspend(error(result, "nvmlInit_v2"), now);
            return false;
        }
        initialized = true;
        retry_at.reset();
        return true;
    }

    template<std::size_t Size, class Function>
    Reading<std::string> text(nvmlDevice_t device, Function function, const std::string& source,
                              GpuSnapshot& snapshot, bool& lost) {
        if (lost) return {{}, Quality::error};
        std::array<char, Size> buffer;
        buffer.fill('\x7f');
        const auto result = function(device, buffer.data(), static_cast<unsigned int>(buffer.size()));
        if (result != NVML_SUCCESS) {
            auto issue = error(result, source);
            snapshot.stamp.issues.push_back(issue);
            lost = result == NVML_ERROR_GPU_IS_LOST;
            return {{}, issue.quality};
        }
        const auto end = std::find(buffer.begin(), buffer.end(), '\0');
        if (end == buffer.end() || end == buffer.begin()) {
            snapshot.stamp.issues.push_back({Quality::error, source, "empty or unterminated NVML string"});
            return {{}, Quality::error};
        }
        return {std::string{buffer.begin(), end}, Quality::valid};
    }

    template<class Function, class... Arguments>
    Reading<double> sensor(Function function, const std::string& source, double divisor,
                           GpuSnapshot& snapshot, bool& lost, Arguments... arguments) {
        if (lost) return {{}, Quality::error};
        if (!function) {
            snapshot.stamp.issues.push_back({Quality::unsupported, source, "optional NVML sensor symbol missing"});
            return {{}, Quality::unsupported};
        }
        unsigned int value{};
        const auto status = function(arguments..., &value);
        const double converted = static_cast<double>(value) / divisor;
        if (status == NVML_SUCCESS && value != std::numeric_limits<unsigned int>::max() &&
            std::isfinite(converted)) return {converted, Quality::valid};
        auto issue = status == NVML_SUCCESS ?
            Issue{Quality::error, source, "invalid NVML sensor value"} : error(status, source);
        snapshot.stamp.issues.push_back(issue);
        lost = status == NVML_ERROR_GPU_IS_LOST;
        return {{}, issue.quality};
    }

    void sample(GpuSnapshot& snapshot, Clock::time_point now) {
        if (!open(now)) {
            snapshot.stamp.quality = unavailable.quality;
            snapshot.stamp.issues.push_back(unavailable);
            return;
        }
        unsigned int device_count{};
        const auto result = count(&device_count);
        if (result != NVML_SUCCESS || device_count > maximum_devices) {
            auto issue = result != NVML_SUCCESS ? error(result, "nvmlDeviceGetCount_v2") :
                Issue{Quality::error, "nvmlDeviceGetCount_v2", "device count exceeds bound of 32"};
            snapshot.stamp.quality = issue.quality;
            snapshot.stamp.issues.push_back(issue);
            suspend(std::move(issue), now, &snapshot);
            return;
        }
        if (device_count == 0) {
            const Issue issue{Quality::unsupported, "NVML", "no NVIDIA devices found"};
            snapshot.stamp.quality = issue.quality;
            snapshot.stamp.issues.push_back(issue);
            suspend(issue, now, &snapshot);
            return;
        }
        if (handles.size() != device_count) handles.assign(device_count, nullptr);
        bool lost{};
        for (unsigned int index = 0; index < device_count; ++index) {
            auto device_reading = absent_device(Quality::error);
            const auto source = "NVML device " + std::to_string(index);
            if (!lost && !handles[index]) {
                const auto status = handle(index, &handles[index]);
                if (status != NVML_SUCCESS || !handles[index]) {
                    auto issue = error(status == NVML_SUCCESS ? NVML_ERROR_UNKNOWN : status, source + " handle");
                    snapshot.stamp.issues.push_back(issue);
                    device_reading = absent_device(issue.quality);
                    handles[index] = nullptr;
                    lost = status == NVML_ERROR_GPU_IS_LOST;
                }
            }
            if (!lost && handles[index]) {
                device_reading.uuid = text<NVML_DEVICE_UUID_BUFFER_SIZE>(handles[index], uuid, source + " UUID", snapshot, lost);
                device_reading.name = text<NVML_DEVICE_NAME_BUFFER_SIZE>(handles[index], name, source + " name", snapshot, lost);
                if (!lost) {
                    nvmlUtilization_t value{};
                    const auto status = utilization(handles[index], &value);
                    if (status == NVML_SUCCESS && value.gpu <= 100)
                        device_reading.utilization_percent = {static_cast<double>(value.gpu), Quality::valid};
                    else {
                        auto issue = status == NVML_SUCCESS ?
                            Issue{Quality::error, source + " utilization", "GPU utilization exceeds 100 percent"} :
                            error(status, source + " utilization");
                        device_reading.utilization_percent = {{}, issue.quality};
                        snapshot.stamp.issues.push_back(std::move(issue));
                        lost = status == NVML_ERROR_GPU_IS_LOST;
                    }
                }
                if (!lost) {
                    nvmlMemory_t value{};
                    const auto status = memory(handles[index], &value);
                    if (status == NVML_SUCCESS && value.total != 0 && value.used <= value.total &&
                        value.free == value.total - value.used) {
                        device_reading.total_bytes = {value.total, Quality::valid};
                        device_reading.used_bytes = {value.used, Quality::valid};
                        device_reading.free_bytes = {value.free, Quality::valid};
                    } else {
                        auto issue = status == NVML_SUCCESS ?
                            Issue{Quality::error, source + " memory", "invalid NVML memory bounds"} :
                            error(status, source + " memory");
                        device_reading.total_bytes = {{}, issue.quality};
                        device_reading.used_bytes = {{}, issue.quality};
                        device_reading.free_bytes = {{}, issue.quality};
                        snapshot.stamp.issues.push_back(std::move(issue));
                        lost = status == NVML_ERROR_GPU_IS_LOST;
                    }
                }
                device_reading.temperature_celsius = sensor(temperature, source + " temperature", 1.0,
                    snapshot, lost, handles[index], NVML_TEMPERATURE_GPU);
                // NVML includes associated circuitry; averaging depends on GPU architecture.
                device_reading.power_watts = sensor(power, source + " power", 1000.0,
                    snapshot, lost, handles[index]);
            }
            snapshot.devices.push_back(std::move(device_reading));
        }
        if (lost) {
            const Issue issue{Quality::error, "NVML", "GPU lost; session invalidated, retry after 5 seconds"};
            snapshot.stamp.quality = issue.quality;
            snapshot.stamp.issues.push_back(issue);
            suspend(issue, now, &snapshot);
        }
    }
};

NvmlCollector::NvmlCollector() : impl_{std::make_unique<Impl>()} {}
NvmlCollector::~NvmlCollector() = default;
GpuSnapshot NvmlCollector::collect(Clock::time_point now) {
    const auto begin = Clock::now();
    GpuSnapshot snapshot;
    snapshot.stamp.sequence = impl_->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    snapshot.stamp.begin = now;
    if (impl_->collecting.test_and_set(std::memory_order_acquire)) {
        snapshot.stamp.quality = Quality::error;
        snapshot.stamp.issues.push_back({Quality::error, "NVML", "collection already in progress"});
    } else {
        struct Release {
            std::atomic_flag& flag;
            ~Release() { flag.clear(std::memory_order_release); }
        } release{impl_->collecting};
        impl_->sample(snapshot, now);
    }
    snapshot.stamp.end = now + (Clock::now() - begin);
    return snapshot;
}
} // namespace night
