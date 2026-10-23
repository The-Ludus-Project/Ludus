#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include "internal/code_document.h"
#include "internal/document_history.h"
#include "internal/render_graph_scene_query_budgets.h"
#include "internal/render_graph_source.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ludus::editor
{
/// Non-authoring cursor hint for the source transaction restored by Undo/Redo.
struct RenderGraphTextState final
{
    QString File;
    CodeTextSelection Selection;
};

/// Non-authoring selection restored by a structural edit's Undo/Redo on the GUI thread.
struct RenderGraphNodeSelection final
{
    QString File;
    QString Configuration;
    QString Node;
};

/// Complete authored render-graph package content: package-relative path to bytes.
struct RenderGraphSnapshot final
{
    QMap<QString, QByteArray> Files;
    RenderGraphTextState TextState;
    RenderGraphNodeSelection NodeSelection;
    /// History cursor hints do not participate in authored equality or serialization.
    [[nodiscard]] LUDUS_INLINE bool operator==(const RenderGraphSnapshot& other) const
    {
        return Files == other.Files;
    }
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
    /// Authoring limits mirrored from the shared render_graph.py format validator.
    static constexpr foundation::int32 MAX_CONFIGURATIONS = 8;
    static constexpr foundation::int32 MAX_NODES = 128;
    static constexpr foundation::int32 MAX_EDGES = 512;
    static constexpr foundation::int32 MAX_IDENTIFIER_LENGTH = 63;
    static constexpr foundation::int32 MAX_COORDINATE = 32768;

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
    /// Package-relative files in the requested source version.
    [[nodiscard]] QStringList GetFiles(RenderGraphSource source = RenderGraphSource::Draft) const;
    [[nodiscard]] QByteArray GetDraft(const QString& file) const;
    /// Borrow source bytes until the next document mutation; empty for missing files.
    [[nodiscard]] foundation::BorrowedView<const char> GetDraftView(const QString& file) const noexcept;
    [[nodiscard]] QByteArray GetSaved(const QString& file) const;
    [[nodiscard]] bool IsFileDirty(const QString& file) const;
    [[nodiscard]] bool IsDirty() const;
    /// Revision of the selected source version. Draft revisions increase with edits,
    /// undo and redo; Saved advances only after a complete save or external refresh.
    [[nodiscard]] LUDUS_INLINE foundation::uint64
    GetRevision(RenderGraphSource source = RenderGraphSource::Draft) const noexcept
    {
        return source == RenderGraphSource::Saved ? mSavedRevision : mHistory.Revision();
    }
    /// Parse a draft graph document; returns false for missing or non-object JSON.
    [[nodiscard]] bool GetGraph(const QString& file, QJsonObject& outGraph) const;

    /// Replace a node atomically, preserving connected port identities and types.
    /// On failure the draft/history is preserved; optional error names the affected endpoint and repair.
    [[nodiscard]] RenderGraphDocumentStatus ReplaceNode(const QString& file,
                                                        const QString& configuration,
                                                        const QJsonObject& node,
                                                        QString* outErrorOrNull = nullptr);
    /// Rename one node and its edge endpoints, configuration output sources and layout
    /// references in the owning configuration as one undoable draft edit. Names are unique
    /// ASCII graph identifiers; "input" is reserved. Equal names succeed without history.
    /// Failure preserves every source and history entry and describes the repair in outError.
    /// Other configurations, shader/program IDs and source files are retained. GUI thread only;
    /// Save publishes the edit. History carries before/after node selection hints.
    [[nodiscard]] RenderGraphDocumentStatus RenameNode(const QString& file,
                                                       const QString& configuration,
                                                       const QString& node,
                                                       const QString& name,
                                                       QString& outError);
    /// Apply a pipeline and create missing shader programs and source templates as one
    /// undoable draft edit, including the selected technique and recipe. Program placeholders receive generated names.
    /// Existing programs must match stage and source. Failure preserves all draft files; Save publishes new files
    /// without replacement.
    [[nodiscard]] RenderGraphDocumentStatus ApplyPipeline(const QString& file,
                                                          const QString& configuration,
                                                          const QJsonObject& node,
                                                          QString* outErrorOrNull = nullptr);
    /// Unpack full Scene wires touching a node into typed members as one undoable edit.
    /// Preserves the selected scene/data. Failure preserves all draft files. No disk writes.
    [[nodiscard]] RenderGraphDocumentStatus
    ExpandSceneBundle(const QString& file, const QString& configuration, const QString& node);
    /// Upgrade the package's shared ray technique to scene materials in one undoable edit.
    /// Preserves original sources/signatures, program IDs and admission budgets.
    /// The builder supplies compatibility adapters and the default payload without project helper files.
    /// Failure preserves draft/history and reports the required repair. No disk writes.
    [[nodiscard]] RenderGraphDocumentStatus EnableSceneMaterials(QString& outError);
    /// Read the shared scene-ray query budgets. Missing/ambiguous techniques, unsupported
    /// versions or invalid budget fields preserve outBudgets and return false. GUI thread only.
    [[nodiscard]] bool GetSceneQueryBudgets(RenderGraphSceneQueryBudgets& outBudgets) const;
    /// Update only the shared scene interface's query budgets in one undoable draft edit.
    /// All ray pipelines using that technique share the values. Save publishes the manifest.
    /// Out-of-range values, absent/ambiguous techniques and unsupported versions preserve
    /// all sources/history and describe the repair. Other interface metadata is retained.
    [[nodiscard]] RenderGraphDocumentStatus SetSceneQueryBudgets(const RenderGraphSceneQueryBudgets& budgets,
                                                                 QString& outError);
    /// Register complete pipelines from the root import closure in one undoable draft edit.
    /// Repairs older packages without changing shaders. Conflict/capacity failures preserve the draft.
    [[nodiscard]] RenderGraphDocumentStatus PrepareTechniques(QString& outError);
    /// Remove programs unused by every loaded graph/configuration/subgraph and manifest technique.
    /// Removes only their exclusive, unmentioned package-owned sources; shared files and
    /// conservative include/import matches survive. Unregistered files and techniques are retained.
    /// One undo step; Save publishes removals with disk conflict checks. Malformed reference
    /// documents or missing subgraphs preserve draft/history and output lists; outError explains why.
    /// Success reports manifest#program identities and package-relative removed sources; no-op
    /// success creates no history entry. GUI thread only; no disk writes.
    [[nodiscard]] RenderGraphDocumentStatus
    RemoveUnusedShaders(QStringList& outPrograms, QStringList& outSources, QString& outError);
    [[nodiscard]] RenderGraphDocumentStatus
    AddNode(const QString& file, const QString& configuration, const QJsonObject& node);
    /// Add or fill an empty library configuration from ReSTIR DI, including shaders/settings.
    /// For the root graph, create a new library and Subgraph node in parentConfiguration instead;
    /// an empty parent identity selects the root default. Existing files/nodes are never replaced.
    /// One undo step, no disk writes. Incompatible inputs/capacities preserve the complete draft.
    [[nodiscard]] RenderGraphDocumentStatus ApplyRestirTemplate(const QString& file,
                                                                const QString& configuration,
                                                                QString& outError,
                                                                const QString& parentConfiguration = {});
    /// Apply restir-di, visibility-buffer or visibility-secondary-rays to a library configuration,
    /// inserting a new library/Subgraph when file is the root. One undo step; no disk writes.
    /// Existing nonempty configurations, shader sources and incompatible ABIs are preserved on failure.
    [[nodiscard]] RenderGraphDocumentStatus ApplyConfigurationTemplate(const QString& file,
                                                                       const QString& configuration,
                                                                       const QString& templateIdentity,
                                                                       QString& outError,
                                                                       const QString& parentConfiguration = {});
    /// Add an empty named configuration to an existing file without changing its default.
    /// Unique ASCII identifiers are required. One undo step; failure preserves the draft.
    [[nodiscard]] RenderGraphDocumentStatus AddConfiguration(const QString& file, const QString& name);
    /// Remove a configuration and all its contents as one undoable draft edit. Rejects
    /// the last configuration and references from surviving loaded Subgraph nodes,
    /// naming the required repair in outError. Removing the default selects the first
    /// remaining configuration. Failure preserves sources/history; Save publishes.
    /// GUI thread only. History restores the removed/surviving configuration selection.
    [[nodiscard]] RenderGraphDocumentStatus
    RemoveConfiguration(const QString& file, const QString& configuration, QString& outError);
    /// Copy authored nodes, their internal edges and displayed layout into a bounded payload.
    /// External connections and configuration interfaces are excluded. outPayload survives failure;
    /// outError describes a failure. GUI thread only.
    [[nodiscard]] RenderGraphDocumentStatus CopyNodes(const QString& file,
                                                      const QString& configuration,
                                                      const QStringList& nodes,
                                                      const QVector<RenderGraphNodePosition>& positions,
                                                      QJsonObject& outPayload,
                                                      QString& outError) const;
    /// Paste nodes with unique IDs and remapped internal edges at a canvas position.
    /// References reuse destination assets; cross-package dependencies must match.
    /// One undo step, no disk writes. Failure preserves the draft and outNodes.
    [[nodiscard]] RenderGraphDocumentStatus PasteNodes(const QString& file,
                                                       const QString& configuration,
                                                       const QJsonObject& payload,
                                                       foundation::int32 x,
                                                       foundation::int32 y,
                                                       QStringList& outNodes,
                                                       QString& outError);
    /// Remove a group and its incident edges/output sources as one undo step.
    /// All IDs must exist; failure preserves the complete draft.
    [[nodiscard]] RenderGraphDocumentStatus
    RemoveNodes(const QString& file, const QString& configuration, const QStringList& nodes);
    /// Create name.lurender.json with an empty Default configuration and bind the named
    /// Subgraph node (or add it when absent). One undoable draft edit; Save publishes it.
    /// Names must be ASCII graph identifiers. Existing assets/files are never replaced.
    /// Failure preserves the draft. GUI thread only; package limits still apply.
    [[nodiscard]] RenderGraphDocumentStatus
    CreateSubgraph(const QString& file, const QString& configuration, const QString& node, const QString& name);
    /// Preview the same repair that ReplaceConfigurationInterface commits, without edits or writes.
    /// Explicit rename maps carry old ID -> new ID, independently in each direction; omitted
    /// maps treat names as identities and never guess renames from row order. Reports rewired
    /// links and cleared incompatible/missing uses across the loaded package. Failure preserves
    /// outRepairs and all draft/history bytes, and describes the reason in outError.
    [[nodiscard]] RenderGraphDocumentStatus
    PreviewConfigurationInterface(const QString& file,
                                  const QString& configuration,
                                  const QJsonArray& inputs,
                                  const QJsonArray& outputs,
                                  QStringList& outRepairs,
                                  QString& outError,
                                  const QMap<QString, QString>& inputRenames = {},
                                  const QMap<QString, QString>& outputRenames = {}) const;
    /// Replace a configuration interface and repair its internal links and every loaded caller
    /// atomically in one undo step. Renames preserve compatible uses; removed/type-changed ports
    /// disconnect only incompatible uses and clear affected configuration output sources while
    /// retaining their declarations. Candidate sources use original input identities when renamed.
    /// Invalid names/maps or explicitly incompatible new output sources reject without mutation.
    /// Empty output sources are incomplete drafts. Save publishes; GUI thread only.
    [[nodiscard]] RenderGraphDocumentStatus
    ReplaceConfigurationInterface(const QString& file,
                                  const QString& configuration,
                                  const QJsonArray& inputs,
                                  const QJsonArray& outputs,
                                  QString& outError,
                                  const QMap<QString, QString>& inputRenames = {},
                                  const QMap<QString, QString>& outputRenames = {});
    [[nodiscard]] RenderGraphDocumentStatus
    RemoveNode(const QString& file, const QString& configuration, const QString& node);
    /// Add an edge, or set the declared configuration output's source for a #outputs.port
    /// destination. Existing output sources must be disconnected first. One undoable edit;
    /// the canvas checks endpoint types before requesting it. Save publishes the draft.
    [[nodiscard]] RenderGraphDocumentStatus
    Connect(const QString& file, const QString& configuration, const QString& from, const QString& to);
    /// Connect an input (including #outputs.port), replacing its previous source in one
    /// undo step. The caller checks types/cycles first. Failure preserves draft/history;
    /// Save publishes. GUI thread only, with the same package limits as Connect.
    [[nodiscard]] RenderGraphDocumentStatus
    ReplaceConnection(const QString& file, const QString& configuration, const QString& from, const QString& to);
    /// Remove the exact edge, or clear a matching #outputs.port source while preserving
    /// the output declaration. Failure preserves the draft; success is one undoable edit.
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
    /// Apply a UTF-8 replacement against the exact draft revision. Preserves draft,
    /// history and cursor hints on rejection. Only adjacent text groups on the same
    /// file may merge; structural edits, Save and history navigation end a group.
    [[nodiscard]] CodeEditStatus ApplyTextEdit(const QString& file,
                                               foundation::uint64 expectedRevision,
                                               const CodeTextEdit& edit,
                                               CodeTextSelection before,
                                               CodeTextSelection after,
                                               bool shouldMerge);
    void BreakTextGroup() noexcept;
    [[nodiscard]] LUDUS_INLINE const RenderGraphTextState& GetHistoryTextState() const noexcept
    {
        return mDraft.TextState;
    }

    /// Selection hint from the current structural transaction; empty for other edits.
    /// The document owns the returned reference until its next mutation.
    [[nodiscard]] LUDUS_INLINE const RenderGraphNodeSelection& GetHistoryNodeSelection() const noexcept
    {
        return mDraft.NodeSelection;
    }

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

    /// Write the selected source version into an empty directory for a validation or build job.
    /// Saved snapshots never include unsaved edits. Failure may leave a partial snapshot;
    /// callers own its directory and must discard it on failure. GUI thread only.
    [[nodiscard]] RenderGraphDocumentStatus WriteSnapshot(const QString& directory,
                                                          RenderGraphSource source = RenderGraphSource::Draft) const;
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
    [[nodiscard]] RenderGraphDocumentStatus prepareConfigurationInterface(const QString& file,
                                                                          const QString& configuration,
                                                                          const QJsonArray& inputs,
                                                                          const QJsonArray& outputs,
                                                                          const QMap<QString, QString>& inputRenames,
                                                                          const QMap<QString, QString>& outputRenames,
                                                                          RenderGraphSnapshot& outSnapshot,
                                                                          QStringList& outRepairs,
                                                                          QString& outError) const;
    using EditFunction = bool (*)(QJsonObject& inoutConfiguration, const QStringList& arguments);
    [[nodiscard]] RenderGraphDocumentStatus editConfiguration(const QString& file,
                                                              const QString& configuration,
                                                              const QStringList& arguments,
                                                              EditFunction edit,
                                                              bool shouldMerge);
    [[nodiscard]] RenderGraphDocumentStatus commit(const RenderGraphSnapshot& candidate,
                                                   bool shouldMerge,
                                                   const RenderGraphTextState& beforeText = {},
                                                   const RenderGraphTextState& afterText = {},
                                                   const RenderGraphNodeSelection& beforeSelection = {},
                                                   const RenderGraphNodeSelection& afterSelection = {});
    [[nodiscard]] RenderGraphDocumentStatus createPackage(QStringList& outSavedFiles);
    void
    adopt(const QString& packageRoot, const QString& rootGraph, const RenderGraphSnapshot& snapshot, bool isBuiltIn);
    QString mPackageRoot;
    QString mRootGraph;
    RenderGraphSnapshot mSaved;
    foundation::uint64 mSavedRevision = 0;
    RenderGraphSnapshot mDraft;
    QString mLastMove;
    QString mLastTextFile;
    bool mIsBuiltIn = false;
    DocumentHistory<RenderGraphSnapshot, HISTORY_CAPACITY> mHistory;
};
} // namespace ludus::editor
