#include "internal/main_window.h"

#include "internal/audio_workspace.h"
#include "internal/configuration_workspace.h"
#include "internal/content_browser.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/world_workspace.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTextDocument>

namespace ludus::editor
{
QWidget* MainWindow::getActiveWorkspace() const noexcept
{
    return mActiveWorkspaceOrNull != nullptr ? mActiveWorkspaceOrNull : WorkTabs_->currentWidget();
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
    QWidget* area = getActiveWorkspace();
    const bool project = area == ProjectSettings_;
    const bool audio = area != nullptr && area->isAncestorOf(Audio_);
    const bool configuration = area == Configuration_;
    const bool scripts = area == Scripts_;
    const bool world = area == World_;
    const bool isRenderer = area == Renderer_;
    SaveAction_->setText(project         ? QStringLiteral("&Save Project Settings")
                         : audio         ? QStringLiteral("&Save Audio")
                         : world         ? QStringLiteral("&Save World")
                         : scripts       ? QStringLiteral("&Save Script Asset")
                         : isRenderer    ? QStringLiteral("&Save Render Graph")
                         : configuration ? QStringLiteral("&Save Preferences")
                                         : QStringLiteral("&Save"));
    // Clean project saves are available too: the focused argument delegate may
    // contain pending text that has not yet made the document dirty.
    const bool rootBlocked = project && Content_->Importing() &&
                             Controller_->State().Draft.SourceDir != Controller_->State().Saved.SourceDir;
    SaveAction_->setToolTip(rootBlocked ? QStringLiteral("Finish or cancel content work before changing its root.")
                                        : QString());
    SaveAction_->setEnabled(project      ? Controller_->Caps().CanEdit && rootBlocked == false
                            : audio      ? Audio_->CanSave()
                            : world      ? World_->CanSave()
                            : scripts    ? Scripts_->CanSave()
                            : isRenderer ? Renderer_->CanSave()
                                         : configuration && Configuration_->Preview() != nullptr);
#if defined(Q_OS_WASM)
    if (project || configuration)
    {
        SaveAction_->setText(project ? QStringLiteral("Save and &Download Project")
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
        const bool inProject = project && (focus == nullptr || ProjectSettings_->isAncestorOf(focus));
        UndoAction_->setEnabled(world        ? World_->CanUndo()
                                : scripts    ? Scripts_->CanUndo()
                                : isRenderer ? Renderer_->CanUndo()
                                             : inProject && Controller_->CanUndoProject());
        RedoAction_->setEnabled(world        ? World_->CanRedo()
                                : scripts    ? Scripts_->CanRedo()
                                : isRenderer ? Renderer_->CanRedo()
                                             : inProject && Controller_->CanRedoProject());
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
    else if (getActiveWorkspace() == World_)
    {
        World_->Undo();
    }
    else if (getActiveWorkspace() == Scripts_)
    {
        Scripts_->Undo();
    }
    else if (getActiveWorkspace() == Renderer_)
    {
        Renderer_->Undo();
    }
    else if (getActiveWorkspace() == ProjectSettings_ && UndoAction_->isEnabled())
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
    else if (getActiveWorkspace() == World_)
    {
        World_->Redo();
    }
    else if (getActiveWorkspace() == Scripts_)
    {
        Scripts_->Redo();
    }
    else if (getActiveWorkspace() == Renderer_)
    {
        Renderer_->Redo();
    }
    else if (getActiveWorkspace() == ProjectSettings_ && RedoAction_->isEnabled())
    {
        Controller_->RedoProjectEdit();
    }
    RenderDocumentActions();
}
} // namespace ludus::editor
