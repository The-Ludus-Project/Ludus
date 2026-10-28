// Project Explorer requests are routed here; document owners retain drafts and creation/import semantics.
#include "internal/audio_workspace.h"
#include "internal/content_browser.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/editor_files.h"
#include "internal/main_window.h"
#include "internal/project_explorer.h"
#include "internal/project_files.h"
#include "internal/renderer_workspace.h"
#include "internal/script_workspace.h"
#include "internal/world_workspace.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QStatusBar>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 FILE_MESSAGE_MILLISECONDS = 12000;

[[nodiscard]] QString GetDefaultSuffix(ProjectItemKind kind)
{
    switch (kind)
    {
        case ProjectItemKind::World:
            return QStringLiteral(".world.json");
        case ProjectItemKind::RenderGraph:
            return QStringLiteral(".lurender.json");
        case ProjectItemKind::Shader:
            return QStringLiteral(".slang");
        case ProjectItemKind::Scripts:
            return QStringLiteral(".luau");
        default:
            return {};
    }
}
} // namespace

void MainWindow::changeProjectItem(ProjectItemAction action, ProjectItemKind kind, const QString& path)
{
    if (ProjectLayoutRoot_.isEmpty() == true || Controller_->Caps().CanEdit == false || Content_->Busy() == true ||
        AudioClosing_ == true)
    {
        statusBar()->showMessage(QStringLiteral("Project files cannot be changed while the project is busy."),
                                 FILE_MESSAGE_MILLISECONDS);
        return;
    }
    const QString root = ProjectLayoutRoot_;
    const foundation::uint64 epoch = Controller_->State().ProjectEpoch;
    const auto isCurrent = [this, root, epoch]() {
        return root == ProjectLayoutRoot_ && epoch == Controller_->State().ProjectEpoch &&
               Controller_->Caps().CanEdit == true && Content_->Busy() == false && AudioClosing_ == false;
    };
    if (kind == ProjectItemKind::Audio || kind == ProjectItemKind::Content)
    {
        // Content publication is catalog-aware, bounded and asynchronous; never raw-copy resource files.
        if (action == ProjectItemAction::CreateSound || action == ProjectItemAction::CreateMusic)
        {
            static_cast<void>(mDocuments->OpenDocument(mAudioDocument));
            Audio_->PromptCreateDefinition(action == ProjectItemAction::CreateMusic);
        }
        else if (action == ProjectItemAction::AddExisting)
        {
            showToolWindow(mContentDock);
            Content_->ChooseImport();
        }
        return;
    }
    if (action == ProjectItemAction::Create && kind == ProjectItemKind::RenderGraph)
    {
        static_cast<void>(mDocuments->OpenDocument(Renderer_));
        static_cast<void>(Renderer_->PromptNewGraph());
        // Graph creation is asynchronous; DocumentChanged refreshes the tree on publication.
        return;
    }
    if (action == ProjectItemAction::AddExisting)
    {
        QString filter;
        switch (kind)
        {
            case ProjectItemKind::World:
                filter = QStringLiteral("Worlds (*.world.json)");
                break;
            case ProjectItemKind::RenderGraph:
                filter = QStringLiteral("Render graphs (*.lurender.json)");
                break;
            case ProjectItemKind::Shader:
                filter = QStringLiteral("Shaders (*.slang *.lushader.json)");
                break;
            case ProjectItemKind::Scripts:
                filter = QStringLiteral("Scripts (*.luau *.lua *.luseq.json)");
                break;
            default:
                return;
        }
        OpenEditorDocument(
            this,
            { .Title = QStringLiteral("Add existing project file"), .Directory = root, .Filter = filter },
            [this, root, epoch, kind](const QString& source) {
                if (root != ProjectLayoutRoot_ || epoch != Controller_->State().ProjectEpoch ||
                    Controller_->Caps().CanEdit == false || Content_->Busy() == true)
                {
                    return;
                }
                if (ProjectFiles::IsProjectFile(root, source) == true)
                {
                    mProjectExplorer->Refresh();
                    openProjectItem(kind, source);
                    return;
                }
                bool isAccepted = false;
                const QString relative =
                    QInputDialog::getText(this,
                                          QStringLiteral("Add existing file"),
                                          QStringLiteral("Destination relative to the project (existing folder). "
                                                         "Companion files must be added separately."),
                                          QLineEdit::Normal,
                                          QFileInfo(source).fileName(),
                                          &isAccepted)
                        .trimmed();
                if (isAccepted == false || root != ProjectLayoutRoot_ || epoch != Controller_->State().ProjectEpoch ||
                    Controller_->Caps().CanEdit == false || Content_->Busy() == true)
                {
                    return;
                }
                QString error;
                if (ProjectFiles::Add(root, source, relative, error) == false)
                {
                    QMessageBox::warning(this, QStringLiteral("Cannot add file"), error);
                    return;
                }
#if defined(Q_OS_WASM)
                MarkBrowserEdited();
#endif
                mProjectExplorer->Refresh();
                statusBar()->showMessage(QStringLiteral("Added %1.").arg(relative), FILE_MESSAGE_MILLISECONDS);
            });
        return;
    }
    if (action == ProjectItemAction::Create)
    {
        const QString suffix = GetDefaultSuffix(kind);
        if (suffix.isEmpty() == true)
        {
            return;
        }
        bool isAccepted = false;
        QString relative = QInputDialog::getText(this,
                                                 QStringLiteral("New project file"),
                                                 QStringLiteral("File path relative to the project (existing folder)"),
                                                 QLineEdit::Normal,
                                                 QStringLiteral("new") + suffix,
                                                 &isAccepted)
                               .trimmed();
        if (isAccepted == false || isCurrent() == false)
        {
            return;
        }
        if (relative.endsWith(suffix, Qt::CaseInsensitive) == false)
        {
            relative += suffix;
        }
        if (kind == ProjectItemKind::World)
        {
            if (World_->Create(QDir(root).filePath(relative)) == true)
            {
                static_cast<void>(mDocuments->OpenDocument(World_));
                // Keep the normal New World draft/save contract: no disk writes until Save.
            }
            return;
        }
        const QByteArray bytes =
            kind == ProjectItemKind::Shader
                ? QByteArray("// Project shader source. Add its entry points to a shader manifest before building.\n")
                : QByteArray("-- Project script source. Register it in the project's script package before cooking.\n");
        QString error;
        if (ProjectFiles::Create(root, relative, bytes, error) == false)
        {
            QMessageBox::warning(this, QStringLiteral("Cannot create file"), error);
            return;
        }
#if defined(Q_OS_WASM)
        MarkBrowserEdited();
#endif
        mProjectExplorer->Refresh();
        statusBar()->showMessage(
            QStringLiteral("Created %1. Register the source in its shader or script package before building.")
                .arg(relative),
            FILE_MESSAGE_MILLISECONDS);
        return;
    }
    if (ProjectFiles::IsProjectFile(root, path) == false)
    {
        mProjectExplorer->Refresh();
        statusBar()->showMessage(QStringLiteral("The selected file no longer exists in this project."),
                                 FILE_MESSAGE_MILLISECONDS);
        return;
    }
    ProjectFileResult result;
    if (action == ProjectItemAction::Duplicate)
    {
        const bool shouldCopyPackage = kind == ProjectItemKind::RenderGraph && QFileInfo(path).absolutePath() != root;
        bool isAccepted = false;
        const QString initial = shouldCopyPackage == true
                                    ? QFileInfo(QFileInfo(path).absolutePath()).fileName() + QStringLiteral("_copy")
                                    : QStringLiteral("copy_") + QFileInfo(path).fileName();
        const QString name =
            QInputDialog::getText(
                this,
                QStringLiteral("Duplicate project item"),
                shouldCopyPackage == true
                    ? QStringLiteral("New sibling package folder (copies all companion files; saved contents only)")
                    : QStringLiteral("New file name (saved contents only; external references remain shared)"),
                QLineEdit::Normal,
                initial,
                &isAccepted)
                .trimmed();
        if (isAccepted == false || isCurrent() == false)
        {
            return;
        }
        if (ProjectFiles::Duplicate(root, path, name, shouldCopyPackage, result) == false)
        {
            QMessageBox::warning(this, QStringLiteral("Cannot duplicate item"), result.Error);
            return;
        }
    }
    else if (action == ProjectItemAction::Remove)
    {
        // A loaded owner can save a removed file back to disk. Require switching documents first,
        // including clean owners; hidden tabs retain their drafts. Never reset unrelated workspace state.
        const auto isInUse = [this, kind, path]() {
            const RenderGraphDocument& graph = Renderer_->GetDocument();
            const bool isOpenWorld =
                QFileInfo(World_->Path()).canonicalFilePath() == QFileInfo(path).canonicalFilePath();
            const bool isOpenGraphFile =
                graph.IsLoaded() == true && graph.IsBuiltIn() == false &&
                graph.GetFiles().contains(QDir(graph.GetPackageRoot()).relativeFilePath(path)) == true;
            const bool isOpenScript =
                kind == ProjectItemKind::Scripts &&
                QFileInfo(Scripts_->GetPath()).canonicalFilePath() == QFileInfo(path).canonicalFilePath();
            return isOpenWorld == true || isOpenGraphFile == true || isOpenScript == true;
        };
        if (isInUse() == true)
        {
            QMessageBox::information(this,
                                     QStringLiteral("Item is in use"),
                                     QStringLiteral("Open another World/render graph/script before removing this file. "
                                                    "Closing a tab retains its document."));
            return;
        }
        if (QMessageBox::question(this,
                                  QStringLiteral("Remove project file"),
                                  QStringLiteral("Move %1 to .ludus-trash?\nReferences from other documents are not "
                                                 "rewritten. The file can be restored from the recovery folder.")
                                      .arg(QDir(root).relativeFilePath(path)),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes ||
            isCurrent() == false || isInUse() == true)
        {
            return;
        }
        if (ProjectFiles::Remove(root, path, result) == false)
        {
            QMessageBox::warning(this, QStringLiteral("Cannot remove file"), result.Error);
            return;
        }
    }
    else
    {
        return;
    }
#if defined(Q_OS_WASM)
    MarkBrowserEdited();
#endif
    mProjectExplorer->Refresh();
    statusBar()->showMessage(QStringLiteral("%1: %2").arg(action == ProjectItemAction::Remove
                                                              ? QStringLiteral("Recoverable copy")
                                                              : QStringLiteral("Created"),
                                                          QDir::toNativeSeparators(result.Path)),
                             FILE_MESSAGE_MILLISECONDS);
}
} // namespace ludus::editor
