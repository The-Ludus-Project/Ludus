#include "internal/document_well.h"

#include <ludus/foundation/base/core.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QMenu>
#include <QPoint>
#include <QPointer>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

// Thanks to Microsoft, "Customize window layouts and personalize document tabs in
// Visual Studio" (Visual Studio documentation, "Tab groups" and "Arrange windows"):
// documents live in tab groups in one direction at a time, and "New Vertical/Horizontal
// Tab Group" and "Move to Next/Previous Tab Group" move the active document between
// them. https://learn.microsoft.com/visualstudio/ide/customizing-window-layouts-in-visual-studio
// Ludus adopts that arrangement on QSplitter/QTabWidget; Qt Widgets has no drag-to-dock
// guide diamond or floating document windows, so those VS behaviours are not provided.

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 STATE_VERSION = 1;
constexpr QLatin1StringView VERSION_KEY{"version"};
constexpr QLatin1StringView ORIENTATION_KEY{"orientation"};
constexpr QLatin1StringView GROUPS_KEY{"groups"};
constexpr QLatin1StringView DOCUMENTS_KEY{"documents"};
constexpr QLatin1StringView CURRENT_KEY{"current"};
constexpr QLatin1StringView SIZES_KEY{"sizes"};
constexpr QLatin1StringView ACTIVE_KEY{"active"};
constexpr QLatin1StringView HORIZONTAL{"horizontal"};
constexpr QLatin1StringView VERTICAL{"vertical"};
// A saved arrangement lists at most every registered document once; this bounds the
// arrays read from a local preference file independently of the registry size.
constexpr qsizetype MAX_SAVED_DOCUMENTS = 64;
constexpr foundation::int32 MAX_GROUP_PIXELS = 1 << 16;
} // namespace

DocumentWell::DocumentWell(QWidget* parentOrNull)
    : QWidget(parentOrNull), mSplitter(new QSplitter(Qt::Horizontal, this)), mParking(new QWidget(this))
{
    setObjectName(QStringLiteral("documentWell"));
    setAccessibleName(QStringLiteral("Documents"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mSplitter);
    mSplitter->setObjectName(QStringLiteral("documentGroupSplitter"));
    mSplitter->setChildrenCollapsible(false);
    mParking->setObjectName(QStringLiteral("closedDocuments"));
    mParking->hide();
    mActiveGroupOrNull = createGroup(0);
    // Like VS, keyboard focus entering an open document makes it the active document,
    // even when focus is moved programmatically to a background tab.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* nowOrNull) {
        if (nowOrNull == nullptr)
        {
            return;
        }
        for (const Entry& entry : mEntries)
        {
            QTabWidget* groupOrNull = groupOfOrNull(entry.Document);
            if (groupOrNull != nullptr &&
                (entry.Document == nowOrNull || entry.Document->isAncestorOf(nowOrNull) == true))
            {
                setActiveGroup(groupOrNull);
                groupOrNull->setCurrentWidget(entry.Document);
                publishActiveDocument();
                return;
            }
        }
        for (QTabWidget* group : mGroups)
        {
            if (group->isAncestorOf(nowOrNull) == true)
            {
                setActiveGroup(group);
                return;
            }
        }
    });
}

DocumentWell::~DocumentWell()
{
    // Child teardown moves focus before QObject disconnects this receiver.
    disconnect(qApp, nullptr, this, nullptr);
    for (QTabWidget* group : mGroups)
    {
        disconnect(group, nullptr, this, nullptr);
    }
}

QTabWidget* DocumentWell::createGroup(foundation::int32 index)
{
    QTabWidget* group = new QTabWidget(mSplitter);
    group->setObjectName(QStringLiteral("documentGroup"));
    group->setAccessibleName(QStringLiteral("Document tab group"));
    group->setDocumentMode(true);
    group->setTabsClosable(true);
    group->setMovable(true);
    group->setUsesScrollButtons(true);
    group->setElideMode(Qt::ElideNone);
    // Documents own their overflow; a long tab strip must not widen the window.
    group->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    group->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    group->installEventFilter(this);
    group->tabBar()->installEventFilter(this);
    mSplitter->insertWidget(index, group);
    mGroups.insert(index, group);
    connect(group, &QTabWidget::tabCloseRequested, this, [this, group](foundation::int32 tab) {
        CloseDocument(group->widget(tab));
    });
    connect(group, &QTabWidget::currentChanged, this, [this, group]() {
        if (group == mActiveGroupOrNull)
        {
            publishActiveDocument();
        }
    });
    connect(group, &QTabWidget::tabBarClicked, this, [this, group]() { setActiveGroup(group); });
    connect(group->tabBar(), &QTabBar::customContextMenuRequested, this, [this, group](const QPoint& position) {
        showTabMenu(group, group->tabBar()->tabAt(position), group->tabBar()->mapToGlobal(position));
    });
    return group;
}

bool DocumentWell::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress)
    {
        for (QTabWidget* group : mGroups)
        {
            if (watched == group || watched == group->tabBar())
            {
                setActiveGroup(group);
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DocumentWell::AddDocument(QWidget* document, const QString& title, DocumentCloseMode closeMode)
{
    LUDUS_REQUIRE(document != nullptr && document->objectName().isEmpty() == false);
    LUDUS_REQUIRE(findEntryOrNull(document) == nullptr);
    mEntries.append({ .Document = document, .Title = title, .IsAvailable = true, .CloseMode = closeMode });
    document->setParent(mParking);
    document->hide();
}

bool DocumentWell::RemoveDocument(QWidget* document)
{
    qsizetype index = 0;
    while (index < mEntries.size() && mEntries.at(index).Document != document)
    {
        ++index;
    }
    if (index == mEntries.size())
    {
        return false;
    }
    const bool wasOpen = groupOfOrNull(document) != nullptr;
    detach(document);
    mEntries.removeAt(index);
    document->hide();
    document->setParent(nullptr);
    if (wasOpen == true)
    {
        removeEmptyGroups();
        publishActiveDocument();
        Q_EMIT LayoutChanged();
    }
    return true;
}

const DocumentWell::Entry* DocumentWell::findEntryOrNull(const QWidget* document) const
{
    for (const Entry& entry : mEntries)
    {
        if (entry.Document == document)
        {
            return &entry;
        }
    }
    return nullptr;
}

DocumentWell::Entry* DocumentWell::findEntryOrNull(const QWidget* document)
{
    for (Entry& entry : mEntries)
    {
        if (entry.Document == document)
        {
            return &entry;
        }
    }
    return nullptr;
}

QTabWidget* DocumentWell::groupOfOrNull(const QWidget* document) const
{
    for (QTabWidget* group : mGroups)
    {
        if (group->indexOf(document) >= 0)
        {
            return group;
        }
    }
    return nullptr;
}

QTabWidget* DocumentWell::activeGroup() const
{
    return mActiveGroupOrNull != nullptr ? mActiveGroupOrNull : mGroups.first();
}

void DocumentWell::insertInto(QTabWidget* group, QWidget* document, foundation::int32 index)
{
    const Entry* entryOrNull = findEntryOrNull(document);
    LUDUS_REQUIRE(entryOrNull != nullptr);
    const foundation::int32 tab = group->insertTab(index, document, entryOrNull->Title);
    group->setTabToolTip(tab, entryOrNull->Title);
}

void DocumentWell::detach(QWidget* document)
{
    QTabWidget* groupOrNull = groupOfOrNull(document);
    if (groupOrNull == nullptr)
    {
        return;
    }
    groupOrNull->removeTab(groupOrNull->indexOf(document));
    document->hide();
    document->setParent(mParking);
}

bool DocumentWell::OpenDocument(QWidget* document)
{
    const Entry* entryOrNull = findEntryOrNull(document);
    if (entryOrNull == nullptr || entryOrNull->IsAvailable == false)
    {
        return false;
    }
    QTabWidget* groupOrNull = groupOfOrNull(document);
    if (groupOrNull == nullptr)
    {
        groupOrNull = activeGroup();
        insertInto(groupOrNull, document, groupOrNull->count());
        Q_EMIT LayoutChanged();
    }
    setActiveGroup(groupOrNull);
    groupOrNull->setCurrentWidget(document);
    publishActiveDocument();
    document->setFocus(Qt::OtherFocusReason);
    return true;
}

void DocumentWell::CloseDocument(QWidget* document)
{
    if (groupOfOrNull(document) == nullptr)
    {
        return;
    }
    if (findEntryOrNull(document)->CloseMode == DocumentCloseMode::OwnerCloses)
    {
        requestClose({document});
        return;
    }
    hideDocument(document);
}

void DocumentWell::hideDocument(QWidget* document)
{
    if (groupOfOrNull(document) == nullptr)
    {
        return;
    }
    detach(document);
    removeEmptyGroups();
    publishActiveDocument();
    Q_EMIT LayoutChanged();
}

void DocumentWell::requestClose(const QList<QWidget*>& documents)
{
    // A handler may remove, and its owner then delete, any view in the list.
    QList<QPointer<QWidget>> pending;
    for (QWidget* document : documents)
    {
        pending.append(document);
    }
    for (const QPointer<QWidget>& document : pending)
    {
        if (document.isNull() == false && findEntryOrNull(document.data()) != nullptr &&
            groupOfOrNull(document.data()) != nullptr)
        {
            Q_EMIT CloseRequested(document.data());
        }
    }
}

void DocumentWell::CloseAllDocuments()
{
    QList<QWidget*> requests;
    for (const QTabWidget* group : mGroups)
    {
        for (foundation::int32 tab = 0; tab < group->count(); ++tab)
        {
            QWidget* document = group->widget(tab);
            if (findEntryOrNull(document)->CloseMode == DocumentCloseMode::OwnerCloses)
            {
                requests.append(document);
            }
        }
    }
    for (const Entry& entry : mEntries)
    {
        if (entry.CloseMode == DocumentCloseMode::Hide)
        {
            detach(entry.Document);
        }
    }
    removeEmptyGroups();
    publishActiveDocument();
    Q_EMIT LayoutChanged();
    requestClose(requests);
}

void DocumentWell::SetDocumentAvailable(QWidget* document, bool isAvailable)
{
    Entry* entryOrNull = findEntryOrNull(document);
    if (entryOrNull == nullptr)
    {
        return;
    }
    entryOrNull->IsAvailable = isAvailable;
    if (isAvailable == false)
    {
        // Unavailability is not a user close, so an owner-closed view is hidden as well.
        hideDocument(document);
    }
}

bool DocumentWell::IsDocumentAvailable(const QWidget* document) const
{
    const Entry* entryOrNull = findEntryOrNull(document);
    return entryOrNull != nullptr && entryOrNull->IsAvailable == true;
}

bool DocumentWell::IsDocumentOpen(const QWidget* document) const
{
    return groupOfOrNull(document) != nullptr;
}

bool DocumentWell::IsDocumentOwnerClosed(const QWidget* document) const
{
    const Entry* entryOrNull = findEntryOrNull(document);
    return entryOrNull != nullptr && entryOrNull->CloseMode == DocumentCloseMode::OwnerCloses;
}

void DocumentWell::SetDocumentTitle(QWidget* document, const QString& title)
{
    Entry* entryOrNull = findEntryOrNull(document);
    if (entryOrNull == nullptr || entryOrNull->Title == title)
    {
        return;
    }
    entryOrNull->Title = title;
    QTabWidget* groupOrNull = groupOfOrNull(document);
    if (groupOrNull != nullptr)
    {
        const foundation::int32 tab = groupOrNull->indexOf(document);
        groupOrNull->setTabText(tab, title);
        groupOrNull->setTabToolTip(tab, title);
    }
}

QString DocumentWell::GetDocumentTitle(const QWidget* document) const
{
    const Entry* entryOrNull = findEntryOrNull(document);
    return entryOrNull != nullptr ? entryOrNull->Title : QString();
}

QWidget* DocumentWell::GetActiveDocumentOrNull() const
{
    return activeGroup()->currentWidget();
}

QList<QWidget*> DocumentWell::GetDocuments() const
{
    QList<QWidget*> documents;
    for (const Entry& entry : mEntries)
    {
        documents.append(entry.Document);
    }
    return documents;
}

foundation::int32 DocumentWell::GetGroupCount() const
{
    return static_cast<foundation::int32>(mGroups.size());
}

foundation::int32 DocumentWell::GetGroupIndex(const QWidget* document) const
{
    QTabWidget* groupOrNull = groupOfOrNull(document);
    return groupOrNull != nullptr ? static_cast<foundation::int32>(mGroups.indexOf(groupOrNull)) : -1;
}

foundation::int32 DocumentWell::GetActiveGroupIndex() const
{
    return static_cast<foundation::int32>(mGroups.indexOf(activeGroup()));
}

Qt::Orientation DocumentWell::GetSplitOrientation() const
{
    return mSplitter->orientation();
}

bool DocumentWell::CanSplit(Qt::Orientation orientation) const
{
    return activeGroup()->count() >= 2 && GetGroupCount() < MAX_DOCUMENT_GROUPS &&
           (GetGroupCount() == 1 || mSplitter->orientation() == orientation);
}

bool DocumentWell::SplitActiveDocument(Qt::Orientation orientation)
{
    if (CanSplit(orientation) == false)
    {
        return false;
    }
    QTabWidget* source = activeGroup();
    QWidget* document = source->currentWidget();
    mSplitter->setOrientation(orientation);
    QTabWidget* target = createGroup(static_cast<foundation::int32>(mGroups.indexOf(source)) + 1);
    detach(document);
    insertInto(target, document, 0);
    // Give the groups equal space, like a fresh VS tab group.
    QList<foundation::int32> sizes;
    const foundation::int32 extent = orientation == Qt::Horizontal ? mSplitter->width() : mSplitter->height();
    for (qsizetype i = 0; i < mGroups.size(); ++i)
    {
        sizes.append(extent / static_cast<foundation::int32>(mGroups.size()));
    }
    mSplitter->setSizes(sizes);
    OpenDocument(document);
    Q_EMIT LayoutChanged();
    return true;
}

bool DocumentWell::MoveActiveDocument(foundation::int32 offset)
{
    QTabWidget* source = activeGroup();
    QWidget* documentOrNull = source->currentWidget();
    const qsizetype target = mGroups.indexOf(source) + offset;
    if (documentOrNull == nullptr || target < 0 || target >= mGroups.size())
    {
        return false;
    }
    QTabWidget* destination = mGroups.at(target);
    detach(documentOrNull);
    insertInto(destination, documentOrNull, destination->count());
    removeEmptyGroups();
    OpenDocument(documentOrNull);
    Q_EMIT LayoutChanged();
    return true;
}

void DocumentWell::MergeGroups()
{
    if (mGroups.size() < 2)
    {
        return;
    }
    QWidget* activeOrNull = GetActiveDocumentOrNull();
    QTabWidget* first = mGroups.first();
    for (qsizetype i = 1; i < mGroups.size(); ++i)
    {
        QTabWidget* group = mGroups.at(i);
        while (group->count() > 0)
        {
            QWidget* document = group->widget(0);
            detach(document);
            insertInto(first, document, first->count());
        }
    }
    removeEmptyGroups();
    setActiveGroup(first);
    if (activeOrNull != nullptr)
    {
        first->setCurrentWidget(activeOrNull);
    }
    publishActiveDocument();
    Q_EMIT LayoutChanged();
}

void DocumentWell::removeEmptyGroups()
{
    for (qsizetype i = mGroups.size() - 1; i >= 0 && mGroups.size() > 1; --i)
    {
        QTabWidget* group = mGroups.at(i);
        if (group->count() != 0)
        {
            continue;
        }
        mGroups.removeAt(i);
        if (mActiveGroupOrNull == group)
        {
            mActiveGroupOrNull = mGroups.at(i > 0 ? i - 1 : 0);
        }
        disconnect(group, nullptr, this, nullptr);
        group->hide();
        group->deleteLater();
    }
}

void DocumentWell::setActiveGroup(QTabWidget* group)
{
    if (group == mActiveGroupOrNull)
    {
        return;
    }
    mActiveGroupOrNull = group;
    publishActiveDocument();
}

void DocumentWell::publishActiveDocument()
{
    QWidget* currentOrNull = GetActiveDocumentOrNull();
    if (currentOrNull == mPublishedDocumentOrNull)
    {
        return;
    }
    mPublishedDocumentOrNull = currentOrNull;
    Q_EMIT ActiveDocumentChanged(currentOrNull);
}

void DocumentWell::showTabMenu(QTabWidget* group, foundation::int32 index, const QPoint& position)
{
    if (index < 0)
    {
        return;
    }
    QWidget* document = group->widget(index);
    setActiveGroup(group);
    group->setCurrentIndex(index);
    QMenu menu(this);
    QAction* close = menu.addAction(QStringLiteral("&Close"));
    QAction* closeOthers = menu.addAction(QStringLiteral("Close All &But This"));
    menu.addSeparator();
    QAction* vertical = menu.addAction(QStringLiteral("New &Vertical Tab Group"));
    QAction* horizontal = menu.addAction(QStringLiteral("New &Horizontal Tab Group"));
    QAction* next = menu.addAction(QStringLiteral("Move to &Next Tab Group"));
    QAction* previous = menu.addAction(QStringLiteral("Move to &Previous Tab Group"));
    vertical->setEnabled(CanSplit(Qt::Horizontal));
    horizontal->setEnabled(CanSplit(Qt::Vertical));
    const qsizetype groupPosition = mGroups.indexOf(group);
    next->setEnabled(groupPosition + 1 < mGroups.size());
    previous->setEnabled(groupPosition > 0);
    QAction* chosenOrNull = menu.exec(position);
    if (chosenOrNull == close)
    {
        CloseDocument(document);
    }
    else if (chosenOrNull == closeOthers)
    {
        QList<QWidget*> requests;
        for (const Entry& entry : mEntries)
        {
            if (entry.Document == document || groupOfOrNull(entry.Document) == nullptr)
            {
                continue;
            }
            if (entry.CloseMode == DocumentCloseMode::OwnerCloses)
            {
                requests.append(entry.Document);
            }
            else
            {
                detach(entry.Document);
            }
        }
        removeEmptyGroups();
        OpenDocument(document);
        Q_EMIT LayoutChanged();
        requestClose(requests);
    }
    else if (chosenOrNull == vertical)
    {
        SplitActiveDocument(Qt::Horizontal);
    }
    else if (chosenOrNull == horizontal)
    {
        SplitActiveDocument(Qt::Vertical);
    }
    else if (chosenOrNull == next || chosenOrNull == previous)
    {
        MoveActiveDocument(chosenOrNull == next ? 1 : -1);
    }
}

QJsonObject DocumentWell::SaveState() const
{
    QJsonArray groups;
    for (const QTabWidget* group : mGroups)
    {
        QJsonArray documents;
        for (foundation::int32 tab = 0; tab < group->count(); ++tab)
        {
            documents.append(group->widget(tab)->objectName());
        }
        const QWidget* currentOrNull = group->currentWidget();
        groups.append(QJsonObject{
            {QString(DOCUMENTS_KEY), documents},
            {QString(CURRENT_KEY), currentOrNull != nullptr ? currentOrNull->objectName() : QString()},
        });
    }
    QJsonArray sizes;
    for (const foundation::int32 size : mSplitter->sizes())
    {
        sizes.append(size);
    }
    return {
        {QString(VERSION_KEY), STATE_VERSION},
        {QString(ORIENTATION_KEY),
         mSplitter->orientation() == Qt::Horizontal ? QString(HORIZONTAL) : QString(VERTICAL)},
        {QString(GROUPS_KEY), groups},
        {QString(SIZES_KEY), sizes},
        {QString(ACTIVE_KEY), GetActiveGroupIndex()},
    };
}

bool DocumentWell::RestoreState(const QJsonObject& state)
{
    const QJsonArray groups = state.value(GROUPS_KEY).toArray();
    const QString orientation = state.value(ORIENTATION_KEY).toString();
    if (state.value(VERSION_KEY).toInt() != STATE_VERSION || groups.isEmpty() == true ||
        groups.size() > MAX_DOCUMENT_GROUPS || (orientation != HORIZONTAL && orientation != VERTICAL))
    {
        return false;
    }
    // Resolve every saved ID before changing anything, so malformed input has no effect.
    QList<QList<QWidget*>> resolved;
    QList<QWidget*> currents;
    QList<QWidget*> seen;
    for (const QJsonValueConstRef value : groups)
    {
        const QJsonArray documents = value.toObject().value(DOCUMENTS_KEY).toArray();
        if (value.isObject() == false || documents.size() > MAX_SAVED_DOCUMENTS)
        {
            return false;
        }
        QList<QWidget*> group;
        for (const QJsonValueConstRef id : documents)
        {
            for (const Entry& entry : mEntries)
            {
                if (entry.IsAvailable == true && entry.Document->objectName() == id.toString() &&
                    seen.contains(entry.Document) == false)
                {
                    group.append(entry.Document);
                    seen.append(entry.Document);
                }
            }
        }
        QWidget* currentOrNull = nullptr;
        for (QWidget* document : group)
        {
            if (document->objectName() == value.toObject().value(CURRENT_KEY).toString())
            {
                currentOrNull = document;
            }
        }
        if (group.isEmpty() == false)
        {
            resolved.append(group);
            currents.append(currentOrNull);
        }
    }
    // Restoring only rearranges: every view is hidden, without asking owners to close.
    for (const Entry& entry : mEntries)
    {
        detach(entry.Document);
    }
    removeEmptyGroups();
    if (resolved.isEmpty() == true)
    {
        publishActiveDocument();
        Q_EMIT LayoutChanged();
        return true;
    }
    mSplitter->setOrientation(orientation == HORIZONTAL ? Qt::Horizontal : Qt::Vertical);
    for (qsizetype i = 0; i < resolved.size(); ++i)
    {
        QTabWidget* group = i == 0 ? mGroups.first() : createGroup(static_cast<foundation::int32>(i));
        for (QWidget* document : resolved.at(i))
        {
            insertInto(group, document, group->count());
        }
        if (currents.at(i) != nullptr)
        {
            group->setCurrentWidget(currents.at(i));
        }
    }
    const QJsonArray sizes = state.value(SIZES_KEY).toArray();
    if (sizes.size() == mGroups.size())
    {
        QList<foundation::int32> values;
        for (const QJsonValueConstRef size : sizes)
        {
            values.append(qBound(1, size.toInt(), MAX_GROUP_PIXELS));
        }
        mSplitter->setSizes(values);
    }
    const foundation::int32 active = state.value(ACTIVE_KEY).toInt();
    setActiveGroup(mGroups.at(active >= 0 && active < GetGroupCount() ? active : 0));
    publishActiveDocument();
    Q_EMIT LayoutChanged();
    return true;
}
} // namespace ludus::editor
