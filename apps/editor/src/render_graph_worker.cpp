#include "internal/render_graph_worker.h"

#include "ludus_editor_shader_reload_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 POLL_MILLISECONDS = 10;
constexpr foundation::int32 MAX_START_POLLS = 300;
constexpr foundation::int32 STOP_MILLISECONDS = 300;
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
    if (mEndpoint.isEmpty() == false && mProcess.state() == QProcess::Running && QFileInfo::exists(mEndpoint) == true)
    {
        mState = RenderGraphWorkerState::Ready;
        return mEndpoint;
    }
    mState = RenderGraphWorkerState::Stopped;
    mError.clear();
    mEndpoint.clear();
    const QString executable = QString::fromUtf8(shader_reload_paths::WORKER_EXECUTABLE);
    if (mDirectory.isValid() == false || QFileInfo(executable).isExecutable() == false)
    {
        mState = RenderGraphWorkerState::Failed;
        mError = QStringLiteral("The shader compiler worker is unavailable. Rerun editor setup with 3D preview.");
        return {};
    }
    if (mProcess.state() == QProcess::NotRunning)
    {
        static_cast<void>(QFile::remove(mDirectory.filePath(QStringLiteral("compiler.sock"))));
    }
    mEndpoint = mDirectory.filePath(QStringLiteral("compiler.sock"));
    mPolls = 0;
    mState = RenderGraphWorkerState::Starting;
    mProcess.start(executable, {QStringLiteral("--serve"), mEndpoint});
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
    if (mProcess.state() == QProcess::Running && QFileInfo::exists(mEndpoint) == true)
    {
        mPoll.stop();
        mState = RenderGraphWorkerState::Ready;
        Q_EMIT Ready();
    }
    else if (++mPolls >= MAX_START_POLLS || mProcess.state() == QProcess::NotRunning)
    {
        mPoll.stop();
        mState = RenderGraphWorkerState::Failed;
        mError = QStringLiteral("The shader compiler worker could not start: %1").arg(mProcess.errorString());
        Q_EMIT Failed();
    }
}
} // namespace ludus::editor
