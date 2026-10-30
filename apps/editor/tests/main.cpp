#include <catch2/catch_session.hpp>

#include <ludus/foundation/base/types.h>

#include <QApplication>
#include <QTemporaryDir>
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
    QApplication application(argc, argv);
    const ludus::foundation::int32 result = Catch::Session().run(argc, argv);
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
