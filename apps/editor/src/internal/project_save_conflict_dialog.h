#pragma once

class QWidget;

namespace ludus::editor
{
struct LoadOutcome;
struct ProjectDescriptor;

// Inspect three descriptor values without editing any of them. Returns true only
// for an explicit Reload choice; the caller must load the descriptor again.
[[nodiscard]] bool ConfirmProjectConflictReload(QWidget& inoutParent,
                                                const ProjectDescriptor& baseline,
                                                const LoadOutcome& disk,
                                                const ProjectDescriptor& draft);
} // namespace ludus::editor
