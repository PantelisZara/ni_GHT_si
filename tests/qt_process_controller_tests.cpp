#include "process_controller.hpp"
#include "application_authority.hpp"
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>
#include <future>
#include <iostream>
#include <QtTest>
#include <csignal>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {
struct Child {
    pid_t pid{-1};
    explicit Child(bool ignoreTerm = false) {
        int ready[2];
        if (::pipe(ready) != 0) return;
        pid = ::fork();
        if (pid == 0) {
            ::close(ready[0]);
            ::alarm(10); // Disposable child cannot outlive an interrupted test indefinitely.
            ::prctl(PR_SET_NAME, "owned-\xE2\x80\xAE" "child", 0, 0, 0);
            if (ignoreTerm) ::signal(SIGTERM, SIG_IGN);
            const char byte = 1;
            if (::write(ready[1], &byte, 1) != 1) ::_exit(2);
            ::close(ready[1]);
            for (;;) ::pause();
        }
        ::close(ready[1]); char byte{};
        if (pid > 0 && ::read(ready[0], &byte, 1) != 1) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); pid = -1; }
        ::close(ready[0]);
    }
    ~Child() { if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); } }
    bool running() const { siginfo_t status{}; return ::waitid(P_PID, static_cast<id_t>(pid), &status, WEXITED | WNOHANG | WNOWAIT) == 0 && status.si_pid == 0; }
};
night::ui::ProcessController::Preparer owned(Child& child) {
    return [&child](std::uint64_t pid, night::ProcessAction action) {
        if (pid != static_cast<std::uint64_t>(child.pid)) return night::ProcessActionPreparation{
            {}, {night::ProcessActionStatus::denied, 0, "Test permits only its own disposable child"}};
        return night::ProcessActionCandidate::prepare_owned_child(pid, action);
    };
}
}
class ProcessControllerTests final : public QObject {
    Q_OBJECT
private slots:
    void cancelAndStaleConfirmation() {
        Child child; QVERIFY(child.pid > 0);
        night::ui::ProcessController controller{owned(child)};
        controller.prepare(QString::number(child.pid));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4000);
        QVERIFY2(controller.awaitingConfirmation(), qPrintable(controller.message()));
        QCOMPARE(controller.candidate().value("pid").toString(), QString::number(child.pid));
        QCOMPARE(controller.candidate().value("name").toString(), QStringLiteral("owned-?child"));
        controller.confirm(QStringLiteral("wrong-generation"));
        QVERIFY(controller.awaitingConfirmation()); QVERIFY(child.running());
        controller.cancel();
        QTRY_VERIFY(!controller.busy()); QVERIFY(!controller.awaitingConfirmation());
        QVERIFY(child.running());
    }
    void termAndSeparateKill() {
        Child child{true}; QVERIFY(child.pid > 0);
        night::ui::ProcessController controller{owned(child)};
        const auto pid = QString::number(child.pid);
        controller.prepare(pid);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4000);
        QVERIFY2(controller.awaitingConfirmation(), qPrintable(controller.message()));
        const auto first = controller.candidate().value("generation").toString();
        controller.confirm(first);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4000);
        QVERIFY2(controller.message().contains("Signal submitted"), qPrintable(controller.message()));
        QTest::qWait(300); QVERIFY(child.running()); // No automatic escalation.
        QTRY_VERIFY(!controller.busy());
        controller.prepare(pid, true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4000);
        QVERIFY2(controller.awaitingConfirmation(), qPrintable(controller.message()));
        QVERIFY(controller.candidate().value("forceKill").toBool());
        const auto second = controller.candidate().value("generation").toString();
        QVERIFY(second != first);
        controller.confirm(first); QVERIFY(controller.awaitingConfirmation()); QVERIFY(child.running());
        controller.confirm(second);
        QTRY_VERIFY_WITH_TIMEOUT(!child.running(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.message().contains("Exit observed"), 4000);
        controller.prepare(pid, true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4000);
        QVERIFY(!controller.awaitingConfirmation()); // Never rebind the exited PID.
    }
    void cooperativeTerm() {
        Child child; QVERIFY(child.pid > 0);
        night::ui::ProcessController controller{owned(child)};
        controller.prepare(QString::number(child.pid));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4000);
        QVERIFY2(controller.awaitingConfirmation(), qPrintable(controller.message()));
        controller.confirm(controller.candidate().value("generation").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!child.running(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.message().contains("Exit observed"), 4000);
    }
    void invalidSelection() {
        int preparations{};
        night::ui::ProcessController controller{[&](std::uint64_t, night::ProcessAction) {
            ++preparations; return night::ProcessActionPreparation{};
        }};
        controller.prepare("not-a-pid"); controller.prepare("0"); controller.prepare("9999999999999");
        QCOMPARE(preparations, 0); QVERIFY(!controller.busy()); QVERIFY(!controller.awaitingConfirmation());
    }
};
namespace {
// Manual positive integration only. The normal CTest target never starts an
// installed application. The isolated scope has a fresh UUID, no user files,
// and an external watchdog; before consent its sole member must be the exact
// direct child of that watchdog, with a retained running pidfd.
int nativeOwnedApplication() {
    const auto require = [](bool valid, const std::string& reason) {
        if (!valid) throw std::runtime_error(reason);
    };
    struct Fixture {
        QTemporaryDir directory;
        QProcess supervisor;
        ~Fixture() {
            // Allow the external18s watchdog to clean up its own child even if
            // an assertion fails. Never kill the supervisor and orphan a GUI.
            if (supervisor.state() != QProcess::NotRunning) supervisor.waitForFinished(22000);
        }
    } fixture;
    require(fixture.directory.isValid(), "temporary application data unavailable");
    const auto unit = QStringLiteral("app-org.kde.kate-nighttest%1.scope").arg(QUuid::createUuid().toString(QUuid::Id128));
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& pair : {qMakePair(QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("/config")),
                             qMakePair(QStringLiteral("XDG_DATA_HOME"), QStringLiteral("/data")),
                             qMakePair(QStringLiteral("XDG_CACHE_HOME"), QStringLiteral("/cache"))})
        environment.insert(pair.first, fixture.directory.path() + pair.second);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_STYLE_OVERRIDE"), QStringLiteral("Fusion"));
    fixture.supervisor.setProcessEnvironment(environment);
    fixture.supervisor.start(QStringLiteral("/usr/bin/timeout"),
        {QStringLiteral("--foreground"), QStringLiteral("--preserve-status"), QStringLiteral("--kill-after=2s"),
         QStringLiteral("18s"), QStringLiteral("/usr/bin/systemd-run"), QStringLiteral("--user"),
         QStringLiteral("--scope"), QStringLiteral("--collect"), QStringLiteral("--quiet"),
         QStringLiteral("--slice=app.slice"), QStringLiteral("--unit=") + unit,
         QStringLiteral("/usr/bin/kate"), QStringLiteral("--new"), QStringLiteral("--startanon")});
    require(fixture.supervisor.waitForStarted(2000), "owned application supervisor did not start");
    const auto supervisor_pid = static_cast<std::uint64_t>(fixture.supervisor.processId());
    std::uint64_t pid{};
    QElapsedTimer startup; startup.start();
    while (startup.elapsed() < 5000 && fixture.supervisor.state() != QProcess::NotRunning && !pid) {
        QProcess lookup;
        lookup.start(QStringLiteral("/usr/bin/systemctl"),
                     {QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("--property=ControlGroup"),
                      QStringLiteral("--value"), unit});
        if (lookup.waitForFinished(1000) && lookup.exitCode() == 0) {
            const auto group = QString::fromUtf8(lookup.readAllStandardOutput()).trimmed();
            if (group.endsWith(QLatin1Char('/') + unit)) {
                QFile membership(QStringLiteral("/sys/fs/cgroup") + group + QStringLiteral("/cgroup.procs"));
                if (membership.open(QIODevice::ReadOnly)) {
                    const auto content = membership.read(4097).toStdString();
                    const auto values = night::words(content);
                    if (values.size() == 1) {
                        bool valid{};
                        const auto candidate = QByteArray(values[0].data(), static_cast<qsizetype>(values[0].size())).toULongLong(&valid);
                        if (valid) {
                            const auto stat = night::read_text(std::filesystem::path{"/proc"} / std::to_string(candidate) / "stat");
                            const auto parsed = stat.issue ? std::optional<night::ProcessStat>{} : night::parse_process_stat(stat.text);
                            if (parsed && parsed->parent_pid == supervisor_pid
                                && QFileInfo(QStringLiteral("/proc/%1/exe").arg(candidate)).canonicalFilePath()
                                    == QFileInfo(QStringLiteral("/usr/bin/kate")).canonicalFilePath()) pid = candidate;
                        }
                    }
                }
            }
        }
        QTest::qWait(30);
    }
    require(pid != 0, "owned isolated Kate member unavailable: " + fixture.supervisor.readAllStandardError().toStdString());
    std::cout << "Owned native fixture: " << unit.toStdString() << " PID " << pid << std::endl;
    std::atomic_bool abort{false};
    auto action = std::async(std::launch::async, [&, pid, supervisor_pid, unit] {
        auto resolver = night::ui::makeApplicationAuthorityResolver();
        auto single = night::ProcessActionCandidate::prepare(pid, night::ProcessAction::terminate, resolver);
        require(single.candidate.has_value(), "native single preparation: " + single.result.reason);
        require(single.candidate->context().credentials.stat.parent_pid == supervisor_pid,
                "fresh candidate is not the owned supervisor's child");
        require(single.candidate->authority().unit_name == unit.toStdString(), "unexpected native authority unit");
        auto group = night::ApplicationActionCandidate::prepare(pid, resolver);
        require(group.candidate.has_value(), "native fixed-set preparation: " + group.result.reason);
        require(group.candidate->members().size() == 1 && group.candidate->members()[0].pid == pid,
                "scope is not the sole confirmed owned instance");
        require(single.candidate->poll_exit().state == night::ProcessExitState::running,
                "original owned instance exited before group consent");
        const auto token = group.candidate->confirm(group.candidate->generation());
        require(token.has_value(), "fresh fixed-set consent unavailable");
        const auto submitted = group.candidate->dispatch(*token, [&] {
            return !abort.load() && single.candidate->poll_exit().state == night::ProcessExitState::running;
        });
        require(submitted.result.status == night::ProcessActionStatus::submitted && submitted.members.size() == 1
                && submitted.members[0].signal_submitted && !submitted.partial,
                "owned native fixed-set submission: " + submitted.result.reason);
        const auto deadline = night::Clock::now() + std::chrono::seconds{2};
        while (night::Clock::now() < deadline && single.candidate->poll_exit().state == night::ProcessExitState::running)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        require(single.candidate->poll_exit().state == night::ProcessExitState::exited,
                "owned native process exit not observed");
    });
    QElapsedTimer execution; execution.start();
    while (action.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready && execution.elapsed() < 10000)
        QTest::qWait(20);
    if (execution.elapsed() >= 10000) abort.store(true);
    action.get();
    if (fixture.supervisor.state() != QProcess::NotRunning) fixture.supervisor.waitForFinished(2500);
    require(fixture.supervisor.state() == QProcess::NotRunning, "owned supervisor did not finish after observed exit");
    std::cout << "PASS native unique user-manager authority, exact native cgroup membership, owned fixed-set TERM and retained-instance exit\n";
    return 0;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--native-owned-application") {
        try { return nativeOwnedApplication(); }
        catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    }
    ProcessControllerTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "qt_process_controller_tests.moc"
