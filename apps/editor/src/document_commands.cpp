#include "internal/main_window.h"

#include "internal/audio_workspace.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/document_well.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/world_workspace.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTextDocument>

namespace ludus::editor
{
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
    QWidget* areaOrNull = getActiveWorkspaceOrNull();
    const bool isProject = areaOrNull == ProjectSettings_;
    const bool isAudio = areaOrNull != nullptr && areaOrNull->isAncestorOf(Audio_);
    const bool isConfiguration = areaOrNull == Configuration_;
    const bool isScripts = areaOrNull == Scripts_;
    const bool isWorld = areaOrNull == World_;
    const bool isRenderer = areaOrNull == Renderer_;
    SaveAction_->setText(isProject         ? QStringLiteral("&Save Project Settings")
                         : isAudio         ? QStringLiteral("&Save Audio")
                         : isWorld         ? QStringLiteral("&Save World")
                         : isScripts       ? QStringLiteral("&Save Script Asset")
                         : isRenderer      ? QStringLiteral("&Save Render Graph")
                         : isConfiguration ? QStringLiteral("&Save Preferences")
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
                                         : isConfiguration && Configuration_->Preview() != nullptr);
#if defined(Q_OS_WASM)
    if (isProject || isConfiguration)
    {
        SaveAction_->setText(isProject ? QStringLiteral("Save and &Download Project")
                                       : QStringLiteral("Save and &Download Preferences"));
    }
#endif
    auto* focus = QApplication::focusWidget();
    auto* line = qobject_cast<QLineEdit*>(focus);
    auto* plain = qobject_cast<QPlainTextEdit*>(focus);
    if (line != nullptr)
    {
        UndoAction_->setEnabled(line->isEnabled() && line->isReadOnly() == false && line->isUndoAvailable());
        RedoAction_->setEnabled(line->isEnabled() && line->isReadOnly() == false && line->isRedoAvailable());
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
        const bool isInProject = isProject && (focus == nullptr || ProjectSettings_->isAncestorOf(focus));
        UndoAction_->setEnabled(isWorld      ? World_->CanUndo()
                                : isScripts  ? Scripts_->CanUndo()
                                : isRenderer ? Renderer_->CanUndo()
                                             : isInProject && Controller_->CanUndoProject());
        RedoAction_->setEnabled(isWorld      ? World_->CanRedo()
                                : isScripts  ? Scripts_->CanRedo()
                                : isRenderer ? Renderer_->CanRedo()
                                             : isInProject && Controller_->CanRedoProject());
    }
}

void MainWindow::OnUndoRequested()
{
    RenderDocumentActions();
    if (UndoAction_->isEnabled() == false)
    {
        return;
    }
    if (auto* line = qobject_cast<QLineEdit*>(QApplication::focusWidget()))
    {
        line->undo();
    }
    else if (auto* plain = qobject_cast<QPlainTextEdit*>(QApplication::focusWidget()))
    {
        plain->undo();
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
    RenderDocumentActions();
}

void MainWindow::OnRedoRequested()
{
    RenderDocumentActions();
    if (RedoAction_->isEnabled() == false)
    {
        return;
    }
    if (auto* line = qobject_cast<QLineEdit*>(QApplication::focusWidget()))
    {
        line->redo();
    }
    else if (auto* plain = qobject_cast<QPlainTextEdit*>(QApplication::focusWidget()))
    {
        plain->redo();
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
    RenderDocumentActions();
}
} // namespace ludus::editor
