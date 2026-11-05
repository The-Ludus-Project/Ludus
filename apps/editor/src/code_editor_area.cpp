#include "internal/code_editor_area.h"

#include "internal/code_conflict_banner.h"
#include "internal/code_document_tab.h"
#include "internal/code_editor.h"
#include "internal/code_file_document.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>

// The central code-tab area (CE01, CE02): per-file tabs in up to two editor
// groups over the shared CodeDocumentRegistry, with preview tabs and mirroring.
// All UI-thread. Hosts are owned by the registry; this area owns the tab widgets
// and releases each tab's host reference on close.

namespace ludus::editor
{
using foundation::int32;
using foundation::uint64;

namespace
{
/// The most editor groups M1 supports side by side.
constexpr int32 MAX_CODE_GROUPS = 2;

constexpr const char* STATE_TABS = "tabs";
constexpr const char* STATE_GROUP = "group";
constexpr const char* STATE_ACTIVE = "active";

[[nodiscard]] QString titleFor(const QString& absolutePath)
{
    const QFileInfo info(absolutePath);
    return info.fileName();
}
} // namespace

CodeEditorArea::CodeEditorArea(QWidget* parentOrNull) : QWidget(parentOrNull), mRegistry(this)
{
    setObjectName(QStringLiteral("codeEditorArea"));
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mSplitter = new QSplitter(Qt::Horizontal, this);
    mSplitter->setObjectName(QStringLiteral("codeEditorAreaSplitter"));
    layout->addWidget(mSplitter);
    // One group always exists.
    static_cast<void>(createGroup());
    // A file changed on disk: a clean buffer reloads, a dirty one shows the
    // conflict bar (CE04). Returning to the editor re-checks every open file,
    // which also covers late file-system events and the browser editor.
    connect(&mExternalChanges, &CodeExternalChangeWatch::FileMayHaveChanged, this, [this](const QString& path) {
        if (CodeFileDocument* host = mRegistry.FindHostOrNull(path); host != nullptr)
        {
            checkExternalChange(host);
        }
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
        {
            CheckExternalChanges();
        }
    });
}

CodeEditorArea::~CodeEditorArea()
{
    // Tabs borrow their hosts from mRegistry, but QWidget::~QWidget deletes the
    // tab widgets only after every member, including the registry, is gone.
    // Hiding a shown area during that teardown sends focus-out to a code editor,
    // whose controller then touches its freed host. Destroy the views first,
    // while the registry is alive, without running this area's slots on the way.
    const QList<QObject*> descendants = mSplitter->findChildren<QObject*>();
    for (QObject* descendant : descendants)
    {
        QObject::disconnect(descendant, nullptr, this, nullptr);
    }
    mGroups.clear();
    mTabHosts.clear();
    delete mSplitter;
    mSplitter = nullptr;
}

QTabWidget* CodeEditorArea::createGroup()
{
    QTabWidget* tabs = new QTabWidget(mSplitter);
    tabs->setObjectName(QStringLiteral("codeEditorGroup%1").arg(mGroups.size()));
    tabs->setTabsClosable(true);
    tabs->setMovable(true);
    tabs->setDocumentMode(true);
    mSplitter->addWidget(tabs);
    Group group;
    group.Tabs = tabs;
    mGroups.append(group);
    connect(tabs, &QTabWidget::currentChanged, this, [this]() { onCurrentChanged(); });
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, tabs](int index) {
        CodeDocumentTab* tab = qobject_cast<CodeDocumentTab*>(tabs->widget(index));
        if (tab == nullptr)
        {
            return;
        }
        if (isLastDirtyView(tab) == true)
        {
            Q_EMIT DirtyCloseRequested(tab);
            return;
        }
        removeTab(tab);
    });
    connect(tabs, &QTabWidget::tabBarDoubleClicked, this, [this, tabs](int index) {
        CodeDocumentTab* tab = qobject_cast<CodeDocumentTab*>(tabs->widget(index));
        if (tab != nullptr)
        {
            // A double-click makes a preview tab permanent (CE01).
            tab->MakePermanent();
            updateTabDecoration(tab);
        }
    });
    return tabs;
}

int32 CodeEditorArea::activeGroupIndex() const
{
    // The group containing the widget with focus, else group 0.
    QWidget* focus = focusWidget();
    for (int32 index = 0; index < mGroups.size(); ++index)
    {
        if (mGroups.at(index).Tabs->isAncestorOf(focus) == true)
        {
            return index;
        }
    }
    return 0;
}

void CodeEditorArea::SetProjectEpoch(uint64 epoch)
{
    if (epoch == mRegistry.GetProjectEpoch())
    {
        return;
    }
    // Close every open tab: their hosts belong to the old epoch. A fresh open
    // after the epoch bump reloads them against the new project.
    const QList<CodeDocumentTab*> tabs = GetTabs();
    for (CodeDocumentTab* tab : tabs)
    {
        removeTab(tab);
    }
    mRegistry.SetProjectEpoch(epoch);
}

void CodeEditorArea::SetProject(const QString& projectRoot, uint64 epoch)
{
    // Make the outgoing project's journal current (clean buffers removed, dirty
    // drafts written) before its hosts are dropped.
    mRegistry.FlushRecoveryNow();
    SetProjectEpoch(epoch);
    // Bind recovery after the epoch bump so the journal is read against the new
    // project, not the one whose hosts were just dropped.
    mRegistry.BindRecoveryProject(projectRoot);
}

void CodeEditorArea::RestoreRecoveredBuffers()
{
    const QVector<CodeRecoveredBuffer> buffers = mRegistry.TakeRecoveredBuffers();
    for (const CodeRecoveredBuffer& buffer : buffers)
    {
        if (buffer.CanonicalPath.isEmpty() == true || QFileInfo::exists(buffer.CanonicalPath) == false)
        {
            // A journal for a deleted file: nothing to reopen, nothing to lose.
            continue;
        }
        CodeFileDocument* host = mRegistry.FindHostOrNull(buffer.CanonicalPath);
        if (host == nullptr)
        {
            // The file was not reopened from the saved layout; open a permanent
            // tab so the recovered draft is visible rather than silently applied.
            CodeDocumentTab* tab = openIn(activeGroupIndex(), buffer.CanonicalPath, false);
            if (tab == nullptr)
            {
                continue;
            }
            host = hostOf(tab);
        }
        if (host == nullptr)
        {
            continue;
        }
        const CodeRecoveredDraftStatus status = host->RestoreRecoveredDraft(buffer.SavedHash, buffer.Draft);
        if (status == CodeRecoveredDraftStatus::Unchanged)
        {
            continue;
        }
        // Push the recovered bytes into every open view of the host and mark the
        // tab dirty, matching an in-editor edit.
        for (CodeDocumentTab* tab : viewsOf(host))
        {
            tab->MirrorFromHost();
            tab->MakePermanent();
            updateTabDecoration(tab);
            if (status == CodeRecoveredDraftStatus::RestoredOverChangedFile)
            {
                // The file changed after the draft was journaled: keep the draft
                // and let the user choose between it and the file.
                tab->GetBanner().ShowRecoveredConflict(titleFor(tab->GetPath()));
            }
        }
    }
    // Rewrite the restored drafts and drop the entries that were not restored,
    // off the UI thread. Until that finishes the old journal files remain, so a
    // second crash still recovers the drafts.
    mRegistry.StartRecoveryCheckpoint();
    Q_EMIT TabsChanged();
    onCurrentChanged();
}

void CodeEditorArea::FlushRecovery()
{
    mRegistry.FlushRecoveryNow();
}

CodeDocumentTab* CodeEditorArea::findTabInGroup(int32 group, const QString& path) const
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    QTabWidget* tabs = mGroups.at(group).Tabs;
    for (int index = 0; index < tabs->count(); ++index)
    {
        CodeDocumentTab* tab = qobject_cast<CodeDocumentTab*>(tabs->widget(index));
        if (tab != nullptr && QFileInfo(tab->GetPath()).canonicalFilePath() == canonical)
        {
            return tab;
        }
    }
    return nullptr;
}

CodeFileDocument* CodeEditorArea::hostOf(CodeDocumentTab* tab) const
{
    return mTabHosts.value(tab, nullptr);
}

void CodeEditorArea::addTabToGroup(int32 group, CodeDocumentTab* tab, bool isPreview)
{
    QTabWidget* tabs = mGroups.at(group).Tabs;
    tab->SetPreview(isPreview);
    const int index = tabs->addTab(tab, titleFor(tab->GetPath()));
    tabs->setCurrentIndex(index);
    updateTabDecoration(tab);

    connect(tab, &CodeDocumentTab::Edited, this, [this, tab]() {
        mirrorEdit(tab);
        updateTabDecoration(tab);
        // Journal the unsaved draft off the UI thread on an idle debounce (CE03).
        mRegistry.ScheduleRecoveryCheckpoint();
        Q_EMIT TabsChanged();
    });
    CodeConflictBanner& banner = tab->GetBanner();
    connect(&banner, &CodeConflictBanner::CompareRequested, this, [this, tab]() { showComparison(tab); });
    connect(&banner, &CodeConflictBanner::KeepMineRequested, this, [this, tab]() {
        if (CodeFileDocument* host = hostOf(tab); host != nullptr)
        {
            keepDraftOverDisk(host);
        }
    });
    connect(&banner, &CodeConflictBanner::ReloadRequested, this, [this, tab]() {
        if (CodeFileDocument* host = hostOf(tab); host != nullptr)
        {
            reloadFromDisk(host);
        }
    });
    connect(tab, &CodeDocumentTab::BecamePermanent, this, [this, group, tab]() {
        if (group < mGroups.size() && mGroups.at(group).PreviewOrNull == tab)
        {
            mGroups[group].PreviewOrNull = nullptr;
        }
        updateTabDecoration(tab);
    });
    if (isPreview == true)
    {
        mGroups[group].PreviewOrNull = tab;
    }
}

void CodeEditorArea::removeTab(CodeDocumentTab* tab)
{
    if (tab == nullptr)
    {
        return;
    }
    for (int32 groupIndex = 0; groupIndex < mGroups.size(); ++groupIndex)
    {
        Group& group = mGroups[groupIndex];
        const int index = group.Tabs->indexOf(tab);
        if (index < 0)
        {
            continue;
        }
        if (group.PreviewOrNull == tab)
        {
            group.PreviewOrNull = nullptr;
        }
        group.Tabs->removeTab(index);
        break;
    }
    CodeFileDocument* host = mTabHosts.value(tab, nullptr);
    mTabHosts.remove(tab);
    tab->deleteLater();
    if (host != nullptr)
    {
        mExternalChanges.Unwatch(host->GetCanonicalPath());
        mRegistry.ReleaseHost(host);
    }
    Q_EMIT TabsChanged();
    onCurrentChanged();
}

void CodeEditorArea::mirrorEdit(CodeDocumentTab* source)
{
    CodeFileDocument* host = hostOf(source);
    if (host == nullptr)
    {
        return;
    }
    // Re-synchronize every other open view of the same host, so split groups
    // stay identical. The controller guard stops a mirrored delta being re-sent.
    for (CodeDocumentTab* tab : GetTabs())
    {
        if (tab != source && hostOf(tab) == host)
        {
            tab->MirrorFromHost();
            updateTabDecoration(tab);
        }
    }
}

void CodeEditorArea::updateTabDecoration(CodeDocumentTab* tab)
{
    for (const Group& group : mGroups)
    {
        const int index = group.Tabs->indexOf(tab);
        if (index < 0)
        {
            continue;
        }
        QString title = titleFor(tab->GetPath());
        if (tab->IsDirty() == true)
        {
            title.prepend(QStringLiteral("\u25cf "));
        }
        if (tab->IsReadOnly() == true)
        {
            title.append(QStringLiteral(" \U0001f512"));
        }
        if (tab->IsPreview() == true)
        {
            // Mark a preview tab so its title reads as provisional, like VS
            // Code's italic preview title (text-only so it needs no stylesheet).
            title.append(QStringLiteral(" \u2013"));
        }
        group.Tabs->setTabText(index, title);
        return;
    }
}

void CodeEditorArea::onCurrentChanged()
{
    CodeDocumentTab* active = GetActiveTabOrNull();
    if (active != nullptr)
    {
        if (CodeFileDocument* host = hostOf(active); host != nullptr)
        {
            checkExternalChange(host);
        }
    }
    Q_EMIT ActiveTabChanged(active);
}

CodeDocumentTab* CodeEditorArea::openIn(int32 group, const QString& absolutePath, bool isPreview)
{
    // Reuse an existing tab of this file in the group; a permanent open promotes
    // a reused preview tab.
    if (CodeDocumentTab* existing = findTabInGroup(group, absolutePath); existing != nullptr)
    {
        if (isPreview == false)
        {
            existing->MakePermanent();
        }
        mGroups.at(group).Tabs->setCurrentWidget(existing);
        existing->GetEditor().setFocus();
        updateTabDecoration(existing);
        return existing;
    }
    CodeFileLoadStatus status = CodeFileLoadStatus::Failed;
    QString explanation;
    CodeFileDocument* host = mRegistry.AcquireHost(absolutePath, status, explanation);
    if (host == nullptr)
    {
        Q_EMIT OpenFailed(absolutePath, explanation);
        return nullptr;
    }
    CodeDocumentTab* tab = new CodeDocumentTab(host, absolutePath, this);
    mTabHosts.insert(tab, host);
    mExternalChanges.Watch(host->GetCanonicalPath());
    // A preview open replaces the group's current preview tab.
    if (isPreview == true && mGroups.at(group).PreviewOrNull != nullptr)
    {
        removeTab(mGroups.at(group).PreviewOrNull);
    }
    addTabToGroup(group, tab, isPreview);
    tab->GetEditor().setFocus();
    Q_EMIT TabsChanged();
    onCurrentChanged();
    return tab;
}

bool CodeEditorArea::OpenFile(const QString& absolutePath, bool isPreview)
{
    return openIn(activeGroupIndex(), absolutePath, isPreview) != nullptr;
}

bool CodeEditorArea::OpenFileInSecondGroup(const QString& absolutePath)
{
    if (mGroups.size() < MAX_CODE_GROUPS)
    {
        static_cast<void>(createGroup());
    }
    return openIn(1, absolutePath, false) != nullptr;
}

void CodeEditorArea::CloseActiveTab()
{
    CodeDocumentTab* tab = GetActiveTabOrNull();
    if (tab == nullptr)
    {
        return;
    }
    if (isLastDirtyView(tab) == true)
    {
        Q_EMIT DirtyCloseRequested(tab);
        return;
    }
    removeTab(tab);
}

bool CodeEditorArea::PromptCloseDirtyTab(CodeDocumentTab* tab)
{
    if (tab == nullptr)
    {
        return false;
    }
    if (isLastDirtyView(tab) == false)
    {
        // Clean, or another view keeps the draft open: close without asking.
        removeTab(tab);
        return true;
    }
    QMessageBox box(this);
    box.setObjectName(QStringLiteral("codeDirtyCloseDialog"));
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved changes"));
    box.setText(QStringLiteral("Save changes to %1 before closing?").arg(QFileInfo(tab->GetPath()).fileName()));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    // Cancel is the safe default: a stray Enter or Escape keeps the draft.
    box.setDefaultButton(QMessageBox::Cancel);
    box.setEscapeButton(QMessageBox::Cancel);
    const int answer = box.exec();
    if (answer == QMessageBox::Cancel)
    {
        return false;
    }
    if (answer == QMessageBox::Save)
    {
        // A failed save leaves the tab open with the cause and next step in its
        // bar (including a pending external change, which needs a decision).
        if (SaveTab(tab) == false)
        {
            return false;
        }
    }
    // Save succeeded or Don't Save: close the tab. A discarded draft must not
    // linger in the registry or the journal, so drop it before releasing.
    if (answer == QMessageBox::Discard)
    {
        if (CodeFileDocument* host = hostOf(tab); host != nullptr)
        {
            host->DiscardDraft();
        }
        mRegistry.StartRecoveryCheckpoint();
    }
    removeTab(tab);
    return true;
}

bool CodeEditorArea::HasDirtyBuffers() const
{
    for (CodeDocumentTab* tab : GetTabs())
    {
        if (tab->IsDirty() == true)
        {
            return true;
        }
    }
    return false;
}

bool CodeEditorArea::ConfirmDiscardAll()
{
    QList<CodeDocumentTab*> dirty;
    for (CodeDocumentTab* tab : GetTabs())
    {
        if (tab->IsDirty() == true)
        {
            dirty.append(tab);
        }
    }
    if (dirty.isEmpty() == true)
    {
        return true;
    }
    QStringList names;
    for (CodeDocumentTab* tab : dirty)
    {
        const QString name = QFileInfo(tab->GetPath()).fileName();
        if (names.contains(name) == false)
        {
            names.append(name);
        }
    }
    QMessageBox box(this);
    box.setObjectName(QStringLiteral("codeDiscardAllDialog"));
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved code buffers"));
    box.setText(QStringLiteral("Save changes to %1 code buffer(s) before continuing?").arg(names.size()));
    box.setInformativeText(names.join(QStringLiteral("\n")));
    box.setStandardButtons(QMessageBox::SaveAll | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setEscapeButton(QMessageBox::Cancel);
    const int answer = box.exec();
    if (answer == QMessageBox::Cancel)
    {
        return false;
    }
    if (answer == QMessageBox::SaveAll)
    {
        QStringList failed;
        for (CodeDocumentTab* tab : dirty)
        {
            // A mirrored view of a host saved earlier in this loop is clean now.
            if (tab->IsDirty() == true && SaveTab(tab) == false)
            {
                failed.append(QFileInfo(tab->GetPath()).fileName());
            }
        }
        if (failed.isEmpty() == false)
        {
            QMessageBox::warning(this,
                                 QStringLiteral("Save failed"),
                                 QStringLiteral("These buffers could not be saved and stay open:\n%1")
                                     .arg(failed.join(QLatin1Char('\n'))));
            return false;
        }
        return true;
    }
    // Don't Save: every dirty draft is discarded, in memory as well as in the
    // journal, so a later checkpoint (project close flushes one) cannot write the
    // discarded drafts back for recovery.
    mRegistry.DiscardDirtyDrafts();
    for (CodeDocumentTab* tab : GetTabs())
    {
        tab->MirrorFromHost();
        updateTabDecoration(tab);
    }
    mRegistry.ClearRecovery();
    Q_EMIT TabsChanged();
    return true;
}

bool CodeEditorArea::SaveTab(CodeDocumentTab* tab)
{
    CodeFileDocument* host = hostOf(tab);
    if (host == nullptr)
    {
        return false;
    }
    const QString fileName = titleFor(tab->GetPath());
    if (host->IsReadOnly() == true)
    {
        tab->GetBanner().ShowExplanation(
            tr("%1 is open read-only and cannot be saved here. Use Open Externally to edit it.").arg(fileName));
        return false;
    }
    // Never overwrite a change made on disk without asking (CE04): show the
    // conflict, and Keep Mine followed by Save writes the draft over it. A file
    // that was deleted has nothing to overwrite; saving writes it back.
    if (QFileInfo::exists(host->GetCanonicalPath()) == true && host->HasExternalChange() == true)
    {
        showConflict(host);
        return false;
    }
    QString error;
    if (host->Save(error) == false)
    {
        tab->GetBanner().ShowExplanation(
            tr("%1 was not saved (%2). Your changes are kept; fix the cause and save again.").arg(fileName, error));
        return false;
    }
    for (CodeDocumentTab* view : viewsOf(host))
    {
        view->MakePermanent();
        view->GetBanner().Dismiss();
        updateTabDecoration(view);
    }
    // The buffer is clean: remove its journal file now rather than after the
    // idle debounce, so a crash right after Save cannot offer stale text.
    mRegistry.StartRecoveryCheckpoint();
    Q_EMIT TabsChanged();
    return true;
}

bool CodeEditorArea::SaveActiveTab()
{
    CodeDocumentTab* tab = GetActiveTabOrNull();
    return tab != nullptr && SaveTab(tab) == true;
}

bool CodeEditorArea::CanSaveActiveTab() const
{
    CodeDocumentTab* tab = GetActiveTabOrNull();
    return tab != nullptr && tab->IsReadOnly() == false;
}

void CodeEditorArea::CheckExternalChanges()
{
    QList<CodeFileDocument*> checked;
    for (CodeDocumentTab* tab : GetTabs())
    {
        CodeFileDocument* host = hostOf(tab);
        if (host != nullptr && checked.contains(host) == false)
        {
            checked.append(host);
            checkExternalChange(host);
        }
    }
}

QList<CodeDocumentTab*> CodeEditorArea::viewsOf(CodeFileDocument* host) const
{
    QList<CodeDocumentTab*> views;
    for (CodeDocumentTab* tab : GetTabs())
    {
        if (hostOf(tab) == host)
        {
            views.append(tab);
        }
    }
    return views;
}

bool CodeEditorArea::isLastDirtyView(CodeDocumentTab* tab) const
{
    if (tab->IsDirty() == false)
    {
        return false;
    }
    CodeFileDocument* host = hostOf(tab);
    return host == nullptr || viewsOf(host).size() == 1;
}

void CodeEditorArea::refreshViewsOf(CodeFileDocument* host)
{
    for (CodeDocumentTab* view : viewsOf(host))
    {
        view->RefreshFromHost();
        updateTabDecoration(view);
    }
    Q_EMIT TabsChanged();
}

void CodeEditorArea::checkExternalChange(CodeFileDocument* host)
{
    if (host->HasExternalChange() == false)
    {
        return;
    }
    if (host->IsDirty() == true)
    {
        showConflict(host);
        return;
    }
    // A clean buffer follows the file silently.
    reloadFromDisk(host);
}

void CodeEditorArea::showConflict(CodeFileDocument* host)
{
    for (CodeDocumentTab* view : viewsOf(host))
    {
        view->GetBanner().ShowConflict(titleFor(view->GetPath()));
    }
}

void CodeEditorArea::keepDraftOverDisk(CodeFileDocument* host)
{
    // The draft stays and the current file becomes the conflict baseline, so
    // the next Save writes the draft over the external change.
    host->AdoptAsBaseline();
    for (CodeDocumentTab* view : viewsOf(host))
    {
        view->GetBanner().Dismiss();
    }
}

void CodeEditorArea::reloadFromDisk(CodeFileDocument* host)
{
    QString error;
    if (host->ReloadFromDisk(error) == false)
    {
        // Typically the file was deleted or became unreadable. The buffer stays
        // open so its text can be saved back to the same path.
        for (CodeDocumentTab* view : viewsOf(host))
        {
            view->GetBanner().ShowExplanation(
                tr("%1 could not be read from disk (%2). Save writes this buffer back to it; close the tab "
                   "to drop it.")
                    .arg(titleFor(view->GetPath()), error));
        }
        return;
    }
    refreshViewsOf(host);
    mRegistry.StartRecoveryCheckpoint();
}

void CodeEditorArea::showComparison(CodeDocumentTab* tab)
{
    CodeFileDocument* host = hostOf(tab);
    if (host == nullptr)
    {
        return;
    }
    // Side-by-side, read-only: the unsaved buffer and the file as it is on disk.
    // The bar stays up, so the decision is still Keep Mine or Reload.
    const CodeFileLoadResult onDisk = LoadCodeFile(host->GetCanonicalPath());
    const CodeDocumentSnapshot draft = host->GetSnapshot();
    QDialog* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("codeCompareDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Compare %1").arg(titleFor(tab->GetPath())));
    QGridLayout* layout = new QGridLayout(dialog);
    layout->addWidget(new QLabel(tr("Your unsaved changes"), dialog), 0, 0);
    layout->addWidget(new QLabel(tr("On disk"), dialog), 0, 1);
    CodeEditor* mine = new CodeEditor(dialog);
    mine->setObjectName(QStringLiteral("codeCompareMine"));
    mine->SetLanguage(tab->GetEditor().GetLanguage());
    mine->setPlainText(QString::fromUtf8(draft.Text.GetDataOrNull(), static_cast<qsizetype>(draft.Text.GetSize())));
    mine->setReadOnly(true);
    CodeEditor* theirs = new CodeEditor(dialog);
    theirs->setObjectName(QStringLiteral("codeCompareDisk"));
    theirs->SetLanguage(tab->GetEditor().GetLanguage());
    theirs->setPlainText(onDisk.Status == CodeFileLoadStatus::Failed
                             ? tr("The file could not be read: %1").arg(onDisk.Explanation)
                             : QString::fromUtf8(onDisk.Bytes));
    theirs->setReadOnly(true);
    layout->addWidget(mine, 1, 0);
    layout->addWidget(theirs, 1, 1);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons, 2, 0, 1, 2);
    dialog->show();
}

int32 CodeEditorArea::GetGroupCount() const
{
    return static_cast<int32>(mGroups.size());
}

int32 CodeEditorArea::GetTabCount() const
{
    int32 count = 0;
    for (const Group& group : mGroups)
    {
        count += group.Tabs->count();
    }
    return count;
}

CodeDocumentTab* CodeEditorArea::GetActiveTabOrNull() const
{
    const int32 group = activeGroupIndex();
    if (group >= mGroups.size())
    {
        return nullptr;
    }
    return qobject_cast<CodeDocumentTab*>(mGroups.at(group).Tabs->currentWidget());
}

QList<CodeDocumentTab*> CodeEditorArea::GetTabs() const
{
    QList<CodeDocumentTab*> result;
    for (const Group& group : mGroups)
    {
        for (int index = 0; index < group.Tabs->count(); ++index)
        {
            if (CodeDocumentTab* tab = qobject_cast<CodeDocumentTab*>(group.Tabs->widget(index)); tab != nullptr)
            {
                result.append(tab);
            }
        }
    }
    return result;
}

QJsonObject CodeEditorArea::SaveState() const
{
    QJsonArray tabs;
    CodeDocumentTab* active = GetActiveTabOrNull();
    int activeIndex = -1;
    for (int32 groupIndex = 0; groupIndex < mGroups.size(); ++groupIndex)
    {
        QTabWidget* groupTabs = mGroups.at(groupIndex).Tabs;
        for (int index = 0; index < groupTabs->count(); ++index)
        {
            CodeDocumentTab* tab = qobject_cast<CodeDocumentTab*>(groupTabs->widget(index));
            if (tab == nullptr)
            {
                continue;
            }
            QJsonObject entry = tab->SaveViewState();
            entry.insert(QLatin1String(STATE_GROUP), groupIndex);
            if (tab == active)
            {
                activeIndex = tabs.size();
            }
            tabs.append(entry);
        }
    }
    QJsonObject state;
    state.insert(QLatin1String(STATE_TABS), tabs);
    state.insert(QLatin1String(STATE_ACTIVE), activeIndex);
    return state;
}

void CodeEditorArea::RestoreState(const QJsonObject& state)
{
    const QJsonArray tabs = state.value(QLatin1String(STATE_TABS)).toArray();
    const int activeIndex = state.value(QLatin1String(STATE_ACTIVE)).toInt(-1);
    CodeDocumentTab* activeTabOrNull = nullptr;
    for (int index = 0; index < tabs.size(); ++index)
    {
        const QJsonObject entry = tabs.at(index).toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty() == true || QFileInfo::exists(path) == false)
        {
            continue;
        }
        const int32 group = entry.value(QLatin1String(STATE_GROUP)).toInt(0);
        if (group >= MAX_CODE_GROUPS)
        {
            continue;
        }
        if (group >= mGroups.size())
        {
            static_cast<void>(createGroup());
        }
        const bool isPreview = entry.value(QStringLiteral("preview")).toBool(false);
        CodeDocumentTab* tab = openIn(group, path, isPreview);
        if (tab != nullptr)
        {
            tab->RestoreViewState(entry);
            if (index == activeIndex)
            {
                activeTabOrNull = tab;
            }
        }
    }
    if (activeTabOrNull != nullptr)
    {
        for (const Group& group : mGroups)
        {
            if (group.Tabs->indexOf(activeTabOrNull) >= 0)
            {
                group.Tabs->setCurrentWidget(activeTabOrNull);
                break;
            }
        }
    }
    Q_EMIT TabsChanged();
    onCurrentChanged();
}
} // namespace ludus::editor
