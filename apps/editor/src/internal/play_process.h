#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#if !defined(Q_OS_WASM)
#    include <QProcess>
#endif
#include <QString>
#include <QTimer>

namespace ludus::editor
{
struct PlayLaunch
{
    QString Python;
    QString ToolingRoot;
    foundation::uint64 Epoch = 0;
    QJsonObject StartRequest;
};

// One persistent play supervisor, independent of the one-shot build adapter.
// All pipes are asynchronous. Unexpected loss pins the editor in recovery:
// observing the adapter's exit alone cannot establish native child cleanup.
class PlayProcess final : public QObject
{
    Q_OBJECT
public:
    explicit PlayProcess(QObject* parent = nullptr);
    ~PlayProcess() override;
    bool Start(PlayLaunch launch);
    bool BeginSession(foundation::uint64 epoch, QJsonObject request);
    bool Send(QJsonObject request);
    void Close();
    [[nodiscard]] bool Active() const noexcept
    {
        return Active_;
    }
    [[nodiscard]] bool CanBeginSession() const noexcept
    {
        return Active_ && Ready_ && Failed_ == false && StopLatched_ == false;
    }

Q_SIGNALS:
    void Event(const QJsonObject& event);
    void Finished(bool cleanupConfirmed);

private:
    void Read();
    void Consume();
    void Fail(const QString& message);
#if !defined(Q_OS_WASM)
    QProcess Process_;
#endif
    QTimer ReadyTimer_;
    QJsonObject StartRequest_;
    QByteArray Backlog_;
    QString Epoch_;
    foundation::usize DiagnosticBytes_ = 0;
    bool Active_ = false;
    bool Ready_ = false;
    bool StartSent_ = false;
    bool StopLatched_ = false;
    bool CloseSent_ = false;
    bool CleanupConfirmed_ = false;
    bool Failed_ = false;
    bool ConsumeScheduled_ = false;
};
} // namespace ludus::editor
