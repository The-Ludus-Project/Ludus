#include "internal/code_editor.h"
#include "internal/main_window.h"
#include "internal/project_rendering_settings.h"

#include "internal/audio_workspace.h"
#include "internal/code_editor_area.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/edit_history.h"
#include "internal/editor_files.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/world_workspace.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTextDocument>

namespace ludus::editor
{
#if defined(Q_OS_WASM)
bool MainWindow::downloadBrowserProject()
{
    if (Content_->Importing() == true)
    {
        StatusLabel_->setText(QStringLiteral("Finish or cancel content import before downloading the project."));
        return false;
    }
    if (Audio_->IsDirty() == true)
    {
        Audio_->Save();
        if (Audio_->IsDirty() == true)
        {
            return false;
        }
    }
    if ((World_->Dirty() == true && World_->Save() == false) ||
        (Scripts_->Dirty() == true && Scripts_->Save() == false) ||
        (Renderer_->IsDirty() == true && Renderer_->Save() == false))
    {
        return false;
    }
    return DownloadEditorProject(this, Controller_->State().DescriptorPath);
}
#endif
QWidget* MainWindow::getActiveWorkspaceOrNull() const
{
    // Save and Undo follow the active document tab. Focus in a tool window (for example
    // the World's entity fields in Properties) keeps the document it belongs to active.
    return mDocuments->GetActiveDocumentOrNull();
}

void MainWindow::CommitProjectFields()
{
    if (Controller_->Caps().CanEdit == false)
    {
        return;
    }
    mRenderingSettingsOrNull->Commit();
    // A QListWidget delegate's text is not in the model until accepted. Save
    // and loss-of-work confirmation must include that pending argument buffer.
    auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    if (edit != nullptr && ArgsList_->isAncestorOf(edit) && ArgsList_->currentIndex().isValid())
    {
        ArgsList_->itemDelegate()->setModelData(edit, ArgsList_->model(), ArgsList_->currentIndex());
    }
    Controller_->EditDraft(DraftFromFields());
    LastEditedField_ = nullptr;
}

void MainWindow::RenderDocumentActions()
{
    if (SaveAction_ == nullptr || UndoAction_ == nullptr)
    {
        return;
    }
    // World changes can leave both the active tab and render-graph commands unchanged.
    renderDocumentTitles();
    QWidget* areaOrNull = getActiveWorkspaceOrNull();
    const bool isProject = areaOrNull == ProjectSettings_;
    const bool isAudio = areaOrNull != nullptr && areaOrNull->isAncestorOf(Audio_);
    const bool isConfiguration = areaOrNull == Configuration_;
    const bool isScripts = areaOrNull == Scripts_;
    const bool isWorld = areaOrNull == World_;
    const bool isRenderer = areaOrNull == Renderer_;
    const bool isCode = areaOrNull == mCodeArea;
    SaveAction_->setText(isProject         ? QStringLiteral("&Save Project Settings")
                         : isAudio         ? QStringLiteral("&Save Audio")
                         : isWorld         ? QStringLiteral("&Save World")
                         : isScripts       ? QStringLiteral("&Save Script Asset")
                         : isRenderer      ? QStringLiteral("&Save Render Graph")
                         : isConfiguration ? QStringLiteral("&Save Preferences")
                         : isCode          ? QStringLiteral("&Save File")
                                           : QStringLiteral("&Save"));
    // Clean project saves are available too: the focused argument delegate may
    // contain pending text that has not yet made the document dirty.
    const bool isRootBlocked = isProject && Content_->Importing() &&
                               Controller_->State().Draft.SourceDir != Controller_->State().Saved.SourceDir;
    SaveAction_->setToolTip(isRootBlocked ? QStringLiteral("Finish or cancel content work before changing its root.")
                                          : QString());
    SaveAction_->setEnabled(isProject    ? Controller_->Caps().CanEdit && isRootBlocked == false
                            : isAudio    ? Audio_->CanSave()
                            : isWorld    ? World_->CanSave()
                            : isScripts  ? Scripts_->CanSave()
                            : isRenderer ? Renderer_->CanSave()
                            : isCode     ? mCodeArea->CanSaveActiveTab()
                                         : isConfiguration && Configuration_->Preview() != nullptr);
#if defined(Q_OS_WASM)
    if (Audio_->IsDirty() == true || World_->Dirty() == true || Scripts_->Dirty() == true ||
        Renderer_->IsDirty() == true)
    {
        MarkBrowserEdited();
    }
    if (isProject || isConfiguration)
    {
        SaveAction_->setText(isProject ? QStringLiteral("Save and &Download Project")
                                       : QStringLiteral("Save and &Download Preferences"));
    }
#endif
    auto* focus = QApplication::focusWidget();
    auto* line = qobject_cast<QLineEdit*>(focus);
    auto* plain = qobject_cast<QPlainTextEdit*>(focus);
    CodeEditor* codeOrNull = qobject_cast<CodeEditor*>(focus);
    if (isProject == true && mRenderingSettingsOrNull->ContainsFocus() == true)
    {
        UndoAction_->setEnabled(Renderer_->CanUndo());
        RedoAction_->setEnabled(Renderer_->CanRedo());
    }
    else if (line != nullptr)
    {
        UndoAction_->setEnabled(line->isEnabled() && line->isReadOnly() == false && line->isUndoAvailable());
        RedoAction_->setEnabled(line->isEnabled() && line->isReadOnly() == false && line->isRedoAvailable());
    }
    else if (codeOrNull != nullptr)
    {
        UndoAction_->setEnabled(codeOrNull->isEnabled() == true && codeOrNull->isReadOnly() == false &&
                                codeOrNull->CanUndo() == true);
        RedoAction_->setEnabled(codeOrNull->isEnabled() == true && codeOrNull->isReadOnly() == false &&
                                codeOrNull->CanRedo() == true);
    }
    else if (plain != nullptr)
    {
        UndoAction_->setEnabled(plain->isEnabled() && plain->isReadOnly() == false &&
                                plain->document()->isUndoAvailable());
        RedoAction_->setEnabled(plain->isEnabled() && plain->isReadOnly() == false &&
                                plain->document()->isRedoAvailable());
    }
    else
    {
        const bool isHistoryFocus = mHistoryDockOrNull != nullptr && mHistoryDockOrNull->isAncestorOf(focus);
        const bool isInProject =
            isProject && (focus == nullptr || isHistoryFocus == true || ProjectSettings_->isAncestorOf(focus));
        UndoAction_->setEnabled(isWorld      ? World_->CanUndo()
                                : isScripts  ? Scripts_->CanUndo()
                                : isRenderer ? Renderer_->CanUndo()
                                             : isInProject && Controller_->CanUndoProject());
        RedoAction_->setEnabled(isWorld      ? World_->CanRedo()
                                : isScripts  ? Scripts_->CanRedo()
                                : isRenderer ? Renderer_->CanRedo()
                                             : isInProject && Controller_->CanRedoProject());
    }
    // Name the command each action will act on ("Undo Rename Entity"). A name is a pointer read per history;
    // QAction ignores an unchanged text, so this costs nothing when focus moves within one document.
    const HistoryTarget target = getHistoryTarget();
    const QString undoName = UndoAction_->isEnabled() == true ? getHistoryCommandName(target, false) : QString();
    const QString redoName = RedoAction_->isEnabled() == true ? getHistoryCommandName(target, true) : QString();
    const auto describe = [](QAction& action, const QString& text, const QString& unavailable) {
        action.setText(text);
        QString tip = action.isEnabled() == true ? QString(text).remove(QLatin1Char('&')) : unavailable;
        const QString shortcut = action.shortcut().toString(QKeySequence::NativeText);
        if (shortcut.isEmpty() == false)
        {
            tip += QStringLiteral(" (") + shortcut + QLatin1Char(')');
        }
        action.setToolTip(tip);
        action.setStatusTip(tip);
    };
    describe(*UndoAction_, MakeUndoMenuText(undoName), QStringLiteral("Nothing to undo"));
    describe(*RedoAction_, MakeRedoMenuText(redoName), QStringLiteral("Nothing to redo"));
    renderHistory();
}

MainWindow::HistoryTarget MainWindow::getHistoryTarget() const
{
    // Mirrors RenderDocumentActions: a focused text control owns Undo before its document does.
    QWidget* areaOrNull = getActiveWorkspaceOrNull();
    QWidget* focusOrNull = QApplication::focusWidget();
    if (areaOrNull == ProjectSettings_ && mRenderingSettingsOrNull->ContainsFocus() == true)
    {
        return HistoryTarget::Renderer;
    }
    if (qobject_cast<QLineEdit*>(focusOrNull) != nullptr || qobject_cast<QPlainTextEdit*>(focusOrNull) != nullptr)
    {
        return qobject_cast<CodeEditor*>(focusOrNull) != nullptr ? HistoryTarget::CodeEditor : HistoryTarget::TextField;
    }
    if (qobject_cast<CodeEditor*>(focusOrNull) != nullptr)
    {
        return HistoryTarget::CodeEditor;
    }
    if (areaOrNull == World_)
    {
        return HistoryTarget::World;
    }
    if (areaOrNull == Scripts_)
    {
        return HistoryTarget::Script;
    }
    if (areaOrNull == Renderer_)
    {
        return HistoryTarget::Renderer;
    }
    const bool isHistoryFocus = mHistoryDockOrNull != nullptr && mHistoryDockOrNull->isAncestorOf(focusOrNull);
    if (areaOrNull == ProjectSettings_ &&
        (focusOrNull == nullptr || isHistoryFocus == true || ProjectSettings_->isAncestorOf(focusOrNull)))
    {
        return HistoryTarget::Project;
    }
    return HistoryTarget::None;
}

QString MainWindow::getHistoryCommandName(HistoryTarget target, bool isRedo) const
{
    const char* nameOrNull = nullptr;
    switch (target)
    {
        case HistoryTarget::None:
            return {};
        case HistoryTarget::TextField:
        case HistoryTarget::CodeEditor:
            return QString::fromUtf8(TYPING_EDIT_NAME);
        case HistoryTarget::Script:
            return QStringLiteral("Script Edit");
        case HistoryTarget::World:
            nameOrNull = isRedo == true ? World_->Document().GetHistory().GetRedoNameOrNull()
                                        : World_->Document().GetHistory().GetUndoNameOrNull();
            break;
        case HistoryTarget::Renderer:
            nameOrNull = isRedo == true ? Renderer_->GetDocument().GetHistory().GetRedoNameOrNull()
                                        : Renderer_->GetDocument().GetHistory().GetUndoNameOrNull();
            break;
        case HistoryTarget::Project:
            nameOrNull = isRedo == true ? Controller_->GetProjectHistory().GetRedoNameOrNull()
                                        : Controller_->GetProjectHistory().GetUndoNameOrNull();
            break;
    }
    return nameOrNull != nullptr ? QString::fromUtf8(nameOrNull) : QString();
}

foundation::uint64 MainWindow::getHistoryRevision(HistoryTarget target) const
{
    switch (target)
    {
        case HistoryTarget::World:
            return World_->Document().GetHistory().Revision();
        case HistoryTarget::Renderer:
            return Renderer_->GetDocument().GetHistory().Revision();
        case HistoryTarget::Project:
            return Controller_->ProjectRevision();
        case HistoryTarget::None:
        case HistoryTarget::TextField:
        case HistoryTarget::CodeEditor:
        case HistoryTarget::Script:
            break;
    }
    return 0;
}

EditHistoryState MainWindow::getHistoryState(HistoryTarget target) const
{
    switch (target)
    {
        case HistoryTarget::World:
            return MakeEditHistoryState(QStringLiteral("World"), World_->Document().GetHistory());
        case HistoryTarget::Renderer:
            return MakeEditHistoryState(QStringLiteral("Render Graph"), Renderer_->GetDocument().GetHistory());
        case HistoryTarget::Project:
            return MakeEditHistoryState(QStringLiteral("Project Settings"), Controller_->GetProjectHistory());
        case HistoryTarget::None:
            return EditHistoryState{ .DocumentName = {}, .Names = {}, .Cursor = 0, .CanJump = true };
        case HistoryTarget::TextField:
        case HistoryTarget::CodeEditor:
        case HistoryTarget::Script:
            break;
    }
    return {};
}

void MainWindow::renderHistory()
{
    if (mHistoryDockOrNull == nullptr || mHistoryPanelOrNull == nullptr || mHistoryDockOrNull->isVisible() == false)
    {
        return;
    }
    mHistoryPanelOrNull->SetState(getHistoryState(getHistoryTarget()));
}

void MainWindow::onHistoryJumpRequested(foundation::int32 cursor)
{
    const HistoryTarget target = getHistoryTarget();
    const EditHistoryState before = getHistoryState(target);
    bool hasJumped = false;
    switch (target)
    {
        case HistoryTarget::World:
            hasJumped = World_->JumpToHistory(cursor);
            break;
        case HistoryTarget::Renderer:
            hasJumped = Renderer_->JumpToHistory(cursor);
            break;
        case HistoryTarget::Project:
            hasJumped = cursor >= 0 && Controller_->JumpToProjectHistory(static_cast<foundation::usize>(cursor));
            break;
        case HistoryTarget::None:
        case HistoryTarget::TextField:
        case HistoryTarget::CodeEditor:
        case HistoryTarget::Script:
            break;
    }
    if (hasJumped == true)
    {
        showUndoFeedback(MakeHistoryJumpFeedbackText(before, cursor));
    }
    else
    {
        statusBar()->showMessage(
            QStringLiteral("That state isn't available right now. Finish the edit or wait for loading to end, "
                           "then choose it again."));
    }
    RenderDocumentActions();
}

void MainWindow::showUndoFeedback(const QString& text)
{
    if (text.isEmpty() == true)
    {
        return;
    }
    if (mUndoToastOrNull == nullptr)
    {
        mUndoToastOrNull = new UndoFeedbackToast(this);
    }
    mUndoToastOrNull->ShowNotice(text, *mDocuments);
}

void MainWindow::OnUndoRequested()
{
    RenderDocumentActions();
    if (UndoAction_->isEnabled() == false)
    {
        return;
    }
    const HistoryTarget target = getHistoryTarget();
    const QString name = getHistoryCommandName(target, false);
    const foundation::uint64 revision = getHistoryRevision(target);
    const auto confirm = [this, target, name, revision]() {
        // Text fields and code editors show the change at the caret, so only document edits get a notice.
        const bool isDocument =
            target == HistoryTarget::World || target == HistoryTarget::Renderer || target == HistoryTarget::Project;
        const bool hasChanged =
            isDocument == true ? getHistoryRevision(target) != revision : target == HistoryTarget::Script;
        if (name.isEmpty() == false && hasChanged == true)
        {
            showUndoFeedback(MakeUndoFeedbackText(name));
        }
    };
    CodeEditor* codeOrNull = qobject_cast<CodeEditor*>(QApplication::focusWidget());
    if (getActiveWorkspaceOrNull() == ProjectSettings_ && mRenderingSettingsOrNull->ContainsFocus() == true)
    {
        Renderer_->Undo();
        confirm();
        RenderDocumentActions();
        return;
    }
    if (codeOrNull != nullptr)
    {
        codeOrNull->Undo();
    }
    else if (QLineEdit* lineOrNull = qobject_cast<QLineEdit*>(QApplication::focusWidget()); lineOrNull != nullptr)
    {
        lineOrNull->undo();
    }
    else if (QPlainTextEdit* plainOrNull = qobject_cast<QPlainTextEdit*>(QApplication::focusWidget());
             plainOrNull != nullptr)
    {
        plainOrNull->undo();
    }
    else if (getActiveWorkspaceOrNull() == World_)
    {
        World_->Undo();
    }
    else if (getActiveWorkspaceOrNull() == Scripts_)
    {
        Scripts_->Undo();
    }
    else if (getActiveWorkspaceOrNull() == Renderer_)
    {
        Renderer_->Undo();
    }
    else if (getActiveWorkspaceOrNull() == ProjectSettings_ && UndoAction_->isEnabled())
    {
        Controller_->UndoProjectEdit();
    }
    confirm();
    RenderDocumentActions();
}

void MainWindow::OnRedoRequested()
{
    RenderDocumentActions();
    if (RedoAction_->isEnabled() == false)
    {
        return;
    }
    const HistoryTarget target = getHistoryTarget();
    const QString name = getHistoryCommandName(target, true);
    const foundation::uint64 revision = getHistoryRevision(target);
    const auto confirm = [this, target, name, revision]() {
        // Text fields and code editors show the change at the caret, so only document edits get a notice.
        const bool isDocument =
            target == HistoryTarget::World || target == HistoryTarget::Renderer || target == HistoryTarget::Project;
        const bool hasChanged =
            isDocument == true ? getHistoryRevision(target) != revision : target == HistoryTarget::Script;
        if (name.isEmpty() == false && hasChanged == true)
        {
            showUndoFeedback(MakeRedoFeedbackText(name));
        }
    };
    CodeEditor* codeOrNull = qobject_cast<CodeEditor*>(QApplication::focusWidget());
    if (getActiveWorkspaceOrNull() == ProjectSettings_ && mRenderingSettingsOrNull->ContainsFocus() == true)
    {
        Renderer_->Redo();
        confirm();
        RenderDocumentActions();
        return;
    }
    if (codeOrNull != nullptr)
    {
        codeOrNull->Redo();
    }
    else if (QLineEdit* lineOrNull = qobject_cast<QLineEdit*>(QApplication::focusWidget()); lineOrNull != nullptr)
    {
        lineOrNull->redo();
    }
    else if (QPlainTextEdit* plainOrNull = qobject_cast<QPlainTextEdit*>(QApplication::focusWidget());
             plainOrNull != nullptr)
    {
        plainOrNull->redo();
    }
    else if (getActiveWorkspaceOrNull() == World_)
    {
        World_->Redo();
    }
    else if (getActiveWorkspaceOrNull() == Scripts_)
    {
        Scripts_->Redo();
    }
    else if (getActiveWorkspaceOrNull() == Renderer_)
    {
        Renderer_->Redo();
    }
    else if (getActiveWorkspaceOrNull() == ProjectSettings_ && RedoAction_->isEnabled())
    {
        Controller_->RedoProjectEdit();
    }
    confirm();
    RenderDocumentActions();
}
} // namespace ludus::editor
