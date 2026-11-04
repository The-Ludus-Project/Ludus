#pragma once

// Plain-language text for project setup problems and results. The tools report a result code such as
// InvalidProject followed by a technical message; people should read what is wrong and what to do, never the
// code's name. Everything here is a pure function of text, so the Start Page, the setup banner and the status
// line all say the same thing and one test can prove no code name reaches the screen.

#include "internal/project_descriptor.h"

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
    /// What the person can do next, for example "Repair the project, or fix its settings file."
    QString Fix;
    /// Whether Repair project is expected to help. Settings-file and version problems are not repairable.
    bool IsRepairable = true;
};

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
