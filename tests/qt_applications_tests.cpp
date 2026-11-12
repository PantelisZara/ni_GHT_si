#include "applications_model.hpp"
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QtTest>
#include <limits>

namespace {
night::ProcessInfo process(std::uint64_t pid, const std::string& unit) {
    night::ProcessInfo p; p.pid = pid; p.start_ticks = pid + 10;
    p.cpu_percent = {2.5, night::Quality::valid}; p.rss_bytes = {1048576, night::Quality::valid};
    p.application_scope = night::parse_application_scope_candidate("0::/app.slice/" + unit + "\n");
    return p;
}
std::shared_ptr<night::ProcessSnapshot> snapshot(std::uint64_t seq, std::vector<night::ProcessInfo> rows) {
    auto s = std::make_shared<night::ProcessSnapshot>();s->stamp.sequence = seq;
    s->stamp.begin = s->stamp.end = night::Clock::now();
    s->boot_id = "boot";s->pid_namespace = "pid:[1]";s->cgroup_namespace = "cgroup:[2]";
    s->processes = std::move(rows);return s;
}
}
class ApplicationsTests final : public QObject {
    Q_OBJECT
private slots:
    void configuredFreshnessAndNotifications() {
        using namespace std::chrono_literals;
        night::ui::ApplicationsModel model;
        auto initial = snapshot(1, {process(1, "app-one.scope")});
        const auto end = initial->stamp.end;
        QVERIFY(model.apply(initial));
        model.refreshAge(end + 2s, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("valid"));
        QCOMPARE(model.dataAgeMs(), 2000LL);
        model.refreshAge(end + 6s, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("valid"));
        model.refreshAge(end + 6001ms, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("stale"));

        model.refreshAge(end + 2s); // Existing callers retain the 1500 ms default.
        QCOMPARE(model.quality(), QStringLiteral("stale"));
        QSignalSpy changes(&model, &night::ui::ApplicationsModel::changed);
        model.refreshAge(end + 2s, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("valid"));
        QCOMPARE(changes.count(), 1); // Threshold alone changed the exposed quality.
        model.refreshAge(end + 2s, 6000ms);
        QCOMPARE(changes.count(), 1);
        model.refreshAge(end + 2s);
        QCOMPARE(model.quality(), QStringLiteral("stale"));
        QCOMPARE(changes.count(), 2);

        auto failed = snapshot(2, {});
        failed->stamp.quality = night::Quality::error;
        QVERIFY(model.apply(failed));
        model.refreshAge(end + 8s, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("error"));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.dataAgeMs(), 8000LL);
        auto recovered = snapshot(3, {process(1, "app-one.scope")});
        recovered->stamp.begin = recovered->stamp.end = end + 8s;
        QVERIFY(model.apply(recovered));
        model.refreshAge(end + 10s, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("valid"));
        QCOMPARE(model.dataAgeMs(), 2000LL);

        auto unavailable = process(2, "app-two.scope");
        unavailable.application_scope = {{}, night::Quality::permission_denied};
        auto partial = snapshot(4, {process(1, "app-one.scope"), unavailable});
        partial->stamp.begin = partial->stamp.end = end + 8s;
        QVERIFY(model.apply(partial));
        model.refreshAge(end + 14s, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("partial"));
        model.refreshAge(end + 14001ms, 6000ms);
        QCOMPARE(model.quality(), QStringLiteral("stale"));
    }
    void groupingAndCoverage() {
        night::ui::ApplicationsModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto unknown = process(3, "ordinary.service");
        auto failed = process(4, "app-two.scope");failed.application_scope = {{}, night::Quality::permission_denied};
        QVERIFY(model.apply(snapshot(1, {process(1, "app-one.scope"), process(2, "app-one.scope"), unknown, failed})));
        QCOMPARE(model.count(), 1);QCOMPARE(model.unclassified(), 1);QCOMPARE(model.unavailable(), 1);
        QCOMPARE(model.unitAtRow(0), QStringLiteral("app-one.scope"));
        QCOMPARE(model.rowForUnit(QStringLiteral("app-one.scope")), 0);
        QCOMPARE(model.seedPidForUnit(QStringLiteral("app-one.scope")), QStringLiteral("1"));
        QVERIFY(model.unitAtRow(-1).isEmpty() && model.unitAtRow(1).isEmpty());
        QVERIFY(model.seedPidForUnit(QStringLiteral("missing.scope")).isEmpty());
        QCOMPARE(model.quality(), QStringLiteral("partial"));
        QCOMPARE(model.index(0, 1).data().toULongLong(), 2ULL);
        QCOMPARE(model.index(0, 2).data().toString(), QStringLiteral("5.0"));
        QCOMPARE(model.index(0, 3).data().toString(), QStringLiteral("2.0"));
        auto forged = process(5, "app-one.scope");
        forged.application_scope.value->unit_name = "app-forged.scope";
        QVERIFY(model.apply(snapshot(2, {forged})));
        QCOMPARE(model.count(), 0);QCOMPARE(model.unavailable(), 1);QCOMPARE(model.quality(), QStringLiteral("partial"));
        forged = process(5, "app-one.scope");
        forged.application_scope.value->cgroup_path += "\n1:cpu:/other";
        QVERIFY(model.apply(snapshot(3, {forged})));
        QCOMPARE(model.count(), 0);QCOMPARE(model.unavailable(), 1);QCOMPARE(model.quality(), QStringLiteral("partial"));
    }
    void retainedIdentityAndNamespace() {
        night::ui::ApplicationsModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.apply(snapshot(1, {process(1, "app-one.scope")})));
        QPersistentModelIndex retained(model.index(0, 0));
        QVERIFY(model.apply(snapshot(2, {process(2, "app-two.scope"), process(1, "app-one.scope")})));
        QVERIFY(retained.isValid());QCOMPARE(retained.data().toString(), QStringLiteral("app-one.scope"));
        auto different = snapshot(3, {process(1, "app-one.scope")});different->cgroup_namespace = "cgroup:[3]";
        QVERIFY(model.apply(different));QVERIFY(!retained.isValid());QCOMPARE(model.count(), 1);
        QVERIFY(!model.apply(snapshot(2, {})));
    }
    void missingAggregatesAndOverflow() {
        night::ui::ApplicationsModel model;
        auto missing = process(2, "app-one.scope");missing.cpu_percent = {{}, night::Quality::warming};
        missing.rss_bytes = {std::numeric_limits<std::uint64_t>::max(), night::Quality::valid};
        QVERIFY(model.apply(snapshot(1, {process(1, "app-one.scope"), missing})));
        QCOMPARE(model.index(0, 2).data().toString(), QString(QChar(0x2014)));
        QCOMPARE(model.index(0, 3).data().toString(), QString(QChar(0x2014)));
    }
    void contextFailureRetainsAgeAndDuplicateFails() {
        night::ui::ApplicationsModel model;
        QVERIFY(model.apply(snapshot(1, {process(1, "app-one.scope")})));
        model.refreshAge(night::Clock::now() + std::chrono::seconds{2});
        QCOMPARE(model.quality(), QStringLiteral("stale"));QVERIFY(model.dataAgeMs() >= 1900);
        auto failure = snapshot(2, {});failure->cgroup_namespace.clear();
        QVERIFY(model.apply(failure));QCOMPARE(model.quality(), QStringLiteral("unsupported"));QCOMPARE(model.count(), 1);
        QVERIFY(model.apply(snapshot(3, {process(1, "app-one.scope"), process(1, "app-two.scope")})));
        QCOMPARE(model.quality(), QStringLiteral("error"));QCOMPARE(model.count(), 1);
    }
};
QTEST_GUILESS_MAIN(ApplicationsTests)
#include "qt_applications_tests.moc"
