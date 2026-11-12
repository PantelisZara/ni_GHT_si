#include "telemetry_bridge.hpp"
#include "process_controller.hpp"
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QT_QUICK_BACKEND") && !qEnvironmentVariableIsSet("QSG_RHI_BACKEND"))
        QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ni_GHT_si"));
    app.setApplicationVersion(QString::fromLatin1(NIGHT_VERSION));
    app.setDesktopFileName(QStringLiteral("ni_GHT_si"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native Linux system monitor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("smoke"), QStringLiteral("Run bounded shell checks without GPU driver collection.")});
    parser.addOption({QStringLiteral("gpu"), QStringLiteral("Enable optional NVIDIA GPU telemetry; driver calls may delay exit.")});
    parser.addOption({QStringLiteral("no-gpu"), QStringLiteral("Disable optional GPU collection for this run (the default).")});
    parser.addOption({QStringLiteral("refresh-mode"), QStringLiteral("Startup sampling preset: fast, balanced, low-power."),
                      QStringLiteral("mode"), QStringLiteral("balanced")});
    parser.process(app);
    const auto mode_name = parser.value(QStringLiteral("refresh-mode"));
    night::RefreshMode mode;
    if (mode_name == QStringLiteral("fast")) mode = night::RefreshMode::fast;
    else if (mode_name == QStringLiteral("balanced")) mode = night::RefreshMode::balanced;
    else if (mode_name == QStringLiteral("low-power")) mode = night::RefreshMode::low_power;
    else { std::cerr << "refresh-mode must be fast, balanced or low-power\n"; return 2; }
    if (!parser.positionalArguments().isEmpty()) { parser.showHelp(2); }
    const bool gpu_requested = parser.isSet(QStringLiteral("gpu"));
    if (gpu_requested && parser.isSet(QStringLiteral("no-gpu"))) {
        std::cerr << "--gpu and --no-gpu cannot be used together\n";
        return 2;
    }
#ifndef NIGHT_WITH_NVML
    if (gpu_requested) {
        std::cerr << "--gpu requires a build with NVIDIA GPU telemetry support\n";
        return 2;
    }
#endif
    const bool smoke = parser.isSet(QStringLiteral("smoke"));
    auto config = night::sampling_configuration(mode);
    config.include_application_scopes = true;
    night::EngineSources sources;
#ifdef NIGHT_WITH_NVML
    night::NvmlCollector gpu;
    // Automated smoke tests exercise the shell without opening the real driver.
    if (!smoke && gpu_requested) sources.gpu = [&gpu] { return gpu.collect(); };
#endif
    night::Engine metrics{config, std::move(sources)};
    night::ui::TelemetryBridge bridge{metrics};
    night::ui::ProcessController process_actions;
    int qml_errors = 0;
    int frames = 0;
    bool smoke_finished = false;
    QQmlApplicationEngine qml;
    QObject callback_context; // Disconnect before captured state and QML teardown.
    QObject::connect(&qml, &QQmlEngine::warnings, &callback_context, [&](const QList<QQmlError>& errors) {
        qml_errors += static_cast<int>(errors.size());
        for (const auto& error : errors) std::cerr << error.toString().toStdString() << '\n';
    });
    qml.setInitialProperties({{QStringLiteral("backend"), QVariant::fromValue(&bridge)},
                              {QStringLiteral("processActions"), QVariant::fromValue(&process_actions)}});
    qml.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (qml.rootObjects().isEmpty()) return 1;
    if (smoke) {
        auto* root_window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
        if (!root_window) return 1;
        QObject::connect(root_window, &QQuickWindow::frameSwapped, &callback_context, [&frames] { ++frames; });
        QTimer::singleShot(1800, &callback_context, [&] {
            auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().front());
            const bool overview = window && window->isVisible() && window->property("currentPage").toInt() == 0;
            const bool data = bridge.systemQuality() == QStringLiteral("valid")
                && bridge.processQuality() == QStringLiteral("valid") && bridge.processes()->count() > 0;
            if (window) window->setProperty("currentPage", 1);
            // Let the process TableView instantiate before checking QML warnings.
            QTimer::singleShot(200, &callback_context, [&, overview, data] {
                const auto* table = qml.rootObjects().front()->findChild<QObject*>(QStringLiteral("processTable"));
                const bool processes = table && table->property("rows").toInt() == bridge.processes()->count()
                    && qml.rootObjects().front()->property("currentPage").toInt() == 1;
                auto* view = bridge.processView();
                view->sortBy(night::ui::ProcessModel::Cpu, true);
                view->selectRow(0);
                const bool selected = !view->selectedIdentity().isEmpty();
                view->setQuery(QStringLiteral("__ni_GHT_si_absent_process__"));
                const bool filtered = view->count() == 0 && view->selectedIdentity().isEmpty();
                view->setQuery({});
                const bool restored = view->count() == bridge.processes()->count();
                const bool view_ok = selected && filtered && restored;
                const int before_charts = frames;
                qml.rootObjects().front()->setProperty("currentPage", 2);
                QTimer::singleShot(200, &callback_context, [&, overview, data, processes, view_ok, before_charts] {
                    const auto* plot = qml.rootObjects().front()->findChild<QObject*>(QStringLiteral("cpuHistoryPlot"));
                    const auto* canvas = plot ? plot->findChild<QObject*>(QStringLiteral("historyCanvas")) : nullptr;
                    const bool charts = !bridge.history().isEmpty() && canvas
                        && canvas->property("available").toBool() && frames > before_charts;
                    qml.rootObjects().front()->setProperty("currentPage", 3);
                    QTimer::singleShot(100, &callback_context, [&, overview, data, processes, view_ok, charts] {
                        const bool settings = qml.rootObjects().front()->property("currentPage").toInt() == 3
                            && bridge.configuration().value("systemMs").toLongLong() == config.system_interval.count()
                            && bridge.configuration().value("processMs").toLongLong() == config.process_interval.count();
                        qml.rootObjects().front()->setProperty("currentPage", 4);
                        QTimer::singleShot(150, &callback_context, [&, overview, data, processes, view_ok, charts, settings] {
                            const auto* app_table = qml.rootObjects().front()->findChild<QObject*>(QStringLiteral("applicationTable"));
                            const bool applications = app_table && app_table->property("rows").toInt() == bridge.applications()->count()
                                && bridge.configuration().value("scopeMetadata").toBool();
                            const auto* io_status = qml.rootObjects().front()->findChild<QObject*>(QStringLiteral("ioStatusText"));
                            const auto* capacity_status = qml.rootObjects().front()->findChild<QObject*>(QStringLiteral("capacityStatusText"));
                            const auto* core_status = qml.rootObjects().front()->findChild<QObject*>(QStringLiteral("coreCpuStatusText"));
                            const bool io_ui = io_status && capacity_status
                                && !io_status->property("text").toString().isEmpty()
                                && !capacity_status->property("text").toString().isEmpty();
                            const bool core_ui = core_status && !core_status->property("text").toString().isEmpty();
                            std::cout << "QML smoke: overview=" << overview << " data=" << data
                                      << " refresh_mode=" << mode_name.toStdString()
                                      << " processes=" << processes << " view=" << view_ok << " charts=" << charts
                                      << " settings=" << settings << " applications=" << applications
                                      << " io_ui=" << io_ui << " io_quality=" << bridge.ioQuality().toStdString()
                                      << " core_ui=" << core_ui << " core_quality=" << bridge.coreCpuQuality().toStdString()
                                      << " capacity_quality=" << bridge.capacityQuality().toStdString()
                                      << " scope_quality=" << bridge.applications()->quality().toStdString()
                                      << " frames=" << frames << " warnings=" << qml_errors
                                      << " graphics_api=" << static_cast<int>(qobject_cast<QQuickWindow*>(qml.rootObjects().front())->rendererInterface()->graphicsApi()) << '\n';
                            smoke_finished = true;
                            app.exit(overview && data && processes && view_ok && charts && settings && applications && io_ui && core_ui && qml_errors == 0 ? 0 : 1);
                        });
                    });
                });
            });
        });
    }
    const int result = app.exec();
    if (smoke && !smoke_finished) {
        std::cerr << "QML smoke exited before checks completed\n";
        return 1;
    }
    return result;
}
