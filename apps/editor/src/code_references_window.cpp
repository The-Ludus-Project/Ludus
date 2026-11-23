#include "internal/code_references_window.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_editor.h"

#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 RESULT_ROLE = Qt::UserRole;
constexpr int32 NO_RESULT = -1;

/// "line 3, column 7  return Value();" for a location in its captured text.
QString DescribeLocation(const CodeNavigationTarget& target)
{
    const QStringView text(target.Text);
    const int32 start = qBound(0, target.Start, static_cast<int32>(text.size()));
    const int32 lineStart = static_cast<int32>(text.first(start).lastIndexOf(QLatin1Char('\n'))) + 1;
    qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), start);
    lineEnd = lineEnd < 0 ? text.size() : lineEnd;
    const int32 line = static_cast<int32>(text.first(start).count(QLatin1Char('\n'))) + 1;
    const QString preview =
        text.sliced(lineStart, lineEnd - lineStart).trimmed().left(CodeReferencesWindow::MAX_PREVIEW_UNITS).toString();
    return QStringLiteral("line %1, column %2   %3").arg(line).arg(start - lineStart + 1).arg(preview);
}
} // namespace

CodeReferencesWindow::CodeReferencesWindow(QWidget* parentOrNull)
    : QWidget(parentOrNull), mSummary(new QLabel(this)), mStatus(new QLabel(this)),
      mFindAgain(new QPushButton(QStringLiteral("Find Again"), this)), mResults(new QTreeWidget(this))
{
    setObjectName(QStringLiteral("codeReferencesWindow"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* header = new QHBoxLayout;
    mSummary->setObjectName(QStringLiteral("codeReferencesSummary"));
    mSummary->setText(QStringLiteral("Run Find References (Shift+F12) on a name in a shader to list its uses here."));
    mSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mSummary->setWordWrap(true);
    header->addWidget(mSummary, 1);
    mFindAgain->setObjectName(QStringLiteral("codeReferencesFindAgain"));
    mFindAgain->setToolTip(QStringLiteral("Find references to the same symbol in the current text."));
    mFindAgain->setEnabled(false);
    header->addWidget(mFindAgain);
    layout->addLayout(header);
    mStatus->setObjectName(QStringLiteral("codeReferencesStatus"));
    mStatus->setWordWrap(true);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mStatus->hide();
    layout->addWidget(mStatus);
    mResults->setObjectName(QStringLiteral("codeReferencesResults"));
    mResults->setHeaderHidden(true);
    mResults->setRootIsDecorated(true);
    mResults->setUniformRowHeights(true);
    mResults->setAccessibleName(QStringLiteral("References"));
    layout->addWidget(mResults, 1);
    connect(mFindAgain, &QPushButton::clicked, this, [this]() { static_cast<void>(FindAgain()); });
    connect(mResults, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int32) { activateItem(item); });
}

void CodeReferencesWindow::Show(CodeNavigation& inoutOrigin, const CodeReferenceResults& results)
{
    mOrigin = &inoutOrigin;
    CodeEditor& editor = inoutOrigin.GetEditor();
    mOriginCursor = QTextCursor(editor.document());
    mOriginCursor.setPosition(editor.textCursor().position());
    mOriginRevision = editor.document()->revision();
    mTargets = results.Targets;
    const QString symbol =
        results.Symbol.isEmpty() == true ? QStringLiteral("the selection") : QStringLiteral("'%1'").arg(results.Symbol);
    mSummary->setText(mTargets.isEmpty() == true
                          ? QStringLiteral("No references to %1. %2").arg(symbol, results.Message)
                      : mTargets.size() == 1 ? QStringLiteral("1 reference to %1").arg(symbol)
                                             : QStringLiteral("%1 references to %2").arg(mTargets.size()).arg(symbol));
    mStatus->clear();
    mStatus->hide();
    mFindAgain->setEnabled(true);
    mResults->clear();
    QHash<QString, QTreeWidgetItem*> groups;
    for (qsizetype index = 0; index < mTargets.size(); ++index)
    {
        const CodeNavigationTarget& target = mTargets.at(index);
        QString group = target.File;
        if (target.IsReadOnly == true)
        {
            group = QStringLiteral("Shader context (read-only)");
        }
        else if (group.isEmpty() == true)
        {
            group =
                inoutOrigin.GetSource().isEmpty() == true ? QStringLiteral("Current source") : inoutOrigin.GetSource();
        }
        QTreeWidgetItem* parent = groups.value(group, nullptr);
        if (parent == nullptr)
        {
            parent = new QTreeWidgetItem(mResults, {group});
            parent->setData(0, RESULT_ROLE, NO_RESULT);
            parent->setExpanded(true);
            groups.insert(group, parent);
        }
        const QString text = target.IsReadOnly == true ? target.Label : DescribeLocation(target);
        QTreeWidgetItem* row = new QTreeWidgetItem(parent, {text});
        row->setData(0, RESULT_ROLE, static_cast<int32>(index));
        row->setToolTip(0, target.Label);
    }
    for (QTreeWidgetItem* parent : groups)
    {
        parent->setText(0, QStringLiteral("%1 (%2)").arg(parent->text(0)).arg(parent->childCount()));
    }
    Q_EMIT ResultsShown();
}

bool CodeReferencesWindow::IsOutOfDate() const
{
    return mOrigin.isNull() == false && mOrigin->GetEditor().document()->revision() != mOriginRevision;
}

QString CodeReferencesWindow::GetSummary() const
{
    return mSummary->text();
}

QString CodeReferencesWindow::GetStatus() const
{
    return mStatus->text();
}

void CodeReferencesWindow::report(const QString& message)
{
    mStatus->setText(message);
    mStatus->setVisible(message.isEmpty() == false);
}

void CodeReferencesWindow::activateItem(const QTreeWidgetItem* item)
{
    const int32 index = item == nullptr ? NO_RESULT : item->data(0, RESULT_ROLE).toInt();
    if (index != NO_RESULT)
    {
        static_cast<void>(ActivateResult(index));
    }
}

bool CodeReferencesWindow::ActivateResult(int32 index)
{
    if (index < 0 || index >= mTargets.size())
    {
        return false;
    }
    if (mOrigin.isNull() == true)
    {
        mFindAgain->setEnabled(false);
        report(QStringLiteral("The editor that found these references has closed. Open the shader and run Find "
                              "References again."));
        return false;
    }
    if (IsOutOfDate() == true)
    {
        report(QStringLiteral("The source changed after these references were found, so their positions may be "
                              "wrong. Choose Find Again to search from the same symbol."));
        return false;
    }
    if (mOrigin->RevealView() == false || mOrigin->Navigate(mTargets.at(index)) == false)
    {
        report(QStringLiteral("The location could not be opened. Choose Find Again to refresh the list."));
        return false;
    }
    report({});
    return true;
}

bool CodeReferencesWindow::FindAgain()
{
    if (mOrigin.isNull() == true || mOriginCursor.isNull() == true)
    {
        mFindAgain->setEnabled(false);
        report(QStringLiteral("The editor that found these references has closed. Open the shader and run Find "
                              "References again."));
        return false;
    }
    if (mOrigin->RevealView() == false)
    {
        report(QStringLiteral("The shader could not be shown. Open it and run Find References again."));
        return false;
    }
    CodeEditor& editor = mOrigin->GetEditor();
    QTextCursor cursor(editor.document());
    cursor.setPosition(mOriginCursor.position());
    editor.setTextCursor(cursor);
    editor.setFocus();
    if (mOrigin->RequestReferences() == false)
    {
        const QString reason = mOrigin->GetReferencesUnavailableReason();
        report(reason.isEmpty() == true ? QStringLiteral("Find References is unavailable for this source.") : reason);
        return false;
    }
    report({});
    return true;
}
} // namespace ludus::editor
