#include "telemetry_bridge.hpp"
#include <QtTest>
#include <atomic>
#include <future>
#include <limits>

using namespace std::chrono_literals;

namespace {
night::CpuReading core(std::string name, double busy = 0, double iowait = 0) {
    return {std::move(name), {busy, night::Quality::valid}, {iowait, night::Quality::valid}};
}
night::SystemSnapshot sample(night::Clock::time_point end, std::vector<night::CpuReading> cores) {
    night::SystemSnapshot result;
    result.stamp.begin = result.stamp.end = end;
    result.cpus = std::move(cores);
    return result;
}
night::EngineSources sources(std::function<night::SystemSnapshot()> system) {
    night::EngineSources result;
    result.system = std::move(system);
    result.processes = [] { return night::ProcessSnapshot{}; };
    result.io = [] { return night::IoSnapshot{}; };
    result.capacity = [] { return night::CapacitySnapshot{}; };
    return result;
}
QVariantMap field(const QVariantList& rows, qsizetype row, const char* name) {
    return rows.at(row).toMap().value(QString::fromLatin1(name)).toMap();
}
}

class CoreCpuTests final : public QObject {
    Q_OBJECT
private slots:
    void missingSourceIsWarming() {
        std::promise<void> release;
        const auto ready = release.get_future().share();
        night::Engine engine{{}, sources([ready] { ready.wait(); return night::SystemSnapshot{}; })};
        night::ui::TelemetryBridge bridge{engine};
        bridge.refresh();
        const auto quality = bridge.coreCpuQuality();
        const auto age = bridge.coreCpuAgeMs();
        const bool empty = bridge.coreCpu().isEmpty();
        release.set_value(); // Release the worker before any test assertion can return.
        QCOMPARE(quality, QStringLiteral("warming"));
        QCOMPARE(age, -1LL);
        QVERIFY(empty);
    }

    void zeroAndHundredExcludeAggregateAndUnrelatedIssues() {
        const auto end = night::Clock::now();
        night::Engine engine{{}, sources([end] {
            auto result = sample(end, {core("cpu", 25, 1), core("cpu0", 0, 100), core("cpu1", 100, 100)});
            result.total_bytes = {{}, night::Quality::permission_denied};
            result.stamp.issues.push_back({night::Quality::permission_denied, "fixture meminfo", "memory unavailable"});
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
        QVERIFY(bridge.coreCpuMessage().isEmpty());
        QCOMPARE(bridge.coreCpuAgeMs(), 0LL);
        QCOMPARE(bridge.coreCpu().size(), 2);
        QCOMPARE(bridge.coreCpu().front().toMap().value("name").toString(), QStringLiteral("cpu0"));
        QCOMPARE(field(bridge.coreCpu(), 0, "busyPercent").value("value").toDouble(), 0.0);
        QCOMPARE(field(bridge.coreCpu(), 0, "busyPercent").value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(field(bridge.coreCpu(), 0, "iowaitPercent").value("value").toDouble(), 100.0);
        QCOMPARE(field(bridge.coreCpu(), 1, "busyPercent").value("value").toDouble(), 100.0);
        QCOMPARE(field(bridge.coreCpu(), 1, "iowaitPercent").value("value").toDouble(), 100.0);
        QCOMPARE(bridge.system().value("cpuBusyPercent").toMap().value("value").toDouble(), 25.0);
        QCOMPARE(bridge.system().value("totalBytes").toMap().value("quality").toString(), QStringLiteral("permission_denied"));
    }

    void invalidAndUnavailableFieldsStayDistinct() {
        const auto end = night::Clock::now();
        night::Engine engine{{}, sources([end] {
            auto result = sample(end, {core("cpu0", -1, 101),
                core("cpu1", std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()),
                core("cpu2"), core("cpu3")});
            result.cpus[2].busy_percent = {{}, night::Quality::valid};
            result.cpus[2].iowait_percent = {{}, night::Quality::unsupported};
            result.cpus[3].busy_percent = {{}, night::Quality::warming};
            result.cpus[3].iowait_percent = {{}, night::Quality::permission_denied};
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("partial"));
        QCOMPARE(bridge.coreCpu().size(), 4);
        QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("fields unavailable or invalid")));
        for (qsizetype row = 0; row < 2; ++row) for (const auto* key : {"busyPercent", "iowaitPercent"}) {
            QCOMPARE(field(bridge.coreCpu(), row, key).value("quality").toString(), QStringLiteral("error"));
            QVERIFY(!field(bridge.coreCpu(), row, key).value("value").isValid());
        }
        QCOMPARE(field(bridge.coreCpu(), 2, "busyPercent").value("quality").toString(), QStringLiteral("error"));
        QCOMPARE(field(bridge.coreCpu(), 2, "iowaitPercent").value("quality").toString(), QStringLiteral("unsupported"));
        QCOMPARE(field(bridge.coreCpu(), 3, "busyPercent").value("quality").toString(), QStringLiteral("warming"));
        QCOMPARE(field(bridge.coreCpu(), 3, "iowaitPercent").value("quality").toString(), QStringLiteral("permission_denied"));
    }

    void aggregateFallbackAvailability_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("expected");
        QTest::newRow("healthy-aggregate-only") << 0 << QStringLiteral("valid");
        QTest::newRow("cpu-error-healthy-memory") << 1 << QStringLiteral("error");
        QTest::newRow("cpu-denied") << 2 << QStringLiteral("permission_denied");
        QTest::newRow("cpu-unsupported") << 3 << QStringLiteral("unsupported");
        QTest::newRow("cpu-warming") << 4 << QStringLiteral("warming");
        QTest::newRow("cpu-stale") << 5 << QStringLiteral("stale");
        QTest::newRow("cpu-missing-valid-value") << 6 << QStringLiteral("error");
        QTest::newRow("healthy-cores-ignore-aggregate-error") << 7 << QStringLiteral("valid");
    }
    void aggregateFallbackAvailability() {
        QFETCH(int, mode);
        QFETCH(QString, expected);
        const auto end = night::Clock::now();
        night::Engine engine{{}, sources([end, mode] {
            auto result = sample(end, {core("cpu")});
            result.total_bytes = {1024, night::Quality::valid};
            auto& aggregate = result.cpus.front();
            if (mode == 1 || mode == 7) {
                aggregate.busy_percent = {{}, night::Quality::error};
                aggregate.iowait_percent = {{}, night::Quality::warming};
            }
            if (mode == 2) {
                aggregate.busy_percent = {{}, night::Quality::permission_denied};
                aggregate.iowait_percent = {{}, night::Quality::unsupported};
            }
            if (mode == 3) {
                aggregate.busy_percent = {{}, night::Quality::unsupported};
                aggregate.iowait_percent = {{}, night::Quality::warming};
            }
            if (mode == 4) aggregate.busy_percent = {{}, night::Quality::warming};
            if (mode == 5) aggregate.busy_percent = {{}, night::Quality::stale};
            if (mode == 6) aggregate.busy_percent = {{}, night::Quality::valid};
            if (mode == 7) result.cpus.push_back(core("cpu0"));
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.system().value("totalBytes").toMap().value("value").toULongLong(), 1024ULL);
        QCOMPARE(bridge.coreCpuQuality(), expected);
        QCOMPARE(bridge.coreCpu().size(), mode == 7 ? 1 : 0);
        QCOMPARE(bridge.coreCpuAgeMs(), mode == 0 || mode == 5 || mode == 7 ? 0LL : -1LL);
        if (mode == 0 || mode == 7) QVERIFY(bridge.coreCpuMessage().isEmpty());
        else QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("aggregate CPU fields")));
    }

    void canonicalNamesAndDuplicates() {
        const auto end = night::Clock::now();
        night::Engine engine{{}, sources([end] {
            return sample(end, {core("cpu"), core("cpu1"), core("cpu01"), core("cpu2"), core("cpu2"), core("cpu0"),
                core("cpu+3"), core("cpu-3"), core("cpu3\n"), core("cpu<script>"), core("cpu18446744073709551616"),
                core("cpu18446744073709551615"), core("cpu000000000000000000000"), core(""), core(std::string(5000, 'x'))});
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("partial"));
        QCOMPARE(bridge.coreCpu().size(), 3);
        // Invalid cpu01 is rejected outright; it neither aliases nor hides canonical cpu1.
        QCOMPARE(bridge.coreCpu()[0].toMap().value("name").toString(), QStringLiteral("cpu1"));
        QCOMPARE(bridge.coreCpu()[1].toMap().value("name").toString(), QStringLiteral("cpu0"));
        QCOMPARE(bridge.coreCpu()[2].toMap().value("name").toString(), QStringLiteral("cpu18446744073709551615"));
        QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("invalid canonical")));
        QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("duplicate IDs")));
        QVERIFY(!bridge.coreCpuMessage().contains(QStringLiteral("<script>")));
        QVERIFY(bridge.coreCpuMessage().size() <= 4096);
    }

    void boundedSourcePrefix_data() {
        QTest::addColumn<bool>("aggregate");
        QTest::newRow("with-aggregate") << true;
        QTest::newRow("without-aggregate") << false;
    }
    void boundedSourcePrefix() {
        QFETCH(bool, aggregate);
        const auto end = night::Clock::now();
        night::Engine engine{{}, sources([end, aggregate] {
            auto result = sample(end, {});
            if (aggregate) result.cpus.push_back(core("cpu"));
            for (int i = 0; i < 70; ++i) result.cpus.push_back(core("cpu" + std::to_string(i)));
            // This duplicate is outside the inspected 65-record prefix.
            result.cpus.push_back(core("cpu0"));
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpu().size(), 64);
        QCOMPARE(bridge.coreCpu().front().toMap().value("name").toString(), QStringLiteral("cpu0"));
        QCOMPARE(bridge.coreCpu().back().toMap().value("name").toString(), QStringLiteral("cpu63"));
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("partial"));
        QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("truncated")));
    }

    void stableGenerationsHotplugFailureAndEmptyRecovery() {
        const auto end = night::Clock::now();
        std::atomic_int mode{0};
        night::EngineConfig config;
        config.system_interval = 10ms;
        night::Engine engine{config, sources([&] {
            const auto selected = mode.load();
            auto result = sample(end + std::chrono::milliseconds{selected}, {core("cpu"), core("cpu0")});
            if (selected <= 2) result.cpus.push_back(core(selected == 0 ? "cpu1" : "cpu2"));
            if (selected == 2) {
                result.stamp.quality = night::Quality::error;
                result.stamp.issues.push_back({night::Quality::error, "fixture", "source failure"});
            }
            if (selected == 4) result.cpus.resize(1);
            if (selected == 5) result.cpus.clear();
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QSignalSpy changes(&bridge, &night::ui::TelemetryBridge::coreCpuChanged);
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end + 1s);
        QCOMPARE(bridge.coreCpu().size(), 2);
        QCOMPARE(changes.count(), 1);
        const auto sequence = engine.system()->stamp.sequence;
        QTRY_VERIFY_WITH_TIMEOUT(engine.system()->stamp.sequence > sequence, 2000);
        bridge.refreshAt(end + 1s);
        QCOMPARE(changes.count(), 1); // Identical fields in a new generation do not republish the list.
        const auto unchanged_message = bridge.coreCpuMessage();
        bridge.refreshAt(end + 1500ms);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
        bridge.refreshAt(end + 1501ms);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.coreCpu().size(), 2);
        QCOMPARE(bridge.coreCpuMessage(), unchanged_message);
        QCOMPARE(changes.count(), 1);
        for (int selected = 1; selected <= 5; ++selected) {
            mode = selected;
            QTRY_VERIFY_WITH_TIMEOUT(engine.system()->stamp.end == end + std::chrono::milliseconds{selected}, 2000);
            bridge.refreshAt(end + 1s);
            if (selected == 1) {
                QCOMPARE(bridge.coreCpu().size(), 2);
                QCOMPARE(bridge.coreCpu().back().toMap().value("name").toString(), QStringLiteral("cpu2"));
                QCOMPARE(changes.count(), 2);
            } else if (selected == 2) {
                QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("error"));
                QCOMPARE(bridge.coreCpuAgeMs(), -1LL);
                QVERIFY(bridge.coreCpu().isEmpty());
                QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("source failure")));
                QCOMPARE(changes.count(), 3);
            } else if (selected == 3) {
                QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
                QCOMPARE(bridge.coreCpu().size(), 1);
                QVERIFY(bridge.coreCpuMessage().isEmpty());
                QCOMPARE(changes.count(), 4);
            } else {
                QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
                QVERIFY(bridge.coreCpu().isEmpty());
                QCOMPARE(changes.count(), 5); // Aggregate-only and fully empty samples share an empty list.
            }
        }
    }

    void systemCadenceIsIndependent_data() {
        QTest::addColumn<bool>("slow_system");
        QTest::newRow("slow-system") << true;
        QTest::newRow("fast-system") << false;
    }
    void systemCadenceIsIndependent() {
        QFETCH(bool, slow_system);
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.system_interval = slow_system ? 2000ms : 250ms;
        config.process_interval = config.io_interval = slow_system ? 250ms : 2000ms;
        auto input = sources([end] { return sample(end, {core("cpu0")}); });
        input.io = [end] {
            night::IoSnapshot result;
            result.stamp.begin = result.stamp.end = end;
            return result;
        };
        night::Engine engine{config, input};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system() && engine.io(), 2000);
        bridge.refreshAt(end + 1500ms);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
        for (const auto offset : {2000ms, 6000ms}) {
            bridge.refreshAt(end + offset);
            QCOMPARE(bridge.coreCpuQuality(), slow_system ? QStringLiteral("valid") : QStringLiteral("stale"));
            QCOMPARE(bridge.ioQuality(), slow_system ? QStringLiteral("stale") : QStringLiteral("valid"));
            QCOMPARE(bridge.coreCpuAgeMs(), offset.count());
        }
        bridge.refreshAt(end + 6001ms);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("stale"));
    }

    void timestampAndSourceQualities_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("expected");
        QTest::newRow("default") << 0 << QStringLiteral("warming");
        QTest::newRow("default-begin") << 1 << QStringLiteral("warming");
        QTest::newRow("default-end") << 2 << QStringLiteral("warming");
        QTest::newRow("reversed") << 3 << QStringLiteral("error");
        QTest::newRow("future") << 4 << QStringLiteral("error");
        QTest::newRow("source-error") << 5 << QStringLiteral("error");
        QTest::newRow("source-denied") << 6 << QStringLiteral("permission_denied");
        QTest::newRow("source-unsupported") << 7 << QStringLiteral("unsupported");
        QTest::newRow("source-warming") << 8 << QStringLiteral("warming");
        QTest::newRow("ordered-stale") << 9 << QStringLiteral("stale");
    }
    void timestampAndSourceQualities() {
        QFETCH(int, mode);
        QFETCH(QString, expected);
        const auto end = night::Clock::now();
        night::Engine engine{{}, sources([end, mode] {
            auto result = sample(end, {core("cpu0")});
            if (mode == 0) result.stamp.begin = result.stamp.end = {};
            if (mode == 1) result.stamp.begin = {};
            if (mode == 2) result.stamp.end = {};
            if (mode == 3) result.stamp.begin = end + 1ms;
            if (mode == 4 || mode == 5) result.stamp.begin = result.stamp.end = end + 1s;
            if (mode == 5) result.stamp.quality = night::Quality::error;
            if (mode == 6) result.stamp.quality = night::Quality::permission_denied;
            if (mode == 7) result.stamp.quality = night::Quality::unsupported;
            if (mode == 8) result.stamp.quality = night::Quality::warming;
            if (mode == 9) result.stamp.quality = night::Quality::stale;
            if (mode >= 5 && mode <= 8) result.stamp.issues.push_back({result.stamp.quality, "fixture", "source diagnostic"});
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpuQuality(), expected);
        QCOMPARE(bridge.coreCpu().isEmpty(), mode != 9);
        QCOMPARE(bridge.coreCpuAgeMs(), mode == 9 ? 0LL : -1LL);
        if (mode >= 5 && mode <= 8) QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("source diagnostic")));
    }

    void futureSnapshotRecoversWithoutPublication() {
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.system_interval = 2000ms;
        night::Engine engine{config, sources([end] { return sample(end + 1s, {core("cpu0")}); })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("error"));
        QVERIFY(bridge.coreCpuMessage().contains(QStringLiteral("future")));
        QSignalSpy changes(&bridge, &night::ui::TelemetryBridge::coreCpuChanged);
        bridge.refreshAt(end + 1s);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.coreCpuAgeMs(), 0LL);
        QCOMPARE(bridge.coreCpu().size(), 1);
        QVERIFY(bridge.coreCpuMessage().isEmpty());
        bridge.refreshAt(end + 7s);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("valid"));
        bridge.refreshAt(end + 7001ms);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("stale"));
        QCOMPARE(changes.count(), 1);
        bridge.refreshAt(end);
        QCOMPARE(bridge.coreCpuQuality(), QStringLiteral("error"));
        QVERIFY(bridge.coreCpu().isEmpty());
        bridge.refreshAt(end + 1s);
        QCOMPARE(bridge.coreCpu().size(), 1);
        QVERIFY(bridge.coreCpuMessage().isEmpty());
        QCOMPARE(changes.count(), 3);
    }
};

QTEST_GUILESS_MAIN(CoreCpuTests)
#include "qt_core_cpu_tests.moc"
