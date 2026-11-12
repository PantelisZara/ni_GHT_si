#include "telemetry_bridge.hpp"
#include "history_series.hpp"
#include <QThread>
#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <QStringList>
#include <type_traits>

namespace night::ui {
namespace {
int presentationInterval(const EngineConfig& config, const QString& mode) {
    if (mode == QStringLiteral("fast")) return 200;
    if (mode == QStringLiteral("balanced")) return 500;
    if (mode == QStringLiteral("low-power")) return 1000;
    const auto fastest = std::min({config.system_interval, config.process_interval, config.io_interval, config.gpu_interval});
    return static_cast<int>(std::clamp(fastest, std::chrono::milliseconds{200}, std::chrono::milliseconds{1000}).count());
}
QString qualityName(Quality quality) {
    switch (quality) {
    case Quality::valid: return QStringLiteral("valid");
    case Quality::warming: return QStringLiteral("warming");
    case Quality::stale: return QStringLiteral("stale");
    case Quality::permission_denied: return QStringLiteral("permission_denied");
    case Quality::unsupported: return QStringLiteral("unsupported");
    case Quality::error: return QStringLiteral("error");
    }
    return QStringLiteral("error");
}
template<class T> QVariantMap field(const Reading<T>& reading) {
    auto quality = reading.quality;
    QVariant value;
    if (quality == Quality::valid && reading.value) {
        if constexpr (std::is_floating_point_v<T>) {
            if (std::isfinite(*reading.value)) value = *reading.value;
            else quality = Quality::error;
        } else value = QVariant::fromValue<qulonglong>(*reading.value);
    } else if (quality == Quality::valid) quality = Quality::error;
    return {{QStringLiteral("value"), value}, {QStringLiteral("quality"), qualityName(quality)}};
}
qint64 age(const std::optional<Clock::time_point>& end, Clock::time_point now) {
    return end && now >= *end ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *end).count() : -1;
}
QVariantMap systemFields(const SystemSnapshot& sample) {
    QVariantMap result;
    const auto put = [&result](const QString& name, const auto& reading) { result.insert(name, field(reading)); };
    Reading<double> busy, iowait;
    for (const auto& cpu : sample.cpus) if (cpu.name == "cpu") {
        busy = cpu.busy_percent; iowait = cpu.iowait_percent; break;
    }
    put(QStringLiteral("cpuBusyPercent"), busy); put(QStringLiteral("cpuIowaitPercent"), iowait);
    put(QStringLiteral("totalBytes"), sample.total_bytes); put(QStringLiteral("availableBytes"), sample.available_bytes);
    put(QStringLiteral("usedBytes"), sample.used_bytes); put(QStringLiteral("swapTotalBytes"), sample.swap_total_bytes);
    put(QStringLiteral("swapUsedBytes"), sample.swap_used_bytes); put(QStringLiteral("uptimeSeconds"), sample.uptime_seconds);
    for (std::size_t i = 0; i < 3; ++i) {
        Reading<double> load{{}, sample.load_average.quality};
        if (sample.load_average.value) load.value = (*sample.load_average.value)[i];
        put(QStringLiteral("load%1").arg(i == 0 ? 1 : i == 1 ? 5 : 15), load);
    }
    return result;
}
QString displayText(std::string_view bytes) {
    const auto decoded = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    QString result;
    result.reserve(decoded.size());
    for (const QChar character : decoded) {
        const auto category = character.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            result += QStringLiteral("\\u%1").arg(static_cast<uint>(character.unicode()), 4, 16, QLatin1Char('0'));
        else result += character;
    }
    return result;
}
QVariantMap gpuText(const Reading<std::string>& reading, bool name) {
    auto quality = reading.quality;
    QVariant value;
    if (quality == Quality::valid) {
        if (!reading.value || reading.value->empty() || reading.value->size() > 4096) quality = Quality::error;
        else value = name ? displayText(*reading.value) : QString::fromUtf8(reading.value->data(), static_cast<qsizetype>(reading.value->size()));
    }
    return {{QStringLiteral("value"), value}, {QStringLiteral("quality"), qualityName(quality)}};
}
QString issueMessage(const Stamp& stamp, qsizetype message_limit = 864) {
    QString result;
    std::array<std::size_t, 4> indices{0, 1, 2, 3};
    if (stamp.issues.size() > 4) {
        // Loss and failed-shutdown summaries are appended after field errors.
        indices[2] = stamp.issues.size() - 2;
        indices[3] = stamp.issues.size() - 1;
    }
    for (std::size_t i = 0; i < std::min(stamp.issues.size(), std::size_t{4}); ++i) {
        const auto& issue = stamp.issues[indices[i]];
        const auto source = displayText(std::string_view{issue.source}.substr(0, 128)).left(128);
        const auto message = displayText(std::string_view{issue.message}.substr(0, 1024)).left(message_limit);
        if (!result.isEmpty()) result += QStringLiteral("; ");
        if (i == 2 && stamp.issues.size() > 4) result += QStringLiteral("…; ");
        if (!source.isEmpty()) result += source + QStringLiteral(": ");
        result += message;
    }
    return result.left(4096);
}
QString withReason(QString message, const QString& reason) {
    if (reason.isEmpty()) return message;
    const auto bounded_reason = reason.left(512);
    if (message.isEmpty()) return bounded_reason;
    return message.left(4096 - bounded_reason.size() - 2) + QStringLiteral("; ") + bounded_reason;
}
Quality presentationQuality(const Stamp& stamp, Clock::time_point now,
                            std::chrono::milliseconds maximum_age, const QString& domain, QString& message) {
    // A failed source keeps its own quality even if its timestamp is malformed.
    if (stamp.quality != Quality::valid && stamp.quality != Quality::stale) return stamp.quality;
    if (stamp.begin == Clock::time_point{} || stamp.end == Clock::time_point{}) {
        message = domain + QStringLiteral(" sample timestamp is missing");
        return Quality::warming;
    }
    if (stamp.end < stamp.begin) {
        message = domain + QStringLiteral(" sample timestamps are reversed");
        return Quality::error;
    }
    if (now < stamp.end) {
        message = domain + QStringLiteral(" sample timestamp is in the future");
        return Quality::error;
    }
    return freshness(stamp, now, maximum_age);
}
struct PresentationIssues {
    qulonglong count{};
    QStringList reasons;
    void add(const QString& reason) {
        ++count;
        if (!reasons.contains(reason)) reasons.push_back(reason);
    }
    QString message() const { return reasons.join(QStringLiteral("; ")).left(512); }
};
std::optional<std::uint64_t> coreId(const std::string& name) {
    if (name.size() < 4 || name.size() > 23 || !name.starts_with("cpu")) return {};
    const auto suffix = std::string_view{name}.substr(3);
    // Linux core names are canonical: cpu0 is valid, cpu01 is not an alias for cpu1.
    if (suffix.size() > 1 && suffix.front() == '0') return {};
    return unsigned_number(suffix);
}
Quality coreFieldQuality(const Reading<double>& reading) {
    if (reading.quality == Quality::valid && (!reading.value || !std::isfinite(*reading.value)
        || *reading.value < 0 || *reading.value > 100)) return Quality::error;
    return reading.quality;
}
Quality cpuAvailability(Quality left, Quality right) {
    // Aggregate fallback precedence: error, denied, unsupported, stale, warming, then valid.
    for (const auto quality : {Quality::error, Quality::permission_denied, Quality::unsupported,
                               Quality::stale, Quality::warming})
        if (left == quality || right == quality) return quality;
    return Quality::valid;
}
QVariantList coreCpuRows(const SystemSnapshot& sample, PresentationIssues& issues, Quality& availability) {
    constexpr std::size_t maximum_source_rows = 65;
    const auto count = std::min(sample.cpus.size(), maximum_source_rows);
    if (sample.cpus.size() > count) issues.add(QStringLiteral("Per-core CPU source rows truncated to 65"));
    std::array<std::optional<std::uint64_t>, maximum_source_rows> ids;
    std::map<std::uint64_t, std::size_t> occurrences;
    auto aggregate_quality = Quality::valid;
    for (std::size_t i = 0; i < count; ++i) {
        if (sample.cpus[i].name == "cpu") {
            aggregate_quality = cpuAvailability(aggregate_quality, cpuAvailability(
                coreFieldQuality(sample.cpus[i].busy_percent), coreFieldQuality(sample.cpus[i].iowait_percent)));
            continue;
        }
        ids[i] = coreId(sample.cpus[i].name);
        if (ids[i]) ++occurrences[*ids[i]];
        else issues.add(QStringLiteral("Per-core CPU rows with invalid canonical names omitted"));
    }
    if (occurrences.empty() && aggregate_quality != Quality::valid) {
        availability = aggregate_quality;
        issues.add(QStringLiteral("Per-core CPU unavailable: aggregate CPU fields are ") + qualityName(availability));
    }
    QVariantList rows;
    for (std::size_t i = 0; i < count; ++i) {
        if (!ids[i]) continue;
        if (occurrences[*ids[i]] > 1) {
            issues.add(QStringLiteral("Per-core CPU rows with duplicate IDs omitted"));
            continue;
        }
        if (rows.size() == 64) {
            issues.add(QStringLiteral("Per-core CPU rows truncated to 64"));
            continue;
        }
        const auto checked = [&issues](Reading<double> reading) {
            reading.quality = coreFieldQuality(reading);
            auto value = field(reading);
            if (value.value(QStringLiteral("quality")) != QStringLiteral("valid"))
                issues.add(QStringLiteral("Per-core CPU fields unavailable or invalid"));
            return value;
        };
        const auto& core = sample.cpus[i];
        rows.push_back(QVariantMap{{QStringLiteral("name"), QString::fromStdString(core.name)},
            {QStringLiteral("busyPercent"), checked(core.busy_percent)},
            {QStringLiteral("iowaitPercent"), checked(core.iowait_percent)}});
    }
    return rows;
}
QString rowLabel(const std::string& bytes) {
    return bytes.empty() || bytes.size() > 4096 ? QString{} : displayText(bytes).left(4096);
}
QVariantList throughputRows(const std::vector<Throughput>& incoming, const QString& domain,
                            PresentationIssues& issues) {
    const auto count = std::min(incoming.size(), std::size_t{64});
    if (incoming.size() > count) issues.add(domain + QStringLiteral(" rows truncated to 64"));
    std::map<QString, std::size_t> names;
    std::map<std::string_view, std::size_t> identities;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& source = incoming[i];
        const auto name = rowLabel(source.name);
        if (!name.isEmpty()) ++names[name];
        if (!source.identity.empty() && source.identity.size() <= 4096) ++identities[source.identity];
    }
    QVariantList rows;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& source = incoming[i];
        const auto name = rowLabel(source.name);
        if (name.isEmpty()) {
            issues.add(domain + QStringLiteral(" rows with empty or oversized names omitted"));
            continue;
        }
        const bool identity_valid = !source.identity.empty() && source.identity.size() <= 4096;
        if (names[name] > 1 || (identity_valid && identities[source.identity] > 1)) {
            issues.add(domain + QStringLiteral(" rows with duplicate names or identities omitted"));
            continue;
        }
        if (!identity_valid) issues.add(domain + QStringLiteral(" rate identity missing or oversized"));
        auto read = source.read_bytes_per_second, written = source.written_bytes_per_second;
        const auto checked = [identity_valid, &issues, &domain](Reading<double> reading) {
            if (reading.quality == Quality::valid && (!identity_valid || (reading.value
                && (!std::isfinite(*reading.value) || *reading.value < 0)))) reading = {{}, Quality::error};
            auto value = field(reading);
            if (value.value(QStringLiteral("quality")) != QStringLiteral("valid"))
                issues.add(domain + QStringLiteral(" rate fields unavailable or invalid"));
            return value;
        };
        rows.push_back(QVariantMap{{QStringLiteral("name"), name},
            {QStringLiteral("readBytesPerSecond"), checked(read)},
            {QStringLiteral("writtenBytesPerSecond"), checked(written)}});
    }
    return rows;
}
QVariantList capacityRows(const CapacitySnapshot& sample, PresentationIssues& issues) {
    const auto count = std::min(sample.mounts.size(), std::size_t{32});
    if (sample.mounts.size() > count) issues.add(QStringLiteral("Filesystem rows truncated to 32"));
    std::map<QString, std::size_t> paths;
    for (std::size_t i = 0; i < count; ++i) {
        const auto path = rowLabel(sample.mounts[i].path);
        if (!path.isEmpty()) ++paths[path];
    }
    QVariantList rows;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& source = sample.mounts[i];
        const auto path = rowLabel(source.path);
        if (path.isEmpty()) {
            issues.add(QStringLiteral("Filesystem rows with empty or oversized paths omitted"));
            continue;
        }
        if (paths[path] > 1) {
            issues.add(QStringLiteral("Filesystem rows with duplicate paths omitted"));
            continue;
        }
        auto total = source.total_bytes, used = source.used_bytes, available = source.available_bytes;
        if (total.quality == Quality::valid && used.quality == Quality::valid && available.quality == Quality::valid
            && total.value && used.value && available.value
            && (*used.value > *total.value || *available.value > *total.value - *used.value))
            total = used = available = {{}, Quality::error};
        const auto checked = [&issues](const Reading<std::uint64_t>& reading) {
            auto value = field(reading);
            if (value.value(QStringLiteral("quality")) != QStringLiteral("valid"))
                issues.add(QStringLiteral("Filesystem capacity fields unavailable or invalid"));
            return value;
        };
        rows.push_back(QVariantMap{{QStringLiteral("path"), path},
            {QStringLiteral("totalBytes"), checked(total)}, {QStringLiteral("usedBytes"), checked(used)},
            {QStringLiteral("availableBytes"), checked(available)}});
    }
    return rows;
}
QString gpuTimestampMessage(const Stamp& stamp, Clock::time_point now) {
    if (stamp.quality != Quality::valid) return {};
    if (stamp.begin == Clock::time_point{} || stamp.end == Clock::time_point{})
        return QStringLiteral("GPU sample timestamp is missing");
    if (stamp.end < stamp.begin) return QStringLiteral("GPU sample timestamps are reversed");
    if (now < stamp.end) return QStringLiteral("GPU sample timestamp is in the future");
    return {};
}
QVariantList gpuFields(const GpuSnapshot& sample, bool& partial, qulonglong& issue_count) {
    constexpr std::size_t maximum_devices = 32;
    QVariantList result;
    issue_count = static_cast<qulonglong>(sample.stamp.issues.size());
    if (sample.devices.size() > maximum_devices) ++issue_count;
    partial = issue_count != 0;
    for (std::size_t i = 0; i < std::min(sample.devices.size(), maximum_devices); ++i) {
        const auto& device = sample.devices[i];
        QVariantMap row;
        qulonglong field_issues = 0;
        const auto put = [&](const QString& key, QVariantMap value) {
            if (value.value(QStringLiteral("quality")) != QStringLiteral("valid")) ++field_issues;
            row.insert(key, std::move(value));
        };
        const auto uuid = gpuText(device.uuid, false);
        put(QStringLiteral("uuid"), uuid);
        if (uuid.value(QStringLiteral("value")).isValid())
            row.insert(QStringLiteral("identity"), QString::fromLatin1(QByteArray::fromStdString(*device.uuid.value).toHex()));
        put(QStringLiteral("name"), gpuText(device.name, true));
        auto utilization = device.utilization_percent;
        if (utilization.quality == Quality::valid && utilization.value
            && (*utilization.value < 0 || *utilization.value > 100)) utilization.quality = Quality::error;
        put(QStringLiteral("utilizationPercent"), field(utilization));
        auto total = device.total_bytes, used = device.used_bytes, free = device.free_bytes;
        if (total.quality == Quality::valid && used.quality == Quality::valid && free.quality == Quality::valid
            && total.value && used.value && free.value
            && (*total.value == 0 || *used.value > *total.value || *free.value != *total.value - *used.value)) {
            total = used = free = {{}, Quality::error};
        }
        put(QStringLiteral("totalBytes"), field(total));
        put(QStringLiteral("usedBytes"), field(used));
        put(QStringLiteral("freeBytes"), field(free));
        auto temperature = device.temperature_celsius, power = device.power_watts;
        if (temperature.value && *temperature.value < 0) temperature.quality = Quality::error;
        if (power.value && *power.value < 0) power.quality = Quality::error;
        put(QStringLiteral("temperatureCelsius"), field(temperature));
        put(QStringLiteral("powerWatts"), field(power));
        row.insert(QStringLiteral("partial"), field_issues != 0);
        row.insert(QStringLiteral("issueCount"), QVariant::fromValue(field_issues));
        partial = partial || field_issues != 0;
        result.push_back(std::move(row));
        // Field issues and source issues can describe the same failure. Keep the
        // source count when supplied; otherwise still expose unavailable fields.
        if (sample.stamp.issues.empty()) issue_count += field_issues;
    }
    return result;
}
}
TelemetryBridge::TelemetryBridge(Engine& engine, QObject* parent)
    : QObject(parent), engine_(engine), presentation_config_(engine.configuration()),
      gpu_quality_(engine.gpuEnabled() ? QStringLiteral("warming") : QStringLiteral("off")) {
    timer_.setInterval(presentationInterval(presentation_config_, refreshMode()));
    connect(&timer_, &QTimer::timeout, this, &TelemetryBridge::refresh);
    timer_.start();
}
void TelemetryBridge::refresh() { refreshAt(Clock::now()); }
QVariantMap TelemetryBridge::configuration() const {
    const auto& config = presentation_config_;
    return {{QStringLiteral("presentationMs"), timer_.interval()},
            {QStringLiteral("systemMs"), static_cast<qlonglong>(config.system_interval.count())},
            {QStringLiteral("processMs"), static_cast<qlonglong>(config.process_interval.count())},
            {QStringLiteral("ioMs"), static_cast<qlonglong>(config.io_interval.count())},
            {QStringLiteral("capacityMs"), static_cast<qlonglong>(config.capacity_interval.count())},
            {QStringLiteral("gpuMs"), static_cast<qlonglong>(config.gpu_interval.count())},
            {QStringLiteral("historyCapacity"), static_cast<qulonglong>(config.history_capacity)},
            {QStringLiteral("scopeMetadata"), config.include_application_scopes}};
}
QString TelemetryBridge::refreshMode() const {
    for (const auto& entry : {std::pair{RefreshMode::fast, QStringLiteral("fast")},
                              std::pair{RefreshMode::balanced, QStringLiteral("balanced")},
                              std::pair{RefreshMode::low_power, QStringLiteral("low-power")}}) {
        const auto preset = sampling_configuration(entry.first);
        if (presentation_config_.system_interval == preset.system_interval
            && presentation_config_.process_interval == preset.process_interval
            && presentation_config_.io_interval == preset.io_interval
            && presentation_config_.capacity_interval == preset.capacity_interval
            && presentation_config_.gpu_interval == preset.gpu_interval) return entry.second;
    }
    return QStringLiteral("custom");
}
bool TelemetryBridge::setRefreshMode(const QString& mode) {
    if (QThread::currentThread() != thread()) return false;
    RefreshMode preset;
    if (mode == QStringLiteral("fast")) preset = RefreshMode::fast;
    else if (mode == QStringLiteral("balanced")) preset = RefreshMode::balanced;
    else if (mode == QStringLiteral("low-power")) preset = RefreshMode::low_power;
    else return false;
    engine_.setRefreshMode(preset);
    refresh();
    return true;
}
QString TelemetryBridge::gpuSamplingWindow() const {
    return QString::fromUtf8(GpuSnapshot::utilization_window.data(), static_cast<qsizetype>(GpuSnapshot::utilization_window.size()));
}
void TelemetryBridge::refreshAt(Clock::time_point now) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto config = engine_.configuration();
    const bool configuration_changed = config.system_interval != presentation_config_.system_interval
        || config.process_interval != presentation_config_.process_interval || config.io_interval != presentation_config_.io_interval
        || config.capacity_interval != presentation_config_.capacity_interval || config.gpu_interval != presentation_config_.gpu_interval;
    presentation_config_ = config;
    const auto presentation_ms = presentationInterval(config, refreshMode());
    const bool presentation_changed = timer_.interval() != presentation_ms;
    if (presentation_changed) timer_.setInterval(presentation_ms);
    if (configuration_changed || presentation_changed) emit configurationChanged();
    const auto system = engine_.system();
    const auto process = engine_.processes();
    if (system && system->stamp.sequence > system_sequence_) {
        system_sequence_ = system->stamp.sequence;
        auto points = engine_.history();
        std::erase_if(points, [&](const auto& point) { return point.time > system->stamp.end; });
        history_ = historySeries(points);
        emit historyChanged();
        if (system->stamp.quality == Quality::valid) {
            auto fields = systemFields(*system);
            if (fields != system_) {
                system_ = std::move(fields);
                emit systemChanged();
            }
            system_data_end_ = system->stamp.end;
        }
    }
    if (process && processes_.apply(process) && processes_.quality() == QStringLiteral("valid"))
        process_data_end_ = process->stamp.end;
    const auto system_maximum_age = std::max(std::chrono::milliseconds{1500}, config.system_interval * 3);
    const auto process_maximum_age = std::max(std::chrono::milliseconds{1500}, config.process_interval * 3);
    applications_.apply(process);
    applications_.refreshAge(now, process_maximum_age);
    system_quality_ = system ? qualityName(freshness(system->stamp, now, system_maximum_age)) : QStringLiteral("warming");
    process_quality_ = process ? qualityName(freshness(process->stamp, now, process_maximum_age)) : QStringLiteral("warming");
    if (processes_.quality() != QStringLiteral("valid")) process_quality_ = processes_.quality();
    system_age_ms_ = age(system_data_end_, now); process_age_ms_ = age(process_data_end_, now);
    const bool new_core_cpu = system && (!core_cpu_snapshot_ || system->stamp.sequence > core_cpu_snapshot_->stamp.sequence);
    if (new_core_cpu) core_cpu_snapshot_ = system;
    if (core_cpu_snapshot_) {
        const auto& stamp = core_cpu_snapshot_->stamp;
        QString timestamp_message;
        const auto quality = presentationQuality(stamp, now, system_maximum_age, QStringLiteral("Per-core CPU"), timestamp_message);
        const bool usable = quality == Quality::valid || quality == Quality::stale;
        if (new_core_cpu || usable != core_cpu_usable_) {
            PresentationIssues issues;
            core_cpu_availability_ = Quality::valid;
            auto cores = usable ? coreCpuRows(*core_cpu_snapshot_, issues, core_cpu_availability_) : QVariantList{};
            core_cpu_partial_ = issues.count != 0;
            // CPU field quality and omissions describe this view; unrelated memory/load issues do not.
            core_cpu_base_message_ = usable ? issues.message() : issueMessage(stamp, 700);
            if (cores != core_cpu_) {
                core_cpu_ = std::move(cores);
                emit coreCpuChanged();
            }
        }
        if (new_core_cpu || usable != core_cpu_usable_ || timestamp_message != core_cpu_timestamp_message_) {
            core_cpu_message_ = withReason(core_cpu_base_message_, timestamp_message);
            core_cpu_timestamp_message_ = timestamp_message;
        }
        core_cpu_usable_ = usable;
        const auto core_quality = usable && core_cpu_availability_ != Quality::valid ? core_cpu_availability_ : quality;
        core_cpu_age_ms_ = core_quality == Quality::valid || core_quality == Quality::stale ? age(stamp.end, now) : -1;
        core_cpu_quality_ = core_quality == Quality::valid && core_cpu_partial_ ? QStringLiteral("partial") : qualityName(core_quality);
    } else core_cpu_message_ = QStringLiteral("Waiting for per-core CPU sample");
    const auto io = engine_.io();
    const bool new_io = io && (!io_snapshot_ || io->stamp.sequence > io_snapshot_->stamp.sequence);
    if (new_io) io_snapshot_ = io;
    if (io_snapshot_) {
        const auto& stamp = io_snapshot_->stamp;
        QString timestamp_message;
        const auto maximum_age = std::max(std::chrono::milliseconds{1500}, config.io_interval * 3);
        const auto quality = presentationQuality(stamp, now, maximum_age, QStringLiteral("I/O"), timestamp_message);
        const bool usable = quality == Quality::valid || quality == Quality::stale;
        if (new_io || usable != io_usable_) {
            QVariantList network, disks;
            PresentationIssues issues;
            if (usable) {
                network = throughputRows(io_snapshot_->network, QStringLiteral("Network"), issues);
                disks = throughputRows(io_snapshot_->disks, QStringLiteral("Disk"), issues);
            }
            io_field_issues_ = issues.count;
            io_base_message_ = withReason(issueMessage(stamp, 700), issues.message());
            if (network != network_rates_ || disks != disk_rates_) {
                network_rates_ = std::move(network);
                disk_rates_ = std::move(disks);
                emit ioChanged();
            }
        }
        if (new_io || usable != io_usable_ || timestamp_message != io_timestamp_message_) {
            io_message_ = withReason(io_base_message_, timestamp_message);
            io_timestamp_message_ = timestamp_message;
        }
        io_usable_ = usable;
        io_age_ms_ = usable ? age(stamp.end, now) : -1;
        io_issue_count_ = static_cast<qulonglong>(stamp.issues.size()) + io_field_issues_
            + (timestamp_message.isEmpty() ? 0ULL : 1ULL);
        io_quality_ = quality == Quality::valid && io_issue_count_ ? QStringLiteral("partial") : qualityName(quality);
    } else io_message_ = QStringLiteral("Waiting for I/O sample");
    const auto capacity = engine_.capacity();
    const bool new_capacity = capacity && (!capacity_snapshot_ || capacity->stamp.sequence > capacity_snapshot_->stamp.sequence);
    if (new_capacity) capacity_snapshot_ = capacity;
    if (capacity_snapshot_) {
        const auto& stamp = capacity_snapshot_->stamp;
        QString timestamp_message;
        const auto maximum_age = std::max(std::chrono::milliseconds{1500}, config.capacity_interval * 3);
        const auto quality = presentationQuality(stamp, now, maximum_age, QStringLiteral("Filesystem"), timestamp_message);
        const bool usable = quality == Quality::valid || quality == Quality::stale;
        if (new_capacity || usable != capacity_usable_) {
            PresentationIssues issues;
            auto mounts = usable ? capacityRows(*capacity_snapshot_, issues) : QVariantList{};
            capacity_field_issues_ = issues.count;
            capacity_base_message_ = withReason(issueMessage(stamp, 700), issues.message());
            if (mounts != filesystem_capacity_) {
                filesystem_capacity_ = std::move(mounts);
                emit capacityChanged();
            }
        }
        if (new_capacity || usable != capacity_usable_ || timestamp_message != capacity_timestamp_message_) {
            capacity_message_ = withReason(capacity_base_message_, timestamp_message);
            capacity_timestamp_message_ = timestamp_message;
        }
        capacity_usable_ = usable;
        capacity_age_ms_ = usable ? age(stamp.end, now) : -1;
        capacity_issue_count_ = static_cast<qulonglong>(stamp.issues.size()) + capacity_field_issues_
            + (timestamp_message.isEmpty() ? 0ULL : 1ULL);
        capacity_quality_ = quality == Quality::valid && capacity_issue_count_ ? QStringLiteral("partial") : qualityName(quality);
    } else capacity_message_ = QStringLiteral("Waiting for filesystem sample");
    if (gpuEnabled()) {
        const auto gpu = engine_.gpu();
        const bool new_sample = gpu && (!gpu_snapshot_ || gpu->stamp.sequence > gpu_snapshot_->stamp.sequence);
        if (new_sample) gpu_snapshot_ = gpu;
        if (gpu_snapshot_) {
            const auto& stamp = gpu_snapshot_->stamp;
            if (new_sample) gpu_message_ = issueMessage(stamp);
            if (stamp.issues.empty()) gpu_message_ = gpuTimestampMessage(stamp, now);
            // Three configured polls tolerate scheduling jitter at both the
            // 10 ms and one-hour Engine interval limits without a fixed cutoff.
            auto quality = freshness(stamp, now, config.gpu_interval * 3);
            if (stamp.quality == Quality::valid && stamp.begin == Clock::time_point{}
                && quality != Quality::error) quality = Quality::warming;
            const bool usable = quality == Quality::valid || quality == Quality::stale;
            gpu_age_ms_ = usable ? age(stamp.end, now) : -1;
            if (new_sample || usable != gpu_usable_) {
                QVariantList devices;
                gpu_partial_ = false;
                gpu_issue_count_ = static_cast<qulonglong>(stamp.issues.size());
                if (usable) devices = gpuFields(*gpu_snapshot_, gpu_partial_, gpu_issue_count_);
                if (devices != gpu_devices_) {
                    gpu_devices_ = std::move(devices);
                    emit gpuDevicesChanged();
                }
            }
            gpu_usable_ = usable;
            gpu_quality_ = quality == Quality::valid && gpu_partial_ ? QStringLiteral("partial") : qualityName(quality);
        }
    }
    emit updated();
}
} // namespace night::ui
