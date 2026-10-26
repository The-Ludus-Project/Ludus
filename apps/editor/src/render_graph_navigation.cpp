#include "internal/render_graph_navigation.h"

#include <ludus/foundation/base/types.h>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QFrame>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSignalBlocker>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 FIND_WIDTH = 480;
constexpr int32 FIND_HEIGHT = 320;
constexpr int32 SCREEN_MARGIN = 8;
} // namespace

RenderGraphNavigation::RenderGraphNavigation(QToolBar& inoutToolbar, QWidget& inoutCanvas)
    : QObject(&inoutCanvas), mToolbar(inoutToolbar), mCanvas(inoutCanvas),
      mFindAction(new QAction(QStringLiteral("Find Node…"), this)),
      mUpstreamAction(new QAction(QStringLiteral("Upstream"), this)),
      mDownstreamAction(new QAction(QStringLiteral("Downstream"), this)),
      mClearAction(new QAction(QStringLiteral("Clear Trace"), this)), mPopup(new QFrame(&inoutCanvas, Qt::Popup)),
      mSearch(new QLineEdit(mPopup)), mResults(new QListWidget(mPopup)), mResultCount(new QLabel(mPopup))
{
    setObjectName(QStringLiteral("renderGraphNavigation"));
    mFindAction->setObjectName(QStringLiteral("renderGraphFindNode"));
    mFindAction->setShortcut(QKeySequence::Find);
    mFindAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    mFindAction->setToolTip(QStringLiteral("Find an existing node by name or kind (%1)")
                                .arg(mFindAction->shortcut().toString(QKeySequence::NativeText)));
    mCanvas.addAction(mFindAction);
    mUpstreamAction->setObjectName(QStringLiteral("renderGraphTraceUpstream"));
    mDownstreamAction->setObjectName(QStringLiteral("renderGraphTraceDownstream"));
    mClearAction->setObjectName(QStringLiteral("renderGraphClearTrace"));
    QActionGroup* directions = new QActionGroup(this);
    for (QAction* action : {mUpstreamAction, mDownstreamAction})
    {
        action->setCheckable(true);
        directions->addAction(action);
    }
    mToolbar.addSeparator();
    for (QAction* action : {mFindAction, mUpstreamAction, mDownstreamAction, mClearAction})
    {
        mToolbar.addAction(action);
    }
    mClearAction->setToolTip(QStringLiteral("Clear dependency tracing, keeping selection (Escape on the canvas)"));
    mPopup->setObjectName(QStringLiteral("renderGraphFindPopup"));
    mPopup->setAccessibleName(QStringLiteral("Find Node"));
    mPopup->setWindowTitle(QStringLiteral("Find Node"));
    QVBoxLayout* layout = new QVBoxLayout(mPopup);
    QLabel* title = new QLabel(QStringLiteral("Find Node · current configuration"), mPopup);
    layout->addWidget(title);
    mSearch->setObjectName(QStringLiteral("renderGraphFindQuery"));
    mSearch->setAccessibleName(QStringLiteral("Find node by name or kind"));
    mSearch->setPlaceholderText(QStringLiteral("Search node name or kind…"));
    layout->addWidget(mSearch);
    mResults->setObjectName(QStringLiteral("renderGraphFindResults"));
    mResults->setAccessibleName(QStringLiteral("Matching nodes"));
    mResults->setSelectionMode(QAbstractItemView::SingleSelection);
    mResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mResults, 1);
    mResultCount->setObjectName(QStringLiteral("renderGraphFindCount"));
    layout->addWidget(mResultCount);
    mPopup->installEventFilter(this);
    mSearch->installEventFilter(this);
    mResults->installEventFilter(this);
    connect(mSearch, &QLineEdit::textChanged, this, &RenderGraphNavigation::refreshResults);
    connect(mResults, &QListWidget::itemDoubleClicked, this, &RenderGraphNavigation::acceptResult);
    connect(mFindAction, &QAction::triggered, this, &RenderGraphNavigation::OpenFindNode);
    connect(mUpstreamAction, &QAction::triggered, this, [this]() {
        SetTraceDirection(RenderGraphTraceDirection::Upstream);
    });
    connect(mDownstreamAction, &QAction::triggered, this, [this]() {
        SetTraceDirection(RenderGraphTraceDirection::Downstream);
    });
    connect(mClearAction, &QAction::triggered, this, [this]() { SetTraceDirection(RenderGraphTraceDirection::None); });
    refreshActions();
}

void RenderGraphNavigation::SetGraph(const QVector<RenderGraphNavigationNode>& nodes,
                                     const QVector<RenderGraphNavigationEdge>& edges)
{
    mPopup->hide();
    mNodes = nodes;
    mEdges = edges;
    mSelection = getAuthoredSelection(mSelection);
    if (mSelection.isEmpty() == true)
    {
        mDirection = RenderGraphTraceDirection::None;
    }
    refreshTrace();
    refreshActions();
}

QStringList RenderGraphNavigation::getAuthoredSelection(const QStringList& nodes) const
{
    const QSet<QString> requested(nodes.cbegin(), nodes.cend());
    QStringList selected;
    for (const RenderGraphNavigationNode& node : mNodes)
    {
        if (node.IsAuthored == true && requested.contains(node.Identity) == true)
        {
            selected.append(node.Identity);
        }
    }
    selected.sort();
    return selected;
}

void RenderGraphNavigation::SetSelection(const QStringList& nodes)
{
    if (nodes == mSelection)
    {
        return;
    }
    mSelection = getAuthoredSelection(nodes);
    if (mSelection.isEmpty() == true)
    {
        mDirection = RenderGraphTraceDirection::None;
    }
    refreshTrace();
    refreshActions();
}

void RenderGraphNavigation::SetTraceDirection(RenderGraphTraceDirection direction)
{
    const RenderGraphTraceDirection next = mSelection.isEmpty() == true ? RenderGraphTraceDirection::None : direction;
    if (next == mDirection)
    {
        return;
    }
    mDirection = next;
    refreshTrace();
    refreshActions();
    Q_EMIT TraceChanged();
}

RenderGraphTraceDirection RenderGraphNavigation::GetTraceDirection() const noexcept
{
    return mDirection;
}

const RenderGraphDependencyTrace& RenderGraphNavigation::GetTrace() const noexcept
{
    return mTrace;
}

void RenderGraphNavigation::refreshTrace()
{
    mTrace = {};
    if (mDirection == RenderGraphTraceDirection::None)
    {
        return;
    }
    QSet<QString> identities;
    for (const RenderGraphNavigationNode& node : std::as_const(mNodes))
    {
        identities.insert(node.Identity);
    }
    QHash<QString, QVector<qsizetype>> adjacent;
    for (qsizetype index = 0; index < mEdges.size(); ++index)
    {
        const RenderGraphNavigationEdge& edge = mEdges.at(index);
        if (identities.contains(edge.Source) == true && identities.contains(edge.Destination) == true)
        {
            adjacent[mDirection == RenderGraphTraceDirection::Upstream ? edge.Destination : edge.Source].append(index);
        }
    }
    // Each node enters the queue once, so cycles and shared branches remain O(nodes + edges).
    QStringList pending = mSelection;
    for (const QString& root : std::as_const(mSelection))
    {
        mTrace.Nodes.insert(root);
    }
    for (qsizetype cursor = 0; cursor < pending.size(); ++cursor)
    {
        const QVector<qsizetype> edges = adjacent.value(pending.at(cursor));
        for (qsizetype index : edges)
        {
            const RenderGraphNavigationEdge& edge = mEdges.at(index);
            mTrace.Edges.insert(index);
            const QString next = mDirection == RenderGraphTraceDirection::Upstream ? edge.Source : edge.Destination;
            if (mTrace.Nodes.contains(next) == false)
            {
                mTrace.Nodes.insert(next);
                pending.append(next);
            }
        }
    }
}

void RenderGraphNavigation::refreshActions()
{
    bool hasAuthoredNodes = false;
    for (const RenderGraphNavigationNode& node : std::as_const(mNodes))
    {
        hasAuthoredNodes = hasAuthoredNodes == true || node.IsAuthored == true;
    }
    mFindAction->setEnabled(hasAuthoredNodes);
    mUpstreamAction->setEnabled(mSelection.isEmpty() == false);
    mDownstreamAction->setEnabled(mSelection.isEmpty() == false);
    mUpstreamAction->setChecked(mDirection == RenderGraphTraceDirection::Upstream);
    mDownstreamAction->setChecked(mDirection == RenderGraphTraceDirection::Downstream);
    mClearAction->setEnabled(mDirection != RenderGraphTraceDirection::None);
    const QString summary = QStringLiteral("\nActive trace: %1 nodes, %2 connections in the current configuration")
                                .arg(mTrace.Nodes.size())
                                .arg(mTrace.Edges.size());
    mUpstreamAction->setToolTip(
        QStringLiteral("Trace every upstream producer of the selected nodes in this configuration.") +
        (mDirection == RenderGraphTraceDirection::Upstream ? summary : QString()));
    mDownstreamAction->setToolTip(
        QStringLiteral("Trace every downstream consumer of the selected nodes in this configuration.") +
        (mDirection == RenderGraphTraceDirection::Downstream ? summary : QString()));
}

void RenderGraphNavigation::OpenFindNode()
{
    if (mFindAction->isEnabled() == false)
    {
        return;
    }
    const QSignalBlocker blocker(mSearch);
    mSearch->clear();
    refreshResults();
    const QScreen* screenOrNull = mCanvas.screen();
    if (screenOrNull == nullptr)
    {
        return;
    }
    const QRect available =
        screenOrNull->availableGeometry().adjusted(SCREEN_MARGIN, SCREEN_MARGIN, -SCREEN_MARGIN, -SCREEN_MARGIN);
    mPopup->resize(qMin(FIND_WIDTH, available.width()), qMin(FIND_HEIGHT, available.height()));
    QPoint positionGlobal =
        mCanvas.mapToGlobal(mCanvas.rect().center()) - QPoint(mPopup->width() / 2, mPopup->height() / 2);
    positionGlobal.setX(
        qBound(available.left(), positionGlobal.x(), qMax(available.left(), available.right() - mPopup->width())));
    positionGlobal.setY(
        qBound(available.top(), positionGlobal.y(), qMax(available.top(), available.bottom() - mPopup->height())));
    mPopup->move(positionGlobal);
    mPopup->show();
    mPopup->raise();
    mPopup->activateWindow();
    mSearch->setFocus(Qt::PopupFocusReason);
}

void RenderGraphNavigation::refreshResults()
{
    const QString query = mSearch->text().simplified();
    const QStringList tokens = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList exact;
    QStringList prefixes;
    QStringList others;
    QHash<QString, QString> kinds;
    for (const RenderGraphNavigationNode& node : std::as_const(mNodes))
    {
        if (node.IsAuthored == false)
        {
            continue;
        }
        bool isMatch = true;
        for (const QString& token : tokens)
        {
            if (node.Identity.contains(token, Qt::CaseInsensitive) == false &&
                node.Kind.contains(token, Qt::CaseInsensitive) == false)
            {
                isMatch = false;
                break;
            }
        }
        if (isMatch == false)
        {
            continue;
        }
        kinds.insert(node.Identity, node.Kind);
        if (query.isEmpty() == false && node.Identity.compare(query, Qt::CaseInsensitive) == 0)
        {
            exact.append(node.Identity);
        }
        else if (query.isEmpty() == false && node.Identity.startsWith(query, Qt::CaseInsensitive) == true)
        {
            prefixes.append(node.Identity);
        }
        else
        {
            others.append(node.Identity);
        }
    }
    exact.sort();
    prefixes.sort();
    others.sort();
    mResults->clear();
    const QStringList matches = exact + prefixes + others;
    for (const QString& identity : matches)
    {
        QListWidgetItem* item =
            new QListWidgetItem(QStringLiteral("%1 · %2").arg(identity, kinds.value(identity)), mResults);
        item->setData(Qt::UserRole, identity);
        item->setToolTip(item->text());
    }
    mResults->setCurrentRow(matches.isEmpty() == true ? -1 : 0);
    mResultCount->setText(
        matches.isEmpty() == true
            ? QStringLiteral("No matching nodes. Try a name or kind.")
            : QStringLiteral("%1 matching nodes · Enter to frame · Escape to cancel").arg(matches.size()));
}

void RenderGraphNavigation::acceptResult()
{
    if (mPopup->isVisible() == false)
    {
        return;
    }
    const QListWidgetItem* itemOrNull = mResults->currentItem();
    if (itemOrNull == nullptr)
    {
        return;
    }
    const QString identity = itemOrNull->data(Qt::UserRole).toString();
    mPopup->hide();
    mCanvas.setFocus(Qt::OtherFocusReason);
    Q_EMIT NodeRequested(identity);
}

bool RenderGraphNavigation::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mPopup && event->type() == QEvent::Hide)
    {
        mCanvas.setFocus(Qt::OtherFocusReason);
    }
    if (event->type() == QEvent::ShortcutOverride)
    {
        // Text editing and list navigation take precedence over the canvas's single-key actions.
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress)
    {
        const QKeyEvent* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape)
        {
            mPopup->hide();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            acceptResult();
            return true;
        }
        if (watched == mSearch && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up))
        {
            const int32 step = key->key() == Qt::Key_Down ? 1 : -1;
            if (mResults->count() > 0)
            {
                mResults->setCurrentRow(qBound(0, mResults->currentRow() + step, mResults->count() - 1));
            }
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
} // namespace ludus::editor
