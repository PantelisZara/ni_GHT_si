#include "process_controller.hpp"
#include "application_authority.hpp"
#include <QMetaObject>
#include <limits>

namespace night::ui {
namespace {
QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString displayName(const std::string& value) {
    auto result = text(value);
    for (auto& c : result) if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format) c = QLatin1Char('?');
    return result;
}
QVariantMap describe(const ProcessActionCandidate& candidate) {
    const auto& context = candidate.context();
    const auto& stat = context.credentials.stat;
    const auto& executable = context.executable.executable;
    return {{QStringLiteral("pid"), QString::number(stat.pid)},
            {QStringLiteral("name"), displayName(stat.name)},
            {QStringLiteral("uid"), QString::number(context.credentials.credentials.uid.real)},
            {QStringLiteral("startTicks"), QString::number(stat.start_ticks)},
            {QStringLiteral("unit"), text(context.scope.unit_name)},
            {QStringLiteral("executable"), QStringLiteral("device %1 / inode %2").arg(executable.device).arg(executable.inode)},
            {QStringLiteral("generation"), QString::number(candidate.generation())},
            {QStringLiteral("forceKill"), candidate.action() == ProcessAction::force_kill}};
}
QString outcome(const ProcessActionResult& result) {
    if (result.status == ProcessActionStatus::submitted)
        return QStringLiteral("Signal submitted to the confirmed process. Waiting for exit; this does not prove termination.");
    auto reason = text(result.reason);
    if (reason.isEmpty()) reason = QStringLiteral("Action unavailable or no longer eligible.");
    if (result.error_number) reason += QStringLiteral(" (errno %1)").arg(result.error_number);
    return reason;
}
}
ProcessController::ProcessController(QObject* parent)
    : ProcessController([](std::uint64_t pid, ProcessAction action) {
        return ProcessActionCandidate::prepare(pid, action, makeApplicationAuthorityResolver());
    }, parent) {}

ProcessController::ProcessController(Preparer preparer, QObject* parent)
    : QObject(parent), preparer_(std::move(preparer)) {
    observation_.setInterval(250);
    connect(&observation_, &QTimer::timeout, this, &ProcessController::observe);
    expiry_.setSingleShot(true);
    connect(&expiry_, &QTimer::timeout, this, [this] {
        if (awaiting_ && !busy_) { cancel(); message_ = QStringLiteral("Confirmation expired. Prepare a fresh confirmation."); emit changed(); }
    });
    worker_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::function<void()> operation;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [this] { return pending_.has_value(); });
                if (stop.stop_requested()) break;
                operation = std::move(*pending_);
                pending_.reset();
            }
            try { operation(); }
            catch (...) {
                retained_.reset(); application_.reset();
                QMetaObject::invokeMethod(this, [this] {
                    busy_ = awaiting_ = false; dispatch_gate_.reset(); display_.clear(); observation_.stop(); expiry_.stop();
                    message_ = QStringLiteral("Action interrupted. Earlier committed signals may already have been submitted. No further signals will be sent; inspect target state."); emit changed();
                }, Qt::QueuedConnection);
            }
        }
        retained_.reset(); application_.reset();
    });
}
ProcessController::~ProcessController() {
    if (dispatch_gate_) {
        if (group_dispatch_) dispatch_gate_->store(2);
        else { int pending = 0; dispatch_gate_->compare_exchange_strong(pending, 2); }
    }
    worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void ProcessController::enqueue(std::function<void()> operation) {
    busy_ = true;
    { std::lock_guard lock(mutex_); pending_ = std::move(operation); }
    wake_.notify_one(); emit changed();
}
void ProcessController::prepare(const QString& pidText, bool forceKill) {
    if (busy_ || awaiting_) return;
    bool valid{};
    const auto pid = pidText.toULongLong(&valid);
    if (!valid || pid == 0 || pid > static_cast<qulonglong>(std::numeric_limits<int>::max())) {
        message_ = QStringLiteral("Select a valid process PID."); emit changed(); return;
    }
    observation_.stop(); expiry_.stop();
    message_ = QStringLiteral("Checking fresh process identity and application ownership…");
    enqueue([this, pid, forceKill] {
        application_.reset();
        const auto action = forceKill ? ProcessAction::force_kill : ProcessAction::terminate;
        ProcessActionResult result;
        // Never rebind a PID after a previous signal. A second action for that
        // PID renews the SAME retained instance, even if it has since exited.
        if (retained_ && retained_->binding().stat.pid == pid) result = retained_->renew(action);
        else {
            retained_.reset();
            auto preparation = preparer_(pid, action);
            result = preparation.result;
            retained_ = std::move(preparation.candidate);
        }
        QVariantMap display;
        if (result.status == ProcessActionStatus::ready && retained_) display = describe(*retained_);
        QMetaObject::invokeMethod(this, [this, result, display = std::move(display)] { showPreparation(result, display); }, Qt::QueuedConnection);
    });
}
void ProcessController::prepareApplication(const QString& seedText) {
    if (busy_ || awaiting_) return;
    bool valid{};
    const auto seed = seedText.toULongLong(&valid);
    if (!valid || !seed || seed > static_cast<qulonglong>(std::numeric_limits<int>::max())) {
        message_ = QStringLiteral("Select an application process first."); emit changed(); return;
    }
    observation_.stop(); expiry_.stop();
    message_ = QStringLiteral("Binding the exact application member set and checking every process…");
    enqueue([this, seed] {
        retained_.reset(); application_.reset();
        auto prepared = ApplicationActionCandidate::prepare(seed, makeApplicationAuthorityResolver());
        application_ = std::move(prepared.candidate);
        QVariantMap display;
        if (prepared.result.status == ProcessActionStatus::ready && application_) {
            QString members;
            for (const auto& member : application_->members()) {
                members += QStringLiteral("Name: %1 · PID %2 · UID %3 · start %4 · executable device %5 / inode %6\n")
                    .arg(displayName(member.name)).arg(member.pid).arg(member.uid).arg(member.start_ticks)
                    .arg(member.executable.device).arg(member.executable.inode);
            }
            display = {{QStringLiteral("application"), true}, {QStringLiteral("forceKill"), false},
                       {QStringLiteral("unit"), text(application_->authority().unit_name)},
                       {QStringLiteral("memberCount"), static_cast<int>(application_->members().size())},
                       {QStringLiteral("membersText"), members},
                       {QStringLiteral("generation"), QString::number(application_->generation())}};
        }
        const auto result = prepared.result;
        QMetaObject::invokeMethod(this, [this, result, display = std::move(display)] { showPreparation(result, display); }, Qt::QueuedConnection);
    });
}
void ProcessController::showPreparation(ProcessActionResult result, QVariantMap display) {
    busy_ = false;
    display_ = std::move(display);
    awaiting_ = result.status == ProcessActionStatus::ready && !display_.isEmpty();
    if (awaiting_) {
        message_ = QStringLiteral("Confirm the fresh process below. Unsaved work may be lost.");
        deadline_ = Clock::now() + std::chrono::seconds{30};
        expiry_.start(30000);
    } else message_ = outcome(result);
    emit changed();
}
void ProcessController::confirm(const QString& generation) {
    if (busy_ || !awaiting_ || generation != display_.value(QStringLiteral("generation")).toString()) return;
    if (Clock::now() >= deadline_) { cancel(); return; }
    awaiting_ = false; expiry_.stop();
    message_ = QStringLiteral("Revalidating and submitting the explicitly confirmed action…");
    group_dispatch_ = display_.value(QStringLiteral("application")).toBool();
    dispatch_gate_ = std::make_shared<std::atomic_int>(0);
    enqueue([this, generation, gate = dispatch_gate_, group = group_dispatch_] {
        ProcessActionResult result;
        QString groupDetails;
        if (group && application_) {
            const auto token = application_->confirm(generation.toULongLong());
            if (token) {
                auto dispatched = application_->dispatch(*token, [gate] {
                    int pending = 0;
                    if (gate->compare_exchange_strong(pending, 1)) return true;
                    return pending == 1; // Each call commits only this one member.
                });
                result = dispatched.result;
                int submitted{};
                for (const auto& member : dispatched.members) {
                    submitted += member.signal_submitted ? 1 : 0;
                    groupDetails += QStringLiteral("\nPID %1: %2").arg(member.member.pid).arg(member.signal_submitted ? QStringLiteral("TERM submitted; exit not observed") : outcome(member.result));
                }
                groupDetails.prepend(text(result.reason) + QLatin1Char('\n') + QStringLiteral("%1 of %2 confirmed members submitted. Exit is not confirmed; new entrants are never added.")
                    .arg(submitted).arg(dispatched.members.size()));
            } else result = {ProcessActionStatus::invalid_confirmation, 0, "Application confirmation no longer valid"};
        } else if (retained_) {
            const auto token = retained_->confirm(generation.toULongLong(), retained_->action());
            result = token ? retained_->dispatch(*token, [gate] { int pending = 0; return gate->compare_exchange_strong(pending, 1); }) : ProcessActionResult{ProcessActionStatus::invalid_confirmation, 0, "Confirmation no longer valid"};
        }
        QMetaObject::invokeMethod(this, [this, result, groupDetails] {
            busy_ = false; dispatch_gate_.reset(); display_.clear(); message_ = groupDetails.isEmpty() ? outcome(result) : groupDetails;
            if (groupDetails.isEmpty() && result.status == ProcessActionStatus::submitted) { observations_ = 0; observation_.start(); }
            emit changed();
        }, Qt::QueuedConnection);
    });
}
void ProcessController::cancel() {
    if (busy_) {
        if (dispatch_gate_) {
            if (group_dispatch_) {
                dispatch_gate_->store(2);
                message_ = QStringLiteral("Cancellation requested for remaining application members. Already committed signals cannot be retracted.");
                emit changed(); return;
            }
            int pending = 0;
            if (dispatch_gate_->compare_exchange_strong(pending, 2))
                message_ = QStringLiteral("Cancellation accepted. The pending action will send no signal.");
            else if (pending == 1)
                message_ = QStringLiteral("Signal dispatch committed; it cannot be cancelled or retracted.");
            emit changed();
        }
        return;
    }
    awaiting_ = false; display_.clear(); observation_.stop(); expiry_.stop();
    message_ = QStringLiteral("Action cancelled. No further signals will be sent.");
    enqueue([this] {
        if (retained_) retained_->cancel();
        if (application_) application_->cancel();
        retained_.reset(); application_.reset();
        QMetaObject::invokeMethod(this, [this] { busy_ = false; emit changed(); }, Qt::QueuedConnection);
    });
}
void ProcessController::observe() {
    if (busy_ || awaiting_) return;
    enqueue([this] {
        const auto state = retained_ ? retained_->poll_exit() : ProcessExitObservation{};
        QMetaObject::invokeMethod(this, [this, state] {
            busy_ = false;
            if (state.state == ProcessExitState::exited) {
                observation_.stop(); message_ = QStringLiteral("Exit observed for the confirmed process. Exit status and cause are unknown.");
            } else if (state.state != ProcessExitState::running) {
                observation_.stop(); message_ = QStringLiteral("Exit observation unavailable; termination is not confirmed.");
            } else if (++observations_ >= 40) {
                observation_.stop(); message_ = QStringLiteral("Still running after 10 seconds. TERM may be ignored, or kernel I/O may delay exit. No automatic escalation; Force Kill requires a separate confirmation.");
            }
            emit changed();
        }, Qt::QueuedConnection);
    });
}
} // namespace night::ui
