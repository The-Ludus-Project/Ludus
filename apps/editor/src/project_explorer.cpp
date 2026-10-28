#include "internal/project_explorer.h"
#include "internal/editor_icons.h"
#include "internal/item_selection.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLatin1StringView>
#include <QLineEdit>
#include <QList>
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
    top->addWidget(refresh);
    layout->addLayout(top);
    mTree->setObjectName(QStringLiteral("projectExplorerTree"));
    mTree->setAccessibleName(QStringLiteral("Project documents"));
    mTree->setHeaderHidden(true);
    ConfigureItemSelection(*mTree);
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
    mRoot = canonical.isEmpty() == true && root.isEmpty() == false ? QDir::cleanPath(root) : canonical;
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
    mTree->clear();
    mStatus->hide();
    if (mRoot.isEmpty() == false)
    {
        QTreeWidgetItem* project = addItem(nullptr, mName, ProjectItemKind::Folder, {});
        project->setToolTip(0, QDir::toNativeSeparators(mRoot));
        static_cast<void>(addItem(project, QStringLiteral("Project Settings"), ProjectItemKind::ProjectSettings, {}));
        QTreeWidgetItem* worlds = addItem(project, QStringLiteral("Worlds"), ProjectItemKind::Folder, {});
        QTreeWidgetItem* graphs = addItem(project, QStringLiteral("Render Graphs"), ProjectItemKind::Folder, {});
        QTreeWidgetItem* shaders = addItem(project, QStringLiteral("Shaders"), ProjectItemKind::Folder, {});
        static_cast<void>(addItem(project, QStringLiteral("Scripts"), ProjectItemKind::Scripts, {}));
        static_cast<void>(addItem(project, QStringLiteral("Audio"), ProjectItemKind::Audio, {}));
        static_cast<void>(addItem(project, QStringLiteral("Content"), ProjectItemKind::Content, {}));
        scan({ .Worlds = worlds, .RenderGraphs = graphs, .Shaders = shaders });
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
    if (kind != ProjectItemKind::Folder)
    {
        Q_EMIT OpenRequested(kind, item->data(0, PATH_ROLE).toString());
    }
}
} // namespace ludus::editor
