#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_view_state.h"

#include <QList>
#include <QString>
#include <QToolBar>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAction;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphCanvas;
class RenderGraphDocument;
class RenderGraphHistoryRecordCache;

/// One breadcrumb location. Instance names the Subgraph node in the preceding location;
/// an empty Instance starts a direct path. Saved views are independent of document undo.
struct RenderGraphHistoryLocation final
{
    QString File;              ///< Package-relative graph filename.
    QString Configuration;     ///< Authored configuration identity in File.
    QString Instance;          ///< Caller node identity in the preceding breadcrumb; empty at a direct root.
    RenderGraphViewState View; ///< Saved view when this location was left.
    bool HasView = false;      ///< True after a view has been captured for this visit.
};

/// Per-package browser history over canvas configurations and Subgraph instance paths.
/// Owns copied snapshots, never graph edits or scene items. GUI thread only. The document
/// and canvas must outlive this canvas-owned toolbar. See
/// docs/development/render-graph-authoring.md#navigate-subgraphs-with-breadcrumbs-and-history.
class RenderGraphHistory final : public QToolBar
{
    Q_OBJECT
public:
    /// Maximum retained visits; oldest entries are evicted without changing document history.
    static constexpr foundation::int32 MAX_ENTRIES = 64;
    /// Maximum nesting of a breadcrumb path, including its root; malformed recursive graphs stay bounded.
    static constexpr foundation::int32 MAX_PATH_DEPTH = 32;

    RenderGraphHistory(const RenderGraphDocument& document, RenderGraphCanvas& inoutCanvas);
    /// Clear locations and pending requests after a package/project is replaced. No graph changes.
    void Reset();
    /// Capture the old view before ShowConfiguration and plan an ordinary visit. Repeated renders
    /// of the same configuration do not add entries. Empty/invalid configurations clear pending state.
    void BeforeShow(const QString& file, const QString& configuration);
    /// Complete the planned visit after the canvas has rebuilt and selected its authored nodes.
    void AfterShow();
    /// Validate a Subgraph instance before the workspace changes configuration. False leaves
    /// the current location intact and reports a NavigationFailed reason. Used by canvas and tree entry.
    [[nodiscard]] bool PrepareSubgraph(const QString& instance, const QString& file, const QString& configuration);
    /// Request a new view visit for the next valid ShowConfiguration, including a jump within
    /// the same configuration. Captures the prior selection/pan/zoom/trace, preserves a current
    /// instance path for same-configuration jumps, and leaves document undo unchanged.
    void PrepareDiagnosticVisit();
    /// Restore an earlier/later valid visit; false when no such target remains.
    [[nodiscard]] bool Back();
    [[nodiscard]] bool Forward();
    /// Visit an ancestor in the current instance path, restoring that ancestor's saved view.
    /// Invalid/current indices are no-ops. A successful jump is a new visit and discards Forward history.
    [[nodiscard]] bool VisitAncestor(foundation::int32 index);
    /// Visit an exact, current caller path and reveal an authored node in one browser-history step.
    /// Invalid/missing/changed paths fail before navigation; document undo and layout are untouched.
    [[nodiscard]] bool VisitNodePath(const QVector<RenderGraphHistoryLocation>& path, const QString& node);
    /// Current instance path, or empty when nothing is shown. Copy is independent of later navigation.
    [[nodiscard]] QVector<RenderGraphHistoryLocation> GetPath() const;
    /// Retained visit count, for accessibility and bounded-history checks.
    [[nodiscard]] foundation::int32 GetEntryCount() const noexcept;

Q_SIGNALS:
    /// Current caller path or its restored view changed; GUI-thread presentation listeners only.
    void Changed();
    /// Synchronous GUI-thread request. The workspace must show this file/configuration, synchronize
    /// its selection and switch to Canvas before returning; the history then restores the saved view.
    void LocationRequested(const QString& file, const QString& configuration, const QStringList& selection);
    /// Navigation failed without editing the graph; suitable for the workspace's status line.
    void NavigationFailed(const QString& reason);

private:
    struct Entry final
    {
        QVector<RenderGraphHistoryLocation> Path;
    };
    void saveCurrentView();
    void prune();
    void appendVisit(const Entry& entry);
    void refresh();
    [[nodiscard]] bool showEntry(const Entry& entry);
    [[nodiscard]] bool step(foundation::int32 direction);
    [[nodiscard]] bool reconcile(Entry& inoutEntry, RenderGraphHistoryRecordCache& inoutRecords) const;
    [[nodiscard]] bool hasConfiguration(const QString& file, const QString& configuration) const;
    [[nodiscard]] QString describe(const RenderGraphHistoryLocation& location) const;

    const RenderGraphDocument& mDocument;
    RenderGraphCanvas& mCanvas;
    QAction* mBack;
    QAction* mForward;
    QList<QAction*> mBreadcrumbs;
    QVector<Entry> mEntries;
    foundation::int32 mCurrent = -1;
    Entry mPending;
    QString mPreparedInstance;
    QString mPreparedFile;
    QString mPreparedConfiguration;
    bool mIsRestoring = false;
    bool mShouldRecordDiagnostic = false;
    foundation::uint64 mGeneration = 0;
};
} // namespace ludus::editor
