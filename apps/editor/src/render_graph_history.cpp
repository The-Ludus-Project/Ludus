#include "internal/render_graph_history.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"

#include <ludus/foundation/base/compiler.h>

#include <QAction>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QStringList>

#include <utility>

namespace ludus::editor
{
using namespace foundation;
// One navigation pass can inspect many visits to the same asset. Parse each file once
// during that pass; no decoded records survive to another document revision or package.
class RenderGraphHistoryRecordCache final
{
public:
    LUDUS_INLINE explicit RenderGraphHistoryRecordCache(const RenderGraphDocument& document) noexcept
        : mDocument(document)
    {
    }

    [[nodiscard]] QJsonObject Get(const RenderGraphHistoryLocation& location)
    {
        if (mGraphs.contains(location.File) == false)
        {
            QJsonObject graph;
            static_cast<void>(mDocument.GetGraph(location.File, graph));
            mGraphs.insert(location.File, graph);
        }
        const QJsonObject graph = mGraphs.value(location.File);
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == location.Configuration)
            {
                return value.toObject();
            }
        }
        return {};
    }

private:
    const RenderGraphDocument& mDocument;
    QHash<QString, QJsonObject> mGraphs;
};

namespace
{
QJsonObject GetRecord(const RenderGraphDocument& document, const RenderGraphHistoryLocation& location)
{
    RenderGraphHistoryRecordCache records(document);
    return records.Get(location);
}

bool HasBinding(const QJsonObject& node, const RenderGraphHistoryLocation& location)
{
    return node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph") &&
           node.value(QStringLiteral("asset")).toString() == location.File &&
           node.value(QStringLiteral("configuration")).toString() == location.Configuration;
}

bool HasSameLocation(const RenderGraphHistoryLocation& first, const RenderGraphHistoryLocation& second)
{
    return first.File == second.File && first.Configuration == second.Configuration &&
           first.Instance == second.Instance;
}
} // namespace

RenderGraphHistory::RenderGraphHistory(const RenderGraphDocument& document, RenderGraphCanvas& inoutCanvas)
    : QToolBar(&inoutCanvas), mDocument(document), mCanvas(inoutCanvas),
      mBack(new QAction(QStringLiteral("Back"), this)), mForward(new QAction(QStringLiteral("Forward"), this))
{
    setObjectName(QStringLiteral("renderGraphHistoryBar"));
    setAccessibleName(QStringLiteral("Graph navigation history and breadcrumbs"));
    setMovable(false);
    setFloatable(false);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    mBack->setObjectName(QStringLiteral("renderGraphBack"));
    mForward->setObjectName(QStringLiteral("renderGraphForward"));
    mBack->setShortcuts(QKeySequence::keyBindings(QKeySequence::Back));
    mForward->setShortcuts(QKeySequence::keyBindings(QKeySequence::Forward));
    for (QAction* action : {mBack, mForward})
    {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        mCanvas.addAction(action);
        addAction(action);
    }
    addSeparator();
    connect(mBack, &QAction::triggered, this, [this]() { static_cast<void>(Back()); });
    connect(mForward, &QAction::triggered, this, [this]() { static_cast<void>(Forward()); });
    refresh();
}

void RenderGraphHistory::Reset()
{
    mEntries.clear();
    mCurrent = -1;
    mPending = {};
    mPreparedInstance.clear();
    mPreparedFile.clear();
    mPreparedConfiguration.clear();
    mShouldRecordDiagnostic = false;
    refresh();
}

bool RenderGraphHistory::hasConfiguration(const QString& file, const QString& configuration) const
{
    return GetRecord(mDocument, {file, configuration, {}, {}, false}).isEmpty() == false;
}

void RenderGraphHistory::saveCurrentView()
{
    if (mCurrent < 0 || mCurrent >= mEntries.size())
    {
        return;
    }
    Entry& current = mEntries[mCurrent];
    RenderGraphHistoryLocation& location = current.Path.last();
    if (location.File != mCanvas.GetFile() || location.Configuration != mCanvas.GetConfiguration())
    {
        return;
    }
    location.View = mCanvas.CaptureViewState();
    location.HasView = true;
    // Keep ancestor snapshots current without overwriting another visit's own saved view.
    for (Entry& entry : mEntries)
    {
        if (entry.Path.size() <= current.Path.size())
        {
            continue;
        }
        bool hasPrefix = true;
        for (qsizetype index = 0; index < current.Path.size(); ++index)
        {
            hasPrefix = hasPrefix == true && HasSameLocation(entry.Path.at(index), current.Path.at(index));
        }
        if (hasPrefix == true)
        {
            entry.Path[current.Path.size() - 1].View = location.View;
            entry.Path[current.Path.size() - 1].HasView = true;
        }
    }
}

bool RenderGraphHistory::reconcile(Entry& inoutEntry, RenderGraphHistoryRecordCache& inoutRecords) const
{
    if (inoutEntry.Path.isEmpty() == true || inoutRecords.Get(inoutEntry.Path.last()).isEmpty() == true)
    {
        return false;
    }
    bool isValidPath = true;
    for (qsizetype index = 1; index < inoutEntry.Path.size(); ++index)
    {
        RenderGraphHistoryLocation& parent = inoutEntry.Path[index - 1];
        RenderGraphHistoryLocation& child = inoutEntry.Path[index];
        const QJsonArray nodes = inoutRecords.Get(parent).value(QStringLiteral("nodes")).toArray();
        QJsonObject exact;
        QStringList candidates;
        for (const QJsonValueConstRef value : nodes)
        {
            const QJsonObject node = value.toObject();
            if (node.value(QStringLiteral("id")).toString() == child.Instance)
            {
                exact = node;
            }
            if (HasBinding(node, child) == true)
            {
                candidates.append(node.value(QStringLiteral("id")).toString());
            }
        }
        if (exact.isEmpty() == false)
        {
            isValidPath = HasBinding(exact, child);
        }
        else if (candidates.size() == 1)
        {
            const QString oldInstance = child.Instance;
            child.Instance = candidates.first();
            for (QString& selected : parent.View.Selection)
            {
                if (selected == oldInstance)
                {
                    selected = child.Instance;
                }
            }
        }
        else
        {
            isValidPath = false;
        }
        if (isValidPath == false)
        {
            break;
        }
    }
    if (isValidPath == false || inoutRecords.Get(inoutEntry.Path.first()).isEmpty() == true)
    {
        RenderGraphHistoryLocation direct = inoutEntry.Path.last();
        direct.Instance.clear();
        inoutEntry.Path = {direct};
    }
    return true;
}

void RenderGraphHistory::prune()
{
    RenderGraphHistoryRecordCache records(mDocument);
    QVector<Entry> valid;
    int32 current = -1;
    for (int32 index = 0; index < mEntries.size(); ++index)
    {
        Entry entry = mEntries.at(index);
        if (reconcile(entry, records) == true)
        {
            if (index <= mCurrent)
            {
                current = static_cast<int32>(valid.size());
            }
            valid.append(entry);
        }
    }
    mEntries = valid;
    mCurrent = current;
}

bool RenderGraphHistory::PrepareSubgraph(const QString& instance, const QString& file, const QString& configuration)
{
    mPreparedInstance.clear();
    mPreparedFile.clear();
    mPreparedConfiguration.clear();
    prune();
    const RenderGraphHistoryLocation parent{mCanvas.GetFile(), mCanvas.GetConfiguration(), {}, {}, false};
    const RenderGraphHistoryLocation child{file, configuration, instance, {}, false};
    for (const QJsonValueConstRef value : GetRecord(mDocument, parent).value(QStringLiteral("nodes")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == instance &&
            HasBinding(value.toObject(), child) == true && hasConfiguration(file, configuration) == true)
        {
            if (mCurrent >= 0 && mEntries.at(mCurrent).Path.size() >= MAX_PATH_DEPTH)
            {
                Q_EMIT NavigationFailed(
                    QStringLiteral("Subgraph navigation is limited to %1 levels.").arg(MAX_PATH_DEPTH));
                return false;
            }
            mPreparedInstance = instance;
            mPreparedFile = file;
            mPreparedConfiguration = configuration;
            return true;
        }
    }
    Q_EMIT NavigationFailed(QStringLiteral("The Subgraph instance or its configuration is no longer available."));
    return false;
}

void RenderGraphHistory::PrepareDiagnosticVisit()
{
    mShouldRecordDiagnostic = true;
    mPreparedInstance.clear();
    mPreparedFile.clear();
    mPreparedConfiguration.clear();
}

void RenderGraphHistory::BeforeShow(const QString& file, const QString& configuration)
{
    if (mIsRestoring == true)
    {
        return;
    }
    mPending = {};
    const bool shouldRecordDiagnostic = mShouldRecordDiagnostic;
    mShouldRecordDiagnostic = false;
    if (hasConfiguration(file, configuration) == false)
    {
        if (file.isEmpty() == true)
        {
            Reset();
        }
        return;
    }
    saveCurrentView();
    prune();
    const bool isSubgraph =
        mPreparedInstance.isEmpty() == false && file == mPreparedFile && configuration == mPreparedConfiguration;
    if (isSubgraph == true && mCurrent >= 0)
    {
        mPending = mEntries.at(mCurrent);
    }
    else if (file == mCanvas.GetFile() && configuration == mCanvas.GetConfiguration() && mCurrent >= 0)
    {
        if (shouldRecordDiagnostic == true)
        {
            mPending = mEntries.at(mCurrent);
        }
        return;
    }
    mPending.Path.append({file, configuration, isSubgraph == true ? mPreparedInstance : QString(), {}, false});
    mPreparedInstance.clear();
    mPreparedFile.clear();
    mPreparedConfiguration.clear();
}

void RenderGraphHistory::appendVisit(const Entry& entry)
{
    mEntries.resize(mCurrent + 1);
    mEntries.append(entry);
    if (mEntries.size() > MAX_ENTRIES)
    {
        mEntries.removeFirst();
    }
    mCurrent = static_cast<int32>(mEntries.size()) - 1;
    refresh();
}

void RenderGraphHistory::AfterShow()
{
    if (mIsRestoring == true)
    {
        return;
    }
    if (mPending.Path.isEmpty() == false)
    {
        const RenderGraphHistoryLocation& target = mPending.Path.last();
        if (target.File == mCanvas.GetFile() && target.Configuration == mCanvas.GetConfiguration())
        {
            appendVisit(mPending);
        }
        mPending = {};
    }
    refresh();
}

bool RenderGraphHistory::showEntry(const Entry& entry)
{
    const RenderGraphHistoryLocation& target = entry.Path.last();
    mIsRestoring = true;
    Q_EMIT LocationRequested(target.File,
                             target.Configuration,
                             target.HasView == true ? target.View.Selection : QStringList{});
    const bool hasShown = mCanvas.GetFile() == target.File && mCanvas.GetConfiguration() == target.Configuration;
    const bool hasRestored =
        hasShown == true && (target.HasView == false || mCanvas.RestoreViewState(target.View) == true);
    mIsRestoring = false;
    if (hasRestored == false)
    {
        Q_EMIT NavigationFailed(QStringLiteral("The navigation target could not be restored."));
    }
    return hasRestored;
}

bool RenderGraphHistory::step(int32 direction)
{
    saveCurrentView();
    prune();
    int32 next = mCurrent + direction;
    if (mCurrent >= 0 && direction < 0 &&
        (mEntries.at(mCurrent).Path.last().File != mCanvas.GetFile() ||
         mEntries.at(mCurrent).Path.last().Configuration != mCanvas.GetConfiguration()))
    {
        next = mCurrent;
    }
    if (next < 0 || next >= mEntries.size())
    {
        refresh();
        return false;
    }
    const Entry target = mEntries.at(next);
    if (showEntry(target) == false)
    {
        return false;
    }
    mCurrent = next;
    refresh();
    mCanvas.setFocus();
    return true;
}

bool RenderGraphHistory::Back()
{
    return step(-1);
}

bool RenderGraphHistory::Forward()
{
    return step(1);
}

bool RenderGraphHistory::VisitAncestor(int32 index)
{
    saveCurrentView();
    prune();
    if (mCurrent < 0 || index < 0 || index >= mEntries.at(mCurrent).Path.size() - 1)
    {
        return false;
    }
    Entry target = mEntries.at(mCurrent);
    target.Path.resize(index + 1);
    if (showEntry(target) == false)
    {
        return false;
    }
    appendVisit(target);
    mCanvas.setFocus();
    return true;
}

QVector<RenderGraphHistoryLocation> RenderGraphHistory::GetPath() const
{
    return mCurrent >= 0 ? mEntries.at(mCurrent).Path : QVector<RenderGraphHistoryLocation>{};
}

int32 RenderGraphHistory::GetEntryCount() const noexcept
{
    return static_cast<int32>(mEntries.size());
}

QString RenderGraphHistory::describe(const RenderGraphHistoryLocation& location) const
{
    QJsonObject graph;
    static_cast<void>(mDocument.GetGraph(location.File, graph));
    const QString name = location.Instance.isEmpty() == false
                             ? location.Instance
                             : graph.value(QStringLiteral("id")).toString(QFileInfo(location.File).fileName());
    return QStringLiteral("%1#%2").arg(name, location.Configuration);
}

void RenderGraphHistory::refresh()
{
    ++mGeneration;
    mBack->setEnabled(mCurrent > 0);
    mForward->setEnabled(mCurrent >= 0 && mCurrent + 1 < mEntries.size());
    mBack->setToolTip(QStringLiteral("Back (%1) · restore the previous graph view")
                          .arg(mBack->shortcut().toString(QKeySequence::NativeText)));
    mForward->setToolTip(QStringLiteral("Forward (%1) · restore the next graph view")
                             .arg(mForward->shortcut().toString(QKeySequence::NativeText)));
    for (QAction* action : std::as_const(mBreadcrumbs))
    {
        removeAction(action);
        action->setObjectName({});
        action->deleteLater();
    }
    mBreadcrumbs.clear();
    const QVector<RenderGraphHistoryLocation> path = GetPath();
    QStringList names;
    for (int32 index = 0; index < path.size(); ++index)
    {
        const RenderGraphHistoryLocation& location = path.at(index);
        const QString name = describe(location);
        names.append(name);
        QAction* action = addAction(index == 0 ? name : QStringLiteral("› ") + name);
        action->setObjectName(QStringLiteral("renderGraphBreadcrumb%1").arg(index));
        const bool isCurrent = index + 1 == path.size();
        action->setToolTip((isCurrent == true ? QStringLiteral("Current location: ") : QStringLiteral("Return to: ")) +
                           QStringLiteral("%1#%2").arg(location.File, location.Configuration));
        action->setCheckable(isCurrent);
        action->setChecked(isCurrent);
        QFont font = action->font();
        font.setBold(isCurrent);
        action->setFont(font);
        const uint64 generation = mGeneration;
        connect(action, &QAction::triggered, this, [this, index, generation, action, isCurrent]() {
            if (generation == mGeneration)
            {
                if (isCurrent == true)
                {
                    action->setChecked(true);
                }
                else
                {
                    static_cast<void>(VisitAncestor(index));
                }
            }
        });
        mBreadcrumbs.append(action);
    }
    setAccessibleDescription(names.join(QStringLiteral(" → ")));
}
} // namespace ludus::editor
