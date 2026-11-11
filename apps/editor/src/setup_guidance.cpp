#include "internal/setup_guidance.h"

#include "internal/workspace.h"

#include <ludus/foundation/base/types.h>

#include <QRegularExpression>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
/// The last result code, so every code is visited once.
constexpr int32 LAST_RESULT_CODE = static_cast<int32>(ResultCode::CleanupUnknown);

/// The settings-file version of projects whose build the editor's tools prepare (version 1 predates CMake projects).
constexpr uint32 CMAKE_PROJECT_VERSION = 2;

/// How many capitals a name needs before it reads as an internal identifier rather than a word.
constexpr int32 IDENTIFIER_CAPITALS = 2;

[[nodiscard]] ResultCode GetResultCode(int32 index)
{
    return static_cast<ResultCode>(index);
}

[[nodiscard]] QString GetCodeName(ResultCode code)
{
    return QString::fromLatin1(ResultCodeName(code));
}

/// Prefixes the controller adds before a tool's message; they say where the problem came from, not what it is.
[[nodiscard]] QStringList GetStatusPrefixes()
{
    return {QStringLiteral("CMake setup needs attention: "),
            QStringLiteral("Project setup needs attention: "),
            QStringLiteral("CMake setup validation unavailable: ")};
}

[[nodiscard]] QString GetFix(ResultCode code)
{
    switch (code)
    {
        case ResultCode::InvalidProject:
            return QStringLiteral("Open Project Settings to correct the file, then check again.");
        case ResultCode::UnsupportedVersion:
            return QStringLiteral("Open the project with a newer Ludus, or ask its owner for a compatible copy.");
        case ResultCode::MissingDebugger:
            return QStringLiteral("Debugging needs a Linux x64 computer with the pinned debugger.");
        case ResultCode::Conflict:
            return QStringLiteral("Reload the project to pick up the file's current contents.");
        case ResultCode::Busy:
            return QStringLiteral("Wait for the running task to finish, or stop it.");
        case ResultCode::Ok:
        case ResultCode::DebuggerFailed:
        case ResultCode::MissingTools:
        case ResultCode::BootstrapStale:
        case ResultCode::ConfigureFailed:
        case ResultCode::TargetInvalid:
        case ResultCode::ReplyInvalid:
        case ResultCode::BuildFailed:
        case ResultCode::ArtifactInvalid:
        case ResultCode::SpawnFailed:
        case ResultCode::RuntimeFailed:
        case ResultCode::RuntimeSignaled:
        case ResultCode::ProtocolError:
        case ResultCode::Cancelled:
        case ResultCode::ReleaseFailed:
        case ResultCode::GenerationInvalid:
        case ResultCode::Superseded:
        case ResultCode::CleanupUnknown:
            break;
    }
    return QStringLiteral("Repair the project to prepare its tools and check that it builds.");
}

/// The cause in plain words for codes whose tool message speaks in implementation terms, such as "presets and caches";
/// empty when the tool's own message already reads well.
[[nodiscard]] QString GetPlainCause(ResultCode code)
{
    if (code == ResultCode::BootstrapStale)
    {
        return QStringLiteral("Ludus or its build tools changed since this project was set up.");
    }
    return QString();
}

[[nodiscard]] bool IsRepairable(ResultCode code)
{
    return code != ResultCode::InvalidProject && code != ResultCode::UnsupportedVersion &&
           code != ResultCode::MissingDebugger && code != ResultCode::Busy && code != ResultCode::Conflict;
}

/// The code a message starts with ("InvalidProject: ..."), and the text after it.
[[nodiscard]] bool TryStripLeadingCode(const QString& text, ResultCode& outCode, QString& outRemainder)
{
    for (int32 index = 0; index <= LAST_RESULT_CODE; ++index)
    {
        const QString prefix = GetCodeName(GetResultCode(index)) + QStringLiteral(": ");
        if (text.startsWith(prefix) == true)
        {
            outCode = GetResultCode(index);
            outRemainder = text.mid(prefix.size()).trimmed();
            return true;
        }
    }
    return false;
}
} // namespace

QString DescribeResultCode(ResultCode code)
{
    switch (code)
    {
        case ResultCode::Ok:
            return QStringLiteral("Done");
        case ResultCode::InvalidProject:
            return QStringLiteral("The project's settings file can't be read");
        case ResultCode::UnsupportedVersion:
            return QStringLiteral("The project needs a newer version of Ludus");
        case ResultCode::Conflict:
            return QStringLiteral("The project changed on disk");
        case ResultCode::MissingTools:
            return QStringLiteral("Build tools need to be prepared");
        case ResultCode::MissingDebugger:
            return QStringLiteral("The debugger isn't available");
        case ResultCode::DebuggerFailed:
            return QStringLiteral("The debugger stopped unexpectedly");
        case ResultCode::BootstrapStale:
            return QStringLiteral("This project's build setup is out of date");
        case ResultCode::Busy:
            return QStringLiteral("Another task is still running");
        case ResultCode::ConfigureFailed:
            return QStringLiteral("The project couldn't be configured");
        case ResultCode::TargetInvalid:
            return QStringLiteral("The selected program isn't available");
        case ResultCode::ReplyInvalid:
        case ResultCode::ProtocolError:
            return QStringLiteral("The build tools gave an unexpected answer");
        case ResultCode::BuildFailed:
            return QStringLiteral("The build didn't finish");
        case ResultCode::ArtifactInvalid:
            return QStringLiteral("The build produced something that can't be used");
        case ResultCode::SpawnFailed:
            return QStringLiteral("The program couldn't be started");
        case ResultCode::RuntimeFailed:
            return QStringLiteral("The program stopped with an error");
        case ResultCode::RuntimeSignaled:
            return QStringLiteral("The program crashed");
        case ResultCode::Cancelled:
            return QStringLiteral("Stopped");
        case ResultCode::ReleaseFailed:
            return QStringLiteral("The release couldn't be made");
        case ResultCode::GenerationInvalid:
            return QStringLiteral("The live code update can't be used");
        case ResultCode::Superseded:
            return QStringLiteral("A newer request replaced this one");
        case ResultCode::CleanupUnknown:
            return QStringLiteral("Cleanup couldn't be confirmed");
    }
    return QStringLiteral("Something went wrong");
}

SetupGuidance DescribeSetupProblem(const QString& problem)
{
    SetupGuidance guidance;
    QString summary = problem.section(QLatin1Char('\n'), 0, 0).trimmed();
    const QString laterLines = problem.section(QLatin1Char('\n'), 1).trimmed();
    for (const QString& prefix : GetStatusPrefixes())
    {
        if (summary.startsWith(prefix) == true)
        {
            summary = summary.mid(prefix.size()).trimmed();
        }
    }
    ResultCode code = ResultCode::Ok;
    QString remainder;
    if (TryStripLeadingCode(summary, code, remainder) == true)
    {
        guidance.Headline = DescribeResultCode(code) + QStringLiteral(".");
        guidance.Detail = remainder;
        guidance.PlainCause = GetPlainCause(code);
        guidance.Fix = GetFix(code);
        guidance.IsRepairable = IsRepairable(code);
    }
    else
    {
        guidance.Headline = summary.isEmpty() == true ? QStringLiteral("This project needs a quick repair.")
                                                      : ReplaceResultCodeNames(summary);
        guidance.Fix = GetFix(ResultCode::Ok);
    }
    if (laterLines.isEmpty() == false)
    {
        guidance.Detail = guidance.Detail.isEmpty() == true
                              ? ReplaceResultCodeNames(laterLines)
                              : guidance.Detail + QStringLiteral("\n") + ReplaceResultCodeNames(laterLines);
    }
    return guidance;
}

QString GetRepairBlocker(const WorkspaceState& state, const RepairGate& gate)
{
    if (gate.IsRepairAllowed == true)
    {
        return QString();
    }
    if (gate.IsBrowser == true)
    {
        return QStringLiteral("Repair needs the desktop editor: the browser can't run local build tools.");
    }
    if (state.OperationPhase == Phase::CleanupUnknown)
    {
        return QStringLiteral("Repair can start once the last task's cleanup is confirmed; see Copy Job Details.");
    }
    if (state.OperationPhase == Phase::Stopping)
    {
        return QStringLiteral("Repair can start once the current task has stopped.");
    }
    if (state.Busy() == true)
    {
        return QStringLiteral("Repair can start when the current task finishes. Wait for it, or stop it.");
    }
    if (gate.IsPlaySessionActive == true)
    {
        return QStringLiteral("Stop the play session to repair the project.");
    }
    if (state.Document != DocumentState::ProjectLoaded)
    {
        return QStringLiteral("Open the project to repair it.");
    }
    if (state.Dirty() == true)
    {
        return QStringLiteral("Save project settings first; repair works from the saved settings.");
    }
    if (state.Saved.ProviderKind != Provider::Cmake || state.Saved.Version != CMAKE_PROJECT_VERSION)
    {
        return QStringLiteral("Repair prepares CMake projects; this project's build is set up another way.");
    }
    return QStringLiteral("Repair can start in a moment, once the last task has finished closing.");
}

SetupNoticeContent DescribeSetupNotice(const QString& problem, const QString& repairBlocker)
{
    const SetupGuidance guidance = DescribeSetupProblem(problem);
    SetupNoticeContent content;
    content.Headline = guidance.Headline;
    const QString cause =
        guidance.PlainCause.isEmpty() == true ? guidance.Detail.section(QLatin1Char('\n'), 0, 0) : guidance.PlainCause;
    const auto withCause = [&cause](const QString& sentence) {
        return cause.isEmpty() == true ? sentence : cause + QStringLiteral(" ") + sentence;
    };
    if (guidance.IsRepairable == false)
    {
        content.ActionLabel = QStringLiteral("Open Project Settings");
        content.ActionTooltip = guidance.Fix;
        content.IsActionEnabled = true;
        content.IsOpeningSettings = true;
        content.Detail = withCause(guidance.Fix);
    }
    else
    {
        content.ActionLabel = QStringLiteral("Repair project");
        content.IsActionEnabled = repairBlocker.isEmpty() == true;
        content.ActionTooltip =
            content.IsActionEnabled == true
                ? QStringLiteral("Refreshes this project's build settings and caches for the current Ludus and "
                                 "tools, then builds and tests the project to confirm it works. It can take a few "
                                 "minutes the first time; you can keep working while it runs.")
                : repairBlocker;
        content.Detail = withCause(content.IsActionEnabled == true
                                       ? QStringLiteral("Repair project refreshes its build settings, then checks "
                                                        "that it builds.")
                                       : repairBlocker);
    }
    const QString toolDetail = guidance.Detail.isEmpty() == true ? ReplaceResultCodeNames(problem) : guidance.Detail;
    content.Tooltip =
        QStringLiteral("%1\n\nFrom the setup check: %2\n\n%3").arg(content.Headline, toolDetail, content.ActionTooltip);
    return content;
}

QString ReplaceResultCodeNames(const QString& text)
{
    QString replaced = text;
    for (int32 index = 0; index <= LAST_RESULT_CODE; ++index)
    {
        const ResultCode code = GetResultCode(index);
        replaced.replace(GetCodeName(code) + QStringLiteral(": "), DescribeResultCode(code) + QStringLiteral(": "));
    }
    return replaced;
}

bool ContainsResultCodeName(const QString& text)
{
    for (int32 index = 0; index <= LAST_RESULT_CODE; ++index)
    {
        const QString name = GetCodeName(GetResultCode(index));
        if (text.contains(name + QLatin1Char(':')) == true)
        {
            return true;
        }
        int32 capitals = 0;
        for (const QChar letter : name)
        {
            capitals += letter.isUpper() == true ? 1 : 0;
        }
        if (capitals >= IDENTIFIER_CAPITALS && text.contains(name) == true)
        {
            return true;
        }
    }
    return false;
}
} // namespace ludus::editor
