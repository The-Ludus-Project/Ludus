// Ludus Editor application entry point (milestone E0).
//
// Launches the Qt Widgets workspace. It expects an already-configured/built
// editor; it does not build or install packages. The trusted tooling root,
// managed Python interpreter and optional project path are supplied explicitly
// by scripts/editor (never read from a project descriptor). See design.md
// sections 2, 7.

#include "internal/controller.h"
#include "internal/editor_files.h"
#include "internal/main_window.h"
#include "internal/qt_accessibility.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QString>

namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_EDITOR_MAIN, "Editor");

// Bounded Qt message handler: forward Qt's own diagnostics to FoundationLogging
// without recursion and without touching widgets. No global logger redesign.
void ForwardQtMessage(QtMsgType type, const QMessageLogContext& /*context*/, const QString& message)
{
    const QByteArray utf8 = message.toUtf8();
    const std::string_view view(utf8.constData(), static_cast<ludus::foundation::usize>(utf8.size()));
    switch (type)
    {
        case QtDebugMsg:
        case QtInfoMsg:
            LUDUS_LOG_TEXT(LOG_EDITOR_MAIN, Info, view);
            break;
        case QtWarningMsg:
            LUDUS_LOG_TEXT(LOG_EDITOR_MAIN, Warning, view);
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            LUDUS_LOG_TEXT(LOG_EDITOR_MAIN, Error, view);
            break;
    }
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Ludus Editor"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/ludus/ludus-icon.png")));

    namespace logging = ludus::foundation::logging;
    logging::LogConfig logConfig;
    logConfig.EnableFile = false; // the editor is a short interactive session
    logging::LogSystem::Initialize(logConfig);
    qInstallMessageHandler(ForwardQtMessage);
#if defined(Q_OS_MACOS)
    if (ludus::editor::InstallQtAccessibilityOwnershipFix() == false)
    {
        LUDUS_LOG_TEXT(LOG_EDITOR_MAIN, Error, "Cannot install the Qt Cocoa accessibility ownership correction.");
        logging::LogSystem::Shutdown();
        return 1;
    }
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Ludus Editor workspace"));
    parser.addHelpOption();
    QCommandLineOption toolingRootOption(QStringLiteral("tooling-root"),
                                         QStringLiteral("Absolute trusted Ludus tooling checkout"),
                                         QStringLiteral("path"));
    QCommandLineOption pythonOption(QStringLiteral("python"),
                                    QStringLiteral("Absolute managed Python interpreter"),
                                    QStringLiteral("path"));
    QCommandLineOption projectOption(QStringLiteral("project"),
                                     QStringLiteral("Absolute project descriptor to open"),
                                     QStringLiteral("path"));
    QCommandLineOption adapterOption(QStringLiteral("adapter"),
                                     QStringLiteral("Absolute editor_tool.py adapter path"),
                                     QStringLiteral("path"));
    parser.addOption(toolingRootOption);
    parser.addOption(pythonOption);
    parser.addOption(projectOption);
    parser.addOption(adapterOption);
    parser.process(app);

    ludus::editor::ToolingPaths tooling;
    tooling.ToolingRoot = parser.value(toolingRootOption);
    tooling.PythonPath = parser.value(pythonOption);
    tooling.AdapterPath = parser.value(adapterOption);

    ludus::editor::EditorController controller(tooling);
    ludus::editor::MainWindow window(&controller);
    window.resize(900, 700);
#if defined(Q_OS_WASM)
    window.showMaximized();
    QString sample;
    if (ludus::editor::CopyEditorSample(QStringLiteral("cornell-box"), sample))
    {
        controller.OpenProject(sample);
    }
#else
    window.show();
#endif

    if (parser.isSet(projectOption))
    {
        controller.OpenProject(parser.value(projectOption));
    }

    const int code = app.exec();
    logging::LogSystem::Shutdown();
    return code;
}
