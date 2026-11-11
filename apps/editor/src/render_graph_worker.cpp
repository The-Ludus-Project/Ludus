#include "internal/render_graph_worker.h"

#include "ludus_editor_shader_reload_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#if defined(Q_OS_WIN)
#    include <QUuid>

#    include <windows.h>
#endif

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 POLL_MILLISECONDS = 10;
constexpr foundation::int32 MAX_START_POLLS = 300;
constexpr foundation::int32 STOP_MILLISECONDS = 300;

[[nodiscard]] bool IsEndpointReady(const QString& endpoint)
{
#if defined(Q_OS_WIN)
    return WaitNamedPipeW(reinterpret_cast<const wchar_t*>(endpoint.utf16()), NMPWAIT_NOWAIT) != FALSE;
#else
    return QFileInfo::exists(endpoint);
#endif
}
} // namespace
RenderGraphWorker::RenderGraphWorker(QObject* parentOrNull)
    : QObject(parentOrNull), mDirectory(QDir::temp().filePath(QStringLiteral("ludus-rg-XXXXXX")))
{
    mPoll.setInterval(POLL_MILLISECONDS);
    connect(&mPoll, &QTimer::timeout, this, &RenderGraphWorker::poll);
    connect(&mProcess, &QProcess::errorOccurred, this, [this]() {
        if (mPoll.isActive() == true)
        {
            poll();
        }
    });
    mProcess.setProcessChannelMode(QProcess::ForwardedErrorChannel);
}
RenderGraphWorker::~RenderGraphWorker()
{
    mPoll.stop();
    mState = RenderGraphWorkerState::Stopped;
    if (mProcess.state() != QProcess::NotRunning)
    {
        mProcess.terminate();
        if (mProcess.waitForFinished(STOP_MILLISECONDS) == false)
        {
            mProcess.kill();
            static_cast<void>(mProcess.waitForFinished(STOP_MILLISECONDS));
        }
    }
}
QString RenderGraphWorker::Prepare()
{
    if (mState == RenderGraphWorkerState::Starting)
    {
        return {};
    }
    if (mProcess.state() != QProcess::NotRunning)
    {
        return mState == RenderGraphWorkerState::Ready ? mEndpoint : QString{};
    }
    mState = RenderGraphWorkerState::Stopped;
    mError.clear();
    mEndpoint.clear();
    const QString executable = QString::fromUtf8(shader_reload_paths::WORKER_EXECUTABLE);
    if (QFileInfo(executable).isExecutable() == false)
    {
        mState = RenderGraphWorkerState::Failed;
        mError = QStringLiteral("The shader compiler worker is unavailable. Rerun editor setup with 3D preview.");
        return {};
    }
#if defined(Q_OS_WIN)
    // The worker uses a unique local named pipe rather than a filesystem socket.
    mEndpoint = QStringLiteral("\\\\.\\pipe\\ludus-rg-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
#else
    if (mDirectory.isValid() == false)
    {
        mState = RenderGraphWorkerState::Failed;
        mError =
            QStringLiteral("The shader compiler needs a writable temporary directory. Check the editor's temp folder.");
        return {};
    }
    mEndpoint = mDirectory.filePath(QStringLiteral("compiler.sock"));
    static_cast<void>(QFile::remove(mEndpoint));
#endif
    mPolls = 0;
    mState = RenderGraphWorkerState::Starting;
    // The worker exits with the editor, even when the editor itself is killed mid-compile.
    mProcess.start(executable, {QStringLiteral("--serve"), mEndpoint, QStringLiteral("--stop-with-parent")});
    mPoll.start();
    return {};
}
QString RenderGraphWorker::GetError() const
{
    return mError;
}
RenderGraphWorkerState RenderGraphWorker::GetState() const noexcept
{
    return mState;
}
void RenderGraphWorker::poll()
{
    if (mProcess.state() == QProcess::Running && IsEndpointReady(mEndpoint) == true)
    {
        mPoll.stop();
        mState = RenderGraphWorkerState::Ready;
        Q_EMIT Ready();
    }
    else if (++mPolls >= MAX_START_POLLS || mProcess.state() == QProcess::NotRunning)
    {
        mPoll.stop();
        mState = RenderGraphWorkerState::Failed;
        if (mProcess.state() != QProcess::NotRunning)
        {
            mProcess.kill();
            mError = QStringLiteral("The shader compiler worker did not open its endpoint. Retry Build & Preview.");
        }
        else
        {
            mError = QStringLiteral("The shader compiler worker could not start: %1").arg(mProcess.errorString());
        }
        Q_EMIT Failed();
    }
}
} // namespace ludus::editor
