// Subprocess fixture: failures deliberately terminate a child, never the test runner.
#include "internal/crash_capture.h"
#include "internal/crash_reporting.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTimer>

#include <cstdlib>

namespace
{
LUDUS_NOINLINE void TriggerCrashFromCaller()
{
    std::abort();
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
    if (mode == QStringLiteral("production"))
    {
        ludus::editor::InitializeEditorCrashReporting();
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
    if (shouldCrash == true || shouldRevoke == true)
    {
        QTimer::singleShot(0, &application, TriggerCrashFromCaller);
    }
    else
    {
        QTimer::singleShot(15000, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
