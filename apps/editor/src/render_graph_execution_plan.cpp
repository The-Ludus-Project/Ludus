#include "internal/render_graph_execution_plan.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_live_build.h"
#include "render_graph_plan_policy.h"

#include <ludus/foundation/base/core.h>

#include <QAction>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace ludus::editor
{
using namespace foundation;
namespace
{
QString FormatOrders(const QVector<int32>& orders)
{
    QStringList text;
    for (const int32 order : orders)
    {
        text.append(QStringLiteral("#%1").arg(order));
    }
    return text.join(QStringLiteral(", "));
}
QJsonObject FindNode(const QJsonObject& record, const QString& identity)
{
    for (const QJsonValueConstRef value : record.value(QStringLiteral("nodes")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return value.toObject();
        }
    }
    return {};
}
} // namespace

RenderGraphExecutionPlan::RenderGraphExecutionPlan(const RenderGraphDocument& document,
                                                   RenderGraphCanvas& inoutCanvas,
                                                   QToolBar& inoutToolbar)
    : QObject(&inoutCanvas), mDocument(document), mCanvas(inoutCanvas), mPage(new QWidget(&inoutCanvas)),
      mOperations(new QTreeWidget(mPage)), mRaw(new QPlainTextEdit(mPage)),
      mReveal(new QPushButton(QStringLiteral("Reveal on Canvas"), mPage)),
      mToggle(new QAction(QStringLiteral("Execution Plan"), this)), mState(new QLabel(&inoutToolbar)),
      mPageState(new QLabel(mPage))
{
    mPage->hide();
    mPage->setObjectName(QStringLiteral("renderGraphPlanPage"));
    mOperations->setObjectName(QStringLiteral("renderGraphPlanOperations"));
    mOperations->setHeaderLabels({QStringLiteral("Order"),
                                  QStringLiteral("Operation"),
                                  QStringLiteral("Expanded instance / node"),
                                  QStringLiteral("Source")});
    mOperations->setRootIsDecorated(false);
    mOperations->setSortingEnabled(false);
    mOperations->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    mOperations->header()->setStretchLastSection(true);
    mOperations->setToolTip(QStringLiteral("Double-click an operation, press Enter, or choose Reveal on Canvas. "
                                           "Order describes the compiler's serial contract plan, not GPU timestamps."));
    mRaw->setObjectName(QStringLiteral("renderGraphPlan"));
    mRaw->setReadOnly(true);
    mRaw->setPlaceholderText(
        QStringLiteral("Validate the graph to inspect its accepted serial plan and source provenance."));
    mReveal->setObjectName(QStringLiteral("renderGraphRevealPlanOperation"));
    mPageState->setObjectName(QStringLiteral("renderGraphPlanPageState"));
    mPageState->setWordWrap(true);
    QSplitter* split = new QSplitter(Qt::Vertical, mPage);
    split->addWidget(mOperations);
    split->addWidget(mRaw);
    QVBoxLayout* layout = new QVBoxLayout(mPage);
    layout->addWidget(mPageState);
    layout->addWidget(split, 1);
    layout->addWidget(mReveal);
    mToggle->setCheckable(true);
    mToggle->setObjectName(QStringLiteral("renderGraphExecutionPlanOverlay"));
    mToggle->setToolTip(QStringLiteral(
        "Show accepted compiler activity and serial operation order; preserves canvas selection, tracing and layout."));
    mState->setObjectName(QStringLiteral("renderGraphPlanState"));
    inoutToolbar.addSeparator();
    inoutToolbar.addAction(mToggle);
    inoutToolbar.addWidget(mState);
    connect(mToggle, &QAction::toggled, this, [this]() {
        refreshControls();
        Q_EMIT Changed();
    });
    connect(mOperations, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int32) {
        static_cast<void>(RevealOperation(item->data(0, Qt::UserRole).toInt()));
    });
    connect(mOperations, &QTreeWidget::currentItemChanged, this, [this]() { refreshControls(); });
    connect(mReveal, &QPushButton::clicked, this, [this]() {
        const QTreeWidgetItem* itemOrNull = mOperations->currentItem();
        if (itemOrNull != nullptr)
        {
            static_cast<void>(RevealOperation(itemOrNull->data(0, Qt::UserRole).toInt()));
        }
    });
    Reset();
}
QWidget* RenderGraphExecutionPlan::GetPage() const noexcept
{
    return mPage;
}
bool RenderGraphExecutionPlan::IsEnabled() const
{
    return mToggle->isChecked();
}
void RenderGraphExecutionPlan::SetEnabled(bool isEnabled)
{
    mToggle->setChecked(isEnabled);
}
RenderGraphPlanFreshness RenderGraphExecutionPlan::GetFreshness() const noexcept
{
    return mFreshness;
}

void RenderGraphExecutionPlan::Reset()
{
    const QSignalBlocker blocker(mOperations);
    mContexts.clear();
    mPasses.clear();
    mGraphs.clear();
    mRoot.clear();
    mInputIdentity.clear();
    mRaw->clear();
    mOperations->clear();
    mLatestOutcome.clear();
    mHasCachedIdentity = false;
    mFreshness = RenderGraphPlanFreshness::Unavailable;
    refreshControls();
    Q_EMIT Changed();
}

bool RenderGraphExecutionPlan::readPlan(const QJsonObject& plan)
{
    const QJsonValue activity = plan.value(QStringLiteral("activity"));
    const QJsonValue passes = plan.value(QStringLiteral("passes"));
    const QString root = plan.value(QStringLiteral("asset")).toString();
    const QRegularExpression identifier(
        QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,%1}\\z").arg(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - 1));
    if (plan.value(QStringLiteral("version")).toInt() != plan_policy::VERSION || activity.isArray() == false ||
        activity.toArray().isEmpty() == true || activity.toArray().size() > plan_policy::MAX_EXPANDED_NODES + 1 ||
        passes.isArray() == false || passes.toArray().size() > plan_policy::MAX_PASSES ||
        identifier.match(root).hasMatch() == false)
    {
        return false;
    }
    QHash<QString, Context> contexts;
    int32 retainedCount = 0;
    for (const QJsonValueConstRef value : activity.toArray())
    {
        const QJsonObject record = value.toObject();
        const QString prefix = record.value(QStringLiteral("instance")).toString();
        const QStringList segments = prefix.split(QLatin1Char('/'));
        Context context{record.value(QStringLiteral("source")).toString(),
                        record.value(QStringLiteral("configuration")).toString(),
                        {}};
        if (segments.first() != root || segments.size() > plan_policy::MAX_DEPTH || contexts.contains(prefix) == true ||
            context.File.isEmpty() == true || identifier.match(context.Configuration).hasMatch() == false ||
            record.value(QStringLiteral("retained")).isArray() == false ||
            record.value(QStringLiteral("retained")).toArray().size() > plan_policy::MAX_NODES)
        {
            return false;
        }
        for (const QString& segment : segments)
        {
            if (identifier.match(segment).hasMatch() == false)
            {
                return false;
            }
        }
        if (segments.size() > 1)
        {
            const QString parent = prefix.left(prefix.lastIndexOf(QLatin1Char('/')));
            if (contexts.contains(parent) == false ||
                contexts.value(parent).Retained.contains(segments.last()) == false)
            {
                return false;
            }
        }
        for (const QJsonValueConstRef node : record.value(QStringLiteral("retained")).toArray())
        {
            const QString identity = node.toString();
            if (identifier.match(identity).hasMatch() == false || context.Retained.contains(identity) == true)
            {
                return false;
            }
            context.Retained.insert(identity);
            ++retainedCount;
            if (retainedCount > plan_policy::MAX_EXPANDED_NODES)
            {
                return false;
            }
        }
        contexts.insert(prefix, context);
    }
    if (contexts.contains(root) == false || contexts.value(root).File != mDocument.GetRootGraph() ||
        contexts.value(root).File != plan.value(QStringLiteral("source")).toString() ||
        contexts.value(root).Configuration != plan.value(QStringLiteral("configuration")).toString())
    {
        return false;
    }
    QVector<Operation> operations;
    QSet<QString> claimed;
    for (const QJsonValueConstRef value : passes.toArray())
    {
        const QJsonObject record = value.toObject();
        const QString identity = record.value(QStringLiteral("id")).toString();
        const int32 separator = static_cast<int32>(identity.lastIndexOf(QLatin1Char('/')));
        Operation operation{identity,
                            identity.left(separator),
                            identity.mid(separator + 1),
                            record.value(QStringLiteral("kind")).toString()};
        const Context context = contexts.value(operation.Prefix);
        if (separator <= 0 || claimed.contains(identity) == true ||
            context.Retained.contains(operation.Node) == false ||
            context.File != record.value(QStringLiteral("source")).toString() ||
            context.Configuration != record.value(QStringLiteral("asset_configuration")).toString() ||
            operation.Kind.isEmpty() == true)
        {
            return false;
        }
        claimed.insert(identity);
        operations.append(operation);
    }
    mContexts = Move(contexts);
    mPasses = Move(operations);
    mRoot = root;
    return true;
}

void RenderGraphExecutionPlan::SetResult(const RenderGraphJobResult& result)
{
    if (result.IsCancelled == true || result.IsSuccessful == false)
    {
        mLatestOutcome = result.IsCancelled == true ? QStringLiteral(" · latest job cancelled")
                                                    : QStringLiteral(" · latest job failed");
    }
    else
    {
        const QJsonObject root = mGraphs.value(mDocument.GetRootGraph());
        const QString rootConfiguration = getRecord({mDocument.GetRootGraph(), mSelectedRoot, {}}).isEmpty() == true
                                              ? root.value(QStringLiteral("default")).toString()
                                              : mSelectedRoot;
        QJsonObject plan = result.Payload.value(QStringLiteral("plan")).toObject();
        if (plan.isEmpty() == true)
        {
            const QJsonArray plans = result.Payload.value(QStringLiteral("plans")).toArray();
            if (plans.size() <= plan_policy::MAX_CONFIGURATIONS)
            {
                for (const QJsonValueConstRef candidate : plans)
                {
                    if (candidate.toObject().value(QStringLiteral("configuration")).toString() == rootConfiguration)
                    {
                        plan = candidate.toObject();
                        break;
                    }
                }
            }
        }
        if (plan.isEmpty() == false && readPlan(plan) == true)
        {
            mInputIdentity = result.Request.InputIdentity;
            mSource = result.Request.Source;
            mLatestOutcome.clear();
            mHasCachedIdentity = false;
            mRaw->setPlainText(QString::fromUtf8(QJsonDocument(plan).toJson(QJsonDocument::Indented)));
            const QSignalBlocker blocker(mOperations);
            mOperations->clear();
            for (int32 index = 0; index < mPasses.size(); ++index)
            {
                const Operation& operation = mPasses.at(index);
                const Context context = mContexts.value(operation.Prefix);
                QTreeWidgetItem* item = new QTreeWidgetItem(mOperations,
                                                            {QString::number(index + 1),
                                                             operation.Kind,
                                                             operation.Identity,
                                                             context.File + QLatin1Char('#') + context.Configuration});
                item->setData(0, Qt::UserRole, index + 1);
            }
        }
        else
        {
            mLatestOutcome = QStringLiteral(" · latest result has no supported plan provenance");
            if (mRoot.isEmpty() == true)
            {
                mRaw->setPlainText(QString::fromUtf8(QJsonDocument(result.Payload).toJson(QJsonDocument::Indented)));
            }
        }
    }
    mHasCachedIdentity = false;
    Refresh(mSelectedRoot);
}

QJsonObject RenderGraphExecutionPlan::getRecord(const Context& context) const
{
    for (const QJsonValueConstRef value : mGraphs.value(context.File).value(QStringLiteral("configurations")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == context.Configuration)
        {
            return value.toObject();
        }
    }
    return {};
}
QVector<RenderGraphHistoryLocation> RenderGraphExecutionPlan::getPath(const QString& prefix) const
{
    const QStringList segments = prefix.split(QLatin1Char('/'));
    QVector<RenderGraphHistoryLocation> path;
    QString current;
    for (const QString& segment : segments)
    {
        current = current.isEmpty() == true ? segment : current + QLatin1Char('/') + segment;
        if (mContexts.contains(current) == false)
        {
            return {};
        }
        const Context context = mContexts.value(current);
        const QJsonObject record = getRecord(context);
        if (record.isEmpty() == true)
        {
            return {};
        }
        if (path.isEmpty() == false)
        {
            const QJsonObject caller = FindNode(getRecord({path.last().File, path.last().Configuration, {}}), segment);
            if (caller.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph") ||
                caller.value(QStringLiteral("asset")).toString() != context.File ||
                caller.value(QStringLiteral("configuration")).toString() != context.Configuration)
            {
                return {};
            }
        }
        else if (context.File != mDocument.GetRootGraph() ||
                 mGraphs.value(context.File).value(QStringLiteral("id")).toString() != mRoot)
        {
            return {};
        }
        path.append({context.File, context.Configuration, path.isEmpty() == true ? QString() : segment, {}, false});
    }
    return path;
}
QString RenderGraphExecutionPlan::getCurrentPrefix() const
{
    const QVector<RenderGraphHistoryLocation> path = mCanvas.GetHistory().GetPath();
    if (path.isEmpty() == true || path.last().File != mCanvas.GetFile() ||
        path.last().Configuration != mCanvas.GetConfiguration())
    {
        return {};
    }
    QString prefix = mRoot;
    for (int32 index = 1; index < path.size(); ++index)
    {
        prefix += QLatin1Char('/') + path.at(index).Instance;
    }
    const QVector<RenderGraphHistoryLocation> expected = getPath(prefix);
    if (expected.size() != path.size())
    {
        return {};
    }
    for (int32 index = 0; index < path.size(); ++index)
    {
        if (expected.at(index).File != path.at(index).File ||
            expected.at(index).Configuration != path.at(index).Configuration ||
            expected.at(index).Instance != path.at(index).Instance)
        {
            return {};
        }
    }
    return prefix;
}
QString RenderGraphExecutionPlan::describeFreshness() const
{
    switch (mFreshness)
    {
        case RenderGraphPlanFreshness::Current:
            return QStringLiteral("Current plan");
        case RenderGraphPlanFreshness::Stale:
            return QStringLiteral("Stale plan");
        case RenderGraphPlanFreshness::Unverified:
            return QStringLiteral("Unverified plan");
        case RenderGraphPlanFreshness::Unavailable:
            return QStringLiteral("Plan unavailable");
    }
    return {};
}
void RenderGraphExecutionPlan::Refresh(const QString& selectedRoot)
{
    if (mHasCachedIdentity == true && mDraftRevision == mDocument.GetRevision() &&
        mSavedRevision == mDocument.GetRevision(RenderGraphSource::Saved) && selectedRoot == mSelectedRoot)
    {
        return;
    }
    {
        mSelectedRoot = selectedRoot;
        mDraftRevision = mDocument.GetRevision();
        mSavedRevision = mDocument.GetRevision(RenderGraphSource::Saved);
        mHasCachedIdentity = true;
        mGraphs.clear();
        for (const QString& file : mDocument.GetFiles())
        {
            QJsonObject graph;
            if (file.endsWith(QStringLiteral(".lurender.json")) == true && mDocument.GetGraph(file, graph) == true)
            {
                mGraphs.insert(file, graph);
            }
        }
        mFreshness =
            mRoot.isEmpty() == true            ? RenderGraphPlanFreshness::Unavailable
            : mInputIdentity.isEmpty() == true ? RenderGraphPlanFreshness::Unverified
            : mInputIdentity == ComputeRenderGraphBuildIdentity(mDocument, selectedRoot, RenderGraphSource::Draft) &&
                    mInputIdentity == ComputeRenderGraphBuildIdentity(mDocument, selectedRoot, mSource)
                ? RenderGraphPlanFreshness::Current
                : RenderGraphPlanFreshness::Stale;
    }
    refreshControls();
    Q_EMIT Changed();
}
void RenderGraphExecutionPlan::refreshControls()
{
    const QSignalBlocker blocker(mOperations);
    const QString text = describeFreshness() + mLatestOutcome;
    mState->setText(describeFreshness());
    mState->setToolTip(text);
    mState->setVisible(IsEnabled());
    mPageState->setText(
        text +
        QStringLiteral(
            ". Serial contract operations include Export/Present and folded processing; these are not GPU timings."));
    const QTreeWidgetItem* selectedOrNull = mOperations->currentItem();
    for (int32 index = 0; index < mOperations->topLevelItemCount(); ++index)
    {
        const Operation& operation = mPasses.at(index);
        const QVector<RenderGraphHistoryLocation> path = getPath(operation.Prefix);
        const bool isAvailable =
            path.isEmpty() == false &&
            FindNode(getRecord({path.last().File, path.last().Configuration, {}}), operation.Node).isEmpty() == false;
        mOperations->topLevelItem(index)->setDisabled(isAvailable == false);
        mOperations->topLevelItem(index)->setToolTip(
            0,
            isAvailable == true ? text : QStringLiteral("Source node or exact caller binding is unavailable."));
    }
    mReveal->setEnabled(selectedOrNull != nullptr && selectedOrNull->isDisabled() == false);
}
RenderGraphPlanAnnotation RenderGraphExecutionPlan::GetNodeAnnotation(const QString& node) const
{
    RenderGraphPlanAnnotation annotation;
    const QString prefix = getCurrentPrefix();
    annotation.Label = QStringLiteral("Plan unavailable");
    annotation.Explanation = describeFreshness() + mLatestOutcome;
    if (prefix.isEmpty() == true)
    {
        annotation.Explanation += QStringLiteral(". This view has no exact caller instance in the accepted plan.");
        return annotation;
    }
    const Context context = mContexts.value(prefix);
    if (FindNode(getRecord(context), node).isEmpty() == true)
    {
        return annotation;
    }
    const bool isRetained = context.Retained.contains(node);
    annotation.HasActivity = isRetained == true || mFreshness == RenderGraphPlanFreshness::Current;
    annotation.IsPruned = isRetained == false && mFreshness == RenderGraphPlanFreshness::Current;
    const QString owner = prefix + QLatin1Char('/') + node;
    for (int32 index = 0; index < mPasses.size(); ++index)
    {
        if (mPasses.at(index).Identity == owner ||
            mPasses.at(index).Identity.startsWith(owner + QLatin1Char('/')) == true)
        {
            annotation.Orders.append(index + 1);
        }
    }
    annotation.Label = annotation.Orders.isEmpty() == false ? FormatOrders(annotation.Orders)
                       : isRetained == true                 ? QStringLiteral("Active value")
                       : annotation.IsPruned == true        ? QStringLiteral("Pruned")
                                                            : QStringLiteral("Unknown");
    if (mFreshness != RenderGraphPlanFreshness::Current)
    {
        annotation.Label += mFreshness == RenderGraphPlanFreshness::Stale ? QStringLiteral(" · stale")
                                                                          : QStringLiteral(" · unverified");
    }
    annotation.Explanation += QStringLiteral(" · %1. %2").arg(owner, annotation.Label);
    return annotation;
}
RenderGraphPlanAnnotation RenderGraphExecutionPlan::GetStageAnnotation(const QStringList& nodes) const
{
    RenderGraphPlanAnnotation result;
    QSet<int32> orders;
    int32 pruned = 0;
    int32 known = 0;
    for (const QString& node : nodes)
    {
        const RenderGraphPlanAnnotation annotation = GetNodeAnnotation(node);
        for (const int32 order : annotation.Orders)
        {
            orders.insert(order);
        }
        pruned += annotation.IsPruned == true ? 1 : 0;
        known += annotation.HasActivity == true ? 1 : 0;
    }
    for (int32 index = 0; index < mPasses.size(); ++index)
    {
        if (orders.contains(index + 1) == true)
        {
            result.Orders.append(index + 1);
        }
    }
    result.HasActivity = nodes.isEmpty() == false && known == nodes.size();
    result.IsPruned = nodes.isEmpty() == false && pruned == nodes.size();
    result.Label = getCurrentPrefix().isEmpty() == true ? QStringLiteral("Plan unavailable")
                                                        : QStringLiteral("%1 passes").arg(orders.size()) +
                                                              (mFreshness == RenderGraphPlanFreshness::Current
                                                                   ? QStringLiteral(" · %1 pruned").arg(pruned)
                                                                   : QStringLiteral(" · pruning unknown"));
    if (result.Orders.isEmpty() == false)
    {
        result.Label += QStringLiteral(" · ") + FormatOrders(result.Orders);
    }
    if (mFreshness != RenderGraphPlanFreshness::Current && getCurrentPrefix().isEmpty() == false)
    {
        result.Label += mFreshness == RenderGraphPlanFreshness::Stale ? QStringLiteral(" · stale")
                                                                      : QStringLiteral(" · unverified");
    }
    result.Explanation = describeFreshness() + mLatestOutcome + QStringLiteral(". ") + result.Label;
    return result;
}
bool RenderGraphExecutionPlan::RevealOperation(int32 order)
{
    if (order <= 0 || order > mPasses.size())
    {
        return false;
    }
    const Operation operation = mPasses.at(order - 1);
    const QVector<RenderGraphHistoryLocation> path = getPath(operation.Prefix);
    const bool hasRevealed = mCanvas.GetHistory().VisitNodePath(path, operation.Node);
    if (hasRevealed == false)
    {
        mPageState->setText(
            QStringLiteral("Plan target unavailable: its source node or caller binding changed. Validate again."));
    }
    return hasRevealed;
}
} // namespace ludus::editor
