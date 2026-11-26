#include "internal/world_materials_panel.h"

#include "design_tokens.h"
#include "internal/caption_label.h"
#include "internal/editor_icons.h"
#include "internal/inspector_fields.h"
#include "internal/material_inspection.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <ludus/foundation/base/types.h>
#include <ludus/runtime/resources/scene.h>

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <string_view>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 NAME_COLUMN = 0;
constexpr foundation::int32 COUNT_COLUMN = 1;
constexpr foundation::int32 MIN_LIST_HEIGHT = 120;
/// Characters of a resource ID's end that tell apart materials sharing a name ("metal_door · 2d6e").
constexpr qsizetype DISTINGUISHING_ID_LENGTH = 4;

/// A row's shown name: the material's name, followed by the end of its resource ID when another listed material
/// has the same name, so two imported "metal_door" materials read as two materials rather than one listed twice.
QString GetShownName(const WorldMaterialRow& row, const QHash<QString, qsizetype>& nameCounts)
{
    if (nameCounts.value(row.Name) <= 1)
    {
        return row.Name;
    }
    return QStringLiteral("%1 · %2").arg(row.Name, row.ResourceId.right(DISTINGUISHING_ID_LENGTH));
}

QString DescribeUsers(qsizetype count)
{
    return count == 1 ? QStringLiteral("Used by 1 renderable") : QStringLiteral("Used by %1 renderables").arg(count);
}

QHash<QString, QString> CollectPresentedNames(const QJsonObject& level, const RenderScene& scene)
{
    QHash<QString, QString> names;
    if (scene.Definition == nullptr)
    {
        return names;
    }

    QHash<QString, QString> bindings;
    const QJsonArray objects = level.value(QStringLiteral("objects")).toArray();
    for (const QJsonValue value : objects)
    {
        const QJsonObject placement = value.toObject();
        bindings.insert(placement.value(QStringLiteral("id")).toString(),
                        WorldDocument::GetMaterialResourceId(placement, level));
    }
    for (const PreviewObject& object : scene.Objects)
    {
        const QString id = bindings.value(object.Id);
        if (id.isEmpty() == true || names.contains(id) == true || object.Surface < 0 ||
            static_cast<foundation::usize>(object.Surface) >= scene.Definition->Materials.GetSize())
        {
            continue;
        }
        const resources::SceneMaterial& material =
            scene.Definition->Materials[static_cast<foundation::usize>(object.Surface)];
        const std::string_view name = material.Name.View();
        // A material the source scene does not name reads "Material N", as the Name row in Properties does, so the
        // list and Properties call the same material by the same name.
        names.insert(id,
                     name.empty() == true ? GetFallbackMaterialName(object.Surface + 1)
                                          : QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    }
    return names;
}

/// The part of a resource ID after its last slash. A resource the presented scene does not describe is listed by
/// it, since it is the part of the ID that tells materials apart; the kind before the slash is the same for all.
QString GetResourceTail(const QString& resourceId)
{
    const QString tail = resourceId.section(QLatin1Char('/'), -1);
    return tail.isEmpty() == true ? resourceId : tail;
}

QString GetItemId(const QTreeWidgetItem& item)
{
    return item.data(NAME_COLUMN, Qt::UserRole).toString();
}

const QTreeWidgetItem* GetResourceItemOrNull(const QTreeWidgetItem* itemOrNull)
{
    if (itemOrNull == nullptr || itemOrNull->parent() == nullptr)
    {
        return itemOrNull;
    }
    return itemOrNull->parent();
}
} // namespace

QVector<WorldMaterialRow> CollectWorldMaterialRows(const WorldDocument& world,
                                                   const QJsonObject& presentedLevel,
                                                   const RenderScene& presentedScene)
{
    const QJsonObject draft = world.Draft();
    const QJsonObject level = draft.value(QStringLiteral("scene")).toObject();
    if (level.isEmpty() == true)
    {
        return {};
    }
    const QHash<QString, QString> names = CollectPresentedNames(presentedLevel, presentedScene);
    QHash<QString, qsizetype> indices;
    QVector<WorldMaterialRow> rows;
    const QJsonArray entities = draft.value(QStringLiteral("entities")).toArray();
    for (const QJsonValue value : entities)
    {
        const QJsonObject entity = value.toObject();
        const QString id =
            WorldDocument::GetMaterialResourceId(entity.value(QStringLiteral("scene")).toObject(), level);
        if (id.isEmpty() == true)
        {
            continue;
        }
        const qsizetype index = indices.value(id, rows.size());
        if (index == rows.size())
        {
            const QString name = names.value(id);
            rows.append({id, name.isEmpty() == true ? GetResourceTail(id) : name, {}});
            indices.insert(id, index);
        }
        rows[index].Users.append({world.GetEntityKey(entity), entity.value(QStringLiteral("name")).toString()});
    }
    return rows;
}

WorldMaterialsPanel::WorldMaterialsPanel(QWidget* parent)
    : QWidget(parent), mTree(new QTreeWidget(this)), mCopy(new QPushButton(QStringLiteral("Copy ID"), this)),
      mEdit(new QPushButton(QStringLiteral("Edit Material…"), this)), mStatus(new CaptionLabel(this))
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mTree->setObjectName(QStringLiteral("worldMaterials"));
    mTree->setAccessibleName(QStringLiteral("World material resources and their renderables"));
    mTree->setHeaderLabels({QStringLiteral("Material"), QStringLiteral("Used by")});
    mTree->header()->setStretchLastSection(false);
    mTree->header()->setSectionResizeMode(NAME_COLUMN, QHeaderView::Stretch);
    mTree->header()->setSectionResizeMode(COUNT_COLUMN, QHeaderView::ResizeToContents);
    mTree->setMinimumHeight(MIN_LIST_HEIGHT);
    mTree->setUniformRowHeights(true);
    // A long name or ID loses its middle, so the start and the end that tells two materials apart both stay visible;
    // the full name and ID are in the row's tooltip.
    mTree->setTextElideMode(Qt::ElideMiddle);
    // Keyboard focus marks the whole row: a ring around the name cell alone read as an inline text field.
    mTree->setAllColumnsShowFocus(true);
    mTree->installEventFilter(this);
    mTree->setToolTip(QStringLiteral("Expand a material, then double-click a renderable or press Enter to select it."));
    layout->addWidget(mTree, 1);

    // One compact row of actions under the list, each with its icon; the caption line below holds hints.
    QHBoxLayout* actions = new QHBoxLayout;
    actions->setSpacing(design::SPACING_SMALL);
    mCopy->setObjectName(QStringLiteral("worldCopyMaterialId"));
    mCopy->setIcon(GetEditorIcon(EditorIcon::Copy));
    mCopy->setAccessibleName(QStringLiteral("Copy Resource ID"));
    mEdit->setObjectName(QStringLiteral("worldEditListedMaterial"));
    mEdit->setIcon(GetEditorIcon(EditorIcon::Material));
    for (QPushButton* button : {mCopy, mEdit})
    {
        button->setProperty("density", QStringLiteral("compact"));
        actions->addWidget(button);
    }
    actions->addStretch(1);
    layout->addLayout(actions);
    connect(mEdit, &QPushButton::clicked, this, [this]() {
        const QTreeWidgetItem* itemOrNull = mTree->currentItem();
        if (itemOrNull != nullptr && itemOrNull->parent() != nullptr)
        {
            mStatus->hide();
            Q_EMIT MaterialEditRequested(GetItemId(*itemOrNull));
        }
    });

    mStatus->setObjectName(QStringLiteral("worldMaterialsStatus"));
    mStatus->SetCaption(QStringLiteral("Import a scene to browse its material resources."));
    layout->addWidget(mStatus);

    connect(mTree, &QTreeWidget::currentItemChanged, this, &WorldMaterialsPanel::updateActions);
    connect(mTree, &QTreeWidget::itemActivated, this, [this](const QTreeWidgetItem* itemOrNull) {
        static_cast<void>(tryActivateRenderable(itemOrNull));
    });
    connect(mCopy, &QPushButton::clicked, this, [this]() {
        const QTreeWidgetItem* resourceOrNull = GetResourceItemOrNull(mTree->currentItem());
        if (resourceOrNull != nullptr)
        {
            QApplication::clipboard()->setText(GetItemId(*resourceOrNull));
        }
    });
    updateActions();
}

void WorldMaterialsPanel::SetRows(const QVector<WorldMaterialRow>& rows)
{
    if (rows == mRows)
    {
        return;
    }

    const QTreeWidgetItem* selectedOrNull = mTree->currentItem();
    const QTreeWidgetItem* selectedResourceOrNull = GetResourceItemOrNull(selectedOrNull);
    const QString selectedResource = selectedResourceOrNull == nullptr ? QString() : GetItemId(*selectedResourceOrNull);
    const QString selectedUser = selectedOrNull == selectedResourceOrNull ? QString() : GetItemId(*selectedOrNull);
    QSet<QString> expanded;
    for (foundation::int32 index = 0; index < mTree->topLevelItemCount(); ++index)
    {
        const QTreeWidgetItem* item = mTree->topLevelItem(index);
        if (item->isExpanded() == true)
        {
            expanded.insert(GetItemId(*item));
        }
    }

    const QSignalBlocker signals(mTree);
    mTree->clear();
    mRows = rows;
    QHash<QString, qsizetype> nameCounts;
    for (const WorldMaterialRow& row : mRows)
    {
        ++nameCounts[row.Name];
    }
    const QIcon materialIcon = GetEditorIcon(EditorIcon::Material);
    const QIcon renderableIcon = GetEditorIcon(EditorIcon::Mesh);
    for (const WorldMaterialRow& row : mRows)
    {
        QTreeWidgetItem* resource =
            new QTreeWidgetItem(mTree, {GetShownName(row, nameCounts), QString::number(row.Users.size())});
        resource->setIcon(NAME_COLUMN, materialIcon);
        resource->setData(NAME_COLUMN, Qt::UserRole, row.ResourceId);
        resource->setToolTip(NAME_COLUMN, row.Name + QLatin1Char('\n') + row.ResourceId);
        resource->setTextAlignment(COUNT_COLUMN, Qt::AlignRight | Qt::AlignVCenter);
        resource->setToolTip(COUNT_COLUMN, DescribeUsers(row.Users.size()));
        const bool isSelectedResource = row.ResourceId == selectedResource;
        QTreeWidgetItem* selection = resource;
        for (const WorldMaterialUser& user : row.Users)
        {
            QTreeWidgetItem* renderable = new QTreeWidgetItem(resource, {user.Name});
            renderable->setIcon(NAME_COLUMN, renderableIcon);
            renderable->setData(NAME_COLUMN, Qt::UserRole, user.EntityId);
            renderable->setToolTip(NAME_COLUMN, QStringLiteral("Select %1 in the World").arg(user.Name));
            if (user.EntityId == selectedUser)
            {
                selection = renderable;
            }
        }
        resource->setExpanded(expanded.contains(row.ResourceId));
        if (isSelectedResource == true)
        {
            mTree->setCurrentItem(selection);
        }
    }
    mTree->sortItems(NAME_COLUMN, Qt::AscendingOrder);
    mStatus->SetCaption(QStringLiteral("Import a scene to browse its material resources."));
    mStatus->setVisible(mRows.isEmpty());
    updateActions();
}

void WorldMaterialsPanel::RevealResource(const QString& id)
{
    for (foundation::int32 index = 0; index < mTree->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem* item = mTree->topLevelItem(index);
        if (GetItemId(*item) == id)
        {
            mStatus->hide();
            item->setExpanded(true);
            mTree->setCurrentItem(item);
            mTree->scrollToItem(item);
            mTree->setFocus();
            return;
        }
    }
    mStatus->SetCaption(
        QStringLiteral("This material is no longer used. Select a renderable to see its current material."));
    mStatus->show();
}

void WorldMaterialsPanel::ShowMissingRenderable()
{
    ShowStatus(QStringLiteral("This renderable is no longer in the World. Choose another renderable."));
}

void WorldMaterialsPanel::ShowStatus(const QString& message)
{
    mStatus->SetCaption(message);
    mStatus->show();
}

bool WorldMaterialsPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mTree && event->type() == QEvent::KeyPress)
    {
        const QKeyEvent& key = *static_cast<const QKeyEvent*>(event);
        // Handle Enter explicitly so navigation is consistent across Qt styles.
        if ((key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter) &&
            tryActivateRenderable(mTree->currentItem()) == true)
        {
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool WorldMaterialsPanel::tryActivateRenderable(const QTreeWidgetItem* itemOrNull)
{
    if (itemOrNull == nullptr || itemOrNull->parent() == nullptr || isEnabled() == false)
    {
        return false;
    }
    mStatus->hide();
    Q_EMIT RenderableActivated(GetItemId(*itemOrNull));
    return true;
}

void WorldMaterialsPanel::updateActions()
{
    const QTreeWidgetItem* itemOrNull = mTree->currentItem();
    mCopy->setEnabled(itemOrNull != nullptr);
    mEdit->setEnabled(itemOrNull != nullptr && itemOrNull->parent() != nullptr);
    // A disabled action says what it needs instead of a hint drawn elsewhere.
    mCopy->setToolTip(mCopy->isEnabled() == true ? QStringLiteral("Copy the selected material's resource ID.")
                                                 : QStringLiteral("Select a material to copy its resource ID."));
    mEdit->setToolTip(mEdit->isEnabled() == true
                          ? QStringLiteral("Open the material used by the selected renderable.")
                          : QStringLiteral("Expand a material and select a renderable to edit its material."));
}
} // namespace ludus::editor
