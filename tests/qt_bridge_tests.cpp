#include "telemetry_bridge.hpp"
#include <QtTest>
#include <atomic>
#include <future>
#include <limits>

using namespace std::chrono_literals;
class BridgeTests final : public QObject {
    Q_OBJECT
private slots:
    void liveRefreshConfigurationAndPreservedPresentation() {
        night::EngineConfig config;
        config.history_capacity = 17;
        config.include_application_scopes = true;
        std::atomic<std::uint64_t> total_bytes{1024};
        night::EngineSources sources;
        sources.system = [&total_bytes] {
            night::SystemSnapshot sample;
            sample.stamp.begin = sample.stamp.end = night::Clock::now();
            sample.total_bytes = {total_bytes.load(), night::Quality::valid};
            return sample;
        };
        sources.processes = [] {
            night::ProcessSnapshot sample;
            sample.stamp.begin = sample.stamp.end = night::Clock::now();
            sample.boot_id = "fixture-boot"; sample.pid_namespace = "fixture-pid";
            night::ProcessInfo process;
            process.pid = 5; process.start_ticks = 7;
            sample.processes.push_back(process);
            return sample;
        };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system() && engine.processes(), 2000);
        bridge.refresh();
        bridge.processView()->selectRow(0);
        const auto selected = bridge.processView()->selectedIdentity();
        QVERIFY(!selected.isEmpty());
        QVERIFY(!bridge.history().isEmpty());
        QSignalSpy changed(&bridge, &night::ui::TelemetryBridge::configurationChanged);
        QList<int> notified_presentation;
        connect(&bridge, &night::ui::TelemetryBridge::configurationChanged, this, [&] {
            notified_presentation.push_back(bridge.configuration().value("presentationMs").toInt());
        });
        QCOMPARE(bridge.refreshMode(), QStringLiteral("balanced"));
        QCOMPARE(bridge.configuration().value("presentationMs").toInt(), 500);
        QVERIFY(!bridge.setRefreshMode(QStringLiteral("unknown")));
        QCOMPARE(changed.count(), 0);
        for (const auto& mode : {QStringLiteral("fast"), QStringLiteral("low-power"), QStringLiteral("balanced")}) {
            QVERIFY(bridge.setRefreshMode(mode));
            QCOMPARE(bridge.refreshMode(), mode);
            const auto presentation_ms = mode == QStringLiteral("fast") ? 200 : mode == QStringLiteral("low-power") ? 1000 : 500;
            QCOMPARE(bridge.configuration().value("presentationMs").toInt(), presentation_ms);
            QCOMPARE(notified_presentation.back(), presentation_ms);
            QCOMPARE(bridge.configuration().value("systemMs").toLongLong(), mode == QStringLiteral("fast") ? 250LL
                : mode == QStringLiteral("low-power") ? 2000LL : 500LL);
            QCOMPARE(bridge.configuration().value("gpuMs").toLongLong(), mode == QStringLiteral("fast") ? 500LL
                : mode == QStringLiteral("low-power") ? 2000LL : 1000LL);
            QCOMPARE(bridge.configuration().value("historyCapacity").toULongLong(), 17ULL);
            QVERIFY(bridge.configuration().value("scopeMetadata").toBool());
            QCOMPARE(bridge.processView()->selectedIdentity(), selected);
            // Publish after the mode's immediate refresh. The live presentation
            // timer must consume this later sample without an explicit refresh.
            const auto next_bytes = total_bytes.fetch_add(1) + 1;
            QTRY_COMPARE_WITH_TIMEOUT(bridge.system().value("totalBytes").toMap().value("value").toULongLong(), next_bytes, 4000);
            QCOMPARE(bridge.processView()->selectedIdentity(), selected);
            QVERIFY(!bridge.history().isEmpty());
        }
        QCOMPARE(changed.count(), 3);
        QVERIFY(bridge.setRefreshMode(QStringLiteral("balanced")));
        QCOMPARE(changed.count(), 3);
        engine.setRefreshMode(night::RefreshMode::low_power);
        bridge.refresh();
        QCOMPARE(bridge.refreshMode(), QStringLiteral("low-power"));
        QCOMPARE(changed.count(), 4); // Changes by another Engine owner notify QML on refresh too.
        QCOMPARE(bridge.configuration().value("presentationMs").toInt(), 1000);
        QCOMPARE(notified_presentation.back(), 1000);
    }
    void customInitialCadenceCanSelectPreset() {
        night::EngineConfig config;
        config.system_interval = 17ms;
        night::EngineSources sources;
        sources.system = [] { return night::SystemSnapshot{}; };
        sources.processes = [] { return night::ProcessSnapshot{}; };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QCOMPARE(bridge.refreshMode(), QStringLiteral("custom"));
        QCOMPARE(bridge.configuration().value("presentationMs").toInt(), 200);
        QVERIFY(bridge.setRefreshMode(QStringLiteral("balanced")));
        QCOMPARE(bridge.refreshMode(), QStringLiteral("balanced"));
        QCOMPARE(bridge.configuration().value("presentationMs").toInt(), 500);
    }
    void configuredDomainFreshness_data() {
        QTest::addColumn<int>("system_ms");
        QTest::addColumn<int>("process_ms");
        QTest::addColumn<QString>("system_later");
        QTest::addColumn<QString>("process_later");
        QTest::newRow("low-power") << 2000 << 2000 << QStringLiteral("valid") << QStringLiteral("valid");
        QTest::newRow("slow-system") << 2000 << 250 << QStringLiteral("valid") << QStringLiteral("stale");
        QTest::newRow("slow-process") << 250 << 2000 << QStringLiteral("stale") << QStringLiteral("valid");
        QTest::newRow("balanced") << 500 << 500 << QStringLiteral("stale") << QStringLiteral("stale");
        QTest::newRow("fast") << 250 << 250 << QStringLiteral("stale") << QStringLiteral("stale");
    }
    void configuredDomainFreshness() {
        QFETCH(int, system_ms);
        QFETCH(int, process_ms);
        QFETCH(QString, system_later);
        QFETCH(QString, process_later);
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.system_interval = std::chrono::milliseconds{system_ms};
        config.process_interval = std::chrono::milliseconds{process_ms};
        night::EngineSources sources;
        sources.system = [end] {
            night::SystemSnapshot sample;
            sample.stamp.begin = sample.stamp.end = end;
            return sample;
        };
        sources.processes = [end] {
            night::ProcessSnapshot sample;
            sample.stamp.begin = sample.stamp.end = end;
            sample.boot_id = "fixture-boot";
            sample.pid_namespace = "fixture-pid";
            sample.cgroup_namespace = "fixture-cgroup";
            night::ProcessInfo process;
            process.pid = 1;
            process.start_ticks = 10;
            process.application_scope = night::parse_application_scope_candidate("0::/app.slice/app-fixture.scope\n");
            sample.processes.push_back(std::move(process));
            return sample;
        };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system() && engine.processes(), 2000);
        bridge.refreshAt(end + 1500ms);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.processQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.applications()->quality(), QStringLiteral("valid"));
        for (const auto offset : {2000ms, 6000ms}) {
            bridge.refreshAt(end + offset);
            QCOMPARE(bridge.systemQuality(), system_later);
            QCOMPARE(bridge.processQuality(), process_later);
            QCOMPARE(bridge.applications()->quality(), process_later);
            QCOMPARE(bridge.systemAgeMs(), offset.count());
            QCOMPARE(bridge.processAgeMs(), offset.count());
            QCOMPARE(bridge.applications()->dataAgeMs(), offset.count());
        }
        bridge.refreshAt(end + 6001ms);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.processQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.applications()->quality(), QStringLiteral("stale"));
    }
    void domainErrorPrecedenceAndRecovery_data() {
        QTest::addColumn<bool>("fast_process");
        QTest::newRow("system-errors") << false;
        QTest::newRow("process-errors") << true;
    }
    void domainErrorPrecedenceAndRecovery() {
        QFETCH(bool, fast_process);
        const auto end = night::Clock::now();
        std::atomic_int mode{0};
        night::EngineConfig config;
        config.system_interval = fast_process ? 2000ms : 10ms;
        config.process_interval = fast_process ? 10ms : 2000ms;
        night::EngineSources sources;
        sources.system = [&] {
            const auto selected = fast_process ? 0 : mode.load();
            night::SystemSnapshot sample;
            sample.stamp.begin = sample.stamp.end = selected == 2 ? end + 10s : end;
            sample.stamp.quality = selected == 1 ? night::Quality::error : night::Quality::valid;
            return sample;
        };
        sources.processes = [&] {
            const auto selected = fast_process ? mode.load() : 0;
            night::ProcessSnapshot sample;
            sample.stamp.begin = sample.stamp.end = selected == 2 ? end + 10s : end;
            sample.stamp.quality = selected == 1 ? night::Quality::error : night::Quality::valid;
            sample.boot_id = "fixture-boot";
            sample.pid_namespace = "fixture-pid";
            sample.cgroup_namespace = "fixture-cgroup";
            night::ProcessInfo process;
            process.pid = 1;
            process.start_ticks = 10;
            process.application_scope = night::parse_application_scope_candidate("0::/app.slice/app-fixture.scope\n");
            sample.processes.push_back(std::move(process));
            return sample;
        };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.system() && engine.processes(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.processQuality(), QStringLiteral("valid"));
        mode = 1;
        QTRY_VERIFY_WITH_TIMEOUT(fast_process ? engine.processes()->stamp.quality == night::Quality::error
                                             : engine.system()->stamp.quality == night::Quality::error, 2000);
        bridge.refreshAt(end + 8s);
        QCOMPARE(bridge.systemQuality(), fast_process ? QStringLiteral("stale") : QStringLiteral("error"));
        QCOMPARE(bridge.processQuality(), fast_process ? QStringLiteral("error") : QStringLiteral("stale"));
        QCOMPARE(bridge.applications()->quality(), fast_process ? QStringLiteral("error") : QStringLiteral("stale"));
        QCOMPARE(bridge.processes()->count(), 1);
        mode = 2;
        QTRY_VERIFY_WITH_TIMEOUT(fast_process ? engine.processes()->stamp.end == end + 10s
                                             : engine.system()->stamp.end == end + 10s, 2000);
        bridge.refreshAt(end + 10s);
        QCOMPARE(bridge.systemQuality(), fast_process ? QStringLiteral("stale") : QStringLiteral("valid"));
        QCOMPARE(bridge.processQuality(), fast_process ? QStringLiteral("valid") : QStringLiteral("stale"));
        QCOMPARE(bridge.applications()->quality(), fast_process ? QStringLiteral("valid") : QStringLiteral("stale"));
    }
    void gpuDisabledAndWarming() {
        night::EngineConfig config;
        config.capacity_paths.clear();
        config.gpu_interval = 17ms;
        night::EngineSources sources;
        sources.system = [] { return night::SystemSnapshot{}; };
        sources.processes = [] { return night::ProcessSnapshot{}; };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine disabled{config, sources};
        night::ui::TelemetryBridge off{disabled};
        off.refresh();
        QVERIFY(!off.gpuEnabled());
        QCOMPARE(off.gpuQuality(), QStringLiteral("off"));
        QCOMPARE(off.gpuAgeMs(), -1LL);
        QVERIFY(off.gpuDevices().isEmpty());
        QCOMPARE(off.configuration().value("gpuMs").toLongLong(), 17LL);
        QCOMPARE(off.gpuSamplingWindow(), QString::fromUtf8(night::GpuSnapshot::utilization_window.data(),
            static_cast<qsizetype>(night::GpuSnapshot::utilization_window.size())));

        std::promise<void> release;
        const auto ready = release.get_future().share();
        sources.gpu = [ready] { ready.wait(); return night::GpuSnapshot{}; };
        night::Engine enabled{config, sources};
        night::ui::TelemetryBridge warming{enabled};
        warming.refresh();
        const auto initial_quality = warming.gpuQuality();
        release.set_value(); // Release the worker before any assertion can return.
        QVERIFY(warming.gpuEnabled());
        QCOMPARE(initial_quality, QStringLiteral("warming"));
        QVERIFY(warming.gpuDevices().isEmpty());
    }
    void gpuPublicationAndFieldQualities() {
        std::atomic_int mode{0};
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.gpu_interval = 10ms;
        config.capacity_paths.clear();
        night::EngineSources sources;
        sources.system = [] { return night::SystemSnapshot{}; };
        sources.processes = [] { return night::ProcessSnapshot{}; };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        sources.gpu = [&] {
            const auto selected = mode.load();
            night::GpuSnapshot sample;
            sample.stamp.begin = sample.stamp.end = end;
            night::GpuReading device;
            device.uuid = {selected == 1 ? "GPU-changed" : "GPU-fixture", night::Quality::valid};
            device.name = {"GPU\n\xE2\x80\xAE fixture", night::Quality::valid};
            device.utilization_percent = {selected == 1 ? 42.0 : 0.0, night::Quality::valid};
            device.temperature_celsius = {51.0, night::Quality::valid};
            device.power_watts = {0.0, night::Quality::valid};
            device.total_bytes = {18446744073709551615ULL, night::Quality::valid};
            device.used_bytes = {0, night::Quality::valid};
            device.free_bytes = {18446744073709551615ULL, night::Quality::valid};
            if (selected == 2) {
                device.uuid = {{}, night::Quality::unsupported};
                device.used_bytes = {{}, night::Quality::permission_denied};
                sample.stamp.issues.push_back({night::Quality::permission_denied, "memory", "fixture"});
            }
            if (selected == 3) {
                device.uuid = {{}, night::Quality::unsupported};
                device.name = {{}, night::Quality::unsupported};
                device.utilization_percent = {{}, night::Quality::unsupported};
                device.total_bytes = device.used_bytes = device.free_bytes = {{}, night::Quality::unsupported};
                device.temperature_celsius = device.power_watts = {{}, night::Quality::unsupported};
            }
            if (selected == 4) device.utilization_percent = {101.0, night::Quality::valid};
            if (selected == 7) {
                device.total_bytes = {8, night::Quality::valid};
                device.used_bytes = {9, night::Quality::valid};
                // Adding used and free would wrap to total in this fixture.
                device.free_bytes = {18446744073709551615ULL, night::Quality::valid};
            }
            if (selected == 8) {
                device.temperature_celsius = {std::numeric_limits<double>::quiet_NaN(), night::Quality::valid};
                device.power_watts = {-1.0, night::Quality::valid};
            }
            if (selected != 5) sample.devices.push_back(device);
            if (selected == 6) sample.devices.resize(40, device);
            return sample;
        };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QSignalSpy changes(&bridge, &night::ui::TelemetryBridge::gpuDevicesChanged);
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.gpuAgeMs(), 0LL);
        QCOMPARE(bridge.gpuDevices().size(), 1);
        auto row = bridge.gpuDevices().front().toMap();
        const auto original_identity = row.value("identity");
        QVERIFY(original_identity.isValid());
        QCOMPARE(row.value("name").toMap().value("value").toString(), QStringLiteral("GPU\\u000a\\u202e fixture"));
        QCOMPARE(row.value("utilizationPercent").toMap().value("value").toDouble(), 0.0);
        QCOMPARE(row.value("temperatureCelsius").toMap().value("value").toDouble(), 51.0);
        QCOMPARE(row.value("powerWatts").toMap().value("value").toDouble(), 0.0);
        QCOMPARE(row.value("usedBytes").toMap().value("value").toULongLong(), 0ULL);
        QCOMPARE(row.value("totalBytes").toMap().value("value").toULongLong(), 18446744073709551615ULL);
        QCOMPARE(row.value("freeBytes").toMap().value("value").toULongLong(), 18446744073709551615ULL);
        QVERIFY(bridge.gpuMessage().isEmpty());
        QVERIFY(!bridge.gpuPartial());
        QCOMPARE(bridge.gpuIssueCount(), 0ULL);
        QCOMPARE(changes.count(), 1);
        const auto initial_sequence = engine.gpu()->stamp.sequence;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->stamp.sequence > initial_sequence, 2000);
        bridge.refreshAt(end);
        QCOMPARE(changes.count(), 1); // New generations with identical fields do not republish devices.
        bridge.refreshAt(end + 30ms);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("valid"));
        bridge.refreshAt(end + 31ms);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.gpuAgeMs(), 31LL);
        QCOMPARE(changes.count(), 1);

        mode = 1;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.front().uuid.value == "GPU-changed", 2000);
        bridge.refreshAt(end);
        row = bridge.gpuDevices().front().toMap();
        QVERIFY(row.value("identity") != original_identity);
        QCOMPARE(row.value("utilizationPercent").toMap().value("value").toDouble(), 42.0);
        mode = 2;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.front().used_bytes.quality == night::Quality::permission_denied, 2000);
        bridge.refreshAt(end);
        row = bridge.gpuDevices().front().toMap();
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("partial"));
        QVERIFY(bridge.gpuPartial());
        QCOMPARE(bridge.gpuIssueCount(), 1ULL);
        QVERIFY(!row.value("identity").isValid());
        QCOMPARE(row.value("issueCount").toULongLong(), 2ULL);
        QCOMPARE(row.value("usedBytes").toMap().value("quality").toString(), QStringLiteral("permission_denied"));
        QVERIFY(!row.value("usedBytes").toMap().value("value").isValid());
        QCOMPARE(row.value("utilizationPercent").toMap().value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(row.value("totalBytes").toMap().value("value").toULongLong(), 18446744073709551615ULL);
        QCOMPARE(row.value("freeBytes").toMap().value("quality").toString(), QStringLiteral("valid"));
        mode = 3;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.front().name.quality == night::Quality::unsupported, 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("partial"));
        QCOMPARE(bridge.gpuIssueCount(), 8ULL);
        QVERIFY(!bridge.gpuDevices().front().toMap().value("utilizationPercent").toMap().value("value").isValid());
        mode = 4;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.front().utilization_percent.value == 101.0, 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuDevices().front().toMap().value("utilizationPercent").toMap().value("quality").toString(), QStringLiteral("error"));
        mode = 5;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.empty(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("valid"));
        QVERIFY(bridge.gpuDevices().isEmpty());
        mode = 6;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.size() == 40, 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuDevices().size(), 32);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("partial"));
        QCOMPARE(bridge.gpuIssueCount(), 1ULL);
        mode = 7;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.size() == 1 && engine.gpu()->devices.front().used_bytes.value == 9, 2000);
        bridge.refreshAt(end);
        row = bridge.gpuDevices().front().toMap();
        for (const auto* key : {"totalBytes", "usedBytes", "freeBytes"}) {
            QCOMPARE(row.value(key).toMap().value("quality").toString(), QStringLiteral("error"));
            QVERIFY(!row.value(key).toMap().value("value").isValid());
        }
        QCOMPARE(row.value("utilizationPercent").toMap().value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("partial"));
        QCOMPARE(bridge.gpuIssueCount(), 3ULL);
        mode = 8;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.front().power_watts.value == -1.0, 2000);
        bridge.refreshAt(end);
        row = bridge.gpuDevices().front().toMap();
        for (const auto* key : {"temperatureCelsius", "powerWatts"}) {
            QCOMPARE(row.value(key).toMap().value("quality").toString(), QStringLiteral("error"));
            QVERIFY(!row.value(key).toMap().value("value").isValid());
        }
        QCOMPARE(row.value("utilizationPercent").toMap().value("quality").toString(), QStringLiteral("valid"));
        QCOMPARE(bridge.gpuIssueCount(), 2ULL);
    }
    void gpuFailuresAndTimestampValidation() {
        std::atomic_int mode{0};
        const auto end = night::Clock::now();
        night::EngineConfig config;
        config.gpu_interval = 10ms;
        config.capacity_paths.clear();
        night::EngineSources sources;
        sources.system = [] { return night::SystemSnapshot{}; };
        sources.processes = [] { return night::ProcessSnapshot{}; };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        sources.gpu = [&] {
            const auto selected = mode.load();
            night::GpuSnapshot sample;
            sample.stamp.begin = sample.stamp.end = end;
            night::GpuReading device;
            device.uuid = {"GPU-fixture", night::Quality::valid};
            device.name = {"Fixture " + std::to_string(selected), night::Quality::valid};
            device.utilization_percent = {0.0, night::Quality::valid};
            device.temperature_celsius = {51.0, night::Quality::valid};
            device.power_watts = {0.0, night::Quality::valid};
            device.total_bytes = device.free_bytes = {1024, night::Quality::valid};
            device.used_bytes = {0, night::Quality::valid};
            sample.devices.push_back(device);
            if (selected == 1) {
                sample.stamp.quality = night::Quality::error;
                sample.stamp.issues.push_back({night::Quality::error, "NVML device 0", "device unavailable"});
                sample.stamp.issues.push_back({night::Quality::error, "NVML", "GPU lost; session invalidated, retry after 5 seconds"});
            }
            if (selected == 2) sample.stamp.begin = sample.stamp.end = end + 1s;
            if (selected == 3) sample.stamp.begin = end + 1ms;
            if (selected == 4) sample.stamp.begin = sample.stamp.end = {};
            if (selected == 5) sample.stamp.quality = night::Quality::permission_denied;
            if (selected == 6) sample.stamp.quality = night::Quality::unsupported;
            if (selected == 7) throw std::runtime_error("GPU fixture failure");
            if (selected == 8) {
                sample.stamp.quality = night::Quality::error;
                sample.stamp.issues.push_back({night::Quality::error,
                    "\xE2\x80\xAE\t" + std::string(5000, '\n'), "unsafe\r\xE2\x80\xAE" + std::string(5000, '\n')});
                sample.stamp.issues.push_back({night::Quality::error, "second", "second issue"});
                sample.stamp.issues.push_back({night::Quality::error, "third", "third issue"});
                sample.stamp.issues.push_back({night::Quality::error, "fourth", "fourth issue"});
                sample.stamp.issues.push_back({night::Quality::error, "NVML", "GPU lost; session invalidated, retry after 5 seconds"});
                sample.stamp.issues.push_back({night::Quality::error, "nvmlShutdown", "shutdown failed; collector permanently disabled"});
            }
            return sample;
        };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu(), 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuDevices().size(), 1);
        QVERIFY(bridge.gpuMessage().isEmpty());
        std::uint64_t sequence = engine.gpu()->stamp.sequence;
        for (int selected = 1; selected <= 6; ++selected) {
            mode = selected;
            const auto marker = "Fixture " + std::to_string(selected);
            QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->devices.front().name.value == marker, 2000);
            QVERIFY(engine.gpu()->stamp.sequence > sequence);
            sequence = engine.gpu()->stamp.sequence;
            bridge.refreshAt(end);
            const auto expected = selected == 4 ? QStringLiteral("warming")
                : selected == 5 ? QStringLiteral("permission_denied")
                : selected == 6 ? QStringLiteral("unsupported") : QStringLiteral("error");
            QCOMPARE(bridge.gpuQuality(), expected);
            QCOMPARE(bridge.gpuAgeMs(), -1LL);
            QVERIFY(bridge.gpuDevices().isEmpty());
            if (selected == 1)
                QVERIFY(bridge.gpuMessage().contains(QStringLiteral("GPU lost; session invalidated, retry after 5 seconds")));
            if (selected == 2) QVERIFY(bridge.gpuMessage().contains(QStringLiteral("future")));
            if (selected == 3) QVERIFY(bridge.gpuMessage().contains(QStringLiteral("reversed")));
            if (selected == 4) QVERIFY(bridge.gpuMessage().contains(QStringLiteral("missing")));
        }
        mode = 7;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->stamp.quality == night::Quality::error && engine.gpu()->devices.empty(), 2000);
        bridge.refresh();
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("error"));
        QCOMPARE(bridge.gpuIssueCount(), 1ULL);
        QVERIFY(bridge.gpuDevices().isEmpty());
        QVERIFY(bridge.gpuMessage().contains(QStringLiteral("GPU fixture failure")));
        mode = 8;
        QTRY_VERIFY_WITH_TIMEOUT(engine.gpu()->stamp.issues.size() == 6, 2000);
        bridge.refreshAt(end);
        const auto message = bridge.gpuMessage();
        QVERIFY(message.size() <= 4096);
        QVERIFY(message.contains(QStringLiteral("GPU lost; session invalidated, retry after 5 seconds")));
        QVERIFY(message.contains(QStringLiteral("\\u202e")));
        QVERIFY(message.contains(QStringLiteral("\\u000a")));
        QVERIFY(!message.contains(QChar{0x202e}));
        QVERIFY(!message.contains(QLatin1Char('\n')));
        QVERIFY(!message.contains(QLatin1Char('\r')));
        QVERIFY(!message.contains(QLatin1Char('\t')));
        QVERIFY(!message.contains(QStringLiteral("third issue")));
        QVERIFY(!message.contains(QStringLiteral("fourth issue")));
        QVERIFY(message.contains(QStringLiteral("collector permanently disabled")));
        QVERIFY(!message.contains(QStringLiteral("GPU-fixture")));
        QVERIFY(!message.contains(QStringLiteral("Fixture 8")));
        QVERIFY(bridge.gpuDevices().isEmpty());
        mode = 0;
        QTRY_VERIFY_WITH_TIMEOUT(!engine.gpu()->devices.empty() && engine.gpu()->devices.front().name.value == "Fixture 0", 2000);
        bridge.refreshAt(end);
        QCOMPARE(bridge.gpuQuality(), QStringLiteral("valid"));
        QCOMPARE(bridge.gpuDevices().size(), 1);
        QVERIFY(bridge.gpuMessage().isEmpty());
    }
    void backgroundPublicationAndErrors() {
        std::atomic_bool fail{false};
        std::atomic<std::uint64_t> total_bytes{1024};
        const auto presentation_thread = std::this_thread::get_id();
        std::atomic_bool collected_on_presentation{false};
        night::EngineConfig config;
        config.system_interval = config.process_interval = config.io_interval = 10ms;
        config.capacity_paths.clear();
        night::EngineSources sources;
        sources.system = [&] {
            if (std::this_thread::get_id() == presentation_thread) collected_on_presentation = true;
            night::SystemSnapshot s;
            s.stamp.begin = s.stamp.end = night::Clock::now();
            s.stamp.quality = fail ? night::Quality::error : night::Quality::valid;
            s.total_bytes = {total_bytes.load(), night::Quality::valid};
            s.available_bytes = {{}, night::Quality::unsupported};
            s.cpus.push_back({"cpu", {25.0, night::Quality::valid}, {{}, night::Quality::warming}});
            return s;
        };
        sources.processes = [&] {
            night::ProcessSnapshot s;
            s.stamp.begin = s.stamp.end = night::Clock::now();
            s.stamp.quality = fail ? night::Quality::error : night::Quality::valid;
            s.boot_id = "boot"; s.pid_namespace = "pid:[1]";
            night::ProcessInfo p; p.pid = 7; p.start_ticks = 123; p.name = "owned fixture";
            s.processes.push_back(p); return s;
        };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QCOMPARE(bridge.configuration().value("systemMs").toLongLong(), 10LL);
        QCOMPARE(bridge.configuration().value("historyCapacity").toULongLong(), 1200ULL);
        QCOMPARE(bridge.configuration().value("scopeMetadata").toBool(), false);
        QSignalSpy notifications(&bridge, &night::ui::TelemetryBridge::updated);
        QSignalSpy system_changes(&bridge, &night::ui::TelemetryBridge::systemChanged);
        QTRY_COMPARE_WITH_TIMEOUT(bridge.processes()->count(), 1, 2000);
        QVERIFY(notifications.count() > 0);
        QVERIFY(!bridge.history().isEmpty());
        QVERIFY(!collected_on_presentation);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("valid"));
        const auto total = bridge.system().value("totalBytes").toMap();
        QCOMPARE(total.value("value").toULongLong(), 1024ULL);
        QCOMPARE(bridge.system().value("availableBytes").toMap().value("quality").toString(), QStringLiteral("unsupported"));
        QVERIFY(!bridge.system().value("availableBytes").toMap().value("value").isValid());
        QVERIFY(!bridge.system().value("cpuIowaitPercent").toMap().value("value").isValid());
        QCOMPARE(system_changes.count(), 1);
        const auto future = night::Clock::now() + 2s;
        bridge.refreshAt(future);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("stale"));
        QCOMPARE(bridge.processQuality(), QStringLiteral("stale"));
        QVERIFY(bridge.processAgeMs() >= 1900);
        const auto system_sequence = engine.system()->stamp.sequence;
        QTRY_VERIFY_WITH_TIMEOUT(engine.system()->stamp.sequence >= system_sequence + 2, 2000);
        bridge.refreshAt(night::Clock::now() + 2s);
        QCOMPARE(system_changes.count(), 1); // New timestamps/ages do not change stable field values.
        total_bytes = 2048;
        QTRY_VERIFY_WITH_TIMEOUT(engine.system()->total_bytes.value == 2048, 2000);
        bridge.refreshAt(night::Clock::now() + 2s);
        QCOMPARE(system_changes.count(), 2);
        QCOMPARE(bridge.system().value("totalBytes").toMap().value("value").toULongLong(), 2048ULL);
        const auto good_sequence = bridge.processes()->sequence();
        fail = true;
        QTRY_VERIFY_WITH_TIMEOUT(engine.processes()->stamp.sequence > good_sequence && engine.processes()->stamp.quality == night::Quality::error, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(engine.system()->stamp.quality == night::Quality::error, 2000);
        bridge.refreshAt(night::Clock::now() + 2s);
        QCOMPARE(bridge.systemQuality(), QStringLiteral("error"));
        QCOMPARE(bridge.processQuality(), QStringLiteral("error"));
        QCOMPARE(bridge.processes()->count(), 1);
        QCOMPARE(bridge.system().value("totalBytes").toMap().value("value").toULongLong(), 2048ULL);
        QCOMPARE(system_changes.count(), 2); // Errors retain values, with quality/age on updated.
        QVERIFY(bridge.processAgeMs() >= 1900); // Failed publications do not freshen retained rows.
    }
    void applicationScopePublicationAndContextFailure() {
        std::atomic_bool fail_context{false};
        night::EngineConfig config;
        config.include_application_scopes = true;
        config.process_interval = 10ms;
        config.capacity_paths.clear();
        night::EngineSources sources;
        sources.system = [] { return night::SystemSnapshot{}; };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        sources.processes = [&] {
            const bool unavailable = fail_context.load();
            night::ProcessSnapshot s;
            s.stamp.begin = s.stamp.end = night::Clock::now();
            s.boot_id = "boot"; s.pid_namespace = "pid:[1]";
            if (!unavailable) s.cgroup_namespace = "cgroup:[2]";
            night::ProcessInfo p; p.pid = 7; p.start_ticks = 123;
            p.application_scope = unavailable
                ? night::Reading<night::ApplicationScopeCandidate>{{}, night::Quality::permission_denied}
                : night::parse_application_scope_candidate("0::/app.slice/app-owned-fixture.scope\n");
            s.processes.push_back(std::move(p)); return s;
        };
        night::Engine engine{config, sources};
        night::ui::TelemetryBridge bridge{engine};
        QCOMPARE(bridge.configuration().value("scopeMetadata").toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(bridge.applications()->count(), 1, 2000);
        QCOMPARE(bridge.applications()->quality(), QStringLiteral("valid"));
        bridge.refreshAt(night::Clock::now() + 2s);
        QCOMPARE(bridge.applications()->quality(), QStringLiteral("stale"));
        fail_context = true;
        QTRY_VERIFY_WITH_TIMEOUT(engine.processes()->cgroup_namespace.empty(), 2000);
        bridge.refreshAt(night::Clock::now() + 2s);
        QCOMPARE(bridge.applications()->quality(), QStringLiteral("permission_denied"));
        QCOMPARE(bridge.applications()->count(), 1);
        QVERIFY(bridge.applications()->dataAgeMs() >= 1900);
        QCOMPARE(bridge.processes()->count(), 1); // Context failure is local to scope metadata.
        QCOMPARE(bridge.processes()->quality(), QStringLiteral("valid"));
    }
};
QTEST_GUILESS_MAIN(BridgeTests)
#include "qt_bridge_tests.moc"
