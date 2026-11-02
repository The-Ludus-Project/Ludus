#include <catch2/catch_session.hpp>

#include <ludus/foundation/base/types.h>

#include <QApplication>
#include <QTemporaryDir>
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/viewport_trace.h"
#    include <QByteArray>
#    include <QFile>
#    include <QJsonDocument>
#    include <QJsonObject>
#    include <ludus/foundation/containers/array.hpp>
#    include <ludus/foundation/profiling/perfetto_export.hpp>
#    include <ludus/foundation/profiling/trace_system.hpp>
#    include <string>
#endif
#if !defined(Q_OS_WASM)
#    include <QProcess>
#endif

int main(int argc, char* argv[])
{
    // Never read or change the user's editor history in offscreen tests.
    QTemporaryDir config;
    if (config.isValid() == false)
    {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("LUDUS_EDITOR_RECOVERY_DIRECTORY", config.filePath(QStringLiteral("source-recovery")).toUtf8());
    QApplication application(argc, argv);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    Catch::Session session;
    std::string tracePath;
    session.cli(session.cli() | Catch::Clara::Opt(tracePath, "path")["--trace-output"]("Save a CPU Perfetto capture"));
    const ludus::foundation::int32 parsed = session.applyCommandLine(argc, argv);
    if (parsed != 0)
    {
        return parsed;
    }
    namespace profile = ludus::foundation::profiling;
    if (tracePath.empty() == false)
    {
        profile::RegisterThreadForTrace("Editor benchmark");
        if (profile::BeginCapture() == false)
        {
            return 1;
        }
        if (ludus::editor::StartViewportTrace() == false)
        {
            profile::EndCapture();
            return 1;
        }
    }
    const ludus::foundation::int32 result = session.run();
    if (tracePath.empty() == false)
    {
        profile::EndCapture();
        const ludus::editor::ViewportTraceSnapshot viewportTrace = ludus::editor::StopViewportTrace();
        ludus::foundation::Array<ludus::foundation::uint8> bytes;
        if (profile::SerializePerfettoTrace(bytes) == false)
        {
            return 1;
        }
        const QByteArray cpuBytes(reinterpret_cast<const char*>(bytes.GetData()),
                                  static_cast<qsizetype>(bytes.GetSize()));
        QJsonObject trace = QJsonDocument::fromJson(cpuBytes).object();
        ludus::editor::AppendViewportTrace(trace, viewportTrace);
        const QByteArray serialized = QJsonDocument(trace).toJson(QJsonDocument::Compact);
        QFile file(QString::fromStdString(tracePath));
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == false ||
            file.write(serialized) != serialized.size())
        {
            return 1;
        }
    }
#else
    const ludus::foundation::int32 result = Catch::Session().run(argc, argv);
#endif
#if !defined(Q_OS_WASM)
    // Tests do not run the application event loop through aboutToQuit. Reap
    // native service children whose asynchronous kill just completed instead.
    constexpr ludus::foundation::int32 PROCESS_SHUTDOWN_MILLISECONDS = 1000;
    for (QProcess* process : application.findChildren<QProcess*>())
    {
        if (process->state() != QProcess::NotRunning)
        {
            process->kill();
            static_cast<void>(process->waitForFinished(PROCESS_SHUTDOWN_MILLISECONDS));
        }
    }
#endif
    return result;
}
