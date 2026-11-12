#pragma once

#include "ni_ght_si/common.hpp"

#include <memory>

namespace night {
struct GpuReading {
    Reading<std::string> uuid, name;
    Reading<double> utilization_percent;
    Reading<std::uint64_t> total_bytes, used_bytes, free_bytes;
    Reading<double> temperature_celsius{{}, Quality::unsupported};
    Reading<double> power_watts{{}, Quality::unsupported};
};

struct GpuSnapshot {
    Stamp stamp;
    std::vector<GpuReading> devices;
    // The driver's averaging window is independent of the caller's poll interval.
    static constexpr std::string_view utilization_window = "NVIDIA driver sample window: 1/6-1 s";
};

// Optional Linux collector. Calls are synchronous and may block in the driver;
// the caller owns scheduling and must serialize destruction with collection.
// Overlapping collect calls are rejected. No device settings are changed.
// A failed shutdown permanently disables this collector and retains its one
// loader reference until process exit because vendor cleanup is unconfirmed.
class NvmlCollector {
public:
    NvmlCollector();
    ~NvmlCollector();
    NvmlCollector(const NvmlCollector&) = delete;
    NvmlCollector& operator=(const NvmlCollector&) = delete;
    GpuSnapshot collect(Clock::time_point now = Clock::now());

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace night
