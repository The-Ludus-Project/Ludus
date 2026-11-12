#include "internal/world_entity_search.h"

#include "design_tokens.h"
#include "internal/editor_icons.h"
#include "internal/world_document.h"

#include <QHBoxLayout>
#include <QHash>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace ludus::editor
{
namespace
{
/// The icon that says what an entity is, from the components it carries, so the list can be scanned by kind.
EditorIcon GetEntityIcon(const QJsonObject& entity)
{
    const QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    if (components.contains(QStringLiteral("camera")) == true)
    {
        return EditorIcon::Camera;
    }
    if (components.contains(QStringLiteral("firstPersonPlayer")) == true)
    {
        return EditorIcon::Player;
    }
    if (components.contains(QStringLiteral("atmosphere")) == true)
    {
        return EditorIcon::Atmosphere;
    }
    if (components.contains(QStringLiteral("light")) == true)
    {
        const QString kind =
            components.value(QStringLiteral("light")).toObject().value(QStringLiteral("kind")).toString();
        return kind == QStringLiteral("directional") ? EditorIcon::Sun : EditorIcon::Lighting;
    }
    if (components.contains(QStringLiteral("renderable")) == true || components.contains(QStringLiteral("terrain")))
    {
        return EditorIcon::Mesh;
    }
    return EditorIcon::Scene;
}

void ExpandAncestors(const QTreeWidgetItem& item)
{
    for (QTreeWidgetItem* ancestorOrNull = item.parent(); ancestorOrNull != nullptr;
         ancestorOrNull = ancestorOrNull->parent())
    {
        ancestorOrNull->setExpanded(true);
    }
}

bool IsBranchExpanded(const QTreeWidgetItem& item)
{
    for (const QTreeWidgetItem* ancestorOrNull = item.parent(); ancestorOrNull != nullptr;
         ancestorOrNull = ancestorOrNull->parent())
    {
        if (ancestorOrNull->isExpanded() == false)
        {
            return false;
        }
    }
    return true;
}
bool DeselectRow(QTreeWidget& inoutTree, QTreeWidgetItem& inoutItem)
{
    const bool isCurrent = inoutTree.currentItem() == &inoutItem;
    const bool isSelected = inoutItem.isSelected();
    if (isSelected == true)
    {
        inoutItem.setSelected(false);
    }
    if (isCurrent == true)
    {
        inoutTree.setCurrentItem(nullptr, 0, QItemSelectionModel::NoUpdate);
    }
    return isCurrent == true || isSelected == true;
}
} // namespace

WorldEntitySearch::WorldEntitySearch(QTreeWidget& inoutEntities, QWidget* parentOrNull)
    : QWidget(parentOrNull), mEntities(inoutEntities), mSearch(new QLineEdit(this)), mCount(new QLabel(this))
{
    // The count sits at the end of the search row in the quiet text colour ("37", "5 of 37"), so it costs no line.
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(design::SPACING_SMALL);
    mSearch->setObjectName(QStringLiteral("worldEntitySearch"));
    mSearch->setAccessibleName(QStringLiteral("Search World entities"));
    mSearch->setPlaceholderText(QStringLiteral("Search entities…"));
    mSearch->setToolTip(QStringLiteral("Filter by name, label or stable ID (case insensitive). Ancestors of matches "
                                       "remain visible for context. Clear the search to show all entities."));
    mSearch->setClearButtonEnabled(true);
    mCount->setObjectName(QStringLiteral("worldEntityCount"));
    mCount->setTextFormat(Qt::PlainText);
    mCount->setProperty("visualRole", QStringLiteral("secondary"));
    layout->addWidget(mSearch, 1);
    layout->addWidget(mCount);
    connect(mSearch, &QLineEdit::textChanged, this, [this]() { refresh(); });
    connect(&mEntities, &QTreeWidget::itemCollapsed, this, [this]() { deselectCollapsedRows(); });
    refresh();
}

void WorldEntitySearch::Rebuild(const WorldDocument& document, const QStringList& selection, const QString& current)
{
    const QSignalBlocker blocker(&mEntities);
    QSet<QString> collapsed;
    for (QTreeWidgetItemIterator iterator(&mEntities); *iterator != nullptr; ++iterator)
    {
        const QTreeWidgetItem* item = *iterator;
        if (item->childCount() > 0 && item->isExpanded() == false)
        {
            collapsed.insert(item->data(0, Qt::UserRole).toString());
        }
    }

    mEntities.clear();
    const QJsonArray entities = document.Draft().value(QStringLiteral("entities")).toArray();
    QHash<QString, QTreeWidgetItem*> rows;
    rows.reserve(entities.size());
    for (const QJsonValue value : entities)
    {
        const QJsonObject entity = value.toObject();
        const QString key = document.GetEntityKey(entity);
        const QString label = document.GetEntityLabel(entity);
        QTreeWidgetItem* item = new QTreeWidgetItem(QStringList{entity.value(QStringLiteral("name")).toString()});
        item->setData(0, Qt::UserRole, key);
        item->setToolTip(0, label == key ? key : label + QLatin1Char('\n') + key);
        item->setIcon(0, GetEditorIcon(GetEntityIcon(entity)));
        rows.insert(key, item);
    }
    // Allocate all rows first so authored order need not put a parent before its children.
    for (const QJsonValue value : entities)
    {
        const QJsonObject entity = value.toObject();
        QTreeWidgetItem* item = rows.value(document.GetEntityKey(entity));
        const QString parentKey =
            entity.value(QStringLiteral("scene")).toObject().value(QStringLiteral("parent")).toString();
        QTreeWidgetItem* parentOrNull = rows.value(parentKey);
        if (parentOrNull != nullptr)
        {
            parentOrNull->addChild(item);
        }
        else
        {
            mEntities.addTopLevelItem(item);
        }
    }
    mEntities.expandAll();
    for (const QString& key : collapsed)
    {
        QTreeWidgetItem* itemOrNull = rows.value(key);
        if (itemOrNull != nullptr)
        {
            itemOrNull->setExpanded(false);
        }
    }
    for (const QString& key : selection)
    {
        QTreeWidgetItem* itemOrNull = rows.value(key);
        if (itemOrNull != nullptr)
        {
            ExpandAncestors(*itemOrNull);
            itemOrNull->setSelected(true);
        }
    }
    mEntities.setCurrentItem(rows.value(current), 0, QItemSelectionModel::NoUpdate);
    refresh();
}

void WorldEntitySearch::UpdateLabels(const WorldDocument& document)
{
    for (QTreeWidgetItemIterator iterator(&mEntities); *iterator != nullptr; ++iterator)
    {
        QTreeWidgetItem* item = *iterator;
        const QString key = item->data(0, Qt::UserRole).toString();
        item->setText(0, document.Entity(key).value(QStringLiteral("name")).toString());
    }
}

void WorldEntitySearch::RevealEntity(const QString& id)
{
    for (QTreeWidgetItemIterator iterator(&mEntities); *iterator != nullptr; ++iterator)
    {
        QTreeWidgetItem* item = *iterator;
        if (item->data(0, Qt::UserRole).toString() == id)
        {
            ExpandAncestors(*item);
            if (item->flags().testFlag(Qt::ItemIsSelectable) == false)
            {
                mSearch->clear();
            }
            return;
        }
    }
}

void WorldEntitySearch::refresh()
{
    const QString query = mSearch->text().trimmed();
    QSet<QTreeWidgetItem*> matches;
    QSet<QTreeWidgetItem*> ancestors;
    qsizetype totalCount = 0;
    for (QTreeWidgetItemIterator iterator(&mEntities); *iterator != nullptr; ++iterator)
    {
        QTreeWidgetItem* item = *iterator;
        ++totalCount;
        if (query.isEmpty() == false && (item->text(0).contains(query, Qt::CaseInsensitive) == true ||
                                         item->toolTip(0).contains(query, Qt::CaseInsensitive) == true))
        {
            matches.insert(item);
            for (QTreeWidgetItem* ancestorOrNull = item->parent();
                 ancestorOrNull != nullptr && ancestors.contains(ancestorOrNull) == false;
                 ancestorOrNull = ancestorOrNull->parent())
            {
                ancestors.insert(ancestorOrNull);
            }
        }
    }
    bool hasSelectionChanged = false;
    {
        const QSignalBlocker blocker(&mEntities);
        for (QTreeWidgetItemIterator iterator(&mEntities); *iterator != nullptr; ++iterator)
        {
            QTreeWidgetItem* item = *iterator;
            const bool isMatch = query.isEmpty() == true || matches.contains(item) == true;
            const bool isAncestor = ancestors.contains(item);
            if (isMatch == false && DeselectRow(mEntities, *item) == true)
            {
                hasSelectionChanged = true;
            }
            // Context rows keep the path readable without joining Select All or other group edits.
            item->setFlags(isMatch == true ? item->flags() | Qt::ItemIsSelectable
                                           : item->flags() & ~Qt::ItemIsSelectable);
            item->setHidden(isMatch == false && isAncestor == false);
            if (isAncestor == true)
            {
                item->setExpanded(true);
            }
        }
    }
    mCount->setText(query.isEmpty() == true ? QString::number(totalCount)
                                            : QStringLiteral("%1 of %2").arg(matches.size()).arg(totalCount));
    const QString spoken = query.isEmpty() == true
                               ? QStringLiteral("%1 entities").arg(totalCount)
                               : QStringLiteral("%1 of %2 entities match").arg(matches.size()).arg(totalCount);
    mCount->setAccessibleName(spoken);
    mCount->setToolTip(matches.isEmpty() == true && query.isEmpty() == false
                           ? QStringLiteral("No matching entities. Clear or change the search to show entities.")
                           : spoken);
    if (hasSelectionChanged == true)
    {
        Q_EMIT SelectionChanged();
    }
}

void WorldEntitySearch::deselectCollapsedRows()
{
    bool hasSelectionChanged = false;
    {
        // Rebuild the inspector once even when a branch or query hides a large selected group.
        const QSignalBlocker blocker(&mEntities);
        for (QTreeWidgetItem* item : mEntities.selectedItems())
        {
            if (IsBranchExpanded(*item) == false && DeselectRow(mEntities, *item) == true)
            {
                hasSelectionChanged = true;
            }
        }
        QTreeWidgetItem* currentOrNull = mEntities.currentItem();
        if (currentOrNull != nullptr && IsBranchExpanded(*currentOrNull) == false &&
            DeselectRow(mEntities, *currentOrNull) == true)
        {
            hasSelectionChanged = true;
        }
    }
    if (hasSelectionChanged == true)
    {
        Q_EMIT SelectionChanged();
    }
}
} // namespace ludus::editor
