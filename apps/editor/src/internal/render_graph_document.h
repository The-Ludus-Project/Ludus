#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/document_history.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ludus::editor
{
/// Complete authored render-graph package content: package-relative path to bytes.
struct RenderGraphSnapshot final
{
    QMap<QString, QByteArray> Files;
    [[nodiscard]] bool operator==(const RenderGraphSnapshot& other) const = default;
};

/// Layout position of one node, in canvas units (presentation only; never compiled).
struct RenderGraphNodePosition final
{
    QString Node;
    foundation::int32 X = 0;
    foundation::int32 Y = 0;
};

enum class RenderGraphDocumentStatus : foundation::uint8
{
    Ok,
    NotFound,
    Capacity,
    Malformed,
    InvalidEdit,
    Conflict,
    Locked,
    WriteFailed,
};

/// Serialize a JSON object exactly like ludus_tools.render_graph.canonical_asset:
/// sorted keys, two-space indentation, ASCII escapes and a trailing newline. Saving
/// an editor draft therefore produces the same bytes as the shared CLI backend.
[[nodiscard]] QByteArray EncodeCanonicalJson(const QJsonObject& value);

// RG3 render-graph authoring document. It owns one package's saved and draft
// sources (graph documents, build manifests and shader functions), a bounded
// undo history and explicit save. Validation, lowering and builds stay in the
// shared CLI backend; this class only applies typed edits by stable IDs and
// writes immutable draft snapshots for those jobs. No Qt widgets; GUI thread.
// See docs/architecture/render-graph-authoring.md, "Editor interaction".
class RenderGraphDocument final
{
public:
    static constexpr foundation::usize MAX_FILES = 64;
    static constexpr qsizetype MAX_FILE_BYTES = qsizetype{128} * 1024;
    static constexpr qsizetype MAX_TOTAL_BYTES = qsizetype{4} * 1024 * 1024;
    static constexpr foundation::usize HISTORY_CAPACITY = 64;

    /// Read every authored source below packageRoot (bounded, no symlinks, no
    /// hidden or `out` directories). rootGraph is package-relative. Failure keeps
    /// the previous document.
    [[nodiscard]] RenderGraphDocumentStatus Load(const QString& packageRoot, const QString& rootGraph);
    /// Load a read-only package that is not part of the project yet, such as the editor's
    /// built-in default graph. sourceRoot may be a Qt resource directory and is never
    /// written. destinationRoot (absolute) is where Save creates the package; the package
    /// root reports it from the start, so builds and messages name the future location.
    /// Edits are ordinary undoable drafts; undoing them returns to the clean built-in.
    /// Failure keeps the previous document.
    [[nodiscard]] RenderGraphDocumentStatus
    LoadBuiltIn(const QString& sourceRoot, const QString& rootGraph, const QString& destinationRoot);
    /// True while the saved state is a built-in package that Save has not created yet.
    [[nodiscard]] LUDUS_INLINE bool IsBuiltIn() const noexcept
    {
        return mIsBuiltIn;
    }
    void Reset();
    [[nodiscard]] bool IsLoaded() const noexcept
    {
        return mPackageRoot.isEmpty() == false;
    }
    [[nodiscard]] const QString& GetPackageRoot() const noexcept
    {
        return mPackageRoot;
    }
    [[nodiscard]] const QString& GetRootGraph() const noexcept
    {
        return mRootGraph;
    }
    [[nodiscard]] QStringList GetFiles() const;
    [[nodiscard]] QByteArray GetDraft(const QString& file) const;
    [[nodiscard]] QByteArray GetSaved(const QString& file) const;
    [[nodiscard]] bool IsFileDirty(const QString& file) const;
    [[nodiscard]] bool IsDirty() const;
    /// Monotonic draft revision; increments on every committed edit, undo and redo.
    [[nodiscard]] foundation::uint64 GetRevision() const noexcept
    {
        return mHistory.Revision();
    }
    /// Parse a draft graph document; returns false for missing or non-object JSON.
    [[nodiscard]] bool GetGraph(const QString& file, QJsonObject& outGraph) const;

    [[nodiscard]] RenderGraphDocumentStatus
    ReplaceNode(const QString& file, const QString& configuration, const QJsonObject& node);
    /// Apply a pipeline and create missing shader programs and source templates as one
    /// undoable draft edit. Program placeholders receive generated names. Existing
    /// programs must match stage and source. Failure preserves all draft files; Save
    /// publishes new files without replacement. Non-raster stages remain authoring drafts.
    [[nodiscard]] RenderGraphDocumentStatus
    ApplyPipeline(const QString& file, const QString& configuration, const QJsonObject& node);
    [[nodiscard]] RenderGraphDocumentStatus
    AddNode(const QString& file, const QString& configuration, const QJsonObject& node);
    /// Create name.lurender.json with an empty Default configuration and bind the named
    /// Subgraph node (or add it when absent). One undoable draft edit; Save publishes it.
    /// Names must be ASCII graph identifiers. Existing assets/files are never replaced.
    /// Failure preserves the draft. GUI thread only; package limits still apply.
    [[nodiscard]] RenderGraphDocumentStatus
    CreateSubgraph(const QString& file, const QString& configuration, const QString& node, const QString& name);
    [[nodiscard]] RenderGraphDocumentStatus
    RemoveNode(const QString& file, const QString& configuration, const QString& node);
    [[nodiscard]] RenderGraphDocumentStatus
    Connect(const QString& file, const QString& configuration, const QString& from, const QString& to);
    [[nodiscard]] RenderGraphDocumentStatus
    Disconnect(const QString& file, const QString& configuration, const QString& from, const QString& to);
    /// Layout-only edit. Consecutive moves of the same node coalesce into one undo step.
    [[nodiscard]] RenderGraphDocumentStatus MoveNode(const QString& file,
                                                     const QString& configuration,
                                                     const QString& node,
                                                     foundation::int32 x,
                                                     foundation::int32 y);
    /// Layout-only edit of several nodes as one undo step, for one canvas drag gesture.
    /// Every node must exist; otherwise nothing changes. Never merges with another edit.
    [[nodiscard]] RenderGraphDocumentStatus
    MoveNodes(const QString& file, const QString& configuration, const QVector<RenderGraphNodePosition>& positions);
    /// Replace an existing source's draft bytes (shader function, build manifest or graph text).
    [[nodiscard]] RenderGraphDocumentStatus SetText(const QString& file, const QByteArray& bytes);

    [[nodiscard]] bool CanUndo() const noexcept
    {
        return mHistory.CanUndo();
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return mHistory.CanRedo();
    }
    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();

    /// Write the complete draft package into an empty directory for a validation or build job.
    [[nodiscard]] RenderGraphDocumentStatus WriteSnapshot(const QString& directory) const;
    /// Replace existing dirty files atomically, create new files without replacement, and
    /// remove saved files absent from the draft (Undo), after checking saved disk bytes.
    /// Conflict or Locked writes nothing. A write failure keeps remaining
    /// files dirty; outSavedFiles names completed writes/removals. New files use exclusive creation.
    /// For a built-in document, Save creates every file below the destination, even when
    /// nothing was edited, and then becomes an ordinary project package. Any existing file
    /// there is a Conflict; a failed creation removes the files it wrote and stays built-in.
    [[nodiscard]] RenderGraphDocumentStatus Save(QStringList& outSavedFiles);
    /// Read external saves using the package admission limits. Changed disk content is
    /// adopted only when the draft is clean, as one undoable revision. Conflict, read
    /// failure or capacity failure preserves the draft/history. Built-ins are unchanged.
    [[nodiscard]] RenderGraphDocumentStatus RefreshFromDisk(bool& outHasChanged);

private:
    using EditFunction = bool (*)(QJsonObject& inoutConfiguration, const QStringList& arguments);
    [[nodiscard]] RenderGraphDocumentStatus editConfiguration(const QString& file,
                                                              const QString& configuration,
                                                              const QStringList& arguments,
                                                              EditFunction edit,
                                                              bool shouldMerge);
    [[nodiscard]] RenderGraphDocumentStatus commit(const RenderGraphSnapshot& candidate, bool shouldMerge);
    [[nodiscard]] RenderGraphDocumentStatus createPackage(QStringList& outSavedFiles);
    void
    adopt(const QString& packageRoot, const QString& rootGraph, const RenderGraphSnapshot& snapshot, bool isBuiltIn);
    QString mPackageRoot;
    QString mRootGraph;
    RenderGraphSnapshot mSaved;
    RenderGraphSnapshot mDraft;
    QString mLastMove;
    bool mIsBuiltIn = false;
    DocumentHistory<RenderGraphSnapshot, HISTORY_CAPACITY> mHistory;
};
} // namespace ludus::editor
