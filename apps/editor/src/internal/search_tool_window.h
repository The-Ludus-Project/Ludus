#pragma once

#include "internal/code_search_service.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace ludus::editor
{
class CodeDocumentRegistry;

/// Supplies the search scope the tool window needs at the moment a search or a
/// Replace in Files starts (design section 8): the authored source roots to
/// search, the absolute path prefixes to skip (the project's `out/` tree and
/// `.git`), and a snapshot of every open file's unsaved draft so edits are
/// searched instead of the stale bytes on disk. The shell implements this over
/// its project model and the code area; tests use a fake. It is queried on the
/// GUI thread and the returned values are copied onto the worker.
class CodeSearchScopeProvider
{
public:
    virtual ~CodeSearchScopeProvider() = default;
    [[nodiscard]] virtual QStringList GetSearchRoots() const = 0;
    [[nodiscard]] virtual QStringList GetExcludedPrefixes() const = 0;
    [[nodiscard]] virtual QHash<QString, QByteArray> GetDirtyBuffers() const = 0;
};

/// A scope provider backed by a CodeEditorArea's registry for dirty buffers and
/// a project root the shell updates on project open/close. The authored root is
/// the one search root for now; the excluded prefixes are that root's `out/`
/// build tree and its `.git` directory, matching the Project Explorer's scan
/// exclusions. An empty root yields no roots, so a search outside a project
/// finds nothing rather than scanning the filesystem.
class CodeAreaSearchScope final : public CodeSearchScopeProvider
{
public:
    explicit CodeAreaSearchScope(CodeDocumentRegistry& inoutRegistry) noexcept;
    /// Set the active authored source root (absolute), or clear it with an empty
    /// string when no project is open. Called from the shell's project chain.
    void SetProjectRoot(const QString& projectRoot);
    [[nodiscard]] QStringList GetSearchRoots() const override;
    [[nodiscard]] QStringList GetExcludedPrefixes() const override;
    [[nodiscard]] QHash<QString, QByteArray> GetDirtyBuffers() const override;

private:
    CodeDocumentRegistry& mRegistry;
    QString mProjectRoot;
};

/// The Search tool window (CE07, design section 8): a query with the same options
/// as the find overlay (regex, whole word, case), a results tree grouped by file,
/// and Replace in Files, which shows the replacement preview in the same tree and
/// applies it as one workspace-edit transaction through the registry. Searching
/// runs on the CodeSearchService worker and streams bounded batches; nothing
/// blocks the UI. GUI thread only.
class SearchToolWindow final : public QWidget
{
    Q_OBJECT
public:
    /// @param registry Applies the Replace in Files workspace edit (CE11).
    /// @param scopeProvider Supplies roots, exclusions and dirty buffers per run;
    /// must outlive the window. Both are borrowed, not owned.
    SearchToolWindow(CodeDocumentRegistry& inoutRegistry,
                     const CodeSearchScopeProvider& scopeProvider,
                     QWidget* parentOrNull = nullptr);

    /// Focus the query field and select it, for the Find in Files command.
    void FocusQuery();

Q_SIGNALS:
    /// The user activated a result; the shell opens `absolutePath` and reveals
    /// the one-based `line` and `column` (CodeEditor::GoToLine coordinates).
    void OpenRequested(const QString& absolutePath, foundation::int32 line, foundation::int32 column);

private:
    void startSearch();
    void onBatchReady(const QList<CodeSearchFileResult>& batch);
    void onFinished(bool wasCancelled, bool wasTruncated, const QString& patternErrorOrEmpty);
    void onItemActivated(QTreeWidgetItem* item, foundation::int32 column);
    void replaceInFiles();
    [[nodiscard]] CodeSearchQuery currentQuery() const;
    void addFileResult(const CodeSearchFileResult& file);
    void updateStatus();

    CodeDocumentRegistry& mRegistry;
    const CodeSearchScopeProvider& mScopeProvider;
    CodeSearchService mService;
    QLineEdit* mQuery;
    QLineEdit* mReplacement;
    QCheckBox* mIsCaseSensitive;
    QCheckBox* mIsWholeWord;
    QCheckBox* mIsRegex;
    QPushButton* mReplaceButton;
    QTreeWidget* mResults;
    QLabel* mStatus;
    foundation::int32 mFileCount = 0;
    foundation::int32 mMatchCount = 0;
    bool mWasTruncated = false;
};
} // namespace ludus::editor
