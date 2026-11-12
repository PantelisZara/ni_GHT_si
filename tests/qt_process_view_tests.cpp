#include "process_view_model.hpp"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QtTest>

#include <algorithm>
#include <limits>

using night::ui::ProcessModel;
using night::ui::ProcessViewModel;

namespace {
night::ProcessInfo process(std::uint64_t pid, std::string name, double cpu = 1,
                           std::uint64_t rss = 1024, std::uint64_t start = 10) {
    night::ProcessInfo value;
    value.pid = pid;
    value.start_ticks = start;
    value.name = std::move(name);
    value.uid = 1000;
    value.state = 'S';
    value.cpu_percent = {cpu, night::Quality::valid};
    value.rss_bytes = {rss, night::Quality::valid};
    return value;
}

std::shared_ptr<night::ProcessSnapshot> snapshot(std::uint64_t sequence,
                                               std::vector<night::ProcessInfo> rows) {
    auto value = std::make_shared<night::ProcessSnapshot>();
    value->stamp.sequence = sequence;
    value->boot_id = "boot-A";
    value->pid_namespace = "pid:[123]";
    value->processes = std::move(rows);
    return value;
}

QList<qulonglong> pids(const ProcessViewModel& model) {
    QList<qulonglong> result;
    for (int row = 0; row < model.count(); ++row)
        result.push_back(model.index(row, ProcessModel::Pid).data().toULongLong());
    return result;
}
} // namespace

class ProcessViewTests final : public QObject {
    Q_OBJECT
private slots:
    void detailMetricsKeepUnavailableDistinctFromZero() {
        ProcessModel source;
        ProcessViewModel view(source);
        auto invalid = process(4, "invalid");
        invalid.cpu_percent = {std::numeric_limits<double>::infinity(), night::Quality::valid};
        invalid.rss_bytes = {{}, night::Quality::valid};
        QVERIFY(source.apply(snapshot(1, {invalid})));
        view.selectRow(0);
        QVERIFY(!view.selectedDetails().value("cpuPercent").isValid());
        QVERIFY(!view.selectedDetails().value("rssBytes").isValid());
        QCOMPARE(view.selectedDetails().value("cpuQuality").toString(), QStringLiteral("error"));
        QCOMPARE(view.selectedDetails().value("rssQuality").toString(), QStringLiteral("error"));
        QVERIFY(source.apply(snapshot(2, {process(4, "zero", 0, 0)})));
        QCOMPARE(view.selectedDetails().value("cpuPercent").toDouble(), 0.0);
        QCOMPARE(view.selectedDetails().value("rssBytes").toULongLong(), 0ULL);
        QCOMPARE(view.selectedDetails().value("cpuQuality").toString(), QStringLiteral("valid"));
        QCOMPARE(view.selectedDetails().value("rssQuality").toString(), QStringLiteral("valid"));
    }

    void detailsFollowRetainedSnapshotAndQuality() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy detailsChanges(&view, &ProcessViewModel::selectedDetailsChanged);
        auto selected = process(20, "worker\n", 2);
        selected.parent_pid = 1;
        selected.threads = 3;
        selected.application_scope = night::parse_application_scope_candidate("0::/app.slice/app-test\\x0a.scope\n");
        QVERIFY(source.apply(snapshot(1, {selected, process(100, "other", 100)})));
        QVERIFY(view.selectedDetails().isEmpty());
        view.selectRow(0);
        const auto identity = view.selectedIdentity();
        const auto details = view.selectedDetails();
        QCOMPARE(details.value("pid").toULongLong(), 20ULL);
        QCOMPARE(details.value("parentPid").toULongLong(), 1ULL);
        QCOMPARE(details.value("threads").toULongLong(), 3ULL);
        QCOMPARE(details.value("startTicks").toULongLong(), 10ULL);
        QCOMPARE(details.value("uid").toULongLong(), 1000ULL);
        QCOMPARE(details.value("name").toString(), QStringLiteral("worker\\u000a"));
        QCOMPARE(details.value("state").toString(), QStringLiteral("S"));
        QCOMPARE(details.value("scopeUnit").toString(), QStringLiteral("app-test\\x0a.scope"));
        QCOMPARE(details.value("scopeQuality").toString(), QStringLiteral("valid"));
        QCOMPARE(details.value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(details.value("cpuPercent").toDouble(), 2.0);
        QCOMPARE(details.value("cpuQuality").toString(), QStringLiteral("valid"));
        QCOMPARE(details.value("rssBytes").toULongLong(), 1024ULL);
        QCOMPARE(details.value("rssQuality").toString(), QStringLiteral("valid"));
        view.sortBy(ProcessModel::Cpu, true);
        QCOMPARE(view.selectedRow(), 1);
        QCOMPARE(view.selectedIdentity(), identity);
        QCOMPARE(view.selectedDetails(), details);
        const auto before_update = detailsChanges.count();
        selected.parent_pid = 7;
        selected.threads = 8;
        selected.name = "renamed";
        selected.cpu_percent = {{}, night::Quality::warming};
        selected.rss_bytes = {{}, night::Quality::permission_denied};
        selected.application_scope = {{}, night::Quality::permission_denied};
        QVERIFY(source.apply(snapshot(2, {process(100, "other", 100), selected})));
        QCOMPARE(view.selectedIdentity(), identity);
        QCOMPARE(view.selectedRow(), 1);
        QVERIFY(detailsChanges.count() > before_update);
        QCOMPARE(view.selectedDetails().value("threads").toULongLong(), 8ULL);
        QCOMPARE(view.selectedDetails().value("parentPid").toULongLong(), 7ULL);
        QCOMPARE(view.selectedDetails().value("name").toString(), QStringLiteral("renamed"));
        QVERIFY(!view.selectedDetails().value("cpuPercent").isValid());
        QCOMPARE(view.selectedDetails().value("cpuQuality").toString(), QStringLiteral("warming"));
        QVERIFY(!view.selectedDetails().value("rssBytes").isValid());
        QCOMPARE(view.selectedDetails().value("rssQuality").toString(), QStringLiteral("permission_denied"));
        QVERIFY(!view.selectedDetails().value("scopeUnit").isValid());
        QCOMPARE(view.selectedDetails().value("scopeQuality").toString(), QStringLiteral("permission_denied"));
        const auto retained = view.selectedDetails();
        const auto before_failure = detailsChanges.count();
        auto failed = snapshot(3, {});
        failed->stamp.quality = night::Quality::permission_denied;
        QVERIFY(source.apply(failed));
        QCOMPARE(view.selectedIdentity(), identity);
        QCOMPARE(view.selectedDetails().value("threads"), retained.value("threads"));
        QCOMPARE(view.selectedDetails().value("name"), retained.value("name"));
        QCOMPARE(view.selectedDetails().value("cpuQuality"), retained.value("cpuQuality"));
        QCOMPARE(view.selectedDetails().value("rssQuality"), retained.value("rssQuality"));
        QCOMPARE(view.selectedDetails().value("quality").toString(), QStringLiteral("permission_denied"));
        QVERIFY(detailsChanges.count() > before_failure);
        view.setQuery(QStringLiteral("other"));
        QVERIFY(view.selectedDetails().isEmpty());
        view.setQuery({});
        QVERIFY(view.selectedDetails().isEmpty());
    }

    void detailsRejectForeignIndicesAndClearOnIdentityChange() {
        ProcessModel source;
        ProcessModel foreign;
        ProcessViewModel view(source);
        QVERIFY(source.apply(snapshot(1, {process(4, "old")})));
        QVERIFY(foreign.apply(snapshot(1, {process(4, "foreign")})));
        QVERIFY(source.details({}).isEmpty());
        QVERIFY(source.details(foreign.index(0, 0)).isEmpty());
        QVERIFY(source.details(view.index(0, 0)).isEmpty());
        auto forged = process(4, "old");
        forged.application_scope = {night::ApplicationScopeCandidate{"/app.slice/app-test.scope", "app-other.scope"},
                                    night::Quality::valid};
        ProcessModel malformed_scope;
        QVERIFY(malformed_scope.apply(snapshot(1, {forged})));
        const auto invalid_details = malformed_scope.details(malformed_scope.index(0, 0));
        QVERIFY(!invalid_details.value("scopeUnit").isValid());
        QCOMPARE(invalid_details.value("scopeQuality").toString(), QStringLiteral("error"));
        view.selectRow(0);
        QVERIFY(!view.selectedDetails().isEmpty());
        QVERIFY(source.apply(snapshot(2, {process(4, "reused", 1, 1024, 11)})));
        QVERIFY(view.selectedDetails().isEmpty());
        view.selectRow(0);
        auto reset = snapshot(3, {process(4, "reused", 1, 1024, 11)});
        reset->boot_id = "boot-B";
        QVERIFY(source.apply(reset));
        QVERIFY(view.selectedDetails().isEmpty());
        view.selectRow(0);
        QVERIFY(source.apply(snapshot(4, {})));
        QVERIFY(view.selectedDetails().isEmpty());
    }

    void numericOrderingAndMissingLast() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto missing = process(9, "unavailable");
        missing.cpu_percent = {{}, night::Quality::warming};
        missing.rss_bytes = {{}, night::Quality::error};
        QVERIFY(source.apply(snapshot(1, {process(100, "one", 100, 9 * 1024 * 1024),
            process(20, "two", 2, 100 * 1024 * 1024),
            process(3, "three", 11, 11 * 1024 * 1024), missing})));
        QCOMPARE(pids(view), (QList<qulonglong>{3, 9, 20, 100}));
        view.sortBy(ProcessModel::Pid, true);
        QCOMPARE(pids(view), (QList<qulonglong>{100, 20, 9, 3}));
        view.sortBy(ProcessModel::Cpu, false);
        QCOMPARE(pids(view), (QList<qulonglong>{20, 3, 100, 9}));
        view.sortBy(ProcessModel::Cpu, true);
        QCOMPARE(pids(view), (QList<qulonglong>{100, 3, 20, 9}));
        view.sortBy(ProcessModel::Rss, false);
        QCOMPARE(pids(view), (QList<qulonglong>{100, 3, 20, 9}));
        view.sortBy(ProcessModel::Rss, true);
        QCOMPARE(pids(view), (QList<qulonglong>{20, 3, 100, 9}));
        view.sortBy(ProcessModel::ColumnCount, false);
        QCOMPARE(pids(view), (QList<qulonglong>{20, 3, 100, 9}));
        // Byte values remain exact above the double integer precision limit.
        QVERIFY(source.apply(snapshot(2, {process(100, "one", 0, (1ULL << 53) + 1),
            process(20, "two", 0, (1ULL << 53)), process(3, "three", 0, 0)})));
        view.sortBy(ProcessModel::Rss, false);
        QCOMPARE(pids(view), (QList<qulonglong>{3, 20, 100}));
    }

    void literalSearchAndBoundedQuery() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy changes(&view, &ProcessViewModel::queryChanged);
        QVERIFY(source.apply(snapshot(1, {process(123, "Alpha[worker].*"),
            process(321, "alphaWorker"), process(77, "Other")})));
        view.setQuery(QStringLiteral("ALPHA"));
        QCOMPARE(pids(view), (QList<qulonglong>{123, 321}));
        view.setQuery(QStringLiteral("[worker].*"));
        QCOMPARE(pids(view), (QList<qulonglong>{123}));
        view.setQuery(QStringLiteral(".*"));
        QCOMPARE(pids(view), (QList<qulonglong>{123}));
        view.setQuery(QStringLiteral("23"));
        QCOMPARE(pids(view), (QList<qulonglong>{123}));
        view.setQuery(QStringLiteral("["));
        QCOMPARE(pids(view), (QList<qulonglong>{123}));
        view.setQuery(QStringLiteral("no match"));
        QCOMPARE(view.count(), 0);
        view.setQuery(QString(300, QLatin1Char('a')));
        QCOMPARE(view.query().size(), 256);
        const auto changes_before = changes.count();
        view.setQuery(QString(400, QLatin1Char('a')));
        QCOMPARE(changes.count(), changes_before);
        view.setQuery({});
        QCOMPARE(view.count(), 3);
    }

    void selectionFollowsIdentityThroughChurnAndSorting() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy changes(&view, &ProcessViewModel::selectionChanged);
        QVERIFY(source.apply(snapshot(1, {process(3, "removed", 11),
            process(20, "retained", 2), process(100, "other", 100)})));
        view.selectRow(1); // Initial PID order: 3, 20, 100.
        const auto identity = view.selectedIdentity();
        QVERIFY(!identity.isEmpty());
        QCOMPARE(view.selectedRow(), 1);
        view.sortBy(ProcessModel::Cpu, true);
        QCOMPARE(view.selectedRow(), 2);
        QCOMPARE(view.selectedIdentity(), identity);
        QVERIFY(source.apply(snapshot(2, {process(100, "other", 100),
            process(20, "renamed", 200), process(7, "inserted", 50)})));
        QCOMPARE(pids(view), (QList<qulonglong>{20, 100, 7}));
        QCOMPARE(view.selectedIdentity(), identity);
        QCOMPARE(view.selectedRow(), 0);
        QCOMPARE(view.index(view.selectedRow(), ProcessModel::Name).data().toString(),
                 QStringLiteral("renamed"));
        auto failed = snapshot(3, {});
        failed->stamp.quality = night::Quality::permission_denied;
        QVERIFY(source.apply(failed));
        QCOMPARE(view.selectedIdentity(), identity);
        QCOMPARE(view.selectedRow(), 0);
        QVERIFY(source.apply(snapshot(4, {process(7, "inserted"), process(100, "other")})));
        QCOMPARE(view.selectedRow(), -1);
        QVERIFY(view.selectedIdentity().isEmpty());
        QVERIFY(changes.count() >= 4);
    }

    void reuseAndContextChangesClearSelection() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(source.apply(snapshot(1, {process(4, "old", 1, 1024, 20)})));
        view.selectRow(0);
        const auto previous = view.selectedIdentity();
        QVERIFY(source.apply(snapshot(2, {process(4, "reused", 1, 1024, 30)})));
        QCOMPARE(view.selectedRow(), -1);
        QVERIFY(view.selectedIdentity().isEmpty());
        QVERIFY(view.index(0, 0).data(ProcessModel::IdentityRole).toString() != previous);
        view.selectRow(0);
        auto changed = snapshot(3, {process(4, "reused", 1, 1024, 30)});
        changed->pid_namespace = "pid:[456]";
        QVERIFY(source.apply(changed));
        QCOMPARE(view.selectedRow(), -1);
        QVERIFY(view.selectedIdentity().isEmpty());
        view.selectRow(0);
        changed = snapshot(4, {process(4, "reused", 1, 1024, 30)});
        changed->pid_namespace = "pid:[456]";
        changed->boot_id = "boot-B";
        QVERIFY(source.apply(changed));
        QVERIFY(view.selectedIdentity().isEmpty());
        QCOMPARE(view.selectedRow(), -1);
    }

    void filteringClearsHiddenSelection() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(source.apply(snapshot(1, {process(1, "alpha"), process(2, "beta")})));
        view.selectRow(1);
        view.setQuery(QStringLiteral("alpha"));
        QVERIFY(view.selectedIdentity().isEmpty());
        QCOMPARE(view.selectedRow(), -1);
        view.setQuery({});
        QVERIFY(view.selectedIdentity().isEmpty());
        view.selectRow(0);
        view.setQuery(QStringLiteral("alpha"));
        QVERIFY(!view.selectedIdentity().isEmpty());
        QVERIFY(source.apply(snapshot(2, {process(1, "renamed"), process(2, "beta")})));
        QCOMPARE(view.count(), 0);
        QVERIFY(view.selectedIdentity().isEmpty());
        QCOMPARE(view.selectedRow(), -1);
        view.setQuery({});
        view.selectRow(-1);
        QCOMPARE(view.selectedRow(), -1);
        view.selectRow(view.count());
        QVERIFY(view.selectedIdentity().isEmpty());
        view.selectRow(0);
        view.clearSelection();
        QVERIFY(view.selectedIdentity().isEmpty());
    }

    void tiedValuesHaveStableIdentityOrder() {
        ProcessModel source;
        ProcessViewModel view(source);
        QAbstractItemModelTester tester(&view, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(source.apply(snapshot(1, {process(12, "same", 5), process(2, "same", 5),
            process(8, "same", 5)})));
        view.sortBy(ProcessModel::Cpu, false);
        const auto initial = pids(view);
        QVERIFY(source.apply(snapshot(2, {process(8, "same", 5), process(12, "same", 5),
            process(2, "same", 5)})));
        QCOMPARE(pids(view), initial);
        view.sortBy(ProcessModel::Name, false);
        QCOMPARE(pids(view), initial);
        view.sortBy(ProcessModel::Cpu, true);
        auto reversed = initial;
        std::reverse(reversed.begin(), reversed.end());
        QCOMPARE(pids(view), reversed);
    }
};

QTEST_GUILESS_MAIN(ProcessViewTests)
#include "qt_process_view_tests.moc"
