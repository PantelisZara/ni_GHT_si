#include "telemetry_bridge.hpp"
#include <QtTest>
#include <atomic>
#include <future>
#include <limits>

using namespace std::chrono_literals;

namespace {
night::Stamp stamp(night::Clock::time_point end) {
    night::Stamp result;
    result.begin = result.end = end;
    return result;
}
night::Throughput rate(std::string name, std::string identity) {
    return {std::move(name), std::move(identity), {0.0, night::Quality::valid}, {1.5, night::Quality::valid}};
}
night::Capacity mount(std::string path, std::uint64_t total = 1000,
                      std::uint64_t used = 200, std::uint64_t available = 700) {
    return {std::move(path), {total, night::Quality::valid}, {used, night::Quality::valid},
            {available, night::Quality::valid}};
}
night::EngineSources sources(std::function<night::IoSnapshot()> io,
                              std::function<night::CapacitySnapshot()> capacity) {
    night::EngineSources result;
    result.system = [] { return night::SystemSnapshot{}; };
    result.processes = [] { return night::ProcessSnapshot{}; };
    result.io = std::move(io);
    result.capacity = std::move(capacity);
    return result;
}
QVariantMap field(const QVariantList& rows, qsizetype row, const char* name) {
    return rows.at(row).toMap().value(QString::fromLatin1(name)).toMap();
}
}

class IoBridgeTests final : public QObject {
    Q_OBJECT
private slots:
    void missingSourcesAreWarming() {
        std::promise<void> release;
        const auto ready = release.get_future().share();
        night::Engine engine{{}, sources([ready] { ready.wait(); return night::IoSnapshot{}; },
                                         [ready] { ready.wait(); return night::CapacitySnapshot{}; })};
        night::ui::TelemetryBridge bridge{engine};
        bridge.refresh();
        const auto io_quality = bridge.ioQuality(), capacity_quality = bridge.capacityQuality();
        const auto io_age = bridge.ioAgeMs(), capacity_age = bridge.capacityAgeMs();
        const bool empty = bridge.networkRates().isEmpty() && bridge.diskRates().isEmpty()
            && bridge.filesystemCapacity().isEmpty();
        release.set_value(); // No early test assertion can leave a collector blocked.
        QCOMPARE(io_quality, QStringLiteral("warming"));
        QCOMPARE(capacity_quality, QStringLiteral("warming"));
        QCOMPARE(io_age, -1LL);
        QCOMPARE(capacity_age, -1LL);
        QVERIFY(empty);
    }

    void zerosExactIntegersAndStableSignals() {
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.io_interval = config.capacity_interval = 10ms;
        auto inputs = sources([end] {
            night::IoSnapshot result;
            result.stamp = stamp(end);
            result.network.push_back(rate("fixture-net", "network-generation"));
            result.disks.push_back(rate("fixture-disk", "disk-generation"));
            return result;
        }, [end] {
            night::CapacitySnapshot result;
            result.stamp = stamp(end);
            result.mounts = {mount("/exact", std::numeric_limits<std::uint64_t>::max(), 0,
                                   std::numeric_limits<std::uint64_t>::max()),
                             mount("/empty", 0, 0, 0), mount("/reserved")};
            return result;
        });
        night::Engine engine{config, inputs};
        night::ui::TelemetryBridge bridge{engine};
        QSignalSpy io_changes(&bridge, &night::ui::TelemetryBridge::ioChanged);
        QSignalSpy capacity_changes(&bridge, &night::ui::TelemetryBridge::capacityChanged);
        QTRY_VERIFY_WITH_TIMEOUT(engine.io() && engine.capacity(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.ioIssueCount(), 0ULL);
        QCOMPARE(bridge.capacityIssueCount(), 0ULL);
        QVERIFY(bridge.ioMessage().isEmpty());
        QVERIFY(bridge.capacityMessage().isEmpty());
        QCOMPARE(bridge.networkRates().size(), 1);
        QCOMPARE(bridge.diskRates().size(), 1);
        QCOMPARE(bridge.filesystemCapacity().size(), 3);
        const auto read = field(bridge.networkRates(), 0, "readBytesPerSecond");
        QCOMPARE(read.value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(read.value("value").toDouble(), 0.0);
        QCOMPARE(field(bridge.diskRates(), 0, "writtenBytesPerSecond").value("value").toDouble(), 1.5);
        const auto exact = field(bridge.filesystemCapacity(), 0, "totalBytes").value("value");
        QCOMPARE(exact.metaType().id(), QMetaType::ULongLong);
        QCOMPARE(exact.toULongLong(), std::numeric_limits<std::uint64_t>::max());
        QCOMPARE(field(bridge.filesystemCapacity(), 1, "totalBytes").value("value").toULongLong(), 0ULL);
        QCOMPARE(field(bridge.filesystemCapacity(), 1, "totalBytes").value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(field(bridge.filesystemCapacity(), 2, "availableBytes").value("value").toULongLong(), 700ULL);
        QCOMPARE(io_changes.count(), 1);
        QCOMPARE(capacity_changes.count(), 1);
        const auto io_sequence = engine.io()->stamp.sequence, capacity_sequence = engine.capacity()->stamp.sequence;
        QTRY_VERIFY_WITH_TIMEOUT(engine.io()->stamp.sequence > io_sequence
                                && engine.capacity()->stamp.sequence > capacity_sequence, 2000);
        bridge.refreshAt(end);
        bridge.refreshAt(end + 1500ms);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
        bridge.refreshAt(end + 1501ms);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.ioAgeMs(), 1501LL);
        QCOMPARE(bridge.capacityAgeMs(), 1501LL);
        QCOMPARE(io_changes.count(), 1);
        QCOMPARE(capacity_changes.count(), 1);
    }

    void independentCadencesAndClockRecovery() {
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.io_interval = 2000ms;
        config.capacity_interval = 30000ms;
        night::Engine engine{config, sources([end] {
            night::IoSnapshot result;
            result.stamp = stamp(end);
            result.network.push_back(rate("fixture-net", "network-generation"));
            return result;
        }, [end] {
            night::CapacitySnapshot result;
            result.stamp = stamp(end);
            result.mounts.push_back(mount("/fixture"));
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.io() && engine.capacity(), 2000);
        for (const auto offset : {2000ms, 6000ms}) {
            bridge.refreshAt(end + offset);
            QCOMPARE(bridge.ioQuality(), QStringLiteral("valid"));
            QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
        }
        bridge.refreshAt(end + 6001ms);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
        bridge.refreshAt(end + 90000ms);
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
        bridge.refreshAt(end + 90001ms);
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.filesystemCapacity().size(), 1);
        QSignalSpy io_changes(&bridge, &night::ui::TelemetryBridge::ioChanged);
        QSignalSpy capacity_changes(&bridge, &night::ui::TelemetryBridge::capacityChanged);
        bridge.refreshAt(end - 1ms);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("error"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("error"));
        QVERIFY(bridge.networkRates().isEmpty() && bridge.filesystemCapacity().isEmpty());
        QCOMPARE(bridge.ioAgeMs(), -1LL);
        QCOMPARE(bridge.capacityAgeMs(), -1LL);
        QVERIFY(bridge.ioMessage().contains(QStringLiteral("future")));
        bridge.refreshAt(end); // Same snapshot becomes usable again; no new generation is required.
        QCOMPARE(bridge.ioQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.networkRates().size(), 1);
        QCOMPARE(bridge.filesystemCapacity().size(), 1);
        QCOMPARE(io_changes.count(), 2);
        QCOMPARE(capacity_changes.count(), 2);
    }

    void invalidTimestampsAndSourceFailures_data() {
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
    void invalidTimestampsAndSourceFailures() {
        QFETCH(int, mode);
        QFETCH(QString, expected);
        const auto end = night::Clock::now();
        auto value = stamp(end);
        if (mode == 0) value.begin = value.end = {};
        if (mode == 1) value.begin = {};
        if (mode == 2) value.end = {};
        if (mode == 3) value.begin = end + 1ms;
        if (mode == 4 || mode == 5) value.begin = value.end = end + 1s;
        if (mode == 5) value.quality = night::Quality::error;
        if (mode == 6) value.quality = night::Quality::permission_denied;
        if (mode == 7) value.quality = night::Quality::unsupported;
        if (mode == 8) value.quality = night::Quality::warming;
        if (mode == 9) value.quality = night::Quality::stale;
        if (mode >= 5 && mode <= 8) value.issues.push_back({value.quality, "fixture", "source diagnostic"});
        night::Engine engine{{}, sources([value] {
            night::IoSnapshot result;
            result.stamp = value;
            result.network.push_back(rate("fixture-net", "network-generation"));
            return result;
        }, [value] {
            night::CapacitySnapshot result;
            result.stamp = value;
            result.mounts.push_back(mount("/fixture"));
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.io() && engine.capacity(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.ioQuality(), expected);
        QCOMPARE(bridge.capacityQuality(), expected);
        QCOMPARE(bridge.networkRates().isEmpty(), mode != 9);
        QCOMPARE(bridge.filesystemCapacity().isEmpty(), mode != 9);
        QCOMPARE(bridge.ioAgeMs(), mode == 9 ? 0LL : -1LL);
        QCOMPARE(bridge.capacityAgeMs(), mode == 9 ? 0LL : -1LL);
        if (mode >= 5 && mode <= 8) {
            QVERIFY(bridge.ioMessage().contains(QStringLiteral("source diagnostic")));
            QVERIFY(bridge.capacityMessage().contains(QStringLiteral("source diagnostic")));
        }
    }

    void fieldValidationEmptyFailureAndRecovery() {
        const auto end = night::Clock::now();
        std::atomic_int mode{0};
        night::EngineConfig config;
        config.io_interval = config.capacity_interval = 10ms;
        night::Engine engine{config, sources([&] {
            const auto selected = mode.load();
            night::IoSnapshot result;
            result.stamp = stamp(end + std::chrono::milliseconds{selected});
            if (selected == 2) return result;
            result.network.push_back(rate("fixture-net", "network-generation"));
            result.disks.push_back(rate("fixture-disk", "disk-generation"));
            if (selected == 1) {
                result.network.front().identity.clear();
                auto partition = rate("fixture-partition", "");
                partition.read_bytes_per_second = {{}, night::Quality::unsupported};
                partition.written_bytes_per_second = {{}, night::Quality::permission_denied};
                result.network.push_back(partition);
                auto mixed = rate("fixture-mixed", "");
                mixed.written_bytes_per_second = {{}, night::Quality::warming};
                result.network.push_back(mixed);
                result.network.push_back(rate("fixture-oversized-identity", std::string(4097, 'x')));
                result.disks.front().read_bytes_per_second = {std::numeric_limits<double>::quiet_NaN(), night::Quality::valid};
                result.disks.front().written_bytes_per_second = {std::numeric_limits<double>::infinity(), night::Quality::valid};
                auto invalid = rate("fixture-invalid", "invalid-generation");
                invalid.read_bytes_per_second = {-1.0, night::Quality::valid};
                invalid.written_bytes_per_second = {{}, night::Quality::valid};
                result.disks.push_back(invalid);
            }
            if (selected == 3) result.stamp.quality = night::Quality::error;
            return result;
        }, [&] {
            const auto selected = mode.load();
            night::CapacitySnapshot result;
            result.stamp = stamp(end + std::chrono::milliseconds{selected});
            if (selected == 2) return result;
            result.mounts.push_back(mount("/fixture"));
            if (selected == 1) {
                result.mounts.front() = mount("/overflow", 8, 9, std::numeric_limits<std::uint64_t>::max());
                result.mounts.push_back(mount("/reserved-invalid", 1000, 200, 801));
                auto absent = mount("/absent");
                absent.total_bytes = {{}, night::Quality::permission_denied};
                absent.available_bytes = {{}, night::Quality::valid};
                result.mounts.push_back(absent);
            }
            if (selected == 3) result.stamp.quality = night::Quality::permission_denied;
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.io() && engine.capacity(), 2000);
        bridge.refreshAt(end + 1s);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("valid"));
        for (int selected = 1; selected <= 4; ++selected) {
            mode = selected;
            const auto sample_end = end + std::chrono::milliseconds{selected};
            QTRY_VERIFY_WITH_TIMEOUT(engine.io()->stamp.end == sample_end && engine.capacity()->stamp.end == sample_end, 2000);
            bridge.refreshAt(end + 1s);
            if (selected == 1) {
                QCOMPARE(bridge.ioQuality(), QStringLiteral("partial"));
                QCOMPARE(bridge.capacityQuality(), QStringLiteral("partial"));
                QVERIFY(bridge.ioIssueCount() > 0 && bridge.capacityIssueCount() > 0);
                QCOMPARE(field(bridge.networkRates(), 0, "readBytesPerSecond").value("quality").toString(), QStringLiteral("error"));
                QCOMPARE(field(bridge.networkRates(), 1, "readBytesPerSecond").value("quality").toString(), QStringLiteral("unsupported"));
                QCOMPARE(field(bridge.networkRates(), 1, "writtenBytesPerSecond").value("quality").toString(), QStringLiteral("permission_denied"));
                QCOMPARE(field(bridge.networkRates(), 2, "readBytesPerSecond").value("quality").toString(), QStringLiteral("error"));
                QCOMPARE(field(bridge.networkRates(), 2, "writtenBytesPerSecond").value("quality").toString(), QStringLiteral("warming"));
                QCOMPARE(field(bridge.networkRates(), 3, "readBytesPerSecond").value("quality").toString(), QStringLiteral("error"));
                for (qsizetype row = 0; row < 2; ++row) for (const auto* key : {"readBytesPerSecond", "writtenBytesPerSecond"}) {
                    QCOMPARE(field(bridge.diskRates(), row, key).value("quality").toString(), QStringLiteral("error"));
                    QVERIFY(!field(bridge.diskRates(), row, key).value("value").isValid());
                }
                for (qsizetype row = 0; row < 2; ++row) for (const auto* key : {"totalBytes", "usedBytes", "availableBytes"}) {
                    QCOMPARE(field(bridge.filesystemCapacity(), row, key).value("quality").toString(), QStringLiteral("error"));
                    QVERIFY(!field(bridge.filesystemCapacity(), row, key).value("value").isValid());
                }
                QCOMPARE(field(bridge.filesystemCapacity(), 2, "totalBytes").value("quality").toString(), QStringLiteral("permission_denied"));
                QCOMPARE(field(bridge.filesystemCapacity(), 2, "availableBytes").value("quality").toString(), QStringLiteral("error"));
            } else if (selected == 2 || selected == 3) {
                QVERIFY(bridge.networkRates().isEmpty() && bridge.diskRates().isEmpty() && bridge.filesystemCapacity().isEmpty());
                QCOMPARE(bridge.ioQuality(), selected == 2 ? QStringLiteral("valid") : QStringLiteral("error"));
                QCOMPARE(bridge.capacityQuality(), selected == 2 ? QStringLiteral("valid") : QStringLiteral("permission_denied"));
            } else {
                QCOMPARE(bridge.ioQuality(), QStringLiteral("valid"));
                QCOMPARE(bridge.capacityQuality(), QStringLiteral("valid"));
                QCOMPARE(bridge.networkRates().size(), 1);
                QCOMPARE(bridge.filesystemCapacity().size(), 1);
                QVERIFY(bridge.ioMessage().isEmpty() && bridge.capacityMessage().isEmpty());
            }
        }
    }

    void boundsDiagnosticsAndAmbiguousNames() {
        const auto end = night::Clock::now();
        std::atomic_bool duplicates{false};
        night::EngineConfig config;
        config.io_interval = config.capacity_interval = 10ms;
        const auto diagnostic_stamp = [&](bool ambiguous) {
            auto result = stamp(end + (ambiguous ? 1ms : 0ms));
            for (int i = 0; i < 6; ++i)
                result.issues.push_back({night::Quality::error, "fixture\n\xE2\x80\xAE", "diagnostic-" + std::to_string(i)});
            result.issues.front().message += "\t" + std::string(5000, '\n');
            return result;
        };
        night::Engine engine{config, sources([&] {
            const bool ambiguous = duplicates.load();
            night::IoSnapshot result;
            result.stamp = diagnostic_stamp(ambiguous);
            if (ambiguous) {
                result.network = {rate("same", "one"), rate("same", "two"), rate("first", "shared"),
                    rate("second", "shared"), rate("\n", "newline"), rate("\\u000a", "literal"),
                    rate("safe", "safe-identity"), rate("", "empty"), rate(std::string(4097, 'x'), "oversized")};
                result.disks = {rate("same", "one"), rate("same", "two")};
            } else {
                for (int i = 0; i < 70; ++i) {
                    result.network.push_back(rate("network-" + std::to_string(i), "network-id-" + std::to_string(i)));
                    result.disks.push_back(rate("disk-" + std::to_string(i), "disk-id-" + std::to_string(i)));
                }
                result.network.front().name = "network\n\xE2\x80\xAE";
                result.disks.front().name = std::string(4090, 'x') + "\n";
            }
            return result;
        }, [&] {
            const bool ambiguous = duplicates.load();
            night::CapacitySnapshot result;
            result.stamp = diagnostic_stamp(ambiguous);
            if (ambiguous) result.mounts = {mount("/same"), mount("/same"), mount("/safe"), mount(""), mount(std::string(4097, 'x'))};
            else {
                for (int i = 0; i < 35; ++i) result.mounts.push_back(mount("/fixture-" + std::to_string(i)));
                result.mounts.front().path = "/fixture\t\xE2\x80\xAE";
            }
            return result;
        })};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.io() && engine.capacity(), 2000);
        bridge.refreshAt(end + 1s);
        QCOMPARE(bridge.networkRates().size(), 64);
        QCOMPARE(bridge.diskRates().size(), 64);
        QCOMPARE(bridge.filesystemCapacity().size(), 32);
        QCOMPARE(bridge.ioQuality(), QStringLiteral("partial"));
        QCOMPARE(bridge.capacityQuality(), QStringLiteral("partial"));
        QVERIFY(bridge.ioIssueCount() >= 8 && bridge.capacityIssueCount() >= 7);
        QCOMPARE(bridge.networkRates().front().toMap().value("name").toString(), QStringLiteral("network\\u000a\\u202e"));
        QVERIFY(bridge.diskRates().front().toMap().value("name").toString().size() <= 4096);
        QCOMPARE(bridge.filesystemCapacity().front().toMap().value("path").toString(), QStringLiteral("/fixture\\u0009\\u202e"));
        for (const auto& message : {bridge.ioMessage(), bridge.capacityMessage()}) {
            QVERIFY(message.size() <= 4096);
            QVERIFY(!message.contains(QLatin1Char('\n')) && !message.contains(QLatin1Char('\t')) && !message.contains(QChar{0x202e}));
            QVERIFY(message.contains(QStringLiteral("diagnostic-0")) && message.contains(QStringLiteral("diagnostic-1")));
            QVERIFY(message.contains(QStringLiteral("diagnostic-4")) && message.contains(QStringLiteral("diagnostic-5")));
            QVERIFY(!message.contains(QStringLiteral("diagnostic-2")) && !message.contains(QStringLiteral("diagnostic-3")));
            QVERIFY(message.contains(QStringLiteral("truncated")));
        }
        duplicates = true;
        QTRY_VERIFY_WITH_TIMEOUT(engine.io()->stamp.end == end + 1ms && engine.capacity()->stamp.end == end + 1ms, 2000);
        bridge.refreshAt(end + 1s);
        QCOMPARE(bridge.networkRates().size(), 1);
        QCOMPARE(bridge.networkRates().front().toMap().value("name").toString(), QStringLiteral("safe"));
        QVERIFY(bridge.diskRates().isEmpty());
        QCOMPARE(bridge.filesystemCapacity().size(), 1);
        QCOMPARE(bridge.filesystemCapacity().front().toMap().value("path").toString(), QStringLiteral("/safe"));
        QVERIFY(bridge.ioMessage().contains(QStringLiteral("duplicate")));
        QVERIFY(bridge.capacityMessage().contains(QStringLiteral("duplicate")));
    }
};

QTEST_GUILESS_MAIN(IoBridgeTests)
#include "qt_io_bridge_tests.moc"
