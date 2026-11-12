#pragma once

#include "ni_ght_si/process_control.hpp"
#include "ni_ght_si/application_control.hpp"
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace night::ui {

// One bounded background executor owns all action handles. Bulk rows only seed
// a fresh preparation; they never carry confirmation or signal authority.
class ProcessController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool awaitingConfirmation READ awaitingConfirmation NOTIFY changed)
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY changed)
    Q_PROPERTY(QVariantMap candidate READ candidate NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
public:
    using Preparer = std::function<ProcessActionPreparation(std::uint64_t, ProcessAction)>;
    explicit ProcessController(QObject* parent = nullptr);
    // C++ test seam only. Production construction always uses manager authority.
    explicit ProcessController(Preparer preparer, QObject* parent = nullptr);
    ~ProcessController() override;
    bool busy() const noexcept { return busy_; }
    bool awaitingConfirmation() const noexcept { return awaiting_; }
    bool cancellable() const noexcept { return awaiting_ || (dispatch_gate_ && (group_dispatch_ ? dispatch_gate_->load() < 2 : dispatch_gate_->load() == 0)); }
    QVariantMap candidate() const { return display_; }
    QString message() const { return message_; }
    Q_INVOKABLE void prepare(const QString& pid, bool forceKill = false);
    Q_INVOKABLE void prepareApplication(const QString& seedPid);
    Q_INVOKABLE void confirm(const QString& generation);
    Q_INVOKABLE void cancel();
signals:
    void changed();
private:
    void enqueue(std::function<void()> operation);
    void showPreparation(ProcessActionResult result, QVariantMap display);
    void observe();
    Preparer preparer_;
    std::shared_ptr<std::atomic_int> dispatch_gate_; // 0 pending, 1 committed, 2 cancelled
    std::optional<ApplicationActionCandidate> application_; // worker thread only
    std::optional<ProcessActionCandidate> retained_; // worker thread only
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<std::function<void()>> pending_;
    std::jthread worker_;
    QTimer observation_, expiry_;
    Clock::time_point deadline_{};
    int observations_{};
    bool busy_{}, awaiting_{}, group_dispatch_{};
    QVariantMap display_;
    QString message_;
};
} // namespace night::ui
