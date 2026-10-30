#pragma once

#include <ludus/foundation/base/types.h>

#include <QJsonObject>
#include <QString>

#if !defined(LUDUS_EDITOR_MAX_SHADER_ARTIFACTS) || !defined(LUDUS_EDITOR_MAX_SHADER_PIPELINES)
#    error "The editor CMake reads the render-graph build ceilings into these definitions."
#endif

namespace ludus::editor
{
/// Supported target artifacts per shader manifest: programs times target profiles (render_graph_build.py LIMITS).
inline constexpr qsizetype MAX_SHADER_ARTIFACTS = LUDUS_EDITOR_MAX_SHADER_ARTIFACTS;
/// Supported target pipelines per shader manifest: raster recipes times target profiles.
inline constexpr qsizetype MAX_SHADER_PIPELINES = LUDUS_EDITOR_MAX_SHADER_PIPELINES;

/// Remove budgets.artifacts and budgets.pipelines. Both counts follow from the manifest's programs, recipes and
/// profiles, so the build ignores these legacy fields; dropping them when the editor rewrites the budgets keeps
/// a stale value from being read as a limit.
inline void RemoveDerivedBudgets(QJsonObject& inoutBudgets)
{
    inoutBudgets.remove(QStringLiteral("artifacts"));
    inoutBudgets.remove(QStringLiteral("pipelines"));
}
} // namespace ludus::editor
