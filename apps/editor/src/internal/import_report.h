#pragma once

// Text summaries of scene import previews and stored import reports, shared by
// the Level and World workspaces. Pure functions over the JSON that
// Ludus::SceneCook returns, so they are tested without a project or a GPU.

#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
/// A picked scene source split into what Ludus::SceneCook expects.
struct ImportSource final
{
    /// Absolute source directory.
    QString Directory;
    /// Scene entry file name inside Directory; empty selects the reference profile.
    QString Entry;
};

/// Split a picked path: a directory keeps pbrt-reference-v1 (empty entry); a scene
/// file (.pbrt, .gltf, .glb or .obj) selects its directory and file name, routed by
/// AssetOptions::SourceEntry to the importer its content selects.
[[nodiscard]] ImportSource SplitImportSource(const QString& path);

/// Multi-line preview of a PreviewImport response: profile, source and cooked
/// counts, budget use and fallbacks.
[[nodiscard]] QString DescribeImportPreview(const QJsonObject& response);

/// Multi-line summary of one stored or previewed import report (either profile).
[[nodiscard]] QString DescribeImportReport(const QJsonObject& report);

/// Summaries of a response's import_reports member, one block per import; empty
/// when the level has no best-effort imports.
[[nodiscard]] QString DescribeImportReports(const QJsonObject& response);
} // namespace ludus::editor
