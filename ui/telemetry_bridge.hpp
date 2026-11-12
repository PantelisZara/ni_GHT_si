#pragma once

#include "process_model.hpp"
#include "process_view_model.hpp"
#include "applications_model.hpp"
#include "ni_ght_si/engine.hpp"
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace night::ui {
// Construct and use on the presentation thread. Engine must outlive the bridge.
// Only immutable latest slots are read here; collection remains on engine lanes.
class TelemetryBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(ProcessModel* processes READ processes CONSTANT)
    Q_PROPERTY(ProcessViewModel* processView READ processView CONSTANT)
    Q_PROPERTY(ApplicationsModel* applications READ applications CONSTANT)
    Q_PROPERTY(QVariantMap system READ system NOTIFY systemChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantMap configuration READ configuration NOTIFY configurationChanged)
    Q_PROPERTY(QString refreshMode READ refreshMode NOTIFY configurationChanged)
    Q_PROPERTY(QString systemQuality READ systemQuality NOTIFY updated)
    Q_PROPERTY(QString processQuality READ processQuality NOTIFY updated)
    Q_PROPERTY(qint64 systemAgeMs READ systemAgeMs NOTIFY updated)
    Q_PROPERTY(qint64 processAgeMs READ processAgeMs NOTIFY updated)
    Q_PROPERTY(QVariantList coreCpu READ coreCpu NOTIFY coreCpuChanged)
    Q_PROPERTY(QString coreCpuQuality READ coreCpuQuality NOTIFY updated)
    Q_PROPERTY(QString coreCpuMessage READ coreCpuMessage NOTIFY updated)
    Q_PROPERTY(qint64 coreCpuAgeMs READ coreCpuAgeMs NOTIFY updated)
    Q_PROPERTY(QVariantList networkRates READ networkRates NOTIFY ioChanged)
    Q_PROPERTY(QVariantList diskRates READ diskRates NOTIFY ioChanged)
    Q_PROPERTY(QVariantList filesystemCapacity READ filesystemCapacity NOTIFY capacityChanged)
    Q_PROPERTY(QString ioQuality READ ioQuality NOTIFY updated)
    Q_PROPERTY(QString capacityQuality READ capacityQuality NOTIFY updated)
    Q_PROPERTY(QString ioMessage READ ioMessage NOTIFY updated)
    Q_PROPERTY(QString capacityMessage READ capacityMessage NOTIFY updated)
    Q_PROPERTY(qint64 ioAgeMs READ ioAgeMs NOTIFY updated)
    Q_PROPERTY(qint64 capacityAgeMs READ capacityAgeMs NOTIFY updated)
    Q_PROPERTY(qulonglong ioIssueCount READ ioIssueCount NOTIFY updated)
    Q_PROPERTY(qulonglong capacityIssueCount READ capacityIssueCount NOTIFY updated)
    Q_PROPERTY(bool gpuEnabled READ gpuEnabled CONSTANT)
    Q_PROPERTY(QVariantList gpuDevices READ gpuDevices NOTIFY gpuDevicesChanged)
    Q_PROPERTY(QString gpuQuality READ gpuQuality NOTIFY updated)
    Q_PROPERTY(QString gpuMessage READ gpuMessage NOTIFY updated)
    Q_PROPERTY(qint64 gpuAgeMs READ gpuAgeMs NOTIFY updated)
    Q_PROPERTY(bool gpuPartial READ gpuPartial NOTIFY updated)
    Q_PROPERTY(qulonglong gpuIssueCount READ gpuIssueCount NOTIFY updated)
    Q_PROPERTY(QString gpuSamplingWindow READ gpuSamplingWindow CONSTANT)
public:
    explicit TelemetryBridge(Engine& engine, QObject* parent = nullptr);
    ProcessModel* processes() { return &processes_; }
    ProcessViewModel* processView() { return &process_view_; }
    ApplicationsModel* applications() { return &applications_; }
    QVariantMap system() const { return system_; }
    QVariantList history() const { return history_; }
    // presentationMs is the actual GUI polling interval; source cadences stay separate.
    QVariantMap configuration() const;
    QString refreshMode() const;
    Q_INVOKABLE bool setRefreshMode(const QString& mode);
    QString systemQuality() const { return system_quality_; }
    QString processQuality() const { return process_quality_; }
    qint64 systemAgeMs() const { return system_age_ms_; }
    qint64 processAgeMs() const { return process_age_ms_; }
    QVariantList coreCpu() const { return core_cpu_; }
    QString coreCpuQuality() const { return core_cpu_quality_; }
    QString coreCpuMessage() const { return core_cpu_message_; }
    qint64 coreCpuAgeMs() const { return core_cpu_age_ms_; }
    QVariantList networkRates() const { return network_rates_; }
    QVariantList diskRates() const { return disk_rates_; }
    QVariantList filesystemCapacity() const { return filesystem_capacity_; }
    QString ioQuality() const { return io_quality_; }
    QString capacityQuality() const { return capacity_quality_; }
    QString ioMessage() const { return io_message_; }
    QString capacityMessage() const { return capacity_message_; }
    qint64 ioAgeMs() const { return io_age_ms_; }
    qint64 capacityAgeMs() const { return capacity_age_ms_; }
    qulonglong ioIssueCount() const { return io_issue_count_; }
    qulonglong capacityIssueCount() const { return capacity_issue_count_; }
    bool gpuEnabled() const { return engine_.gpuEnabled(); }
    QVariantList gpuDevices() const { return gpu_devices_; }
    QString gpuQuality() const { return gpu_quality_; }
    QString gpuMessage() const { return gpu_message_; }
    qint64 gpuAgeMs() const { return gpu_age_ms_; }
    bool gpuPartial() const { return gpu_partial_; }
    qulonglong gpuIssueCount() const { return gpu_issue_count_; }
    QString gpuSamplingWindow() const;
    // Explicit time keeps age/stale transitions testable without wall-clock sleeps.
    void refreshAt(Clock::time_point now);
public slots:
    void refresh();
signals:
    void updated();
    void configurationChanged();
    void systemChanged();
    void historyChanged();
    void coreCpuChanged();
    void ioChanged();
    void capacityChanged();
    void gpuDevicesChanged();
private:
    Engine& engine_;
    EngineConfig presentation_config_;
    ProcessModel processes_;
    ProcessViewModel process_view_{processes_};
    ApplicationsModel applications_;
    QTimer timer_;
    QVariantMap system_;
    QVariantList history_;
    QString system_quality_{QStringLiteral("warming")}, process_quality_{QStringLiteral("warming")};
    qint64 system_age_ms_{-1}, process_age_ms_{-1};
    std::uint64_t system_sequence_{};
    std::optional<Clock::time_point> system_data_end_, process_data_end_;
    QVariantList core_cpu_;
    std::shared_ptr<const SystemSnapshot> core_cpu_snapshot_;
    QString core_cpu_quality_{QStringLiteral("warming")};
    QString core_cpu_message_, core_cpu_base_message_, core_cpu_timestamp_message_;
    qint64 core_cpu_age_ms_{-1};
    bool core_cpu_usable_{}, core_cpu_partial_{};
    Quality core_cpu_availability_{Quality::valid};
    QVariantList network_rates_, disk_rates_, filesystem_capacity_;
    std::shared_ptr<const IoSnapshot> io_snapshot_;
    std::shared_ptr<const CapacitySnapshot> capacity_snapshot_;
    QString io_quality_{QStringLiteral("warming")}, capacity_quality_{QStringLiteral("warming")};
    QString io_message_, capacity_message_, io_base_message_, capacity_base_message_;
    QString io_timestamp_message_, capacity_timestamp_message_;
    qint64 io_age_ms_{-1}, capacity_age_ms_{-1};
    bool io_usable_{}, capacity_usable_{};
    // Diagnostic counts combine source issues and presentation checks; they may describe the same failure.
    qulonglong io_issue_count_{}, capacity_issue_count_{}, io_field_issues_{}, capacity_field_issues_{};
    QVariantList gpu_devices_;
    std::shared_ptr<const GpuSnapshot> gpu_snapshot_;
    QString gpu_quality_;
    QString gpu_message_;
    qint64 gpu_age_ms_{-1};
    bool gpu_partial_{};
    bool gpu_usable_{};
    qulonglong gpu_issue_count_{};
};
} // namespace night::ui
