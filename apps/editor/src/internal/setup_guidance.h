#pragma once

// Plain-language text for project setup problems and results. The tools report a result code such as
// InvalidProject followed by a technical message; people should read what is wrong and what to do, never the
// code's name. Everything here is a pure function of text, so the Start Page, the setup banner and the status
// line all say the same thing and one test can prove no code name reaches the screen.

#include "internal/project_descriptor.h"
#include "internal/workspace.h"

#include <QString>

namespace ludus::editor
{
/// What the setup banner or Start Page says about one problem.
struct SetupGuidance final
{
    /// What is wrong, as one short sentence without an internal name, for example "The project's settings file
    /// can't be read."
    QString Headline;
    /// The technical reason in plain words, for example "'preset' must be linux-clang-debug"; empty when the
    /// tools gave none.
    QString Detail;
    /// The cause in everyday words when the tools' detail speaks in implementation terms ("presets and caches"), for
    /// example "Ludus or its build tools changed since this project was set up."; empty when the detail reads well.
    QString PlainCause;
    /// What the person can do next, for example "Repair the project, or fix its settings file."
    QString Fix;
    /// Whether Repair project is expected to help. Settings-file and version problems are not repairable.
    bool IsRepairable = true;
};

/// What, besides the controller's own answer, decides whether Repair project can start now.
struct RepairGate final
{
    /// Whether the controller lets a repair start now (its ProjectSetup capability).
    bool IsRepairAllowed = false;
    /// Whether a play session or a live code update owns the project.
    bool IsPlaySessionActive = false;
    /// Whether this is the browser editor, which cannot run local build tools.
    bool IsBrowser = false;
};

/// Why Repair project can't start right now, as one sentence that says what to do about it; empty when it can start.
/// A disabled button cannot explain itself, so the setup banner shows this beside it and in its tooltip.
[[nodiscard]] QString GetRepairBlocker(const WorkspaceState& state, const RepairGate& gate);

/// Everything the setup banner shows while a problem waits to be fixed.
struct SetupNoticeContent final
{
    /// What is wrong, as one short sentence, for example "This project's build setup is out of date."
    QString Headline;
    /// One line: the cause in plain words, then what pressing the action will do, or why it can't be pressed yet.
    QString Detail;
    /// The full explanation behind the headline and detail, including the tools' own message.
    QString Tooltip;
    /// "Repair project", or "Open Project Settings" for a problem repair cannot fix.
    QString ActionLabel;
    /// What pressing the action does, or why it is unavailable.
    QString ActionTooltip;
    /// Whether the action can be pressed now. Repair is enabled whenever it can start, and it is the primary action.
    bool IsActionEnabled = false;
    /// Whether the action opens Project Settings rather than repairing.
    bool IsOpeningSettings = false;
};

/// The setup banner for a problem as the controller stores it, given why repair can't start (empty when it can).
[[nodiscard]] SetupNoticeContent DescribeSetupNotice(const QString& problem, const QString& repairBlocker);

/// A short plain-language phrase for a result code: "Build tools need to be prepared" for MissingTools.
[[nodiscard]] QString DescribeResultCode(ResultCode code);

/// Guidance for a setup problem as the controller stores it: possibly several lines, possibly led by a result
/// code ("InvalidProject: ...") or a status prefix ("CMake setup needs attention: ..."). Only the first line is
/// summarised; the rest is the detail. Text with no code keeps its own first sentence as the headline.
[[nodiscard]] SetupGuidance DescribeSetupProblem(const QString& problem);

/// Text with every leading or embedded "<ResultCodeName>:" replaced by its plain phrase, so a message that
/// already reads well is kept and one that starts with a code name loses it.
[[nodiscard]] QString ReplaceResultCodeNames(const QString& text);

/// Whether text still names a result code the way the tools write it: a code name followed by a colon, or a
/// multi-word code name such as InvalidProject or BootstrapStale. Used to prove the interface is clean.
[[nodiscard]] bool ContainsResultCodeName(const QString& text);
} // namespace ludus::editor
