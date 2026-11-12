#include "internal/world_materials_panel.h"

#include "design_tokens.h"
#include "internal/editor_icons.h"
#include "internal/inspector_fields.h"
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
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
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

/// A one-line caption under the actions: it keeps its line whether or not it shows a hint, so nothing below moves
/// or draws over it, and a hint too long for the panel ends in an ellipsis with the whole text in the tooltip.
class CaptionLabel final : public QLabel
{
public:
    explicit CaptionLabel(QWidget* parentOrNull) : QLabel(parentOrNull)
    {
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        QSizePolicy policy = sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        setSizePolicy(policy);
        setProperty("visualRole", QStringLiteral("secondary"));
    }

    [[nodiscard]] QSize minimumSizeHint() const override
    {
        return {0, fontMetrics().height()};
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

/// Shows text on the caption line with the whole text in its tooltip, since a long hint is elided.
void SetCaption(const QString& text, QLabel& inoutCaption)
{
    inoutCaption.setText(text);
    inoutCaption.setToolTip(text);
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
        if (name.empty() == false)
        {
            names.insert(id, QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        }
    }
    return names;
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
            rows.append({id, name.isEmpty() == true ? ShortenIdentifier(id) : name, {}});
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
    mTree->setTextElideMode(Qt::ElideRight);
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
    SetCaption(QStringLiteral("Import a scene to browse its material resources."), *mStatus);
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
    for (const WorldMaterialRow& row : mRows)
    {
        QTreeWidgetItem* resource = new QTreeWidgetItem(mTree, {row.Name, QString::number(row.Users.size())});
        resource->setData(NAME_COLUMN, Qt::UserRole, row.ResourceId);
        resource->setToolTip(NAME_COLUMN, row.Name + QLatin1Char('\n') + row.ResourceId);
        resource->setTextAlignment(COUNT_COLUMN, Qt::AlignRight | Qt::AlignVCenter);
        resource->setToolTip(COUNT_COLUMN, QStringLiteral("Used by %1 renderables").arg(row.Users.size()));
        const bool isSelectedResource = row.ResourceId == selectedResource;
        QTreeWidgetItem* selection = resource;
        for (const WorldMaterialUser& user : row.Users)
        {
            QTreeWidgetItem* renderable = new QTreeWidgetItem(resource, {user.Name});
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
    SetCaption(QStringLiteral("Import a scene to browse its material resources."), *mStatus);
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
    SetCaption(QStringLiteral("This material is no longer used. Select a renderable to see its current material."),
               *mStatus);
    mStatus->show();
}

void WorldMaterialsPanel::ShowMissingRenderable()
{
    ShowStatus(QStringLiteral("This renderable is no longer in the World. Choose another renderable."));
}

void WorldMaterialsPanel::ShowStatus(const QString& message)
{
    SetCaption(message, *mStatus);
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
