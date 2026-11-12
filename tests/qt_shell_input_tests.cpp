#include "telemetry_bridge.hpp"
#include "process_controller.hpp"
#include <QProcess>
#include <csignal>
#include <cmath>
#include <unistd.h>
#include <QAccessible>
#include <QFont>
#include <QMap>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtTest>

class ShellInputTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QQuickStyle::setStyle(QStringLiteral("Basic")); }
    void semanticHeadingsRemainReadable() {
        QAccessible::setActive(true);
        night::EngineSources sources;
        sources.system = [] { return night::SystemSnapshot{}; };
        sources.processes = [] { return night::ProcessSnapshot{}; };
        sources.io = [] { return night::IoSnapshot{}; };
        sources.capacity = [] { return night::CapacitySnapshot{}; };
        night::Engine engine{{}, sources};
        night::ui::TelemetryBridge bridge{engine};
        QQmlApplicationEngine qml;
        QStringList warnings;
        QObject warning_context;
        connect(&qml, &QQmlEngine::warnings, &warning_context, [&](const QList<QQmlError>& errors) {
            for (const auto& error : errors) warnings.push_back(error.toString());
        });
        qml.setInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&bridge)}});
        qml.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        QVERIFY2(!qml.rootObjects().isEmpty(), qPrintable(warnings.join(QStringLiteral("; "))));
        auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
        QVERIFY(window);
        QMap<QString, QQuickItem*> headings;
        for (auto* item : window->findChildren<QQuickItem*>()) {
            auto* accessible = QAccessible::queryAccessibleInterface(item);
            if (!accessible || accessible->role() != QAccessible::Heading) continue;
            const auto name = accessible->text(QAccessible::Name);
            QVERIFY(!name.isEmpty());
            QVERIFY2(!headings.contains(name), "Each heading must have one unambiguous accessible name");
            headings.insert(name, item);
        }
        const QList<QPair<QString, int>> expected{
            {QStringLiteral("ni_GHT_si"), 0}, {QStringLiteral("Overview"), 0},
            {QStringLiteral("Processes"), 1}, {QStringLiteral("Performance"), 2},
            {QStringLiteral("Settings"), 3}, {QStringLiteral("Applications"), 4}};
        QCOMPARE(headings.size(), expected.size());
        qreal title_size = 0, page_size = 0;
        const auto body_size = QGuiApplication::font().pointSizeF();
        QVERIFY(body_size > 0);
        for (const auto& entry : expected) {
            auto* heading = headings.value(entry.first);
            QVERIFY2(heading, qPrintable(entry.first));
            QVERIFY(window->setProperty("currentPage", entry.second));
            QTRY_VERIFY_WITH_TIMEOUT(heading->isVisible() && heading->width() > 0 && heading->height() > 0, 2000);
            QVERIFY(std::isfinite(heading->width()) && std::isfinite(heading->height()));
            auto* accessible = QAccessible::queryAccessibleInterface(heading);
            QVERIFY(accessible && accessible->isValid() && !accessible->state().invisible);
            QCOMPARE(accessible->role(), QAccessible::Heading);
            QCOMPARE(accessible->text(QAccessible::Name), entry.first);
            QCOMPARE(heading->property("text").toString(), entry.first);
            const auto font = heading->property("font").value<QFont>();
            QCOMPARE(font.weight(), QFont::Normal);
            QVERIFY2(std::isfinite(font.pointSizeF()) && font.pointSizeF() > body_size,
                     "Headings must retain a readable size above body text");
            if (entry.first == QStringLiteral("ni_GHT_si")) title_size = font.pointSizeF();
            else if (page_size == 0) page_size = font.pointSizeF();
            else QCOMPARE(font.pointSizeF(), page_size);
        }
        QVERIFY(title_size < page_size); // Page headings retain the stronger visual level.
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QStringLiteral("; "))));
        window->close();
    }
    void processConfirmationKeyboard() {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--owned-action-child")});
        QVERIFY(child.waitForStarted(2000));
        const auto pid = static_cast<std::uint64_t>(child.processId());
        night::ui::ProcessController actions{[pid](std::uint64_t requested, night::ProcessAction action) {
            if (requested != pid) return night::ProcessActionPreparation{};
            return night::ProcessActionCandidate::prepare_owned_child(pid, action);
        }};
        night::Engine engine;
        night::ui::TelemetryBridge bridge{engine};
        QQmlApplicationEngine qml;
        qml.setInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&bridge)},
                                  {QStringLiteral("processActions"), QVariant::fromValue(&actions)}});
        qml.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front()); QVERIFY(window);
        window->setProperty("currentPage", 1);
        auto* dialog = window->findChild<QObject*>(QStringLiteral("actionConfirmation"));
        auto* cancel = window->findChild<QQuickItem*>(QStringLiteral("cancelProcessAction"));
        auto* confirm = window->findChild<QQuickItem*>(QStringLiteral("confirmProcessAction"));
        auto* details = window->findChild<QObject*>(QStringLiteral("actionCandidateDetails"));
        QVERIFY(dialog && cancel && confirm && details);
        actions.prepare(QString::number(pid));
        QTRY_VERIFY_WITH_TIMEOUT(actions.awaitingConfirmation(), 4000);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QTRY_VERIFY(cancel->hasActiveFocus());
        QVERIFY(details->property("text").toString().contains("PID " + QString::number(pid)));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!actions.awaitingConfirmation()); QTRY_VERIFY(!actions.busy());
        QCOMPARE(child.state(), QProcess::Running); // Escape sent no signal.
        actions.prepare(QString::number(pid));
        QTRY_VERIFY_WITH_TIMEOUT(actions.awaitingConfirmation(), 4000);
        QTRY_VERIFY(cancel->hasActiveFocus());
        confirm->forceActiveFocus(); QTRY_VERIFY(confirm->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space); // Explicit consent for owned child only.
        QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(actions.message().contains("Exit observed"), 4000);
        QVERIFY(!dialog->property("visible").toBool());
        window->close();
    }
    void samplingModeKeyboard() {
        night::Engine engine;
        night::ui::TelemetryBridge bridge{engine};
        QQmlApplicationEngine qml;
        qml.setInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&bridge)}});
        qml.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
        QVERIFY(window);
        window->setProperty("currentPage", 3);
        auto* selector = window->findChild<QQuickItem*>(QStringLiteral("refreshModeSelector"));
        QVERIFY(selector); selector->forceActiveFocus(); QTRY_VERIFY(selector->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_End);
        QTRY_COMPARE(bridge.refreshMode(), QStringLiteral("low-power"));
        QTest::keyClick(window, Qt::Key_Home);
        QTRY_COMPARE(bridge.refreshMode(), QStringLiteral("fast"));
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(bridge.refreshMode(), QStringLiteral("balanced"));
        window->close();
    }
    void searchAndSelectionKeys() {
        QAccessible::setActive(true);
        night::Engine engine;
        night::ui::TelemetryBridge bridge{engine};
        QQmlApplicationEngine qml;
        qml.setInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&bridge)}});
        qml.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
        QVERIFY(window);
        window->setProperty("currentPage", 1);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.processView()->count() > 0, 2000);
        auto* search = window->findChild<QQuickItem*>(QStringLiteral("processSearch"));
        auto* table = window->findChild<QQuickItem*>(QStringLiteral("processTable"));
        auto* previous = window->findChild<QQuickItem*>(QStringLiteral("previousProcess"));
        auto* next = window->findChild<QQuickItem*>(QStringLiteral("nextProcess"));
        QVERIFY(search && table && previous && next);
        search->forceActiveFocus();
        QTRY_VERIFY(search->hasActiveFocus());
        bridge.processView()->selectRow(0);
        const QByteArray absent("__night_absent_query__");
        for (const char c : absent) QTest::keyClick(window, c);
        QTRY_COMPARE(bridge.processView()->query(), QString::fromLatin1(absent));
        QCOMPARE(bridge.processView()->count(), 0);
        QVERIFY(!previous->isEnabled() && !next->isEnabled());
        QVERIFY(bridge.processView()->selectedIdentity().isEmpty());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Backspace);
        QTRY_VERIFY(bridge.processView()->query().isEmpty());
        QTRY_COMPARE(bridge.processView()->count(), bridge.processes()->count());
        table->forceActiveFocus();
        QTRY_VERIFY(table->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(bridge.processView()->selectedRow(), 0);
        const auto selected = bridge.processView()->selectedIdentity();
        QVERIFY(!selected.isEmpty());
        auto* details = window->findChild<QQuickItem*>(QStringLiteral("selectedProcessDetails"));
        auto* details_text = window->findChild<QQuickItem*>(QStringLiteral("processDetailsText"));
        QVERIFY(details && details_text);
        QTRY_VERIFY(details->isVisible());
        const auto pid = bridge.processView()->selectedDetails().value("pid").toString();
        QVERIFY(!pid.isEmpty());
        QTRY_VERIFY(details_text->property("text").toString().contains("PID " + pid));
        QVERIFY(details_text->property("text").toString().contains(QStringLiteral("CPU ")));
        QVERIFY(details_text->property("text").toString().contains(QStringLiteral("RSS ")));
        auto* accessible = QAccessible::queryAccessibleInterface(details_text);
        QVERIFY(accessible && accessible->isValid());
        QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Selected process details"));
        QVERIFY(accessible->state().readOnly && accessible->state().multiLine && accessible->state().focusable);
        auto* text = accessible->textInterface();
        QVERIFY(text && text->characterCount() > 0);
        // Qt's text document exposes line breaks as Unicode paragraph separators.
        QCOMPARE(text->text(0, text->characterCount()).replace(QChar(0x2029), QLatin1Char('\n')),
                 details_text->property("text").toString());
        auto* next_accessible = QAccessible::queryAccessibleInterface(next);
        auto* previous_accessible = QAccessible::queryAccessibleInterface(previous);
        QVERIFY(next_accessible && previous_accessible);
        auto* next_action = next_accessible->actionInterface();
        auto* previous_action = previous_accessible->actionInterface();
        QVERIFY(next_action && previous_action);
        QVERIFY(next_action->actionNames().contains(QAccessibleActionInterface::pressAction()));
        QVERIFY(previous_action->actionNames().contains(QAccessibleActionInterface::pressAction()));
        QVERIFY(!previous->isEnabled());
        if (bridge.processView()->count() > 1) {
            next_action->doAction(QAccessibleActionInterface::pressAction());
            QTRY_COMPARE_WITH_TIMEOUT(bridge.processView()->selectedRow(), 1, 2000);
            QVERIFY(bridge.processView()->selectedIdentity() != selected);
            previous_action->doAction(QAccessibleActionInterface::pressAction());
            QTRY_COMPARE_WITH_TIMEOUT(bridge.processView()->selectedRow(), 0, 2000);
            QCOMPARE(bridge.processView()->selectedIdentity(), selected);
        } else QVERIFY(!next->isEnabled());
        table->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(details_text->hasActiveFocus(), 2000);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(search->hasActiveFocus(), 2000);
        details_text->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Backtab);
        QTRY_VERIFY_WITH_TIMEOUT(table->hasActiveFocus(), 2000);
        details_text->forceActiveFocus();
        const auto before_refresh = bridge.processes()->sequence();
        QTRY_VERIFY_WITH_TIMEOUT(bridge.processes()->sequence() > before_refresh, 2000);
        QVERIFY(details_text->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(bridge.processView()->selectedIdentity().isEmpty());
        QCOMPARE(bridge.processView()->selectedRow(), -1);
        QVERIFY(bridge.processView()->selectedDetails().isEmpty());
        QTRY_VERIFY(!details->isVisible());
        QTRY_VERIFY(table->hasActiveFocus());
        window->close();
    }
};
int main(int argc, char** argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--owned-action-child") {
        ::alarm(10);
        for (;;) ::pause();
    }
    QGuiApplication app(argc, argv);
    ShellInputTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "qt_shell_input_tests.moc"
