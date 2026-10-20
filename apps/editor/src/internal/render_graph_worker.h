#pragma once

#include <ludus/foundation/base/types.h>

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

namespace ludus::editor
{
/// Lifecycle of the editor-owned shader compiler process.
enum class RenderGraphWorkerState : foundation::uint8
{
    Stopped,
    Starting,
    Ready,
    Failed,
};

/// Asynchronous private compiler worker shared by successive project graph builds.
/// Owns the process and socket directory; never writes editor preferences or project sources.
class RenderGraphWorker final : public QObject
{
    Q_OBJECT
public:
    explicit RenderGraphWorker(QObject* parentOrNull = nullptr);
    ~RenderGraphWorker() override;
    /// Start if needed. Ready returns its socket; Starting emits Ready once the socket exists.
    /// Failure returns an empty socket and GetError; another explicit build can retry.
    [[nodiscard]] QString Prepare();
    [[nodiscard]] QString GetError() const;
    /// Current startup/connection state; readiness never waits on the GUI thread.
    [[nodiscard]] RenderGraphWorkerState GetState() const noexcept;
Q_SIGNALS:
    void Ready();
    void Failed();

private:
    void poll();
    QProcess mProcess;
    QTemporaryDir mDirectory;
    QTimer mPoll;
    QString mEndpoint;
    QString mError;
    RenderGraphWorkerState mState = RenderGraphWorkerState::Stopped;
    foundation::int32 mPolls = 0;
};
} // namespace ludus::editor
