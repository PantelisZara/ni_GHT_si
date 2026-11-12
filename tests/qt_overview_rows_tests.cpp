#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QVariantMap>
#include <QtTest>
#include <algorithm>
#include <memory>

class OverviewBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList networkRates READ networkRates NOTIFY ioChanged)
    Q_PROPERTY(QVariantList diskRates READ diskRates NOTIFY ioChanged)
    Q_PROPERTY(QVariantList filesystemCapacity READ filesystemCapacity NOTIFY capacityChanged)
    Q_PROPERTY(QVariantList coreCpu READ coreCpu NOTIFY coreCpuChanged)
    Q_PROPERTY(QString coreCpuQuality READ quality CONSTANT)
    Q_PROPERTY(QString coreCpuMessage READ message CONSTANT)
    Q_PROPERTY(qint64 coreCpuAgeMs READ age CONSTANT)
    Q_PROPERTY(QString ioQuality READ quality CONSTANT)
    Q_PROPERTY(QString capacityQuality READ quality CONSTANT)
    Q_PROPERTY(QString ioMessage READ message CONSTANT)
    Q_PROPERTY(QString capacityMessage READ message CONSTANT)
    Q_PROPERTY(qint64 ioAgeMs READ age CONSTANT)
    Q_PROPERTY(qint64 capacityAgeMs READ age CONSTANT)
    Q_PROPERTY(qulonglong ioIssueCount READ issueCount CONSTANT)
    Q_PROPERTY(qulonglong capacityIssueCount READ issueCount CONSTANT)
public:
    QVariantList networkRates() const { return network_; }
    QVariantList diskRates() const { return disks_; }
    QVariantList filesystemCapacity() const { return capacity_; }
    QVariantList coreCpu() const { return cores_; }
    QString quality() const { return QStringLiteral("valid"); }
    QString message() const { return {}; }
    qint64 age() const { return 0; }
    qulonglong issueCount() const { return 0; }
    void publishIo(QVariantList network, QVariantList disks) {
        network_ = std::move(network);
        disks_ = std::move(disks);
        emit ioChanged();
    }
    void publishCapacity(QVariantList capacity) {
        capacity_ = std::move(capacity);
        emit capacityChanged();
    }
    void publishCores(QVariantList cores) {
        cores_ = std::move(cores);
        emit coreCpuChanged();
    }
signals:
    void ioChanged();
    void capacityChanged();
    void coreCpuChanged();
private:
    QVariantList network_, disks_, capacity_, cores_;
};

namespace {
QVariantMap reading(QVariant value, const QString& quality = QStringLiteral("valid")) {
    return {{QStringLiteral("value"), std::move(value)}, {QStringLiteral("quality"), quality}};
}
QVariantMap rate(const QString& name, QVariantMap read, QVariantMap written) {
    return {{QStringLiteral("name"), name}, {QStringLiteral("readBytesPerSecond"), std::move(read)},
            {QStringLiteral("writtenBytesPerSecond"), std::move(written)}};
}
QVariantMap capacity(const QString& path, QVariantMap total, QVariantMap used, QVariantMap available) {
    return {{QStringLiteral("path"), path}, {QStringLiteral("totalBytes"), std::move(total)},
            {QStringLiteral("usedBytes"), std::move(used)}, {QStringLiteral("availableBytes"), std::move(available)}};
}
QVariantMap cpu(const QString& name, QVariantMap busy, QVariantMap iowait) {
    return {{QStringLiteral("name"), name}, {QStringLiteral("busyPercent"), std::move(busy)},
            {QStringLiteral("iowaitPercent"), std::move(iowait)}};
}
QVariantList cores(int count, int first = 0) {
    QVariantList result;
    for (int i = 0; i < count; ++i)
        result.push_back(cpu(QStringLiteral("cpu") + QString::number(first + i), reading(static_cast<double>(i)),
                             reading(static_cast<double>(100 - i))));
    return result;
}
QVariantList rates(int count, const QString& prefix, double scale) {
    QVariantList result;
    for (int i = 0; i < count; ++i)
        result.push_back(rate(prefix + QString::number(i), reading(i * scale), reading(0.0)));
    return result;
}
QVariantList capacities(int count, const QString& prefix) {
    QVariantList result;
    for (int i = 0; i < count; ++i)
        result.push_back(capacity(prefix + QString::number(i), reading(128.0 * 1073741824),
                                  reading(i * 1073741824.0), reading((127 - i) * 1073741824.0)));
    return result;
}
QList<QQuickItem*> visualItems(QQuickItem* root) {
    QList<QQuickItem*> items;
    if (root) items.push_back(root);
    for (qsizetype i = 0; i < items.size(); ++i) items.append(items[i]->childItems());
    return items;
}
QString rowText(QObject* row) {
    QStringList result;
    for (auto* child : visualItems(qobject_cast<QQuickItem*>(row))) {
        const auto text = child->property("text");
        if (text.isValid()) result.push_back(text.toString());
    }
    return result.join(QLatin1Char('\n'));
}

struct Summary {
    OverviewBackend backend;
    QQmlEngine engine;
    QQuickWindow window;
    QQmlComponent component{&engine};
    std::unique_ptr<QObject> root;
    int warnings{};
    QObject callback_context; // Disconnect before captured state, QML objects, and backend teardown.
    explicit Summary(const QString& resource = QStringLiteral("qrc:/qml/IoSummary.qml")) {
        QObject::connect(&engine, &QQmlEngine::warnings, &callback_context,
                         [this](const QList<QQmlError>& errors) { warnings += static_cast<int>(errors.size()); });
        component.loadUrl(QUrl(resource));
        root.reset(component.createWithInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&backend)},
                                                         {QStringLiteral("width"), 840}}));
        if (auto* item = qobject_cast<QQuickItem*>(root.get())) {
            item->setParentItem(window.contentItem());
            window.resize(840, 720);
            window.show();
        }
    }
    QObject* row(const char* prefix, int index) const {
        const auto name = QString::fromLatin1(prefix) + QLatin1Char('_') + QString::number(index);
        for (auto* child : visualItems(qobject_cast<QQuickItem*>(root.get())))
            if (child->objectName() == name) return child;
        return nullptr;
    }
    qsizetype count(const char* prefix) const {
        qsizetype result = 0;
        for (auto* child : visualItems(qobject_cast<QQuickItem*>(root.get())))
            if (child->objectName().startsWith(QString::fromLatin1(prefix) + QLatin1Char('_'))) ++result;
        return result;
    }
};
}

class OverviewRowsTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }
    void sameCountRefreshAndHiddenRestoration() {
        Summary summary;
        QVERIFY2(summary.root, qPrintable(summary.component.errorString()));
        summary.backend.publishIo({rate(QStringLiteral("initial-net"), reading(0.0), reading({}, QStringLiteral("unsupported"))),
                                   rate(QStringLiteral("second-net"), reading(1024.0), reading(2048.0))},
                                  {rate(QStringLiteral("initial-disk"), reading(0.0), reading({}, QStringLiteral("permission_denied"))),
                                   rate(QStringLiteral("second-disk"), reading(1048576.0), reading(2097152.0))});
        summary.backend.publishCapacity({capacity(QStringLiteral("/initial"), reading(10737418240.0), reading(0.0),
                                                  reading({}, QStringLiteral("unsupported"))),
                                         capacity(QStringLiteral("/second"), reading(21474836480.0), reading(5368709120.0), reading(15032385536.0))});
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 2, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 2, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 2, 1000);
        QVERIFY(rowText(summary.row("networkRow", 0)).contains(QStringLiteral("Receive: 0.0 KiB/s (valid)")));
        QVERIFY(rowText(summary.row("networkRow", 0)).contains(QStringLiteral("transmit: — (unsupported)")));
        QVERIFY(rowText(summary.row("diskRow", 0)).contains(QStringLiteral("Read: 0.0 MiB/s (valid)")));
        QVERIFY(rowText(summary.row("diskRow", 0)).contains(QStringLiteral("write: — (permission denied)")));
        QVERIFY(rowText(summary.row("capacityRow", 0)).contains(QStringLiteral("Used: 0.0 GiB (valid)")));
        QVERIFY(rowText(summary.row("capacityRow", 0)).contains(QStringLiteral("total: 10.0 GiB (valid)")));
        QVERIFY(rowText(summary.row("capacityRow", 0)).contains(QStringLiteral("available: — (unsupported)")));
        QPointer<QObject> network0 = summary.row("networkRow", 0), network1 = summary.row("networkRow", 1);
        QPointer<QObject> disk0 = summary.row("diskRow", 0), disk1 = summary.row("diskRow", 1);
        QPointer<QObject> capacity0 = summary.row("capacityRow", 0), capacity1 = summary.row("capacityRow", 1);

        summary.backend.publishIo({rate(QStringLiteral("renamed-net"), reading(3072.0), reading(0.0)),
                                   rate(QStringLiteral("unavailable-net"), reading({}, QStringLiteral("error")), reading(0.0))},
                                  {rate(QStringLiteral("renamed-disk"), reading(4194304.0), reading({}, QStringLiteral("unsupported"))),
                                   rate(QStringLiteral("unavailable-disk"), reading({}, QStringLiteral("warming")), reading(0.0))});
        summary.backend.publishCapacity({capacity(QStringLiteral("/renamed"), reading(21474836480.0), reading(2147483648.0), reading(18253611008.0)),
                                         capacity(QStringLiteral("/unavailable"), reading({}, QStringLiteral("permission_denied")),
                                                  reading({}, QStringLiteral("warming")), reading({}, QStringLiteral("error")))});
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("networkRow", 0)).contains(QStringLiteral("renamed-net")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("diskRow", 0)).contains(QStringLiteral("Read: 4.0 MiB/s (valid)")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("capacityRow", 0)).contains(QStringLiteral("/renamed")), 1000);
        QCOMPARE(summary.row("networkRow", 0), network0.data());
        QCOMPARE(summary.row("networkRow", 1), network1.data());
        QCOMPARE(summary.row("diskRow", 0), disk0.data());
        QCOMPARE(summary.row("diskRow", 1), disk1.data());
        QCOMPARE(summary.row("capacityRow", 0), capacity0.data());
        QCOMPARE(summary.row("capacityRow", 1), capacity1.data());
        QVERIFY(rowText(network0).contains(QStringLiteral("Receive: 3.0 KiB/s (valid)")));
        QVERIFY(rowText(network0).contains(QStringLiteral("transmit: 0.0 KiB/s (valid)")));
        QVERIFY(!rowText(network0).contains(QStringLiteral("initial-net")));
        QVERIFY(rowText(network1).contains(QStringLiteral("Receive: — (error)")));
        QVERIFY(rowText(disk0).contains(QStringLiteral("renamed-disk")));
        QVERIFY(rowText(disk0).contains(QStringLiteral("write: — (unsupported)")));
        QVERIFY(rowText(disk1).contains(QStringLiteral("Read: — (warming)")));
        QVERIFY(rowText(capacity0).contains(QStringLiteral("Used: 2.0 GiB (valid)")));
        QVERIFY(rowText(capacity0).contains(QStringLiteral("available: 17.0 GiB (valid)")));
        QVERIFY(rowText(capacity1).contains(QStringLiteral("total: — (permission denied)")));
        QVERIFY(rowText(capacity1).contains(QStringLiteral("available: — (error)")));
        QCOMPARE(summary.warnings, 0);

        QVERIFY(summary.root->setProperty("visible", false));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 0, 1000);
        summary.backend.publishIo(rates(3, QStringLiteral("hidden-net-"), 1024), rates(3, QStringLiteral("hidden-disk-"), 1048576));
        summary.backend.publishCapacity(capacities(3, QStringLiteral("/hidden-")));
        QCoreApplication::processEvents();
        QCOMPARE(summary.count("networkRow"), 0);
        QCOMPARE(summary.count("diskRow"), 0);
        QCOMPARE(summary.count("capacityRow"), 0);
        QVERIFY(summary.root->setProperty("visible", true));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 3, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 3, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 3, 1000);
        QVERIFY(rowText(summary.row("networkRow", 2)).contains(QStringLiteral("hidden-net-2")));
        QVERIFY(rowText(summary.row("networkRow", 2)).contains(QStringLiteral("Receive: 2.0 KiB/s (valid)")));
        QVERIFY(rowText(summary.row("diskRow", 2)).contains(QStringLiteral("Read: 2.0 MiB/s (valid)")));
        QVERIFY(rowText(summary.row("capacityRow", 2)).contains(QStringLiteral("/hidden-2")));
        QVERIFY(rowText(summary.row("capacityRow", 2)).contains(QStringLiteral("Used: 2.0 GiB (valid)")));
        QCoreApplication::processEvents();
        QCOMPARE(summary.warnings, 0);
    }

    void growReorderShrinkAndEmptyAtPresentationLimits() {
        Summary summary;
        QVERIFY2(summary.root, qPrintable(summary.component.errorString()));
        summary.backend.publishIo(rates(2, QStringLiteral("net-"), 1024), rates(2, QStringLiteral("disk-"), 1048576));
        summary.backend.publishCapacity(capacities(2, QStringLiteral("/mount-")));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 2, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 2, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 2, 1000);
        auto network = rates(64, QStringLiteral("net-"), 1024);
        auto disks = rates(64, QStringLiteral("disk-"), 1048576);
        auto mounts = capacities(32, QStringLiteral("/mount-"));
        summary.backend.publishIo(network, disks);
        summary.backend.publishCapacity(mounts);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 64, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 64, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 32, 1000);
        QPointer<QObject> network_first = summary.row("networkRow", 0), network_last = summary.row("networkRow", 63);
        QPointer<QObject> disk_first = summary.row("diskRow", 0), disk_last = summary.row("diskRow", 63);
        QPointer<QObject> capacity_first = summary.row("capacityRow", 0), capacity_last = summary.row("capacityRow", 31);
        std::reverse(network.begin(), network.end());
        std::reverse(disks.begin(), disks.end());
        std::reverse(mounts.begin(), mounts.end());
        summary.backend.publishIo(network, disks);
        summary.backend.publishCapacity(mounts);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("networkRow", 0)).contains(QStringLiteral("net-63")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("diskRow", 0)).contains(QStringLiteral("disk-63")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("capacityRow", 0)).contains(QStringLiteral("/mount-31")), 1000);
        QCOMPARE(summary.row("networkRow", 0), network_first.data());
        QCOMPARE(summary.row("networkRow", 63), network_last.data());
        QCOMPARE(summary.row("diskRow", 0), disk_first.data());
        QCOMPARE(summary.row("diskRow", 63), disk_last.data());
        QCOMPARE(summary.row("capacityRow", 0), capacity_first.data());
        QCOMPARE(summary.row("capacityRow", 31), capacity_last.data());
        QVERIFY(rowText(network_first).contains(QStringLiteral("Receive: 63.0 KiB/s (valid)")));
        QVERIFY(rowText(network_last).contains(QStringLiteral("Receive: 0.0 KiB/s (valid)")));
        QVERIFY(rowText(disk_first).contains(QStringLiteral("Read: 63.0 MiB/s (valid)")));
        QVERIFY(rowText(capacity_first).contains(QStringLiteral("Used: 31.0 GiB (valid)")));
        summary.backend.publishIo(rates(1, QStringLiteral("replacement-net-"), 1024), rates(1, QStringLiteral("replacement-disk-"), 1048576));
        summary.backend.publishCapacity(capacities(1, QStringLiteral("/replacement-")));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 1, 1000);
        QVERIFY(rowText(summary.row("networkRow", 0)).contains(QStringLiteral("replacement-net-0")));
        QVERIFY(rowText(summary.row("diskRow", 0)).contains(QStringLiteral("replacement-disk-0")));
        QVERIFY(rowText(summary.row("capacityRow", 0)).contains(QStringLiteral("/replacement-0")));
        summary.backend.publishIo({}, {});
        summary.backend.publishCapacity({});
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("networkRow"), 0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("diskRow"), 0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("capacityRow"), 0, 1000);
        QCoreApplication::processEvents();
        QCOMPARE(summary.warnings, 0);
    }

    void coreSameCountRefreshAndHiddenRestoration() {
        Summary summary{QStringLiteral("qrc:/qml/CoreCpuSummary.qml")};
        QVERIFY2(summary.root, qPrintable(summary.component.errorString()));
        summary.backend.publishCores({cpu(QStringLiteral("cpu0"), reading(0.0), reading(100.0)),
            cpu(QStringLiteral("cpu1"), reading(100.0), reading(100.0)),
            cpu(QStringLiteral("cpu2"), reading({}, QStringLiteral("warming")), reading({}, QStringLiteral("unsupported"))),
            cpu(QStringLiteral("cpu3"), reading({}, QStringLiteral("permission_denied")), reading({}, QStringLiteral("error")))});
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 4, 1000);
        QVERIFY(rowText(summary.row("coreRow", 0)).contains(QStringLiteral("Busy: 0.0 % (valid)")));
        QVERIFY(rowText(summary.row("coreRow", 0)).contains(QStringLiteral("I/O wait: 100.0 % (valid)")));
        QVERIFY(rowText(summary.row("coreRow", 1)).contains(QStringLiteral("Busy: 100.0 % (valid)")));
        QVERIFY(rowText(summary.row("coreRow", 1)).contains(QStringLiteral("I/O wait: 100.0 % (valid)")));
        QVERIFY(rowText(summary.row("coreRow", 2)).contains(QStringLiteral("Busy: — (warming)")));
        QVERIFY(rowText(summary.row("coreRow", 2)).contains(QStringLiteral("I/O wait: — (unsupported)")));
        QVERIFY(rowText(summary.row("coreRow", 3)).contains(QStringLiteral("Busy: — (permission denied)")));
        QVERIFY(rowText(summary.row("coreRow", 3)).contains(QStringLiteral("I/O wait: — (error)")));
        QPointer<QObject> core0 = summary.row("coreRow", 0), core1 = summary.row("coreRow", 1);
        QPointer<QObject> core2 = summary.row("coreRow", 2), core3 = summary.row("coreRow", 3);
        summary.backend.publishCores({cpu(QStringLiteral("cpu4"), reading(12.5), reading(0.0)),
            cpu(QStringLiteral("cpu5"), reading({}, QStringLiteral("unsupported")), reading(25.0)),
            cpu(QStringLiteral("cpu6"), reading(100.0), reading(50.0)),
            cpu(QStringLiteral("cpu7"), reading({}, QStringLiteral("error")), reading({}, QStringLiteral("warming")))});
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("coreRow", 0)).contains(QStringLiteral("cpu4")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("coreRow", 1)).contains(QStringLiteral("I/O wait: 25.0 % (valid)")), 1000);
        QCOMPARE(summary.row("coreRow", 0), core0.data());
        QCOMPARE(summary.row("coreRow", 1), core1.data());
        QCOMPARE(summary.row("coreRow", 2), core2.data());
        QCOMPARE(summary.row("coreRow", 3), core3.data());
        QVERIFY(rowText(core0).contains(QStringLiteral("Busy: 12.5 % (valid)")));
        QVERIFY(rowText(core0).contains(QStringLiteral("I/O wait: 0.0 % (valid)")));
        QVERIFY(!rowText(core0).contains(QStringLiteral("cpu0")));
        QVERIFY(rowText(core1).contains(QStringLiteral("Busy: — (unsupported)")));
        QVERIFY(rowText(core2).contains(QStringLiteral("cpu6")));
        QVERIFY(rowText(core2).contains(QStringLiteral("I/O wait: 50.0 % (valid)")));
        QVERIFY(rowText(core3).contains(QStringLiteral("Busy: — (error)")));
        QVERIFY(rowText(core3).contains(QStringLiteral("I/O wait: — (warming)")));
        QCOMPARE(summary.warnings, 0);
        QVERIFY(summary.root->setProperty("visible", false));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 0, 1000);
        summary.backend.publishCores(cores(3, 200));
        QCoreApplication::processEvents();
        QCOMPARE(summary.count("coreRow"), 0);
        QVERIFY(summary.root->setProperty("visible", true));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 3, 1000);
        QVERIFY(rowText(summary.row("coreRow", 2)).contains(QStringLiteral("cpu202")));
        QVERIFY(rowText(summary.row("coreRow", 2)).contains(QStringLiteral("Busy: 2.0 % (valid)")));
        QVERIFY(rowText(summary.row("coreRow", 2)).contains(QStringLiteral("I/O wait: 98.0 % (valid)")));
        QCoreApplication::processEvents();
        QCOMPARE(summary.warnings, 0);
    }

    void coreBoundaryReorderShrinkAndEmpty() {
        Summary summary{QStringLiteral("qrc:/qml/CoreCpuSummary.qml")};
        QVERIFY2(summary.root, qPrintable(summary.component.errorString()));
        summary.backend.publishCores(cores(2));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 2, 1000);
        auto rows = cores(64);
        summary.backend.publishCores(rows);
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 64, 1000);
        QPointer<QObject> first = summary.row("coreRow", 0), last = summary.row("coreRow", 63);
        std::reverse(rows.begin(), rows.end());
        summary.backend.publishCores(rows);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("coreRow", 0)).contains(QStringLiteral("cpu63")), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(rowText(summary.row("coreRow", 63)).contains(QStringLiteral("cpu0")), 1000);
        QCOMPARE(summary.row("coreRow", 0), first.data());
        QCOMPARE(summary.row("coreRow", 63), last.data());
        QVERIFY(rowText(first).contains(QStringLiteral("Busy: 63.0 % (valid)")));
        QVERIFY(rowText(first).contains(QStringLiteral("I/O wait: 37.0 % (valid)")));
        QVERIFY(rowText(last).contains(QStringLiteral("Busy: 0.0 % (valid)")));
        QVERIFY(rowText(last).contains(QStringLiteral("I/O wait: 100.0 % (valid)")));
        summary.backend.publishCores(cores(1, 128));
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 1, 1000);
        QVERIFY(rowText(summary.row("coreRow", 0)).contains(QStringLiteral("cpu128")));
        summary.backend.publishCores({});
        QTRY_COMPARE_WITH_TIMEOUT(summary.count("coreRow"), 0, 1000);
        QCoreApplication::processEvents();
        QCOMPARE(summary.warnings, 0);
    }
};

QTEST_MAIN(OverviewRowsTests)
#include "qt_overview_rows_tests.moc"
