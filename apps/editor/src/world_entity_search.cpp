#include "internal/world_entity_search.h"

#include "internal/world_document.h"

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
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
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
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mSearch->setObjectName(QStringLiteral("worldEntitySearch"));
    mSearch->setAccessibleName(QStringLiteral("Search World entities"));
    mSearch->setPlaceholderText(QStringLiteral("Search entities…"));
    mSearch->setToolTip(QStringLiteral("Filter by name, label or stable ID (case insensitive). Ancestors of matches "
                                       "remain visible for context. Clear the search to show all entities."));
    mSearch->setClearButtonEnabled(true);
    mCount->setObjectName(QStringLiteral("worldEntityCount"));
    mCount->setTextFormat(Qt::PlainText);
    mCount->setWordWrap(true);
    layout->addWidget(mSearch);
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
    mCount->setText(query.isEmpty() == true ? QStringLiteral("%1 entities").arg(totalCount)
                                            : QStringLiteral("%1 of %2 entities").arg(matches.size()).arg(totalCount));
    mCount->setToolTip(matches.isEmpty() == true && query.isEmpty() == false
                           ? QStringLiteral("No matching entities. Clear or change the search to show entities.")
                           : QString());
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
