#include "internal/search_tool_window.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_document_registry.h"
#include "internal/code_file_limits.h"

#include <QCheckBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

// The Search tool window (CE07, design section 8). The results tree is itself
// the Replace in Files preview: every match the user sees is exactly what the
// workspace edit will rewrite, and Replace in Files turns the current results
// into one CodeWorkspaceEdit transaction through the registry. Searching runs on
// the CodeSearchService worker; the window only presents and routes.
namespace ludus::editor
{
CodeAreaSearchScope::CodeAreaSearchScope(CodeDocumentRegistry& inoutRegistry) noexcept : mRegistry(inoutRegistry) {}

void CodeAreaSearchScope::SetProjectRoot(const QString& projectRoot)
{
    mProjectRoot = projectRoot;
}

QStringList CodeAreaSearchScope::GetSearchRoots() const
{
    return mProjectRoot.isEmpty() == true ? QStringList{} : QStringList{mProjectRoot};
}

QStringList CodeAreaSearchScope::GetExcludedPrefixes() const
{
    if (mProjectRoot.isEmpty() == true)
    {
        return {};
    }
    // Skip the whole build-output tree and version control, as the Project
    // Explorer does; both the preset build dir and the render-graph output live
    // under out/.
    return {QDir(mProjectRoot).filePath(QStringLiteral("out")), QDir(mProjectRoot).filePath(QStringLiteral(".git"))};
}

QHash<QString, QByteArray> CodeAreaSearchScope::GetDirtyBuffers() const
{
    return mRegistry.GetDirtyDrafts();
}

namespace
{
using namespace foundation;

// Roles on a tree item carrying a match, so activation and Replace in Files can
// recover the file, the editor line and the match columns without re-searching.
constexpr int32 ROLE_PATH = Qt::UserRole + 1;
constexpr int32 ROLE_LINE = Qt::UserRole + 2;
constexpr int32 ROLE_START_COLUMN = Qt::UserRole + 3;
constexpr int32 ROLE_START_OFFSET = Qt::UserRole + 4;
constexpr int32 ROLE_END_OFFSET = Qt::UserRole + 5;
} // namespace

SearchToolWindow::SearchToolWindow(CodeDocumentRegistry& inoutRegistry,
                                   const CodeSearchScopeProvider& scopeProvider,
                                   QWidget* parentOrNull)
    : QWidget(parentOrNull), mRegistry(inoutRegistry), mScopeProvider(scopeProvider), mQuery(new QLineEdit(this)),
      mReplacement(new QLineEdit(this)), mIsCaseSensitive(new QCheckBox(QStringLiteral("Match case"), this)),
      mIsWholeWord(new QCheckBox(QStringLiteral("Whole word"), this)),
      mIsRegex(new QCheckBox(QStringLiteral("Regex"), this)),
      mReplaceButton(new QPushButton(QStringLiteral("Replace in Files"), this)), mResults(new QTreeWidget(this)),
      mStatus(new QLabel(this))
{
    setObjectName(QStringLiteral("searchToolWindow"));
    QVBoxLayout* layout = new QVBoxLayout(this);

    mQuery->setObjectName(QStringLiteral("searchQuery"));
    mQuery->setPlaceholderText(QStringLiteral("Search"));
    mQuery->setClearButtonEnabled(true);
    QPushButton* search = new QPushButton(QStringLiteral("Search"), this);
    search->setObjectName(QStringLiteral("searchRun"));
    QHBoxLayout* queryRow = new QHBoxLayout;
    queryRow->addWidget(mQuery, 1);
    queryRow->addWidget(search);
    layout->addLayout(queryRow);

    mReplacement->setObjectName(QStringLiteral("searchReplacement"));
    mReplacement->setPlaceholderText(QStringLiteral("Replace"));
    mReplaceButton->setObjectName(QStringLiteral("searchReplaceInFiles"));
    QHBoxLayout* replaceRow = new QHBoxLayout;
    replaceRow->addWidget(mReplacement, 1);
    replaceRow->addWidget(mReplaceButton);
    layout->addLayout(replaceRow);

    mIsRegex->setObjectName(QStringLiteral("searchRegex"));
    mIsWholeWord->setObjectName(QStringLiteral("searchWholeWord"));
    mIsCaseSensitive->setObjectName(QStringLiteral("searchMatchCase"));
    QHBoxLayout* optionRow = new QHBoxLayout;
    optionRow->addWidget(mIsCaseSensitive);
    optionRow->addWidget(mIsWholeWord);
    optionRow->addWidget(mIsRegex);
    optionRow->addStretch(1);
    layout->addLayout(optionRow);

    mResults->setObjectName(QStringLiteral("searchResults"));
    mResults->setColumnCount(1);
    mResults->setHeaderHidden(true);
    mResults->setUniformRowHeights(true);
    layout->addWidget(mResults, 1);

    mStatus->setObjectName(QStringLiteral("searchStatus"));
    layout->addWidget(mStatus);

    connect(search, &QPushButton::clicked, this, &SearchToolWindow::startSearch);
    connect(mQuery, &QLineEdit::returnPressed, this, &SearchToolWindow::startSearch);
    connect(mReplaceButton, &QPushButton::clicked, this, &SearchToolWindow::replaceInFiles);
    connect(mResults, &QTreeWidget::itemActivated, this, &SearchToolWindow::onItemActivated);
    connect(&mService, &CodeSearchService::BatchReady, this, &SearchToolWindow::onBatchReady);
    connect(&mService, &CodeSearchService::Finished, this, &SearchToolWindow::onFinished);
}

void SearchToolWindow::FocusQuery()
{
    mQuery->setFocus();
    mQuery->selectAll();
}

CodeSearchQuery SearchToolWindow::currentQuery() const
{
    return CodeSearchQuery
    {
        .Pattern = mQuery->text(),
        .IsRegex = mIsRegex->isChecked(),
        .IsWholeWord = mIsWholeWord->isChecked(),
        .IsCaseSensitive = mIsCaseSensitive->isChecked(),
    };
}

void SearchToolWindow::startSearch()
{
    if (mQuery->text().isEmpty() == true)
    {
        return;
    }
    mResults->clear();
    mFileCount = 0;
    mMatchCount = 0;
    mWasTruncated = false;
    mStatus->setText(QStringLiteral("Searching..."));
    mService.Start(mScopeProvider.GetSearchRoots(),
                   mScopeProvider.GetExcludedPrefixes(),
                   currentQuery(),
                   mScopeProvider.GetDirtyBuffers());
}

void SearchToolWindow::addFileResult(const CodeSearchFileResult& file)
{
    QTreeWidgetItem* fileItem = new QTreeWidgetItem(mResults);
    fileItem->setText(0, QStringLiteral("%1 (%2)").arg(file.AbsolutePath).arg(file.Matches.size()));
    fileItem->setData(0, ROLE_PATH, file.AbsolutePath);
    fileItem->setExpanded(true);
    for (const CodeSearchMatch& match : file.Matches)
    {
        QTreeWidgetItem* matchItem = new QTreeWidgetItem(fileItem);
        // One-based line, like the gutter and Go to Line, prefixing the line text.
        matchItem->setText(0, QStringLiteral("%1: %2").arg(match.Line + 1).arg(match.LineText));
        matchItem->setData(0, ROLE_PATH, file.AbsolutePath);
        matchItem->setData(0, ROLE_LINE, match.Line);
        matchItem->setData(0, ROLE_START_COLUMN, match.StartColumn);
        matchItem->setData(0, ROLE_START_OFFSET, match.StartOffset);
        matchItem->setData(0, ROLE_END_OFFSET, match.EndOffset);
    }
    ++mFileCount;
    mMatchCount += static_cast<int32>(file.Matches.size());
}

void SearchToolWindow::onBatchReady(const QList<CodeSearchFileResult>& batch)
{
    for (const CodeSearchFileResult& file : batch)
    {
        addFileResult(file);
    }
    updateStatus();
}

void SearchToolWindow::onFinished(bool wasCancelled, bool wasTruncated, const QString& patternErrorOrEmpty)
{
    if (patternErrorOrEmpty.isEmpty() == false)
    {
        mStatus->setText(QStringLiteral("Invalid pattern: %1").arg(patternErrorOrEmpty));
        return;
    }
    mWasTruncated = wasTruncated;
    if (wasCancelled == true)
    {
        mStatus->setText(QStringLiteral("Search cancelled"));
        return;
    }
    updateStatus();
}

void SearchToolWindow::updateStatus()
{
    if (mMatchCount == 0)
    {
        mStatus->setText(QStringLiteral("No results"));
        return;
    }
    QString text = QStringLiteral("%1 matches in %2 files").arg(mMatchCount).arg(mFileCount);
    if (mWasTruncated == true)
    {
        text += QStringLiteral(" (showing the first %1)").arg(CODE_SEARCH_MAX_MATCHES);
    }
    mStatus->setText(text);
}

void SearchToolWindow::onItemActivated(QTreeWidgetItem* item, int32 column)
{
    static_cast<void>(column);
    if (item == nullptr)
    {
        return;
    }
    const QVariant lineData = item->data(0, ROLE_LINE);
    if (lineData.isValid() == false)
    {
        return; // A file header, not a match.
    }
    const QString path = item->data(0, ROLE_PATH).toString();
    const int32 line = lineData.toInt();
    const int32 startColumn = item->data(0, ROLE_START_COLUMN).toInt();
    Q_EMIT OpenRequested(path, line + 1, startColumn + 1);
}

void SearchToolWindow::replaceInFiles()
{
    if (mMatchCount == 0 || mResults->topLevelItemCount() == 0)
    {
        return;
    }
    const QString replacement = mReplacement->text();
    // The current results tree is the preview: turn every file's matches into one
    // file edit, in document order (the matches arrive and are shown in order).
    QList<CodeWorkspaceFileEdit> edits;
    for (int32 fileIndex = 0; fileIndex < mResults->topLevelItemCount(); ++fileIndex)
    {
        const QTreeWidgetItem* fileItem = mResults->topLevelItem(fileIndex);
        CodeWorkspaceFileEdit fileEdit;
        fileEdit.AbsolutePath = fileItem->data(0, ROLE_PATH).toString();
        for (int32 childIndex = 0; childIndex < fileItem->childCount(); ++childIndex)
        {
            const QTreeWidgetItem* matchItem = fileItem->child(childIndex);
            // The match carries its absolute editor positions; the registry maps
            // them to byte offsets against the file's current snapshot.
            const int32 startOffset = matchItem->data(0, ROLE_START_OFFSET).toInt();
            const int32 endOffset = matchItem->data(0, ROLE_END_OFFSET).toInt();
            fileEdit.Replacements.append(CodeWorkspaceReplacement
            {
                .StartPosition = startOffset,
                .EndPosition = endOffset,
                .Replacement = replacement,
            });
        }
        if (fileEdit.Replacements.isEmpty() == false)
        {
            edits.append(fileEdit);
        }
    }
    const CodeWorkspaceEditStatus status = mRegistry.ApplyWorkspaceEdit(edits);
    if (status == CodeWorkspaceEditStatus::Rejected)
    {
        mStatus->setText(QStringLiteral("Nothing was replaced: the edit was rejected (a file changed, is read-only, "
                                        "or the edit is too large). Run the search again."));
        return;
    }
    const int32 replaced = mMatchCount;
    mStatus->setText(status == CodeWorkspaceEditStatus::AppliedButSaveFailed
                         ? QStringLiteral("Replaced %1 matches; some files could not be saved to disk.").arg(replaced)
                         : QStringLiteral("Replaced %1 matches in %2 files.").arg(replaced).arg(mFileCount));
    // Re-run the search so the tree reflects the rewritten files.
    startSearch();
}
} // namespace ludus::editor
