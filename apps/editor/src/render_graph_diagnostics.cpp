#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_document.h"

#include <QAction>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QToolBar>
#include <QWidget>

namespace ludus::editor
{
using namespace foundation;

namespace
{
QJsonObject ResolveTarget(const RenderGraphDocument& document,
                          const QJsonObject& location,
                          QHash<QString, QJsonObject>& inoutGraphs)
{
    const QString source = QDir::cleanPath(location.value(QStringLiteral("source")).toString());
    if (source.endsWith(QStringLiteral(".lurender.json")) == false ||
        location.contains(QStringLiteral("line")) == true || location.contains(QStringLiteral("generated")) == true ||
        document.GetFiles().contains(source) == false)
    {
        return {};
    }
    if (inoutGraphs.contains(source) == false)
    {
        QJsonObject graph;
        static_cast<void>(document.GetGraph(source, graph));
        inoutGraphs.insert(source, graph);
    }
    const QJsonObject graph = inoutGraphs.value(source);
    if (graph.isEmpty() == true)
    {
        return {};
    }
    const QString configuration = location.contains(QStringLiteral("configuration")) == true
                                      ? location.value(QStringLiteral("configuration")).toString()
                                      : graph.value(QStringLiteral("default")).toString();
    for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
    {
        const QJsonObject record = value.toObject();
        if (record.value(QStringLiteral("id")).toString() != configuration)
        {
            continue;
        }
        const QString node = location.value(QStringLiteral("node")).toString();
        bool hasOwner = node.isEmpty();
        if (node == QStringLiteral("input") || node == QStringLiteral("#outputs"))
        {
            hasOwner =
                record.value(node == QStringLiteral("input") ? QStringLiteral("inputs") : QStringLiteral("outputs"))
                    .toArray()
                    .isEmpty() == false;
        }
        else
        {
            for (const QJsonValueConstRef candidate : record.value(QStringLiteral("nodes")).toArray())
            {
                hasOwner = hasOwner == true || candidate.toObject().value(QStringLiteral("id")).toString() == node;
            }
        }
        if (hasOwner == false)
        {
            return {};
        }
        QJsonObject target = location;
        target.insert(QStringLiteral("source"), source);
        target.insert(QStringLiteral("configuration"), configuration);
        return target;
    }
    return {};
}

} // namespace

QJsonObject ResolveRenderGraphDiagnosticTarget(const RenderGraphDocument& document, const QJsonObject& location)
{
    QHash<QString, QJsonObject> graphs;
    return ResolveTarget(document, location, graphs);
}

RenderGraphDiagnostics::RenderGraphDiagnostics(const RenderGraphDocument& document,
                                               QToolBar& inoutToolbar,
                                               QWidget& inoutCanvas)
    : QObject(&inoutCanvas), mDocument(document), mPrevious(new QAction(QStringLiteral("Previous"), this)),
      mNext(new QAction(QStringLiteral("Next"), this)), mCounts(new QLabel(&inoutToolbar)),
      mState(new QLabel(&inoutToolbar))
{
    setObjectName(QStringLiteral("renderGraphDiagnostics"));
    mCounts->setObjectName(QStringLiteral("renderGraphDiagnosticCounts"));
    mCounts->setAccessibleName(QStringLiteral("Render graph report error and warning counts"));
    mState->setObjectName(QStringLiteral("renderGraphDiagnosticState"));
    mState->setAccessibleName(QStringLiteral("Diagnostic position and source freshness"));
    mPrevious->setObjectName(QStringLiteral("renderGraphPreviousDiagnostic"));
    mNext->setObjectName(QStringLiteral("renderGraphNextDiagnostic"));
    mPrevious->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8));
    mNext->setShortcut(QKeySequence(Qt::Key_F8));
    inoutToolbar.addSeparator();
    inoutToolbar.addWidget(mCounts);
    for (QAction* action : {mPrevious, mNext})
    {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        inoutCanvas.addAction(action);
        inoutToolbar.addAction(action);
    }
    mPrevious->setToolTip(QStringLiteral("Previous canvas diagnostic (%1)")
                              .arg(mPrevious->shortcut().toString(QKeySequence::NativeText)));
    mNext->setToolTip(
        QStringLiteral("Next canvas diagnostic (%1)").arg(mNext->shortcut().toString(QKeySequence::NativeText)));
    inoutToolbar.addWidget(mState);
    connect(mPrevious, &QAction::triggered, this, [this]() { static_cast<void>(Previous()); });
    connect(mNext, &QAction::triggered, this, [this]() { static_cast<void>(Next()); });
    Reset();
}

void RenderGraphDiagnostics::Reset()
{
    mResult = {};
    mProblemRows.clear();
    mAvailableRows.clear();
    mCurrentRow = -1;
    mHasTargetSnapshot = false;
    mErrors = 0;
    mWarnings = 0;
    mFreshness = RenderGraphDiagnosticFreshness::None;
    refreshControls();
}

void RenderGraphDiagnostics::SetResult(const RenderGraphJobResult& result)
{
    Reset();
    mResult = result;
    if (result.IsSuccessful == false && result.IsCancelled == false)
    {
        const int32 count = result.GetDiagnosticCount();
        const int32 first = result.Diagnostics.isEmpty() == false && count == result.Diagnostics.size() + 1 ? 1 : 0;
        for (int32 row = first; row < count; ++row)
        {
            const QString severity = result.GetDiagnostic(row).value(QStringLiteral("severity")).toString().toLower();
            if (severity == QStringLiteral("warning"))
            {
                ++mWarnings;
            }
            else if (severity == QStringLiteral("error") || severity == QStringLiteral("fatal error"))
            {
                ++mErrors;
            }
            else
            {
                continue;
            }
            mProblemRows.append(row);
        }
    }
    Refresh(RenderGraphDiagnosticFreshness::Unverified);
}

void RenderGraphDiagnostics::Refresh(RenderGraphDiagnosticFreshness freshness)
{
    mFreshness = freshness;
    if (mHasTargetSnapshot == true && mTargetRevision == mDocument.GetRevision())
    {
        refreshControls();
        return;
    }
    mTargetRevision = mDocument.GetRevision();
    mHasTargetSnapshot = true;
    mAvailableRows.clear();
    QHash<QString, QJsonObject> graphs;
    for (const int32 row : mProblemRows)
    {
        const QJsonObject location = mResult.GetDiagnostic(row).value(QStringLiteral("location")).toObject();
        if (ResolveTarget(mDocument, location, graphs).isEmpty() == false)
        {
            mAvailableRows.append(row);
        }
    }
    refreshControls();
}

void RenderGraphDiagnostics::refreshControls()
{
    mCounts->setText(QStringLiteral("Errors %1 · Warnings %2").arg(mErrors).arg(mWarnings));
    mCounts->setToolTip(QStringLiteral("Report errors: %1; warnings: %2; available canvas targets: %3. "
                                       "Source-only or unavailable targets remain in Error List.")
                            .arg(mErrors)
                            .arg(mWarnings)
                            .arg(mAvailableRows.size()));
    mNext->setEnabled(mAvailableRows.isEmpty() == false);
    mPrevious->setEnabled(mAvailableRows.isEmpty() == false);
    QString freshness;
    switch (mFreshness)
    {
        case RenderGraphDiagnosticFreshness::None:
            freshness = QStringLiteral("No report");
            break;
        case RenderGraphDiagnosticFreshness::Current:
            freshness = QStringLiteral("Current");
            break;
        case RenderGraphDiagnosticFreshness::Stale:
            freshness = QStringLiteral("Stale");
            break;
        case RenderGraphDiagnosticFreshness::Unverified:
            freshness = QStringLiteral("Unverified");
            break;
    }
    if (mResult.IsCancelled == true && mFreshness != RenderGraphDiagnosticFreshness::None)
    {
        freshness = QStringLiteral("Cancelled");
    }
    const qsizetype index = mAvailableRows.indexOf(mCurrentRow);
    mState->setText(
        QStringLiteral("%1/%2 canvas · %3").arg(index < 0 ? 0 : index + 1).arg(mAvailableRows.size()).arg(freshness));
    mState->setToolTip(
        QStringLiteral("%1. Report: %2 sources, revision %3. Navigation never changes document undo.")
            .arg(freshness,
                 mResult.Request.Source == RenderGraphSource::Draft ? QStringLiteral("draft") : QStringLiteral("saved"))
            .arg(mResult.Request.Revision));
}

bool RenderGraphDiagnostics::step(int32 direction)
{
    if (mIsNavigating == true)
    {
        return false;
    }
    mIsNavigating = true;
    Q_EMIT RefreshRequested();
    if (mAvailableRows.isEmpty() == true)
    {
        mIsNavigating = false;
        return false;
    }
    qsizetype index = mAvailableRows.indexOf(mCurrentRow);
    if (index < 0)
    {
        index = direction > 0 ? 0 : mAvailableRows.size() - 1;
        if (mCurrentRow >= 0)
        {
            if (direction > 0)
            {
                for (qsizetype candidate = 0; candidate < mAvailableRows.size(); ++candidate)
                {
                    if (mAvailableRows.at(candidate) > mCurrentRow)
                    {
                        index = candidate;
                        break;
                    }
                }
            }
            else
            {
                for (qsizetype candidate = mAvailableRows.size(); candidate > 0; --candidate)
                {
                    if (mAvailableRows.at(candidate - 1) < mCurrentRow)
                    {
                        index = candidate - 1;
                        break;
                    }
                }
            }
        }
    }
    else
    {
        index = (index + direction + mAvailableRows.size()) % mAvailableRows.size();
    }
    mCurrentRow = mAvailableRows.at(index);
    refreshControls();
    Q_EMIT DiagnosticRequested(mCurrentRow);
    mIsNavigating = false;
    return true;
}
bool RenderGraphDiagnostics::Next()
{
    return step(1);
}
bool RenderGraphDiagnostics::Previous()
{
    return step(-1);
}
void RenderGraphDiagnostics::SetCurrentRow(int32 row)
{
    if (mProblemRows.contains(row) == true)
    {
        mCurrentRow = row;
        refreshControls();
    }
}
int32 RenderGraphDiagnostics::GetErrorCount() const noexcept
{
    return mErrors;
}
int32 RenderGraphDiagnostics::GetWarningCount() const noexcept
{
    return mWarnings;
}
int32 RenderGraphDiagnostics::GetAvailableCount() const noexcept
{
    return static_cast<int32>(mAvailableRows.size());
}
int32 RenderGraphDiagnostics::GetCurrentRow() const noexcept
{
    return mCurrentRow;
}
RenderGraphDiagnosticFreshness RenderGraphDiagnostics::GetFreshness() const noexcept
{
    return mFreshness;
}
} // namespace ludus::editor
