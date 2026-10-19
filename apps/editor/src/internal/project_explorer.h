#pragma once

// ProjectExplorer: the Project Explorer tool window (private editor header, not installed).
//
// A read-only tree of the open project's documents: project settings, worlds,
// render graphs, shaders, scripts, audio and content. Activating an item asks the
// shell to open the matching document; the explorer never opens, edits or writes
// files itself. The tree is a bounded snapshot of the project folder, refreshed on
// project changes, saves and explicit Refresh. Symbolic links, hidden folders and
// build output folders are not scanned.

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Kind of a Project Explorer item; it selects the document the shell opens.
enum class ProjectItemKind : foundation::uint8
{
    Folder,
    ProjectSettings,
    World,
    RenderGraph,
    Shader,
    Scripts,
    Audio,
    Content,
    Configuration,
};

/// Most project files one scan lists; the tree says when a project has more.
inline constexpr foundation::int32 MAX_PROJECT_EXPLORER_FILES = 4096;
/// Deepest folder level below the project root that a scan enters.
inline constexpr foundation::int32 MAX_PROJECT_EXPLORER_DEPTH = 12;

/// Project Explorer tool window. UI-thread only.
class ProjectExplorer final : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectExplorer(QWidget* parentOrNull = nullptr);

    /// Show the project rooted at root (its source folder) under name; an empty root
    /// shows only the project-independent items. Rescans immediately.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the root path and display name are distinct.
    void SetProject(const QString& root, const QString& name);
    /// Rescan the current project folder.
    void Refresh();
    /// Absolute paths of the listed files of one kind, in tree order.
    [[nodiscard]] QStringList GetFiles(ProjectItemKind kind) const;
    /// Activate the item for kind and path (empty path for document items such as
    /// Scripts), exactly like a double-click. False when no such item is listed.
    bool ActivateItem(ProjectItemKind kind, const QString& path);

Q_SIGNALS:
    /// The user asked to open an item. path is absolute for file items, else empty.
    void OpenRequested(ludus::editor::ProjectItemKind kind, const QString& path);

private:
    [[nodiscard]] QTreeWidgetItem*
    addItem(QTreeWidgetItem* parentOrNull, const QString& text, ProjectItemKind kind, const QString& path);
    /// Tree folders that receive each kind of scanned file.
    struct ScanFolders final
    {
        QTreeWidgetItem* Worlds;
        QTreeWidgetItem* RenderGraphs;
        QTreeWidgetItem* Shaders;
    };
    void scan(const ScanFolders& folders);
    void applyFilter();
    void activate(const QTreeWidgetItem* item);

    QString mRoot;
    QString mName;
    QLineEdit* mFilter;
    QTreeWidget* mTree;
    QLabel* mStatus;
};
} // namespace ludus::editor
