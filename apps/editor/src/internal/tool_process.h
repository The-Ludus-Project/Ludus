#pragma once

// ToolProcess: owns one QProcess running the Python editor_tool adapter and the
// private version-1 JSON-lines control protocol (design.md sections 8, 10).
//
// Private editor header (not installed). It uses only QProcess's asynchronous
// surface (started/readyRead/finished/errorOccurred) and timers: no
// waitForStarted/waitForFinished, no startDetached, and the destructor is never
// used as a cancellation mechanism. One request per process; cancellation and
// output-credit are sent on the still-open stdin pipe.

#include "internal/engine_pin.h"
#include "internal/project_descriptor.h"

#include <ludus/foundation/base/types.h>

#include <QObject>
#if !defined(Q_OS_WASM)
#    include <QProcess>
#endif
#include <QString>
#include <QStringList>
#include <QTimer>

#include <optional>

namespace ludus::editor
{
using foundation::uint64;
using foundation::usize;

// The operation a job performs. Matches the protocol `operation` field.
enum class ToolOperation : foundation::uint8
{
    Configure,
    Build,
    BuildRun,
    BuildDebug,
    ProjectCheck,
    ProjectSetup,
    ProjectCreate,
    ReleaseInit,
    Package,
    BuildGeneration,
    CookScripts,
    InspectSetup,
};

[[nodiscard]] const char* ToolOperationName(ToolOperation op) noexcept;

// Parsed protocol events surfaced to the controller. Raw bytes never reach the
// controller; ToolProcess owns framing/decoding/bounds.
struct ProtocolEvent
{
    enum class Type : foundation::uint8
    {
        Ready,
        Phase,
        Command,
        Targets,
        Output,
        RuntimeStarted,
        DebuggerStarted,
        Generation,
        SetupDetails,
        EnginePin,
        Result,
        Error, // local framing/protocol error (not from the adapter)
    };

    enum class OutputStreamTag : foundation::uint8
    {
        Stdout,
        Stderr,
    };

    Type Kind = Type::Error;
    uint64 Job = 0;
    QString Stage;
    // Command
    QStringList Argv;
    QString Cwd;
    // Targets
    QStringList Targets;
    QString Preset;
    // Output
    OutputStreamTag Stream = OutputStreamTag::Stdout;
    QString Text;
    uint64 EndOffset = 0;
    // RuntimeStarted
    qint64 Pid = 0;
    QString Executable;
    QString GenerationPath;
    // EnginePin
    EnginePinStatus Pin;
    // Result
    QString Outcome; // success/failed/cancelled
    ResultCode Code = ResultCode::Ok;
    QString Message;
    bool CleanupConfirmed = false;
    std::optional<foundation::int32> ExitCode;
    std::optional<foundation::int32> Signal;
};

// Launch parameters for a single tool run.
struct ToolLaunch
{
    QString PythonPath;     // absolute managed interpreter
    QString AdapterPath;    // absolute editor_tool.py in the trusted tooling root
    QString ToolingRoot;    // absolute tooling checkout
    QString ProjectPath;    // absolute descriptor path
    QString ExpectedSha256; // digest of the clean saved descriptor
    ToolOperation Operation = ToolOperation::Configure;
    QString DebuggerPath;
    bool SetupDebugger = false;
    QString SetupSdk;
    QString SetupWebSdk;
    QString ProjectName;
    bool PrepareEngine = false;
    bool DisableWeb = false;
    /// ProjectCreate only: also generate the GitHub Actions build/test workflow.
    bool ShouldAddGitHubCi = false;
    QString ReleasePlatform;
    QString ItchTarget;
    QString ReleaseProfile;
    QString ReleaseVersion;
    QString ReleaseSdk;
    /// BuildRun only: the game's hosted-window channel address (Play in the editor viewport).
    /// Empty launches the game in its own window. Sent only to the adapter, never logged.
    QString HostedWindowAddress;
    uint64 Job = 0; // 16-hex-encoded on the wire
};

class ToolProcess : public QObject
{
    Q_OBJECT
public:
    explicit ToolProcess(QObject* parent = nullptr);
    ~ToolProcess() override;

    // Fixed protocol/credit bounds (design section 10).
    static constexpr usize OutputCreditWindow = usize{256} * 1024u; // encoded bytes
    static constexpr usize MaxProtocolLine = usize{256} * 1024u;    // including framing
    static constexpr usize MaxFramingBacklog = usize{2} * 1024u * 1024u;
    static constexpr int ReadyTimeoutMs = 5000;

    // Start the adapter and send exactly one request after it reports ready.
    // Returns false if a process is already active (one request per process).
    bool Start(const ToolLaunch& launch);

    // Latch cancellation and write a single idempotent cancel control message on
    // the open stdin pipe. Safe to call repeatedly. Never retries build/run.
    void Cancel();

    // True from Start() until the terminal result is delivered and the process
    // has finished and streams are drained.
    [[nodiscard]] bool Active() const noexcept
    {
        return Active_;
    }

    [[nodiscard]] uint64 Job() const noexcept
    {
        return Job_;
    }

Q_SIGNALS:
    // One parsed event. Controller binds this to its QObject context and tags
    // handling with the job id.
    void Event(const ProtocolEvent& event);
    void Finished();

#if !defined(Q_OS_WASM)
private Q_SLOTS:
    void OnStarted();
    void OnStdout();
    void OnStderr();
    void OnFinished(int exitCode, QProcess::ExitStatus status);
    void OnErrorOccurred(QProcess::ProcessError error);
    void OnReadyTimeout();

private:
    void SendRequest();
    void SendCancellation();
    void SendControl(const QByteArray& jsonLine);
    void ConsumeFrames();
    bool DispatchFrame(const QByteArray& line);
    void FailProtocol(const QString& message);
    void AcknowledgeCredit();
    void Finish();

    QProcess Process_;
    QTimer ReadyTimer_;
    ToolLaunch Launch_;
    uint64 Job_ = 0;
#else
private:
    uint64 Job_ = 0;
#endif

    bool Active_ = false;
    bool ReadyReceived_ = false;
    bool RequestSent_ = false;
    bool CancelLatched_ = false;
    bool TerminalDelivered_ = false;

    QByteArray StdoutBacklog_; // undecoded protocol stream (JSON lines)
    usize StderrBytes_ = 0;    // bounded adapter diagnostics counter

    // Output-credit accounting as nonwrapping totals (design section 8).
    uint64 OutputBytesSeen_ = 0;         // cumulative encoded output bytes seen
    uint64 OutputBytesAcknowledged_ = 0; // last acknowledged boundary
    uint64 ControlBytesSeen_ = 0;        // aggregate control-event bytes (<= 1 MiB)
};

} // namespace ludus::editor
