// Subprocess fixture: failures deliberately terminate a child, never the test runner.
#include "internal/crash_capture.h"
#include "internal/crash_reporting.h"

#include <ludus/foundation/base/assert.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdlib>

namespace
{
LUDUS_NOINLINE void TriggerCrashFromCaller()
{
    std::abort();
}

LUDUS_NOINLINE void TriggerFatalAssertionFromCaller()
{
    // Verify the callback ran. A native assertion report requires the separate
    // emergency socket, which launch fixtures leave unconfigured.
    QFile marker;
    const QByteArray message = QByteArrayLiteral("triggering fatal assertion fixture\n");
    if (marker.open(2, QIODevice::WriteOnly | QIODevice::Unbuffered) == false ||
        marker.write(message) != message.size())
    {
        QCoreApplication::exit(1);
        return;
    }
    LUDUS_REQUIRE(false, "crash capture assertion fixture");
}

int WriteCounts(const char* database)
{
    ludus::foundation::usize pending = 0;
    ludus::foundation::usize uploaded = 0;
    if (ludus::editor::ReadCrashReportCounts(database, pending, uploaded) == false)
    {
        return 1;
    }
    QFile output;
    if (output.open(1, QIODevice::WriteOnly) == false)
    {
        return 1;
    }
    const QJsonObject counts{
        {QStringLiteral("pending"), static_cast<ludus::foundation::int64>(pending)},
        {QStringLiteral("uploaded"), static_cast<ludus::foundation::int64>(uploaded)},
    };
    output.write(QJsonDocument(counts).toJson(QJsonDocument::Compact));
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        return 1;
    }
    const QString mode = QString::fromLocal8Bit(argv[1]);
    if (mode == QStringLiteral("consent"))
    {
        QTemporaryDir temporary;
        if (temporary.isValid() == false)
        {
            return 1;
        }
        QSettings settings(temporary.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        const QString original = QStringLiteral("https://abc@first.example.test/42");
        const QString different = QStringLiteral("https://abc@second.example.test/42");
        if (ludus::editor::HasEditorCrashUploadConsent(settings, original) == true)
        {
            return 1;
        }
        settings.setValue(QStringLiteral("crashReporting/automaticSubmission"), true);
        if (ludus::editor::HasEditorCrashUploadConsent(settings, original) == true)
        {
            return 1;
        }
        settings.setValue(QStringLiteral("crashReporting/approvedDsn"), original);
        if (ludus::editor::HasEditorCrashUploadConsent(settings, original) == false ||
            ludus::editor::HasEditorCrashUploadConsent(settings, different) == true)
        {
            return 1;
        }
        settings.setValue(QStringLiteral("crashReporting/automaticSubmission"), false);
        return ludus::editor::HasEditorCrashUploadConsent(settings, original) == false ? 0 : 1;
    }
    constexpr int PRODUCTION_ARGUMENT_COUNT = 3;
    if (mode == QStringLiteral("production") && argc == PRODUCTION_ARGUMENT_COUNT)
    {
        // Use the actual startup initializer with a fresh test-only preferences store
        // and a loopback destination. Native macOS preferences may outlive HOME changes.
        QSettings settings(QString::fromLocal8Bit(argv[2]), QSettings::IniFormat);
        settings.setValue(QStringLiteral("crashReporting/dsn"), QStringLiteral("https://fixture@127.0.0.1/42"));
        ludus::editor::InitializeEditorCrashReporting(settings);
        QApplication application(argc, argv);
        QTimer::singleShot(0, &application, TriggerCrashFromCaller);
        return application.exec();
    }
    if (mode == QStringLiteral("counts") && argc == 3)
    {
        return WriteCounts(argv[2]);
    }
    if (mode == QStringLiteral("url") && argc == 3)
    {
        const QString url = ludus::editor::MakeSentryMinidumpUrl(QString::fromLocal8Bit(argv[2]));
        QFile output;
        if (output.open(1, QIODevice::WriteOnly) == false)
        {
            return 1;
        }
        output.write(url.toUtf8());
        return 0;
    }
    if (mode == QStringLiteral("permission") && argc == 4)
    {
        return ludus::editor::SetCrashUploadsEnabled(argv[2],
                                                     QString::fromLocal8Bit(argv[3]) == QStringLiteral("yes")) == true
                   ? 0
                   : 1;
    }
    if (mode != QStringLiteral("launch") || argc != 7)
    {
        return 1;
    }
    const bool canUpload = QString::fromLocal8Bit(argv[5]) == QStringLiteral("yes");
    const bool shouldCrash = QString::fromLocal8Bit(argv[6]) == QStringLiteral("crash");
    const bool shouldFailAssertion = QString::fromLocal8Bit(argv[6]) == QStringLiteral("assert");
    const bool shouldRevoke = QString::fromLocal8Bit(argv[6]) == QStringLiteral("revoke");
    const bool shouldEnable = QString::fromLocal8Bit(argv[6]) == QStringLiteral("enable");
    if (ludus::editor::StartCrashCapture(argv[2],
                                         argv[3],
                                         argv[4],
                                         R"({"release":"ludus-crash-probe@fixture","tags":{"component":"editor"}})",
                                         canUpload) == false)
    {
        return 2;
    }
    QCoreApplication application(argc, argv);
    if (shouldRevoke == true)
    {
        if (ludus::editor::SetCrashUploadsEnabled(argv[3], false) == false)
        {
            return 2;
        }
    }
    if (shouldEnable == true)
    {
        if (ludus::editor::SetCrashUploadsEnabled(argv[3], true) == false)
        {
            return 2;
        }
    }
    if (shouldFailAssertion == true)
    {
        QTimer::singleShot(0, &application, TriggerFatalAssertionFromCaller);
    }
    else if (shouldCrash == true || shouldRevoke == true)
    {
        QTimer::singleShot(0, &application, TriggerCrashFromCaller);
    }
    else
    {
        QTimer::singleShot(15000, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
