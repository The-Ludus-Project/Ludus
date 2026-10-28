#pragma once

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
/// Result of a copy or recovery move; Path is preserved on failure, Error describes a failure.
struct ProjectFileResult final
{
    QString Path;
    QString Error;
};

/// Bounded authored-file operations, called on the UI thread after document-owner checks.
/// No operation overwrites existing destinations or follows project child symlinks.
class ProjectFiles final
{
public:
    /// Maximum bytes copied by one file or render-graph package operation.
    static constexpr foundation::int64 MAX_COPY_BYTES = foundation::int64{64} * 1024 * 1024;
    /// Maximum entries and nesting inspected in one package copy.
    static constexpr foundation::int32 MAX_COPY_ENTRIES = 4096;
    static constexpr foundation::int32 MAX_COPY_DEPTH = 12;

    /// True for an existing regular file inside root, without child symlink components.
    [[nodiscard]] static bool IsProjectFile(const QString& root, const QString& path);
    /// Create a new relative file. Parents must exist. Preserve destination on failure.
    [[nodiscard]] static bool
    Create(const QString& root, const QString& relative, const QByteArray& bytes, QString& outError);
    /// Copy a regular external source into an existing project folder, without overwrite.
    [[nodiscard]] static bool
    Add(const QString& root, const QString& source, const QString& relative, QString& outError);
    /// Copy a file beside itself, or its whole containing package for a render graph.
    /// name is a leaf file/folder name. Root-level graphs copy only the graph file.
    /// outResult.Path is set only on success. Relative references in single-file copies stay shared.
    [[nodiscard]] static bool Duplicate(const QString& root,
                                        const QString& source,
                                        const QString& name,
                                        bool shouldCopyPackage,
                                        ProjectFileResult& outResult);
    /// Move one file to a unique directory under .ludus-trash. References are never rewritten.
    /// outResult.Path receives the recoverable file's location only on success.
    [[nodiscard]] static bool Remove(const QString& root, const QString& source, ProjectFileResult& outResult);
};
} // namespace ludus::editor
