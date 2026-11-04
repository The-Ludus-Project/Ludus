#include "internal/code_editor_area.h"

#include "internal/code_document_tab.h"
#include "internal/code_file_document.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QSplitter>
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
}

CodeEditorArea::~CodeEditorArea() = default;

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
        if (tab->IsDirty() == true)
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
        Q_EMIT TabsChanged();
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
    Q_EMIT ActiveTabChanged(GetActiveTabOrNull());
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
    if (tab->IsDirty() == true)
    {
        Q_EMIT DirtyCloseRequested(tab);
        return;
    }
    removeTab(tab);
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
