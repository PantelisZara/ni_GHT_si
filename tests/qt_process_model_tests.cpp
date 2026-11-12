#include "process_model.hpp"

#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QtTest>

#include <limits>

using night::ui::ProcessModel;

namespace {
night::ProcessInfo process(std::uint64_t pid, std::uint64_t started = 10) {
    night::ProcessInfo value;
    value.pid = pid;
    value.start_ticks = started;
    value.uid = 1000;
    value.name = "worker";
    value.state = 'S';
    value.cpu_percent = {12.5, night::Quality::valid};
    value.rss_bytes = {2 * 1024 * 1024, night::Quality::valid};
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
} // namespace

class ProcessModelTests final : public QObject {
    Q_OBJECT
private slots:
    void identicalGenerationDoesNotInvalidateCells() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.apply(snapshot(1, {process(1), process(2)})));
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy changes(&model, &ProcessModel::changed);
        QVERIFY(model.apply(snapshot(2, {process(2), process(1)})));
        QCOMPARE(cells.count(), 0);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(model.sequence(), 2ULL);
        QCOMPARE(model.index(0, ProcessModel::Pid).data().toULongLong(), 1ULL);
    }

    void dirtyColumnsUseBoundedRetainedRowRanges() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto first = process(1), second = process(2), third = process(3), fourth = process(4);
        QVERIFY(model.apply(snapshot(1, {first, second, third, fourth})));
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);
        first.cpu_percent.value = 1.0;
        fourth.cpu_percent.value = 2.0;
        QVERIFY(model.apply(snapshot(2, {fourth, third, second, first})));
        QCOMPARE(cells.count(), 1);
        QCOMPARE(qvariant_cast<QModelIndex>(cells.at(0).at(0)), model.index(0, ProcessModel::Cpu));
        QCOMPARE(qvariant_cast<QModelIndex>(cells.at(0).at(1)), model.index(3, ProcessModel::Cpu));
        QCOMPARE(qvariant_cast<QList<int>>(cells.at(0).at(2)),
                 (QList<int>{Qt::DisplayRole, ProcessModel::SortRole}));
        QCOMPARE(model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 1.0);
        QCOMPARE(model.index(3, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 2.0);
        cells.clear();
        second.rss_bytes.value = 1;
        third.rss_bytes.value = 2;
        QVERIFY(model.apply(snapshot(3, {third, first, fourth, second})));
        QCOMPARE(cells.count(), 1);
        QCOMPARE(qvariant_cast<QModelIndex>(cells.at(0).at(0)), model.index(1, ProcessModel::Rss));
        QCOMPARE(qvariant_cast<QModelIndex>(cells.at(0).at(1)), model.index(2, ProcessModel::Rss));
    }

    void numericSortChangesNotifyWithinRoundedDisplay() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto value = process(1);
        value.cpu_percent.value = 12.51;
        value.rss_bytes.value = 2097153;
        QVERIFY(model.apply(snapshot(1, {value})));
        const auto cpu_display = model.index(0, ProcessModel::Cpu).data();
        const auto rss_display = model.index(0, ProcessModel::Rss).data();
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);
        value.cpu_percent.value = 12.52;
        value.rss_bytes.value = 2097154;
        QVERIFY(model.apply(snapshot(2, {value})));
        QCOMPARE(cells.count(), 2);
        QCOMPARE(qvariant_cast<QModelIndex>(cells.at(0).at(0)), model.index(0, ProcessModel::Cpu));
        QCOMPARE(qvariant_cast<QModelIndex>(cells.at(1).at(0)), model.index(0, ProcessModel::Rss));
        QCOMPARE(model.index(0, ProcessModel::Cpu).data(), cpu_display);
        QCOMPARE(model.index(0, ProcessModel::Rss).data(), rss_display);
        QCOMPARE(model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 12.52);
        QCOMPARE(model.index(0, ProcessModel::Rss).data(ProcessModel::SortRole).toULongLong(), 2097154ULL);
    }

    void unavailableCellsOnlyNotifyForAvailabilityChanges() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto value = process(1);
        value.cpu_percent = {std::numeric_limits<double>::quiet_NaN(), night::Quality::valid};
        value.rss_bytes = {{}, night::Quality::warming};
        QVERIFY(model.apply(snapshot(1, {value})));
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy changes(&model, &ProcessModel::changed);
        value.cpu_percent = {std::numeric_limits<double>::infinity(), night::Quality::valid};
        value.rss_bytes = {9000, night::Quality::error};
        QVERIFY(model.apply(snapshot(2, {value})));
        QCOMPARE(cells.count(), 0);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(model.details(model.index(0, 0)).value(QStringLiteral("rssQuality")).toString(),
                 QStringLiteral("error"));
        value.cpu_percent = {0.0, night::Quality::valid};
        value.rss_bytes = {0, night::Quality::valid};
        QVERIFY(model.apply(snapshot(3, {value})));
        QCOMPARE(cells.count(), 2);
        QCOMPARE(model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 0.0);
        QCOMPARE(model.index(0, ProcessModel::Rss).data(ProcessModel::SortRole).toULongLong(), 0ULL);
        cells.clear();
        value.cpu_percent = {0.0, night::Quality::stale};
        value.rss_bytes = {0, night::Quality::stale};
        QVERIFY(model.apply(snapshot(4, {value})));
        QCOMPARE(cells.count(), 2);
        QVERIFY(!model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).isValid());
        QVERIFY(!model.index(0, ProcessModel::Rss).data(ProcessModel::SortRole).isValid());
        QCOMPARE(changes.count(), 3);
    }

    void nameStateAndUidNotifyTheirColumns() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto value = process(1);
        QVERIFY(model.apply(snapshot(1, {value})));
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);
        value.name = "renamed";
        value.state = 'R';
        value.uid = 2000;
        QVERIFY(model.apply(snapshot(2, {value})));
        QCOMPARE(cells.count(), 3);
        const QList<int> columns{ProcessModel::Name, ProcessModel::State, ProcessModel::Uid};
        for (int signal = 0; signal < columns.size(); ++signal) {
            const auto index = model.index(0, columns.at(signal));
            QCOMPARE(qvariant_cast<QModelIndex>(cells.at(signal).at(0)), index);
            QCOMPARE(qvariant_cast<QModelIndex>(cells.at(signal).at(1)), index);
        }
        QCOMPARE(model.index(0, ProcessModel::Name).data().toString(), QStringLiteral("renamed"));
        QCOMPARE(model.index(0, ProcessModel::State).data().toString(), QStringLiteral("R"));
        QCOMPARE(model.index(0, ProcessModel::Uid).data().toULongLong(), 2000ULL);
    }

    void detailsOnlyChangesStillNotifySource() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto value = process(1);
        QVERIFY(model.apply(snapshot(1, {value})));
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy changes(&model, &ProcessModel::changed);
        value.parent_pid = 42;
        value.threads = 9;
        value.application_scope.quality = night::Quality::error;
        QVERIFY(model.apply(snapshot(2, {value})));
        QCOMPARE(cells.count(), 0);
        QCOMPARE(changes.count(), 1);
        const auto details = model.details(model.index(0, 0));
        QCOMPARE(details.value(QStringLiteral("parentPid")).toULongLong(), 42ULL);
        QCOMPARE(details.value(QStringLiteral("threads")).toULongLong(), 9ULL);
        QCOMPARE(details.value(QStringLiteral("scopeQuality")).toString(), QStringLiteral("error"));
    }

    void contiguousRemovalsPreservePersistentRows() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.apply(snapshot(1, {process(1), process(2), process(3), process(4),
                                        process(5), process(6), process(7), process(8)})));
        QPersistentModelIndex retained(model.index(5, ProcessModel::Pid));
        QPersistentModelIndex removed(model.index(3, ProcessModel::Pid));
        const auto identity = retained.data(ProcessModel::IdentityRole);
        QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);
        QVERIFY(model.apply(snapshot(2, {process(8), process(6), process(2), process(9)})));
        QCOMPARE(model.count(), 4);
        QCOMPARE(removals.count(), 3);
        QCOMPARE(removals.at(0).at(1).toInt(), 6);
        QCOMPARE(removals.at(0).at(2).toInt(), 6);
        QCOMPARE(removals.at(1).at(1).toInt(), 2);
        QCOMPARE(removals.at(1).at(2).toInt(), 4);
        QCOMPARE(removals.at(2).at(1).toInt(), 0);
        QCOMPARE(removals.at(2).at(2).toInt(), 0);
        QVERIFY(retained.isValid());
        QCOMPARE(retained.row(), 1);
        QCOMPARE(retained.data(ProcessModel::IdentityRole), identity);
        QCOMPARE(retained.data().toULongLong(), 6ULL);
        QVERIFY(!removed.isValid());
        removals.clear();
        QVERIFY(model.apply(snapshot(3, {})));
        QCOMPARE(removals.count(), 1);
        QCOMPARE(removals.at(0).at(1).toInt(), 0);
        QCOMPARE(removals.at(0).at(2).toInt(), 3);
        QVERIFY(!retained.isValid());
    }

    void churnPreservesIdentity() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QVERIFY(model.apply(snapshot(1, {process(1), process(2), process(3)})));
        QPersistentModelIndex retained(model.index(1, ProcessModel::Name));
        QPersistentModelIndex removed(model.index(0, ProcessModel::Name));
        const auto identity = retained.data(ProcessModel::IdentityRole);
        auto updated = process(2);
        updated.name = "updated";
        QVERIFY(model.apply(snapshot(2, {process(4), process(3), updated})));
        QCOMPARE(model.count(), 3);
        QVERIFY(!removed.isValid());
        QVERIFY(retained.isValid());
        QCOMPARE(retained.row(), 0);
        QCOMPARE(retained.data(ProcessModel::IdentityRole), identity);
        QCOMPARE(retained.data().toString(), QStringLiteral("updated"));
        QCOMPARE(model.data(model.index(2, ProcessModel::Pid)).toULongLong(), 4ULL);
        QCOMPARE(resets.count(), 0);
        QVERIFY(model.apply(snapshot(3, {})));
        QCOMPARE(model.count(), 0);
        QVERIFY(!retained.isValid());
    }

    void pidReuseAndContextReset() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QVERIFY(model.apply(snapshot(1, {process(4, 20)})));
        QPersistentModelIndex previous(model.index(0, 0));
        const auto old_identity = previous.data(ProcessModel::IdentityRole);
        QVERIFY(model.apply(snapshot(2, {process(4, 30)})));
        QVERIFY(!previous.isValid());
        QVERIFY(model.index(0, 0).data(ProcessModel::IdentityRole) != old_identity);
        QPersistentModelIndex before_context(model.index(0, 0));
        auto changed = snapshot(3, {process(4, 30)});
        changed->pid_namespace = "pid:[456]";
        QVERIFY(model.apply(changed));
        QVERIFY(!before_context.isValid());
        QCOMPARE(resets.count(), 1);
        changed = snapshot(4, {process(4, 30)});
        changed->boot_id = "boot-B";
        changed->pid_namespace = "pid:[456]";
        QVERIFY(model.apply(changed));
        QCOMPARE(resets.count(), 2);
    }

    void binaryContextIdentityEncodingAndReuse() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto binary_snapshot = [](std::uint64_t sequence, std::vector<night::ProcessInfo> rows) {
            auto value = snapshot(sequence, std::move(rows));
            value->boot_id = std::string{"b:\0\n\xff", 5};
            value->pid_namespace = std::string{"p:\t\0", 4};
            return value;
        };
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto prefix = QStringLiteral("623a000aff:703a0900:");
        const auto low_identity = prefix + QStringLiteral("1:0");
        const auto high_identity = prefix + QStringLiteral("18446744073709551615:18446744073709551615");
        QVERIFY(model.apply(binary_snapshot(1, {process(1, 0), process(maximum, maximum)})));
        QCOMPARE(model.index(0, 0).data(ProcessModel::IdentityRole).toString(), low_identity);
        QCOMPARE(model.index(1, 0).data(ProcessModel::IdentityRole).toString(), high_identity);
        QPersistentModelIndex low(model.index(0, 0)), high(model.index(1, 0));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy cells(&model, &QAbstractItemModel::dataChanged);

        QVERIFY(model.apply(binary_snapshot(2, {process(maximum, maximum), process(17, maximum), process(1, 0)})));
        QVERIFY(low.isValid());
        QVERIFY(high.isValid());
        QCOMPARE(low.row(), 0);
        QCOMPARE(high.row(), 1);
        QCOMPARE(low.data(ProcessModel::IdentityRole).toString(), low_identity);
        QCOMPARE(high.data(ProcessModel::IdentityRole).toString(), high_identity);
        QCOMPARE(model.index(2, 0).data(ProcessModel::IdentityRole).toString(),
                 prefix + QStringLiteral("17:18446744073709551615"));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(cells.count(), 0);

        auto invalid = binary_snapshot(3, {process(1, 0), process(1, maximum)});
        invalid->boot_id = "discarded context";
        QVERIFY(model.apply(invalid));
        QCOMPARE(model.quality(), QStringLiteral("error"));
        QVERIFY(low.isValid());
        QVERIFY(high.isValid());
        QCOMPARE(low.data(ProcessModel::IdentityRole).toString(), low_identity);
        QCOMPARE(high.data(ProcessModel::IdentityRole).toString(), high_identity);
        QCOMPARE(resets.count(), 0);

        auto changed = binary_snapshot(4, {process(17, maximum), process(maximum, maximum), process(1, 0)});
        changed->pid_namespace.push_back('x'); // Differ only after the embedded NUL.
        QVERIFY(model.apply(changed));
        QVERIFY(!low.isValid());
        QVERIFY(!high.isValid());
        QCOMPARE(model.quality(), QStringLiteral("valid"));
        QCOMPARE(resets.count(), 1);
        const auto changed_prefix = QStringLiteral("623a000aff:703a090078:");
        QCOMPARE(model.index(0, 0).data(ProcessModel::IdentityRole).toString(),
                 changed_prefix + QStringLiteral("17:18446744073709551615"));
        QCOMPARE(model.index(1, 0).data(ProcessModel::IdentityRole).toString(),
                 changed_prefix + QStringLiteral("18446744073709551615:18446744073709551615"));
        QCOMPARE(model.index(2, 0).data(ProcessModel::IdentityRole).toString(),
                 changed_prefix + QStringLiteral("1:0"));

        QPersistentModelIndex before_boot_change(model.index(2, 0));
        changed = binary_snapshot(5, {process(maximum, maximum)});
        changed->boot_id.back() = '\xfe';
        changed->pid_namespace.push_back('x');
        QVERIFY(model.apply(changed));
        QVERIFY(!before_boot_change.isValid());
        QCOMPARE(resets.count(), 2);
        QCOMPARE(model.index(0, 0).data(ProcessModel::IdentityRole).toString(),
                 QStringLiteral("623a000afe:703a090078:18446744073709551615:18446744073709551615"));
    }

    void unavailableFieldsAndSafeNames() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto value = process(7);
        value.name = std::string("<b>\n\0", 6) + "\xff" + "\xe2\x80\xae";
        value.cpu_percent = {0.0, night::Quality::warming};
        value.rss_bytes = {1024, night::Quality::error};
        QVERIFY(model.apply(snapshot(1, {value})));
        const auto name = model.index(0, ProcessModel::Name).data().toString();
        QVERIFY(name.startsWith(QStringLiteral("<b>\\u000a\\u0000")));
        QVERIFY(name.contains(QChar::ReplacementCharacter));
        QVERIFY(name.endsWith(QStringLiteral("\\u202e")));
        const auto dash = QString(QChar(0x2014));
        QCOMPARE(model.index(0, ProcessModel::Cpu).data().toString(), dash);
        QCOMPARE(model.index(0, ProcessModel::Rss).data().toString(), dash);
        QVERIFY(!model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).isValid());
        value.cpu_percent = {std::numeric_limits<double>::infinity(), night::Quality::valid};
        QVERIFY(model.apply(snapshot(2, {value})));
        QCOMPARE(model.index(0, ProcessModel::Cpu).data().toString(), dash);
        value.cpu_percent = {0.0, night::Quality::valid};
        value.rss_bytes = {0, night::Quality::valid};
        QVERIFY(model.apply(snapshot(3, {value})));
        QCOMPARE(model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 0.0);
        QCOMPARE(model.index(0, ProcessModel::Rss).data(ProcessModel::SortRole).toULongLong(), 0ULL);
    }

    void failedSnapshotAndSequence() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy changes(&model, &ProcessModel::changed);
        QVERIFY(model.apply(snapshot(2, {process(8)})));
        QPersistentModelIndex retained(model.index(0, 0));
        auto failed = snapshot(3, {});
        failed->stamp.quality = night::Quality::permission_denied;
        QVERIFY(model.apply(failed));
        QCOMPARE(model.sequence(), 3ULL);
        QCOMPARE(model.quality(), QStringLiteral("permission_denied"));
        QCOMPARE(model.count(), 1);
        QVERIFY(retained.isValid());
        QVERIFY(!model.apply(snapshot(2, {})));
        QVERIFY(!model.apply(snapshot(3, {})));
        QVERIFY(!model.apply({}));
        QCOMPARE(changes.count(), 2);
        QCOMPARE(model.count(), 1);
        QVERIFY(model.apply(snapshot(4, {process(8)})));
        QCOMPARE(model.quality(), QStringLiteral("valid"));
        QVERIFY(retained.isValid());
    }

    void malformedIdentityFailsClosed() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.apply(snapshot(1, {process(8)})));
        QPersistentModelIndex retained(model.index(0, 0));
        QVERIFY(model.apply(snapshot(2, {process(9), process(9, 100)})));
        QCOMPARE(model.quality(), QStringLiteral("error"));
        QCOMPARE(model.count(), 1);
        QVERIFY(retained.isValid());
        auto invalid = snapshot(3, {process(10)});
        invalid->boot_id.clear();
        QVERIFY(model.apply(invalid));
        QCOMPARE(model.quality(), QStringLiteral("error"));
        QVERIFY(model.apply(snapshot(4, {process(0)})));
        QCOMPARE(model.quality(), QStringLiteral("error"));
        QCOMPARE(model.index(0, 0).data().toULongLong(), 8ULL);
    }

    void headersAndNumericSort() {
        ProcessModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.apply(snapshot(1, {process(123)})));
        QCOMPARE(model.columnCount(), 6);
        QCOMPARE(model.rowCount(model.index(0, 0)), 0);
        QCOMPARE(model.headerData(ProcessModel::Rss, Qt::Horizontal).toString(), QStringLiteral("RSS MiB"));
        QCOMPARE(model.index(0, ProcessModel::Cpu).data(ProcessModel::SortRole).toDouble(), 12.5);
        QCOMPARE(model.index(0, ProcessModel::Rss).data(ProcessModel::SortRole).toULongLong(), 2097152ULL);
        QCOMPARE(model.index(0, ProcessModel::Pid).data(ProcessModel::SortRole).toULongLong(), 123ULL);
        QVERIFY(!model.data({}).isValid());
    }
};

QTEST_GUILESS_MAIN(ProcessModelTests)
#include "qt_process_model_tests.moc"
