#include "internal/play_process.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

#include <utility>

namespace ludus::editor
{
using foundation::usize;
namespace
{
constexpr usize kMaxBacklog = usize{1} * 1024U * 1024U;
constexpr usize kMaxLine = usize{256} * 1024U;
} // namespace

PlayProcess::PlayProcess(QObject* parent) : QObject(parent)
{
    Process_.setProcessChannelMode(QProcess::SeparateChannels);
    ReadyTimer_.setSingleShot(true);
    ReadyTimer_.setInterval(5000);
    connect(&ReadyTimer_, &QTimer::timeout, this, [this]() { Fail(QStringLiteral("Play supervisor ready deadline")); });
    connect(&Process_, &QProcess::readyReadStandardOutput, this, &PlayProcess::Read);
    connect(&Process_, &QProcess::readyReadStandardError, this, [this]() {
        DiagnosticBytes_ += static_cast<usize>(Process_.readAllStandardError().size());
        if (DiagnosticBytes_ > usize{64} * 1024U)
        {
            Fail(QStringLiteral("Play diagnostics exceeded 64 KiB"));
        }
    });
    connect(&Process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            Active_ = false;
            CleanupConfirmed_ = true; // No supervisor or native host was spawned.
            ReadyTimer_.stop();
            Q_EMIT Finished(true);
        }
    });
    connect(&Process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        Read();
        // Consume the bounded final backlog before reporting ownership release.
        while (Failed_ == false && Backlog_.contains('\n'))
        {
            Consume();
        }
        ReadyTimer_.stop();
        Active_ = false;
        const bool confirmed = CleanupConfirmed_ && exitCode == 0 && status == QProcess::NormalExit && Failed_ == false;
        Q_EMIT Finished(confirmed);
    });
}

PlayProcess::~PlayProcess()
{
    // Normal close waits for confirmed cleanup before destroying this owner.
    // On unexpected teardown QProcess may emit signals from its destructor,
    // after our buffers have died. Retire callbacks while every member is alive.
    ReadyTimer_.stop();
    ReadyTimer_.disconnect(this);
    Process_.disconnect(this);
    Process_.closeWriteChannel();
}

bool PlayProcess::Start(PlayLaunch launch)
{
    if (Active_ || launch.Epoch == 0)
    {
        return false;
    }
    Active_ = true;
    Ready_ = StartSent_ = StopLatched_ = CloseSent_ = CleanupConfirmed_ = Failed_ = ConsumeScheduled_ = false;
    DiagnosticBytes_ = 0;
    Backlog_.clear();
    Epoch_ = QStringLiteral("%1").arg(launch.Epoch, 16, 16, QLatin1Char('0'));
    StartRequest_ = std::move(launch.StartRequest);
    Process_.setProgram(launch.Python);
    Process_.setArguments({QDir(launch.ToolingRoot).filePath(QStringLiteral("scripts/python/play_tool.py")),
                           QStringLiteral("--stdio"),
                           QStringLiteral("--tooling-root"),
                           launch.ToolingRoot});
    ReadyTimer_.start();
    Process_.start();
    return true;
}

bool PlayProcess::BeginSession(foundation::uint64 epoch, QJsonObject request)
{
    if (Active_ == false || Ready_ == false || Failed_ || epoch == 0 ||
        request.value(QStringLiteral("type")).toString() != QStringLiteral("start"))
    {
        return false;
    }
    Epoch_ = QStringLiteral("%1").arg(epoch, 16, 16, QLatin1Char('0'));
    CleanupConfirmed_ = false;
    StartSent_ = true;
    return Send(std::move(request));
}

bool PlayProcess::Send(QJsonObject request)
{
    if (Active_ == false || Ready_ == false || Failed_ || Process_.state() == QProcess::NotRunning)
    {
        return false;
    }
    request.insert(QStringLiteral("protocol"), 1);
    if (request.value(QStringLiteral("type")) != QStringLiteral("close"))
    {
        request.insert(QStringLiteral("epoch"), Epoch_);
    }
    const QByteArray frame = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    // The supervisor reserves Stop; reserve stdin capacity here too. Never
    // retry a mutation automatically if its outcome is uncertain.
    const bool stop =
        request.value(QStringLiteral("type")) == QStringLiteral("close") ||
        request.value(QStringLiteral("command")).toObject().value(QStringLiteral("command")) == QStringLiteral("Stop");
    if (static_cast<usize>(frame.size()) > kMaxLine ||
        static_cast<usize>(Process_.bytesToWrite()) + static_cast<usize>(frame.size()) >
            (stop ? kMaxLine : kMaxLine - 4096U))
    {
        Fail(QStringLiteral("Play command backpressure"));
        return false;
    }
    if (Process_.write(frame) != frame.size())
    {
        Fail(QStringLiteral("Could not queue a complete Play frame"));
        return false;
    }
    return true;
}

void PlayProcess::Close()
{
    StopLatched_ = true;
    if (Ready_ && CloseSent_ == false)
    {
        CloseSent_ = true;
        (void)Send({{QStringLiteral("type"), QStringLiteral("close")}});
    }
}

void PlayProcess::Read()
{
    while (Process_.bytesAvailable() > 0)
    {
        Backlog_.append(Process_.read(65536));
        if (static_cast<usize>(Backlog_.size()) > kMaxBacklog)
        {
            Fail(QStringLiteral("Play framing exceeded 1 MiB"));
            return;
        }
    }
    Consume();
}

void PlayProcess::Consume()
{
    ConsumeScheduled_ = false;
    for (int i = 0; i < 32 && Failed_ == false; ++i)
    {
        const auto newline = Backlog_.indexOf('\n');
        if (newline < 0)
        {
            if (static_cast<usize>(Backlog_.size()) > kMaxLine)
            {
                Fail(QStringLiteral("Play frame exceeded 256 KiB"));
            }
            return;
        }
        const QByteArray line = Backlog_.left(newline);
        Backlog_.remove(0, newline + 1);
        if (static_cast<usize>(line.size()) > kMaxLine)
        {
            Fail(QStringLiteral("Play frame exceeded 256 KiB"));
            return;
        }
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || document.isObject() == false)
        {
            Fail(QStringLiteral("Malformed Play frame"));
            return;
        }
        const auto event = document.object();
        if (event.value(QStringLiteral("protocol")).toInt(-1) != 1)
        {
            Fail(QStringLiteral("Unsupported Play protocol"));
            return;
        }
        const auto type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("ready"))
        {
            if (Ready_)
            {
                Fail(QStringLiteral("Duplicate Play ready"));
                return;
            }
            Ready_ = true;
            ReadyTimer_.stop();
            if (StopLatched_)
            {
                Close();
            }
            else
            {
                StartSent_ = true;
                (void)Send(StartRequest_);
            }
            continue;
        }
        const bool stoppedBeforeStart = StartSent_ == false && StopLatched_ && type == QStringLiteral("ended") &&
                                        event.value(QStringLiteral("epoch")) == QStringLiteral("0000000000000000");
        if (event.value(QStringLiteral("epoch")).toString() != Epoch_ && stoppedBeforeStart == false)
        {
            Fail(QStringLiteral("Play epoch mismatch"));
            return;
        }
        if (type == QStringLiteral("ended"))
        {
            CleanupConfirmed_ = event.value(QStringLiteral("cleanup_confirmed")).toBool();
        }
        if (type == QStringLiteral("fatal"))
        {
            CleanupConfirmed_ = false;
        }
        Q_EMIT Event(event);
        if (type == QStringLiteral("result"))
        {
            (void)Send({{QStringLiteral("type"), QStringLiteral("ack")},
                        {QStringLiteral("request"), event.value(QStringLiteral("request"))}});
        }
    }
    if (Backlog_.contains('\n') && ConsumeScheduled_ == false && Failed_ == false)
    {
        ConsumeScheduled_ = true;
        QTimer::singleShot(0, this, &PlayProcess::Consume);
    }
}

void PlayProcess::Fail(const QString& message)
{
    if (Failed_)
    {
        return;
    }
    // EOF asks the owner to stop its entire process group. We still require a
    // confirmed ended record; an adapter crash cannot be treated as cleanup.
    Failed_ = true;
    CleanupConfirmed_ = false;
    ReadyTimer_.stop();
    Process_.closeWriteChannel();
    Backlog_.clear();
    Q_EMIT Event({{QStringLiteral("type"), QStringLiteral("fatal")},
                  {QStringLiteral("epoch"), Epoch_},
                  {QStringLiteral("message"), message},
                  {QStringLiteral("cleanup_confirmed"), false}});
}
} // namespace ludus::editor
