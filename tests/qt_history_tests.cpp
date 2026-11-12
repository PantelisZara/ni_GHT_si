#include "history_series.hpp"
#include <QtTest>
#include <limits>

class HistoryTests final : public QObject {
    Q_OBJECT
    static night::HistoryPoint point(int index, double value = 50) {
        night::HistoryPoint p;
        p.time = night::Clock::time_point{std::chrono::milliseconds{index + 1}};
        p.cpus.push_back({"cpu", {value, night::Quality::valid}, {}});
        p.available_bytes = {2ULL * 1024 * 1024 * 1024, night::Quality::valid};
        return p;
    }
private slots:
    void valuesAndGaps() {
        auto failed = point(1); failed.quality = night::Quality::error;
        auto warming = point(2); warming.cpus[0].busy_percent.quality = night::Quality::warming;
        const auto result = night::ui::historySeries({point(0, 0), failed, warming, point(3, 100)});
        QCOMPARE(result.size(), 4);
        QCOMPARE(result[0].toMap().value("cpu").toDouble(), 0.0);
        QCOMPARE(result[0].toMap().value("availableGiB").toDouble(), 2.0);
        QVERIFY(!result[1].toMap().value("cpu").isValid());
        QVERIFY(!result[1].toMap().value("availableGiB").isValid());
        QVERIFY(!result[2].toMap().value("cpu").isValid());
        QCOMPARE(result[3].toMap().value("cpu").toDouble(), 100.0);
        QCOMPARE(result[3].toMap().value("timeMs").toLongLong(), 4LL);
    }
    void boundedAndNoExtremaDropped() {
        std::vector<night::HistoryPoint> points;
        for (int i = 0; i < 2700; ++i) points.push_back(point(i, i == 900 ? 100 : 0));
        const auto result = night::ui::historySeries(points);
        QCOMPARE(result.size(), 2400);
        QCOMPARE(result.front().toMap().value("timeMs").toLongLong(), 301LL);
        QCOMPARE(result[600].toMap().value("cpu").toDouble(), 100.0);
        QCOMPARE(result.back().toMap().value("timeMs").toLongLong(), 2700LL);
    }
    void invalidNumbersAndTimes() {
        auto invalid_time = point(0); invalid_time.time = {};
        auto nan = point(1, std::numeric_limits<double>::quiet_NaN());
        auto backward = point(0);
        auto invalid_ram = point(3); invalid_ram.available_bytes.quality = night::Quality::permission_denied;
        const auto result = night::ui::historySeries({invalid_time, nan, backward, invalid_ram});
        QVERIFY(!result[0].toMap().value("cpu").isValid());
        QVERIFY(!result[1].toMap().value("cpu").isValid());
        QVERIFY(!result[2].toMap().value("cpu").isValid());
        QVERIFY(!result[3].toMap().value("availableGiB").isValid());
        QCOMPARE(result[3].toMap().value("cpu").toDouble(), 50.0);
        QVERIFY(night::ui::historySeries({}).isEmpty());
        const auto rollback = night::ui::historySeries({point(9), point(4), point(5), point(11)});
        QCOMPARE(rollback[0].toMap().value("timeMs").toLongLong(), 10LL);
        QVERIFY(!rollback[1].toMap().value("timeMs").isValid());
        QVERIFY(!rollback[2].toMap().value("timeMs").isValid());
        QVERIFY(!rollback[2].toMap().value("cpu").isValid());
        QCOMPARE(rollback[3].toMap().value("timeMs").toLongLong(), 12LL);
    }
};
QTEST_GUILESS_MAIN(HistoryTests)
#include "qt_history_tests.moc"
