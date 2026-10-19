#include "internal/external_code_editor.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QWidget>

namespace ludus::editor
{
namespace
{
#if !defined(Q_OS_WASM)
const QString APPLICATION_KEY = QStringLiteral("sourceEditor/application");

bool IsCodeEditorApplication(const QFileInfo& application)
{
#    if defined(Q_OS_MACOS)
    if (application.isDir() == true && application.suffix().compare(QStringLiteral("app"), Qt::CaseInsensitive) == 0)
    {
        return true;
    }
#    endif
    return application.isFile() == true && application.isExecutable() == true;
}
#endif
} // namespace

bool MakeExternalCodeEditorCommand(const QString& application,
                                   const QString& file,
                                   ExternalCodeEditorCommand& outCommand)
{
#if defined(Q_OS_WASM)
    static_cast<void>(application);
    static_cast<void>(file);
    static_cast<void>(outCommand);
    return false;
#else
    const QFileInfo program(application);
    const QFileInfo source(file);
    if (IsCodeEditorApplication(program) == false || source.isFile() == false)
    {
        return false;
    }
    ExternalCodeEditorCommand command;
#    if defined(Q_OS_MACOS)
    if (program.isDir() == true)
    {
        command.Program = QStringLiteral("/usr/bin/open");
        command.Arguments = {QStringLiteral("-a"), program.canonicalFilePath(), source.canonicalFilePath()};
    }
    else
#    endif
    {
        command.Program = program.canonicalFilePath();
        command.Arguments = {source.canonicalFilePath()};
    }
    outCommand = command;
    return true;
#endif
}

bool ExternalCodeEditor::Configure(QWidget* ownerOrNull)
{
#if defined(Q_OS_WASM)
    static_cast<void>(ownerOrNull);
    return false;
#else
    const QString application = QFileDialog::getOpenFileName(ownerOrNull,
                                                             QStringLiteral("Choose Code Editor"),
#    if defined(Q_OS_MACOS)
                                                             QStringLiteral("/Applications"),
                                                             QStringLiteral("Applications (*.app);;All files (*)"));
#    else
                                                             QStandardPaths::writableLocation(
                                                                 QStandardPaths::ApplicationsLocation),
                                                             QStringLiteral("All files (*)"));
#    endif
    if (application.isEmpty() == true)
    {
        return false;
    }
    const QFileInfo program(application);
    if (IsCodeEditorApplication(program) == false)
    {
        QMessageBox::warning(ownerOrNull,
                             QStringLiteral("Code Editor"),
                             QStringLiteral("Choose an application or executable."));
        return false;
    }
    QSettings settings;
    settings.setValue(APPLICATION_KEY, program.canonicalFilePath());
    return true;
#endif
}

bool ExternalCodeEditor::Open(QWidget* ownerOrNull, const QString& file, QString& outError)
{
#if defined(Q_OS_WASM)
    static_cast<void>(ownerOrNull);
    static_cast<void>(file);
    outError = QStringLiteral("External code editors require the native editor.");
    return false;
#else
    QSettings settings;
    QString application = settings.value(APPLICATION_KEY).toString();
    if (application.isEmpty() == true)
    {
        if (Configure(ownerOrNull) == false)
        {
            outError = QStringLiteral("No code editor selected; source files were retained.");
            return false;
        }
        application = settings.value(APPLICATION_KEY).toString();
    }
    ExternalCodeEditorCommand command;
    if (MakeExternalCodeEditorCommand(application, file, command) == false ||
        QProcess::startDetached(command.Program, command.Arguments, QFileInfo(file).absolutePath()) == false)
    {
        outError =
            QStringLiteral("Could not open this source. Use Choose Code Editor to select an installed application.");
        return false;
    }
    outError.clear();
    return true;
#endif
}
} // namespace ludus::editor
