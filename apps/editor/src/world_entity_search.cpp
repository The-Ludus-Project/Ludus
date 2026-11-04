#include "internal/world_entity_search.h"

#include <ludus/foundation/base/types.h>

#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ludus::editor
{
WorldEntitySearch::WorldEntitySearch(QListWidget& inoutEntities, QWidget* parentOrNull)
    : QWidget(parentOrNull), mEntities(inoutEntities), mSearch(new QLineEdit(this)), mCount(new QLabel(this))
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mSearch->setObjectName(QStringLiteral("worldEntitySearch"));
    mSearch->setAccessibleName(QStringLiteral("Search World entities"));
    mSearch->setPlaceholderText(QStringLiteral("Search entities…"));
    mSearch->setToolTip(QStringLiteral("Filter by name, label or stable ID (case insensitive). Hidden entities are "
                                       "deselected. Clear the search to show all entities."));
    mSearch->setClearButtonEnabled(true);
    mCount->setObjectName(QStringLiteral("worldEntityCount"));
    mCount->setTextFormat(Qt::PlainText);
    mCount->setWordWrap(true);
    layout->addWidget(mSearch);
    layout->addWidget(mCount);
    connect(mSearch, &QLineEdit::textChanged, this, [this]() { Refresh(); });
    Refresh();
}
void WorldEntitySearch::RevealEntity(const QString& id)
{
    for (foundation::int32 index = 0; index < mEntities.count(); ++index)
    {
        const QListWidgetItem* item = mEntities.item(index);
        if (item->data(Qt::UserRole).toString() == id && item->isHidden() == true)
        {
            mSearch->clear();
            return;
        }
    }
}
void WorldEntitySearch::Refresh()
{
    const QString query = mSearch->text().trimmed();
    foundation::int32 visibleCount = 0;
    bool hasSelectionChanged = false;
    {
        // Rebuild the owning inspector once, even when a query hides a large selected group.
        const QSignalBlocker blocker(&mEntities);
        for (foundation::int32 index = 0; index < mEntities.count(); ++index)
        {
            QListWidgetItem* item = mEntities.item(index);
            const bool isVisible = query.isEmpty() == true ||
                                   item->text().contains(query, Qt::CaseInsensitive) == true ||
                                   item->toolTip().contains(query, Qt::CaseInsensitive) == true;
            if (isVisible == false && (item->isSelected() == true || mEntities.currentItem() == item))
            {
                hasSelectionChanged = true;
            }
            item->setHidden(isVisible == false);
            item->setFlags(isVisible == true ? item->flags() | Qt::ItemIsSelectable
                                             : item->flags() & ~Qt::ItemIsSelectable);
            if (isVisible == true)
            {
                ++visibleCount;
            }
            else
            {
                item->setSelected(false);
                if (mEntities.currentItem() == item)
                {
                    mEntities.setCurrentItem(nullptr, QItemSelectionModel::NoUpdate);
                }
            }
        }
    }
    mCount->setText(query.isEmpty() == true
                        ? QStringLiteral("%1 entities").arg(mEntities.count())
                        : QStringLiteral("%1 of %2 entities").arg(visibleCount).arg(mEntities.count()));
    mCount->setToolTip(visibleCount == 0 && query.isEmpty() == false
                           ? QStringLiteral("No matching entities. Clear or change the search to show entities.")
                           : QString());
    if (hasSelectionChanged == true)
    {
        Q_EMIT SelectionChanged();
    }
}
} // namespace ludus::editor
