#include "internal/tool_process.h"

#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace ludus::editor
{
namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_EDITOR, "Editor");

// Combined UTF-8 text of one engine_pin report (editor_tool.ENGINE_PIN_MAX_BYTES).
constexpr usize MAX_ENGINE_PIN_TEXT_BYTES = usize{8} * 1024u;

// A protocol job id is a 16-character lowercase hex string so JSON number
// representation can never lose an integer (design section 6).
QString EncodeJob(uint64 job)
{
    return QString::fromLatin1(QByteArray::number(static_cast<qulonglong>(job), 16).rightJustified(16, '0'));
}

QString EncodeHex16(uint64 value)
{
    return EncodeJob(value);
}

std::optional<uint64> DecodeHex16(const QString& text)
{
    if (text.size() != 16)
    {
        return std::nullopt;
    }
    bool ok = false;
    const qulonglong value = text.toULongLong(&ok, 16);
    if (ok == false)
    {
        return std::nullopt;
    }
    return static_cast<uint64>(value);
}
} // namespace

ToolProcess::ToolProcess(QObject* parent) : QObject(parent)
{
    Process_.setProcessChannelMode(QProcess::SeparateChannels);
    ReadyTimer_.setSingleShot(true);
    ReadyTimer_.setInterval(ReadyTimeoutMs);

    connect(&Process_, &QProcess::started, this, &ToolProcess::OnStarted);
    connect(&Process_, &QProcess::readyReadStandardOutput, this, &ToolProcess::OnStdout);
    connect(&Process_, &QProcess::readyReadStandardError, this, &ToolProcess::OnStderr);
    connect(&Process_, &QProcess::finished, this, &ToolProcess::OnFinished);
    connect(&Process_, &QProcess::errorOccurred, this, &ToolProcess::OnErrorOccurred);
    connect(&ReadyTimer_, &QTimer::timeout, this, &ToolProcess::OnReadyTimeout);
}

ToolProcess::~ToolProcess()
{
    Process_.disconnect(this);
    ReadyTimer_.stop();
    ReadyTimer_.disconnect(this);
    // The destructor must not be used as cancellation. If a process somehow
    // outlives this object (it should not: the controller confirms cleanup
    // first), ask it to terminate without blocking the GUI thread.
    if (Process_.state() != QProcess::NotRunning)
    {
        Process_.terminate();
    }
}

bool ToolProcess::Start(const ToolLaunch& launch)
{
    if (Active_)
    {
        return false; // one request per process
    }
    Launch_ = launch;
    Job_ = launch.Job;
    Active_ = true;
    ReadyReceived_ = false;
    RequestSent_ = false;
    CancelLatched_ = false;
    TerminalDelivered_ = false;
    StdoutBacklog_.clear();
    StderrBytes_ = 0;
    OutputBytesSeen_ = 0;
    OutputBytesAcknowledged_ = 0;
    ControlBytesSeen_ = 0;

    // Launch the adapter with --stdio using the program/argument-list form so no
    // shell is involved and argument boundaries are preserved end to end.
    QStringList args;
    args << launch.AdapterPath << QStringLiteral("--stdio") << QStringLiteral("--tooling-root") << launch.ToolingRoot;
    Process_.setProgram(launch.PythonPath);
    Process_.setArguments(args);
    ReadyTimer_.start();
    Process_.start();
    return true;
}

void ToolProcess::OnStarted()
{
    // The adapter emits `ready` before any child spawn; we wait for it rather
    // than sending the request on `started` alone.
    LUDUS_LOG_TEXT(LOG_EDITOR, Info, "editor tool adapter process started");
}

void ToolProcess::SendRequest()
{
    if (RequestSent_)
    {
        return;
    }
    RequestSent_ = true;
    QJsonObject request;
    request.insert(QStringLiteral("protocol"), 1);
    request.insert(QStringLiteral("job"), EncodeJob(Job_));
    request.insert(QStringLiteral("operation"), QString::fromLatin1(ToolOperationName(Launch_.Operation)));
    request.insert(QStringLiteral("project"), Launch_.ProjectPath);
    request.insert(QStringLiteral("expected_sha256"), Launch_.ExpectedSha256);
    if (Launch_.Operation == ToolOperation::ProjectSetup || Launch_.Operation == ToolOperation::ProjectCreate)
    {
        request.insert(QStringLiteral("sdk"), Launch_.SetupSdk);
        request.insert(QStringLiteral("web_sdk"), Launch_.SetupWebSdk);
        request.insert(QStringLiteral("name"), Launch_.ProjectName);
        request.insert(QStringLiteral("prepare_engine"), Launch_.PrepareEngine);
        request.insert(QStringLiteral("disable_web"), Launch_.DisableWeb);
    }
    if (Launch_.Operation == ToolOperation::BuildDebug)
    {
        request.insert(QStringLiteral("debugger"), Launch_.DebuggerPath);
        request.insert(QStringLiteral("setup_debugger"), Launch_.SetupDebugger);
    }
    if (Launch_.Operation == ToolOperation::ReleaseInit)
    {
        request.insert(QStringLiteral("platform"), Launch_.ReleasePlatform);
        request.insert(QStringLiteral("itch_target"), Launch_.ItchTarget);
    }
    if (Launch_.Operation == ToolOperation::BuildRun && Launch_.HostedWindowAddress.isEmpty() == false)
    {
        request.insert(QStringLiteral("hosted_window"), Launch_.HostedWindowAddress);
    }
    if (Launch_.Operation == ToolOperation::Package)
    {
        request.insert(QStringLiteral("profile"), Launch_.ReleaseProfile);
        request.insert(QStringLiteral("version"), Launch_.ReleaseVersion);
        request.insert(QStringLiteral("sdk"), Launch_.ReleaseSdk);
    }
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    Process_.write(line);
}

void ToolProcess::SendControl(const QByteArray& jsonLine)
{
    if (Process_.state() == QProcess::NotRunning)
    {
        return;
    }
    Process_.write(jsonLine);
}

void ToolProcess::Cancel()
{
    // Idempotent: latch once, write one cancel message. Never retries anything.
    if (CancelLatched_)
    {
        return;
    }
    CancelLatched_ = true;
    if (ReadyReceived_)
    {
        SendCancellation();
    }
}

void ToolProcess::SendCancellation()
{
    QJsonObject cancel;
    cancel.insert(QStringLiteral("protocol"), 1);
    cancel.insert(QStringLiteral("job"), EncodeJob(Job_));
    cancel.insert(QStringLiteral("type"), QStringLiteral("cancel"));
    SendControl(QJsonDocument(cancel).toJson(QJsonDocument::Compact) + '\n');
}

void ToolProcess::AcknowledgeCredit()
{
    // Acknowledge the latest consumed output boundary so the adapter can refill
    // the fixed credit window. Control events do not consume credit.
    if (OutputBytesAcknowledged_ == OutputBytesSeen_)
    {
        return;
    }
    OutputBytesAcknowledged_ = OutputBytesSeen_;
    QJsonObject credit;
    credit.insert(QStringLiteral("protocol"), 1);
    credit.insert(QStringLiteral("job"), EncodeJob(Job_));
    credit.insert(QStringLiteral("type"), QStringLiteral("credit"));
    credit.insert(QStringLiteral("through"), EncodeHex16(OutputBytesAcknowledged_));
    SendControl(QJsonDocument(credit).toJson(QJsonDocument::Compact) + '\n');
}

void ToolProcess::OnStdout()
{
    // Read bounded raw bytes and append to the undecoded backlog. Framing is
    // line-based; a line is one JSON object terminated by '\n'.
    StdoutBacklog_.append(Process_.readAllStandardOutput());
    if (static_cast<usize>(StdoutBacklog_.size()) > MaxFramingBacklog)
    {
        FailProtocol(QStringLiteral("protocol framing backlog exceeded 2 MiB"));
        return;
    }
    ConsumeFrames();
}

void ToolProcess::OnStderr()
{
    // Adapter diagnostics are bounded; excess is a protocol diagnosis.
    const QByteArray chunk = Process_.readAllStandardError();
    StderrBytes_ += static_cast<usize>(chunk.size());
    if (StderrBytes_ > usize{64} * 1024u)
    {
        FailProtocol(QStringLiteral("adapter diagnostics exceeded 64 KiB"));
    }
}

void ToolProcess::ConsumeFrames()
{
    auto newline = StdoutBacklog_.indexOf('\n');
    while (newline >= 0)
    {
        const QByteArray line = StdoutBacklog_.left(newline);
        StdoutBacklog_.remove(0, newline + 1);
        if (static_cast<usize>(line.size()) > MaxProtocolLine)
        {
            FailProtocol(QStringLiteral("protocol line exceeded 256 KiB"));
            return;
        }
        if (line.trimmed().isEmpty() == false)
        {
            if (DispatchFrame(line) == false)
            {
                return; // protocol error already reported
            }
        }
        if (Active_ == false)
        {
            return;
        }
        newline = StdoutBacklog_.indexOf('\n');
    }
}

bool ToolProcess::DispatchFrame(const QByteArray& line)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        FailProtocol(QStringLiteral("malformed protocol frame"));
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("protocol")).toInt(-1) != 1)
    {
        FailProtocol(QStringLiteral("unsupported protocol version"));
        return false;
    }
    const QString type = object.value(QStringLiteral("type")).toString();

    // `ready` has protocol/type only; it does not echo the job.
    if (type == QStringLiteral("ready"))
    {
        if (ReadyReceived_)
        {
            FailProtocol(QStringLiteral("duplicate ready"));
            return false;
        }
        ReadyReceived_ = true;
        ReadyTimer_.stop();
        SendRequest();
        if (CancelLatched_)
        {
            SendCancellation();
        }
        ProtocolEvent event;
        event.Kind = ProtocolEvent::Type::Ready;
        event.Job = Job_;
        Q_EMIT Event(event);
        return true;
    }

    // Every other event echoes the job; a mismatch is a protocol error.
    const std::optional<uint64> job = DecodeHex16(object.value(QStringLiteral("job")).toString());
    if (job.has_value() == false || job.value() != Job_)
    {
        FailProtocol(QStringLiteral("protocol job mismatch"));
        return false;
    }

    // Bound aggregate control-event size (reserving 4 KiB for the result).
    if (type != QStringLiteral("output"))
    {
        ControlBytesSeen_ += static_cast<uint64>(line.size()) + 1; // + newline
        if (ControlBytesSeen_ > uint64{1} * 1024u * 1024u)
        {
            FailProtocol(QStringLiteral("aggregate control output exceeded 1 MiB"));
            return false;
        }
    }

    ProtocolEvent event;
    event.Job = Job_;
    event.Stage = object.value(QStringLiteral("stage")).toString();

    if (type == QStringLiteral("phase"))
    {
        event.Kind = ProtocolEvent::Type::Phase;
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("command"))
    {
        event.Kind = ProtocolEvent::Type::Command;
        for (const auto& value : object.value(QStringLiteral("argv")).toArray())
        {
            event.Argv.append(value.toString());
        }
        event.Cwd = object.value(QStringLiteral("cwd")).toString();
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("targets"))
    {
        event.Kind = ProtocolEvent::Type::Targets;
        for (const auto& value : object.value(QStringLiteral("targets")).toArray())
        {
            event.Targets.append(value.toString());
        }
        event.Preset = object.value(QStringLiteral("preset")).toString();
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("output"))
    {
        event.Kind = ProtocolEvent::Type::Output;
        event.Stream = object.value(QStringLiteral("stream")).toString() == QStringLiteral("stderr")
                           ? ProtocolEvent::OutputStreamTag::Stderr
                           : ProtocolEvent::OutputStreamTag::Stdout;
        event.Text = object.value(QStringLiteral("text")).toString();
        // Each output frame consumes its complete encoded byte count including
        // the newline; end_offset is the cumulative encoded-output total.
        const std::optional<uint64> endOffset = DecodeHex16(object.value(QStringLiteral("end_offset")).toString());
        if (endOffset.has_value() == false || endOffset.value() < OutputBytesSeen_)
        {
            FailProtocol(QStringLiteral("non-monotonic output end_offset"));
            return false;
        }
        event.EndOffset = endOffset.value();
        OutputBytesSeen_ = endOffset.value();
        Q_EMIT Event(event);
        AcknowledgeCredit();
        return true;
    }
    if (type == QStringLiteral("setup_details"))
    {
        event.Text = object.value(QStringLiteral("text")).toString();
        if ((Launch_.Operation != ToolOperation::ProjectCheck && Launch_.Operation != ToolOperation::ProjectSetup) ||
            event.Text.isEmpty() || static_cast<usize>(event.Text.toUtf8().size()) > usize{16} * 1024u ||
            TerminalDelivered_)
        {
            FailProtocol(QStringLiteral("invalid setup details"));
            return false;
        }
        event.Kind = ProtocolEvent::Type::SetupDetails;
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("engine_pin"))
    {
        // The selected engine install versus this editor's checkout; only a
        // setup check may report it, and only before its terminal result.
        const std::optional<EnginePinState> state =
            ParseEnginePinState(object.value(QStringLiteral("state")).toString());
        event.Pin.Installation = object.value(QStringLiteral("installation")).toString();
        event.Pin.PinnedRevision = object.value(QStringLiteral("pinned_revision")).toString();
        event.Pin.EditorRevision = object.value(QStringLiteral("editor_revision")).toString();
        event.Pin.Message = object.value(QStringLiteral("message")).toString();
        event.Pin.IsEditorOwned = object.value(QStringLiteral("editor_owned")).toBool();
        const usize textBytes = static_cast<usize>(event.Pin.Installation.toUtf8().size()) +
                                static_cast<usize>(event.Pin.PinnedRevision.toUtf8().size()) +
                                static_cast<usize>(event.Pin.EditorRevision.toUtf8().size()) +
                                static_cast<usize>(event.Pin.Message.toUtf8().size());
        if (Launch_.Operation != ToolOperation::ProjectCheck || state.has_value() == false ||
            event.Pin.Message.isEmpty() || textBytes > MAX_ENGINE_PIN_TEXT_BYTES || TerminalDelivered_)
        {
            FailProtocol(QStringLiteral("invalid engine pin report"));
            return false;
        }
        event.Pin.State = state.value();
        event.Kind = ProtocolEvent::Type::EnginePin;
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("generation"))
    {
        event.Kind = ProtocolEvent::Type::Generation;
        event.GenerationPath = object.value(QStringLiteral("path")).toString();
        if (Launch_.Operation != ToolOperation::BuildGeneration || event.GenerationPath.isEmpty())
        {
            FailProtocol(QStringLiteral("unexpected generation publication"));
            return false;
        }
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("runtime_started"))
    {
        if (Launch_.Operation == ToolOperation::BuildDebug)
        {
            FailProtocol(QStringLiteral("a debugger session cannot confirm game runtime state"));
            return false;
        }
        event.Kind = ProtocolEvent::Type::RuntimeStarted;
        event.Pid = static_cast<qint64>(object.value(QStringLiteral("pid")).toDouble());
        event.Executable = object.value(QStringLiteral("executable")).toString();
        event.Cwd = object.value(QStringLiteral("cwd")).toString();
        for (const auto& value : object.value(QStringLiteral("args")).toArray())
        {
            event.Argv.append(value.toString());
        }
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("debugger_started"))
    {
        if (Launch_.Operation != ToolOperation::BuildDebug)
        {
            FailProtocol(QStringLiteral("unexpected debugger session"));
            return false;
        }
        const auto rawPid = object.value(QStringLiteral("pid")).toDouble(-1);
        event.Executable = object.value(QStringLiteral("executable")).toString();
        if (rawPid < 1 || rawPid > 2147483647 ||
            rawPid != static_cast<foundation::float64>(static_cast<qint64>(rawPid)) || event.Executable.isEmpty())
        {
            FailProtocol(QStringLiteral("invalid debugger session identity"));
            return false;
        }
        event.Kind = ProtocolEvent::Type::DebuggerStarted;
        event.Pid = static_cast<qint64>(rawPid);
        Q_EMIT Event(event);
        return true;
    }
    if (type == QStringLiteral("result"))
    {
        event.Kind = ProtocolEvent::Type::Result;
        event.Outcome = object.value(QStringLiteral("outcome")).toString();
        event.Message = object.value(QStringLiteral("message")).toString();
        event.CleanupConfirmed = object.value(QStringLiteral("cleanup_confirmed")).toBool(false);
        const QJsonValue exitValue = object.value(QStringLiteral("exit_code"));
        if (exitValue.isDouble())
        {
            event.ExitCode = static_cast<foundation::int32>(exitValue.toInt());
        }
        const QJsonValue signalValue = object.value(QStringLiteral("signal"));
        if (signalValue.isDouble())
        {
            event.Signal = static_cast<foundation::int32>(signalValue.toInt());
        }
        // The named result code maps to the stable ResultCode vocabulary.
        const QString codeName = object.value(QStringLiteral("code")).toString();
        event.Code = ResultCode::ProtocolError;
        for (foundation::uint8 i = 0; i <= static_cast<foundation::uint8>(ResultCode::CleanupUnknown); ++i)
        {
            if (codeName == QString::fromLatin1(ResultCodeName(static_cast<ResultCode>(i))))
            {
                event.Code = static_cast<ResultCode>(i);
                break;
            }
        }
        TerminalDelivered_ = true;
        Q_EMIT Event(event);
        return true;
    }

    // Unknown event types are rejected in version 1.
    FailProtocol(QStringLiteral("unknown protocol event '%1'").arg(type));
    return false;
}

void ToolProcess::FailProtocol(const QString& message)
{
    if (Active_ == false)
    {
        return;
    }
    LUDUS_LOG_TEXT(LOG_EDITOR, Error, "editor protocol error");
    ProtocolEvent event;
    event.Kind = ProtocolEvent::Type::Error;
    event.Job = Job_;
    event.Code = ResultCode::ProtocolError;
    event.Message = message;
    Q_EMIT Event(event);
    // Orderly cleanup: ask the adapter to stop and drop the stream.
    Cancel();
    StdoutBacklog_.clear();
}

void ToolProcess::OnReadyTimeout()
{
    if (ReadyReceived_)
    {
        return;
    }
    FailProtocol(QStringLiteral("adapter did not report ready within 5 seconds"));
}

void ToolProcess::OnErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart)
    {
        ProtocolEvent event;
        event.Kind = ProtocolEvent::Type::Error;
        event.Job = Job_;
        event.Code = ResultCode::SpawnFailed;
        event.Message = QStringLiteral("failed to start the editor tool adapter");
        Q_EMIT Event(event);
        Finish();
    }
}

void ToolProcess::OnFinished(int exitCode, QProcess::ExitStatus status)
{
    // Drain any remaining buffered output before concluding.
    StdoutBacklog_.append(Process_.readAllStandardOutput());
    ConsumeFrames();

    // A result is provisional until the process finishes AND output drained.
    // Missing result or abnormal bridge exit is never success.
    if (TerminalDelivered_ == false)
    {
        ProtocolEvent event;
        event.Kind = ProtocolEvent::Type::Error;
        event.Job = Job_;
        event.Code = ResultCode::CleanupUnknown;
        if (status == QProcess::CrashExit)
        {
            event.Message = QStringLiteral("adapter supervisor terminated without a terminal result");
        }
        else
        {
            event.Message = QStringLiteral("adapter exited (code %1) without a terminal result").arg(exitCode);
        }
        event.CleanupConfirmed = false;
        Q_EMIT Event(event);
    }
    Finish();
}

void ToolProcess::Finish()
{
    Active_ = false;
    ReadyTimer_.stop();
    Q_EMIT Finished();
}

} // namespace ludus::editor
