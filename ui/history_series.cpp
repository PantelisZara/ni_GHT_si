#include "history_series.hpp"
#include <cmath>

namespace night::ui {
QVariantList historySeries(const std::vector<HistoryPoint>& points) {
    constexpr std::size_t maximum = 2400;
    const auto first = points.size() > maximum ? points.size() - maximum : 0;
    QVariantList result;
    result.reserve(static_cast<qsizetype>(points.size() - first));
    Clock::time_point previous{};
    for (std::size_t i = first; i < points.size(); ++i) {
        const auto& point = points[i];
        QVariant cpu, available;
        const bool valid_time = point.time != Clock::time_point{} && (i == first || point.time > previous);
        if (point.quality == Quality::valid && valid_time) {
            for (const auto& reading : point.cpus) if (reading.name == "cpu") {
                if (reading.busy_percent.quality == Quality::valid && reading.busy_percent.value
                    && std::isfinite(*reading.busy_percent.value)
                    && *reading.busy_percent.value >= 0 && *reading.busy_percent.value <= 100)
                    cpu = *reading.busy_percent.value;
                break;
            }
            if (point.available_bytes.quality == Quality::valid && point.available_bytes.value)
                available = static_cast<double>(*point.available_bytes.value) / 1073741824.0;
        }
        QVariant time;
        if (valid_time) {
            time = static_cast<qlonglong>(std::chrono::duration_cast<std::chrono::milliseconds>(point.time.time_since_epoch()).count());
            previous = point.time;
        }
        result.push_back(QVariantMap{{QStringLiteral("timeMs"), time},
            {QStringLiteral("cpu"), cpu}, {QStringLiteral("availableGiB"), available}});
    }
    return result;
}
}
