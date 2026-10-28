#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_job.h"

#include <QObject>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAction;
class QLabel;
class QToolBar;
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphDocument;

/// Relationship between the report's captured semantic inputs and the current draft/build sources.
enum class RenderGraphDiagnosticFreshness : foundation::uint8
{
    None,       ///< No accepted report exists for this package.
    Current,    ///< Captured identity matches both current draft and selected build source.
    Stale,      ///< Draft or build inputs differ from the captured identity.
    Unverified, ///< The report has no semantic input identity; currency cannot be established.
};

/// Resolve a package-owned graph diagnostic to its current configuration. Missing nodes/configurations,
/// source-only/generated messages and malformed graphs return empty without changing selection or history.
/// Missing configurations default only when the report omits the configuration identity.
[[nodiscard]] QJsonObject ResolveRenderGraphDiagnosticTarget(const RenderGraphDocument& document,
                                                             const QJsonObject& location);

/// Canvas report controls and bounded result cursor, independent of document undo and scene items.
/// Owns a copied accepted result; the document and canvas must outlive this canvas-owned component.
/// Counts include report errors/warnings, excluding duplicate compiler summary rows. Navigation wraps
/// over available graph targets in report order; source-only/unavailable messages stay in Error List.
/// GUI thread only. Freshness is supplied by the workspace, which owns build-source identity.
class RenderGraphDiagnostics final : public QObject
{
    Q_OBJECT
public:
    /// Add compact controls to the existing toolbar. F8 / Shift+F8 apply only to canvas descendants.
    RenderGraphDiagnostics(const RenderGraphDocument& document, QToolBar& inoutToolbar, QWidget& inoutCanvas);
    /// Clear all result/cursor state on package/project replacement. No document changes.
    void Reset();
    /// Replace a report and reset the cursor. Cancelled/successful reports expose no problem rows.
    void SetResult(const RenderGraphJobResult& result);
    /// Re-evaluate target availability after edits/undo and update the report freshness label.
    /// Does not reveal a target, change selection or reset an otherwise valid result cursor.
    void Refresh(RenderGraphDiagnosticFreshness freshness);
    /// Ask the owner to refresh, then visit the next/previous available target, wrapping in report order.
    /// False when no canvas target remains. Never edits the document.
    [[nodiscard]] bool Next();
    [[nodiscard]] bool Previous();
    /// Synchronize with Error List activation; ignored when the row is not a report problem.
    void SetCurrentRow(foundation::int32 row);
    /// Report error/warning counts, independently of whether their targets are still available.
    [[nodiscard]] foundation::int32 GetErrorCount() const noexcept;
    [[nodiscard]] foundation::int32 GetWarningCount() const noexcept;
    /// Error rows targeting these authored nodes in this exact graph/configuration. Excludes unavailable targets.
    [[nodiscard]] foundation::int32
    CountNodeErrors(const QString& file, const QString& configuration, const QStringList& nodes) const;
    /// Number of currently available canvas targets.
    [[nodiscard]] foundation::int32 GetAvailableCount() const noexcept;
    /// Last visited source row in RenderGraphJobResult, or -1 before navigation/new result.
    [[nodiscard]] foundation::int32 GetCurrentRow() const noexcept;
    /// Last owner-supplied relationship to current sources.
    [[nodiscard]] RenderGraphDiagnosticFreshness GetFreshness() const noexcept;

Q_SIGNALS:
    /// Report counts or freshness changed; presentation listeners must not mutate the report.
    void Changed();
    /// Synchronous request for the owner to refresh source identity before an explicit step.
    void RefreshRequested();
    /// Synchronous reveal through the shared Error List activation path; row is a source result index.
    void DiagnosticRequested(foundation::int32 row);

private:
    [[nodiscard]] bool step(foundation::int32 direction);
    void refreshControls();

    const RenderGraphDocument& mDocument;
    RenderGraphJobResult mResult;
    QAction* mPrevious;
    QAction* mNext;
    QLabel* mCounts;
    QLabel* mState;
    QVector<foundation::int32> mProblemRows;
    QVector<foundation::int32> mAvailableRows;
    foundation::int32 mErrors = 0;
    foundation::int32 mWarnings = 0;
    foundation::int32 mCurrentRow = -1;
    RenderGraphDiagnosticFreshness mFreshness = RenderGraphDiagnosticFreshness::None;
    bool mIsNavigating = false;
    bool mHasTargetSnapshot = false;
    foundation::uint64 mTargetRevision = 0;
};
} // namespace ludus::editor
