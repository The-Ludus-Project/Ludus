#include "internal/project_explorer.h"
#include "internal/editor_icons.h"
#include "internal/item_selection.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLatin1StringView>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 KIND_ROLE = Qt::UserRole;
constexpr foundation::int32 PATH_ROLE = Qt::UserRole + 1;
constexpr QLatin1StringView WORLD_SUFFIX{".world.json"};
constexpr QLatin1StringView RENDER_GRAPH_SUFFIX{".lurender.json"};
constexpr QLatin1StringView SHADER_MANIFEST_SUFFIX{".lushader.json"};
constexpr QLatin1StringView SHADER_SOURCE_SUFFIX{".slang"};

[[nodiscard]] EditorIcon GetKindIcon(ProjectItemKind kind)
{
    switch (kind)
    {
        case ProjectItemKind::Folder:
            return EditorIcon::Folder;
        case ProjectItemKind::ProjectSettings:
            return EditorIcon::ProjectSettings;
        case ProjectItemKind::World:
            return EditorIcon::World;
        case ProjectItemKind::RenderGraph:
            return EditorIcon::RenderGraph;
        case ProjectItemKind::Shader:
        case ProjectItemKind::Scripts:
            return EditorIcon::Code;
        case ProjectItemKind::Audio:
            return EditorIcon::Audio;
        case ProjectItemKind::Content:
            return EditorIcon::Content;
        case ProjectItemKind::Configuration:
            break;
    }
    return EditorIcon::Configuration;
}

/// Folders that hold generated output, dependencies or tool state rather than authored documents.
[[nodiscard]] bool IsSkippedFolder(const QString& name)
{
    return name.startsWith(QLatin1Char('.')) == true || name == QLatin1StringView("out") ||
           name == QLatin1StringView("build") || name == QLatin1StringView("node_modules");
}

struct PendingFolder final
{
    QString Path;
    foundation::int32 Depth;
};
} // namespace

ProjectExplorer::ProjectExplorer(QWidget* parentOrNull)
    : QWidget(parentOrNull), mFilter(new QLineEdit(this)), mTree(new QTreeWidget(this)), mStatus(new QLabel(this))
{
    setObjectName(QStringLiteral("projectExplorer"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    QHBoxLayout* top = new QHBoxLayout;
    mFilter->setObjectName(QStringLiteral("projectExplorerFilter"));
    mFilter->setPlaceholderText(QStringLiteral("Search Project Explorer"));
    mFilter->setClearButtonEnabled(true);
    mFilter->setAccessibleName(QStringLiteral("Search Project Explorer"));
    QToolButton* refresh = new QToolButton(this);
    refresh->setObjectName(QStringLiteral("projectExplorerRefresh"));
    refresh->setText(QStringLiteral("Refresh"));
    refresh->setToolTip(QStringLiteral("Rescan the project folder."));
    top->addWidget(mFilter, 1);
    QToolButton* add = new QToolButton(this);
    add->setObjectName(QStringLiteral("projectExplorerAdd"));
    add->setText(QStringLiteral("Add"));
    add->setAccessibleName(QStringLiteral("Add project item"));
    add->setPopupMode(QToolButton::InstantPopup);
    QMenu* addMenu = new QMenu(add);
    add->setMenu(addMenu);
    connect(addMenu, &QMenu::aboutToShow, this, [this, addMenu]() {
        addMenu->clear();
        populateAddMenu(addMenu, ProjectItemKind::Folder);
    });
    top->addWidget(add);
    top->addWidget(refresh);
    layout->addLayout(top);
    mTree->setObjectName(QStringLiteral("projectExplorerTree"));
    mTree->setAccessibleName(QStringLiteral("Project documents"));
    mTree->setHeaderHidden(true);
    ConfigureItemSelection(*mTree);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTree, &QTreeWidget::customContextMenuRequested, this, &ProjectExplorer::showContextMenu);
    const auto addShortcut = [this](const QKeySequence& shortcut, ProjectItemAction action) {
        QAction* command = new QAction(mTree);
        command->setShortcut(shortcut);
        command->setShortcutContext(Qt::WidgetShortcut);
        mTree->addAction(command);
        connect(command, &QAction::triggered, this, [this, action]() { requestSelectedAction(action); });
    };
    addShortcut(QKeySequence(QStringLiteral("Ctrl+D")), ProjectItemAction::Duplicate);
    addShortcut(QKeySequence(Qt::Key_Delete), ProjectItemAction::Remove);
#if defined(Q_OS_MACOS)
    addShortcut(QKeySequence(Qt::Key_Backspace), ProjectItemAction::Remove);
#endif
    mTree->setUniformRowHeights(true);
    mTree->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(mTree, 1);
    mStatus->setObjectName(QStringLiteral("projectExplorerStatus"));
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mStatus->hide();
    layout->addWidget(mStatus);
    connect(refresh, &QToolButton::clicked, this, &ProjectExplorer::Refresh);
    connect(mFilter, &QLineEdit::textChanged, this, &ProjectExplorer::applyFilter);
    connect(mTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { activate(item); });
    Refresh();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the root path and display name are distinct.
void ProjectExplorer::SetProject(const QString& root, const QString& name)
{
    // Canonical, so listed paths compare equal to the paths documents report.
    const QString canonical = root.isEmpty() == true ? QString() : QFileInfo(root).canonicalFilePath();
    const QString nextRoot = canonical.isEmpty() == true && root.isEmpty() == false ? QDir::cleanPath(root) : canonical;
    if (nextRoot != mRoot)
    {
        mTree->clear();
    }
    mRoot = nextRoot;
    mName = name;
    Refresh();
}

QTreeWidgetItem*
ProjectExplorer::addItem(QTreeWidgetItem* parentOrNull, const QString& text, ProjectItemKind kind, const QString& path)
{
    QTreeWidgetItem* item = parentOrNull != nullptr ? new QTreeWidgetItem(parentOrNull) : new QTreeWidgetItem(mTree);
    item->setText(0, text);
    item->setIcon(0, GetEditorIcon(GetKindIcon(kind)));
    item->setData(0, KIND_ROLE, static_cast<foundation::int32>(kind));
    item->setData(0, PATH_ROLE, path);
    item->setToolTip(0, path.isEmpty() == true ? text : QDir::toNativeSeparators(path));
    return item;
}

void ProjectExplorer::Refresh()
{
    QStringList selectedPaths;
    QStringList expandedCategories;
    const bool hasPreviousTree = mTree->topLevelItemCount() != 0;
    for (QTreeWidgetItemIterator iterator(mTree); *iterator != nullptr; ++iterator)
    {
        const QTreeWidgetItem* item = *iterator;
        const QString path = item->data(0, PATH_ROLE).toString();
        if (item->isSelected() == true && path.isEmpty() == false)
        {
            selectedPaths.append(path);
        }
        if (path.isEmpty() == true && item->isExpanded() == true)
        {
            expandedCategories.append(item->text(0));
        }
    }
    mTree->clear();
    mStatus->hide();
    if (mRoot.isEmpty() == false)
    {
        QTreeWidgetItem* project = addItem(nullptr, mName, ProjectItemKind::Folder, {});
        project->setToolTip(0, QDir::toNativeSeparators(mRoot));
        static_cast<void>(addItem(project, QStringLiteral("Project Settings"), ProjectItemKind::ProjectSettings, {}));
        QTreeWidgetItem* worlds = addItem(project, QStringLiteral("Worlds"), ProjectItemKind::World, {});
        QTreeWidgetItem* graphs = addItem(project, QStringLiteral("Render Graphs"), ProjectItemKind::RenderGraph, {});
        QTreeWidgetItem* shaders = addItem(project, QStringLiteral("Shaders"), ProjectItemKind::Shader, {});
        QTreeWidgetItem* scripts = addItem(project, QStringLiteral("Scripts"), ProjectItemKind::Scripts, {});
        static_cast<void>(addItem(project, QStringLiteral("Audio"), ProjectItemKind::Audio, {}));
        static_cast<void>(addItem(project, QStringLiteral("Content"), ProjectItemKind::Content, {}));
        scan({ .Worlds = worlds, .RenderGraphs = graphs, .Shaders = shaders, .Scripts = scripts });
        project->setExpanded(true);
        worlds->setExpanded(true);
        graphs->setExpanded(true);
    }
    else
    {
        mStatus->setText(QStringLiteral("No project is open. Open or create a project to browse its documents."));
        mStatus->show();
    }
    static_cast<void>(addItem(nullptr, QStringLiteral("Configuration"), ProjectItemKind::Configuration, {}));
    for (QTreeWidgetItemIterator iterator(mTree); *iterator != nullptr; ++iterator)
    {
        QTreeWidgetItem* item = *iterator;
        const QString path = item->data(0, PATH_ROLE).toString();
        if (path.isEmpty() == true && hasPreviousTree == true)
        {
            item->setExpanded(expandedCategories.contains(item->text(0)));
        }
        if (path.isEmpty() == false && selectedPaths.contains(path) == true)
        {
            item->setSelected(true);
        }
    }
    applyFilter();
}

void ProjectExplorer::scan(const ScanFolders& folders)
{
    const QDir root(mRoot);
    QList<PendingFolder> pending{{ .Path = mRoot, .Depth = 0 }};
    foundation::int32 files = 0;
    bool isTruncated = false;
    // Breadth-first, sorted per folder, so a truncated listing keeps shallow documents.
    while (pending.isEmpty() == false && isTruncated == false)
    {
        const PendingFolder folder = pending.takeFirst();
        const QFileInfoList entries =
            QDir(folder.Path)
                .entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                               QDir::Name | QDir::DirsLast);
        for (const QFileInfo& entry : entries)
        {
            if (entry.isDir() == true)
            {
                if (folder.Depth + 1 < MAX_PROJECT_EXPLORER_DEPTH && IsSkippedFolder(entry.fileName()) == false)
                {
                    pending.append({ .Path = entry.filePath(), .Depth = folder.Depth + 1 });
                }
                continue;
            }
            const QString name = entry.fileName();
            QTreeWidgetItem* parentOrNull = nullptr;
            ProjectItemKind kind = ProjectItemKind::Folder;
            if (name.endsWith(WORLD_SUFFIX) == true)
            {
                parentOrNull = folders.Worlds;
                kind = ProjectItemKind::World;
            }
            else if (name.endsWith(RENDER_GRAPH_SUFFIX) == true)
            {
                parentOrNull = folders.RenderGraphs;
                kind = ProjectItemKind::RenderGraph;
            }
            else if (name.endsWith(SHADER_MANIFEST_SUFFIX) == true || name.endsWith(SHADER_SOURCE_SUFFIX) == true)
            {
                parentOrNull = folders.Shaders;
                kind = ProjectItemKind::Shader;
            }
            else if (name.endsWith(QLatin1StringView(".luau")) == true ||
                     name.endsWith(QLatin1StringView(".lua")) == true ||
                     name.endsWith(QLatin1StringView(".luseq.json")) == true)
            {
                parentOrNull = folders.Scripts;
                kind = ProjectItemKind::Scripts;
            }
            if (parentOrNull == nullptr)
            {
                continue;
            }
            if (files == MAX_PROJECT_EXPLORER_FILES)
            {
                isTruncated = true;
                break;
            }
            ++files;
            static_cast<void>(addItem(parentOrNull, root.relativeFilePath(entry.filePath()), kind, entry.filePath()));
        }
    }
    if (isTruncated == true)
    {
        mStatus->setText(QStringLiteral("Showing the first %1 project documents.").arg(MAX_PROJECT_EXPLORER_FILES));
        mStatus->show();
    }
}

void ProjectExplorer::applyFilter()
{
    const QString filter = mFilter->text().trimmed();
    for (QTreeWidgetItemIterator iterator(mTree); *iterator != nullptr; ++iterator)
    {
        QTreeWidgetItem* item = *iterator;
        const bool isFile = item->data(0, PATH_ROLE).toString().isEmpty() == false;
        item->setHidden(isFile == true && filter.isEmpty() == false &&
                        item->text(0).contains(filter, Qt::CaseInsensitive) == false);
    }
}

QStringList ProjectExplorer::GetFiles(ProjectItemKind kind) const
{
    QStringList files;
    for (QTreeWidgetItemIterator iterator(mTree); *iterator != nullptr; ++iterator)
    {
        const QTreeWidgetItem* item = *iterator;
        const QString path = item->data(0, PATH_ROLE).toString();
        if (path.isEmpty() == false && item->data(0, KIND_ROLE).toInt() == static_cast<foundation::int32>(kind))
        {
            files.append(path);
        }
    }
    return files;
}

bool ProjectExplorer::ActivateItem(ProjectItemKind kind, const QString& path)
{
    for (QTreeWidgetItemIterator iterator(mTree); *iterator != nullptr; ++iterator)
    {
        const QTreeWidgetItem* item = *iterator;
        if (item->data(0, KIND_ROLE).toInt() == static_cast<foundation::int32>(kind) &&
            item->data(0, PATH_ROLE).toString() == path)
        {
            activate(item);
            return true;
        }
    }
    return false;
}

void ProjectExplorer::activate(const QTreeWidgetItem* item)
{
    const ProjectItemKind kind = static_cast<ProjectItemKind>(item->data(0, KIND_ROLE).toInt());
    if (kind != ProjectItemKind::Folder &&
        (item->data(0, PATH_ROLE).toString().isEmpty() == false ||
         (kind != ProjectItemKind::World && kind != ProjectItemKind::RenderGraph && kind != ProjectItemKind::Shader)))
    {
        Q_EMIT OpenRequested(kind, item->data(0, PATH_ROLE).toString());
    }
}
void ProjectExplorer::populateAddMenu(QMenu* menu, ProjectItemKind kind)
{
    const auto add =
        [this, menu](const QString& text, ProjectItemAction action, const QString& name, ProjectItemKind target) {
            QAction* command = menu->addAction(text);
            command->setObjectName(name);
            command->setEnabled(mRoot.isEmpty() == false);
            const QString root = mRoot;
            connect(command, &QAction::triggered, this, [this, action, target, root]() {
                if (root == mRoot && root.isEmpty() == false)
                {
                    Q_EMIT ActionRequested(action, target, {});
                }
            });
        };
    if (kind == ProjectItemKind::Folder || kind == ProjectItemKind::World)
    {
        add(QStringLiteral("New World..."),
            ProjectItemAction::Create,
            QStringLiteral("projectExplorerNewWorld"),
            ProjectItemKind::World);
    }
    if (kind == ProjectItemKind::Folder || kind == ProjectItemKind::RenderGraph)
    {
        add(QStringLiteral("New Render Graph..."),
            ProjectItemAction::Create,
            QStringLiteral("projectExplorerNewRenderGraph"),
            ProjectItemKind::RenderGraph);
    }
    if (kind == ProjectItemKind::Folder || kind == ProjectItemKind::Shader)
    {
        add(QStringLiteral("New Shader..."),
            ProjectItemAction::Create,
            QStringLiteral("projectExplorerNewShader"),
            ProjectItemKind::Shader);
    }
    if (kind == ProjectItemKind::Folder || kind == ProjectItemKind::Scripts)
    {
        add(QStringLiteral("New Script..."),
            ProjectItemAction::Create,
            QStringLiteral("projectExplorerNewScript"),
            ProjectItemKind::Scripts);
    }
    if (kind == ProjectItemKind::Folder || kind == ProjectItemKind::Audio)
    {
        add(QStringLiteral("New Sound..."),
            ProjectItemAction::CreateSound,
            QStringLiteral("projectExplorerNewSound"),
            ProjectItemKind::Audio);
        add(QStringLiteral("New Music..."),
            ProjectItemAction::CreateMusic,
            QStringLiteral("projectExplorerNewMusic"),
            ProjectItemKind::Audio);
        add(QStringLiteral("Import Audio..."),
            ProjectItemAction::AddExisting,
            QStringLiteral("projectExplorerImportAudio"),
            ProjectItemKind::Audio);
    }
    if (kind == ProjectItemKind::Folder || kind == ProjectItemKind::Content)
    {
        add(QStringLiteral("Import Content..."),
            ProjectItemAction::AddExisting,
            QStringLiteral("projectExplorerImportContent"),
            ProjectItemKind::Content);
    }
    if (kind == ProjectItemKind::World || kind == ProjectItemKind::RenderGraph || kind == ProjectItemKind::Shader ||
        kind == ProjectItemKind::Scripts)
    {
        add(QStringLiteral("Add Existing File..."),
            ProjectItemAction::AddExisting,
            QStringLiteral("projectExplorerAddExisting"),
            kind);
    }
}

void ProjectExplorer::showContextMenu(const QPoint& position)
{
    QTreeWidgetItem* itemOrNull = mTree->itemAt(position);
    if (itemOrNull != nullptr && itemOrNull->isSelected() == false)
    {
        mTree->clearSelection();
        mTree->setCurrentItem(itemOrNull);
    }
    const ProjectItemKind kind = itemOrNull == nullptr
                                     ? ProjectItemKind::Folder
                                     : static_cast<ProjectItemKind>(itemOrNull->data(0, KIND_ROLE).toInt());
    const QString path = itemOrNull == nullptr ? QString() : itemOrNull->data(0, PATH_ROLE).toString();
    QMenu* menu = new QMenu(mTree);
    menu->setObjectName(QStringLiteral("projectExplorerContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (itemOrNull != nullptr && kind != ProjectItemKind::Folder &&
        (path.isEmpty() == false || kind == ProjectItemKind::ProjectSettings || kind == ProjectItemKind::Scripts ||
         kind == ProjectItemKind::Audio || kind == ProjectItemKind::Content || kind == ProjectItemKind::Configuration))
    {
        QAction* open = menu->addAction(QStringLiteral("Open"));
        open->setObjectName(QStringLiteral("projectExplorerOpen"));
        connect(open, &QAction::triggered, this, [this, kind, path]() { static_cast<void>(ActivateItem(kind, path)); });
        menu->addSeparator();
    }
    populateAddMenu(menu, kind);
    if (path.isEmpty() == false)
    {
        menu->addSeparator();
        const auto addFileAction =
            [this, menu, kind, path](const QString& text, ProjectItemAction action, const QString& name) {
                QAction* command = menu->addAction(text);
                command->setObjectName(name);
                command->setEnabled(mTree->selectedItems().size() == 1);
                const QString root = mRoot;
                connect(command, &QAction::triggered, this, [this, action, kind, path, root]() {
                    if (root == mRoot)
                    {
                        Q_EMIT ActionRequested(action, kind, path);
                    }
                });
            };
        addFileAction(QStringLiteral("Duplicate..."),
                      ProjectItemAction::Duplicate,
                      QStringLiteral("projectExplorerDuplicate"));
        addFileAction(QStringLiteral("Remove..."), ProjectItemAction::Remove, QStringLiteral("projectExplorerRemove"));
    }
    menu->addSeparator();
    connect(menu->addAction(QStringLiteral("Refresh")), &QAction::triggered, this, &ProjectExplorer::Refresh);
    menu->popup(mTree->viewport()->mapToGlobal(position));
}

void ProjectExplorer::requestSelectedAction(ProjectItemAction action)
{
    if (mTree->selectedItems().size() != 1)
    {
        return;
    }
    const QTreeWidgetItem* item = mTree->selectedItems().first();
    const QString path = item->data(0, PATH_ROLE).toString();
    if (path.isEmpty() == false)
    {
        Q_EMIT ActionRequested(action, static_cast<ProjectItemKind>(item->data(0, KIND_ROLE).toInt()), path);
    }
}
} // namespace ludus::editor
