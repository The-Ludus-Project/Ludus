#include "internal/viewport_trace.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#if !defined(Q_OS_WASM)
#    include <QProcess>
#endif

#include <catch2/catch_session.hpp>

#include <string>

namespace
{
// Distinct from Catch2's own exit codes so a runtime profile log names the
// trace request, not a test, as the failure.
constexpr ludus::foundation::int32 TRACE_FAILURE_EXIT_CODE = 3;
} // namespace

// The entry point of every editor test executable. `--trace-output <path>`
// records a FoundationProfiling CPU capture and the editor viewport wall-clock
// spans around the whole Catch2 session and writes them as one Perfetto JSON
// trace, independently of which editor configuration the executable links;
// config/runtime_profile_scenarios.json relies on that for the editor viewport
// scenario. A profiling-disabled build cannot honour the request, so it fails
// instead of silently producing no trace.
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
    Catch::Session session;
    std::string tracePath;
    session.cli(session.cli() | Catch::Clara::Opt(tracePath, "path")["--trace-output"](
                                    "Save a CPU Perfetto capture with the viewport spans (needs a profiling-enabled "
                                    "Debug, Development or Profile build)"));
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
            return TRACE_FAILURE_EXIT_CODE;
        }
        if (ludus::editor::StartViewportTrace() == false)
        {
            profile::EndCapture();
            return TRACE_FAILURE_EXIT_CODE;
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
            return TRACE_FAILURE_EXIT_CODE;
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
            return TRACE_FAILURE_EXIT_CODE;
        }
    }
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
