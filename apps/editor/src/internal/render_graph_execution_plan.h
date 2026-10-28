#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_history.h"
#include "internal/render_graph_job.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAction;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
QT_END_NAMESPACE
namespace ludus::editor
{
class RenderGraphCanvas;
/// Relationship of the accepted compiler snapshot to the selected root and current sources.
enum class RenderGraphPlanFreshness : foundation::uint8
{
    Unavailable, ///< No accepted plan with supported provenance.
    Current,     ///< Both draft and captured source identities match.
    Stale,       ///< Current draft, saved sources or root selection differ.
    Unverified,  ///< The result has no captured input identity.
};
/// Read-only canvas feedback; Orders are one-based serial contract operations, not GPU timings.
struct RenderGraphPlanAnnotation final
{
    QString Label;                     ///< Compact status/order text, suitable for an elided badge.
    QString Explanation;               ///< Complete provenance and freshness for a tooltip.
    QVector<foundation::int32> Orders; ///< Exact distinct operation ordinals in ascending order.
    bool HasActivity = false;          ///< Whether the snapshot establishes activity for this node/stage.
    bool IsPruned = false;             ///< True only for current, authoritatively absent nodes.
};
/// Owns accepted plan provenance, its Plan page and the canvas toggle. Borrows the GUI-thread
/// document/canvas for its lifetime; never edits graph metadata, selection or layout on refresh.
/// Navigation uses the existing browser history and validates exact current caller bindings.
class RenderGraphExecutionPlan final : public QObject
{
    Q_OBJECT
public:
    /// Add view-only controls and a Plan page; the borrowed canvas/document/toolbar outlive this component.
    RenderGraphExecutionPlan(const RenderGraphDocument& document,
                             RenderGraphCanvas& inoutCanvas,
                             QToolBar& inoutToolbar);
    /// Drop a package's plan and raw result. The view-only toggle preference is retained.
    void Reset();
    /// Accept bounded provenance from successful dump/validate or package plans. Failed, cancelled,
    /// missing or malformed results retain the last accepted plan and describe the latest outcome.
    void SetResult(const RenderGraphJobResult& result);
    /// Re-evaluate semantic freshness and current navigation availability, caching source parses
    /// by document revision. selectedRoot names the workspace's root configuration choice.
    void Refresh(const QString& selectedRoot);
    /// Page ownership transfers to the workspace's tab container when inserted.
    [[nodiscard]] QWidget* GetPage() const noexcept;
    /// Current canvas overlay preference; changing it never changes the document/view snapshot.
    [[nodiscard]] bool IsEnabled() const;
    /// Toggle badges and stage summaries without changing document state or the canvas view snapshot.
    void SetEnabled(bool isEnabled);
    /// Last supported plan's relation to current sources, independent of overlay visibility.
    [[nodiscard]] RenderGraphPlanFreshness GetFreshness() const noexcept;
    /// Exact caller-scoped activity/order badge; unavailable when opened without that caller path.
    [[nodiscard]] RenderGraphPlanAnnotation GetNodeAnnotation(const QString& node) const;
    /// Aggregate distinct orders and pruning over authored members of one comment stage.
    [[nodiscard]] RenderGraphPlanAnnotation GetStageAnnotation(const QStringList& nodes) const;
    /// Reveal a one-based operation in one browser-history visit. Missing/changed owners reject
    /// before navigation, leaving document, selection and history untouched.
    [[nodiscard]] bool RevealOperation(foundation::int32 order);
Q_SIGNALS:
    /// Presentation changed; listeners may refresh badges without editing document state.
    void Changed();

private:
    struct Context final
    {
        QString File;
        QString Configuration;
        QSet<QString> Retained;
    };
    struct Operation final
    {
        QString Identity;
        QString Prefix;
        QString Node;
        QString Kind;
    };
    [[nodiscard]] bool readPlan(const QJsonObject& plan);
    [[nodiscard]] QJsonObject getRecord(const Context& context) const;
    [[nodiscard]] QVector<RenderGraphHistoryLocation> getPath(const QString& prefix) const;
    [[nodiscard]] QString getCurrentPrefix() const;
    [[nodiscard]] QString describeFreshness() const;
    void refreshControls();
    const RenderGraphDocument& mDocument;
    RenderGraphCanvas& mCanvas;
    QWidget* mPage;
    QTreeWidget* mOperations;
    QPlainTextEdit* mRaw;
    QPushButton* mReveal;
    QAction* mToggle;
    QLabel* mState;
    QLabel* mPageState;
    QHash<QString, Context> mContexts;
    QVector<Operation> mPasses;
    QHash<QString, QJsonObject> mGraphs;
    QString mRoot;
    QString mLatestOutcome;
    QString mSelectedRoot;
    QByteArray mInputIdentity;
    RenderGraphSource mSource = RenderGraphSource::Draft;
    RenderGraphPlanFreshness mFreshness = RenderGraphPlanFreshness::Unavailable;
    foundation::uint64 mDraftRevision = 0;
    foundation::uint64 mSavedRevision = 0;
    bool mHasCachedIdentity = false;
};
} // namespace ludus::editor
