#include "internal/world_lights_panel.h"

#include "internal/render_scene.h"

#include <ludus/foundation/base/core.h>
#include <ludus/runtime/resources/scene.h>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::int32;
using foundation::uint64;
using foundation::usize;

constexpr int32 NAME_COLUMN = 0;
constexpr int32 KIND_COLUMN = 1;
/// Significant digits of radiance, intensity and power in row tooltips.
constexpr int32 DISPLAY_SIGNIFICANT_DIGITS = 4;
/// Smallest list height that still shows a header and a few rows.
constexpr int32 MIN_LIST_HEIGHT = 120;

bool IsImported(WorldLightOrigin origin) noexcept
{
    return origin == WorldLightOrigin::ImportedEmitter || origin == WorldLightOrigin::ImportedLight;
}
/// Whether `a` and `b` switch the same light; labels, details and states may differ.
bool IsSameLight(const WorldLightRow& a, const WorldLightRow& b) noexcept
{
    return a.Origin == b.Origin && a.Key == b.Key && a.EntityKey == b.EntityKey;
}
QString FormatRgb(const foundation::math::Vector3& rgb)
{
    return QStringLiteral("%1, %2, %3")
        .arg(static_cast<float64>(rgb.X), 0, 'g', DISPLAY_SIGNIFICANT_DIGITS)
        .arg(static_cast<float64>(rgb.Y), 0, 'g', DISPLAY_SIGNIFICANT_DIGITS)
        .arg(static_cast<float64>(rgb.Z), 0, 'g', DISPLAY_SIGNIFICANT_DIGITS);
}
QString GetLightKindLabel(resources::SceneLightKind kind)
{
    switch (kind)
    {
        case resources::SceneLightKind::Directional:
            return QStringLiteral("Directional light");
        case resources::SceneLightKind::Point:
            return QStringLiteral("Point light");
        case resources::SceneLightKind::Spot:
            return QStringLiteral("Spot light");
    }
    return QStringLiteral("Light");
}
QString GetLightKindLabel(const QString& kind)
{
    if (kind == QStringLiteral("directional"))
    {
        return GetLightKindLabel(resources::SceneLightKind::Directional);
    }
    if (kind == QStringLiteral("point"))
    {
        return GetLightKindLabel(resources::SceneLightKind::Point);
    }
    if (kind == QStringLiteral("spot"))
    {
        return GetLightKindLabel(resources::SceneLightKind::Spot);
    }
    return QStringLiteral("Light");
}
/// The component a World binds as its light, and the properties that describe the row.
struct WorldLightBinding final
{
    QString Component;
    QString EnabledProperty;
    QString KindProperty;
};
WorldLightBinding ReadLightBinding(const RenderScene& scene)
{
    const QJsonObject properties = scene.LightBinding.value(QStringLiteral("properties")).toObject();
    return {
        scene.LightBinding.value(QStringLiteral("component")).toString(),
        properties.value(QStringLiteral("enabled")).toString(),
        properties.value(QStringLiteral("kind")).toString(),
    };
}
} // namespace

QString GetEmitterLabel(const QString& objectName, qsizetype emitterNumber)
{
    static const QRegularExpression GENERATED_NAME(QStringLiteral("^Imported object \\d+$"));
    if (objectName.isEmpty() == true || GENERATED_NAME.match(objectName).hasMatch() == true)
    {
        return QStringLiteral("Area light %1").arg(emitterNumber);
    }
    return objectName;
}

QVector<WorldLightRow> CollectWorldLightRows(const WorldDocument& world, const RenderScene& scene)
{
    QVector<WorldLightRow> rows;
    const LightSwitchSet switches = world.GetLightSwitches();
    const WorldLightBinding binding = ReadLightBinding(scene);
    QVector<WorldLightRow> worldLights;
    QSet<uint64> authoredSources;
    if (binding.Component.isEmpty() == false && binding.EnabledProperty.isEmpty() == false)
    {
        for (const QJsonValue value : world.Draft().value(QStringLiteral("entities")).toArray())
        {
            const QJsonObject entity = value.toObject();
            const QJsonValue component = entity.value(QStringLiteral("components")).toObject().value(binding.Component);
            if (entity.contains(QStringLiteral("scene")) == true || component.isObject() == false)
            {
                continue;
            }
            WorldLightRow row;
            row.Origin = WorldLightOrigin::WorldLight;
            row.EntityKey = world.GetEntityKey(entity);
            const QString entityLabel = world.GetEntityLabel(entity);
            row.Label = entity.value(QStringLiteral("name")).toString(entityLabel);
            row.Detail = QStringLiteral("World light entity %1").arg(entityLabel);
            const QJsonObject properties = component.toObject();
            row.KindLabel = GetLightKindLabel(properties.value(binding.KindProperty).toString());
            row.IsEnabled = properties.value(binding.EnabledProperty).toBool();
            const uint64 sourceId = ComputeEntitySourceId(row.EntityKey);
            row.IsBakedAlwaysOn = scene.AlwaysOnLightIds.contains(sourceId);
            if (row.IsBakedAlwaysOn == true)
            {
                row.Detail += QStringLiteral(" · Baked always on. Clear the bake option in Properties to switch it.");
            }
            authoredSources.insert(sourceId);
            worldLights.append(row);
        }
    }
    const resources::SceneDefinition* definitionOrNull = scene.Definition.Get();
    if (definitionOrNull != nullptr)
    {
        const resources::SceneDefinition& definition = *definitionOrNull;
        QVector<qsizetype> objectCounts;
        for (usize i = 0; i < definition.Materials.GetSize(); ++i)
        {
            const resources::SceneMaterial& material = definition.Materials[i];
            const bool isEmissive = material.Emission.X > 0 || material.Emission.Y > 0 || material.Emission.Z > 0 ||
                                    material.HasEmissionPower == true;
            if (material.AreaSourceId == 0 || isEmissive == false)
            {
                continue;
            }
            const LightSourceKey key{LightSourceKind::Emitter, material.AreaSourceId};
            QStringList names;
            for (const PreviewObject& object : scene.Objects)
            {
                if (object.Surface == static_cast<int32>(i))
                {
                    names.append(object.Name);
                }
            }
            qsizetype index = -1;
            for (qsizetype j = 0; j < rows.size(); ++j)
            {
                if (rows[j].Key == key)
                {
                    index = j;
                }
            }
            if (index < 0)
            {
                WorldLightRow row;
                row.Origin = WorldLightOrigin::ImportedEmitter;
                row.Key = key;
                row.IsEnabled = switches.IsDisabled(key) == false;
                row.Label = GetEmitterLabel(names.value(0), objectCounts.size() + 1);
                row.KindLabel = QStringLiteral("Area light");
                row.Detail =
                    material.HasEmissionPower == true
                        ? QStringLiteral("Emitter %1 · %2 lm")
                              .arg(FormatLightSourceId(key.SourceId))
                              .arg(static_cast<float64>(material.EmissionPower), 0, 'g', DISPLAY_SIGNIFICANT_DIGITS)
                        : QStringLiteral("Emitter %1 · radiance %2 nits")
                              .arg(FormatLightSourceId(key.SourceId), FormatRgb(material.Emission));
                rows.append(row);
                objectCounts.append(0);
                index = rows.size() - 1;
            }
            objectCounts[index] += names.size();
        }
        for (qsizetype i = 0; i < rows.size(); ++i)
        {
            if (objectCounts[i] > 1)
            {
                rows[i].Label += QStringLiteral(" (+%1)").arg(objectCounts[i] - 1);
            }
            rows[i].Detail += QStringLiteral(" · %1 objects").arg(objectCounts[i]);
        }
        qsizetype lightNumber = 0;
        for (const resources::SceneLight& light : definition.Lights)
        {
            if (light.IsEnabled == false || authoredSources.contains(light.SourceId) == true)
            {
                continue;
            }
            WorldLightRow row;
            row.Origin = WorldLightOrigin::ImportedLight;
            row.Key = {LightSourceKind::Light, light.SourceId};
            row.IsEnabled = switches.IsDisabled(row.Key) == false;
            row.KindLabel = GetLightKindLabel(light.Kind);
            row.Label = QStringLiteral("Light %1").arg(++lightNumber);
            row.Detail = QStringLiteral("Light %1 · intensity %2")
                             .arg(FormatLightSourceId(light.SourceId), FormatRgb(light.IntensityRgb));
            rows.append(row);
        }
        for (const LightSourceKey& key : switches.Disabled)
        {
            bool isPresent = false;
            for (const WorldLightRow& row : rows)
            {
                isPresent = isPresent == true || (IsImported(row.Origin) == true && row.Key == key);
            }
            if (isPresent == true)
            {
                continue;
            }
            WorldLightRow row;
            row.Origin = WorldLightOrigin::Missing;
            row.Key = key;
            row.IsEnabled = false;
            row.Label = key.Kind == LightSourceKind::Emitter ? QStringLiteral("Missing emitter")
                                                             : QStringLiteral("Missing light");
            row.KindLabel =
                key.Kind == LightSourceKind::Emitter ? QStringLiteral("Area light") : QStringLiteral("Light");
            row.Detail = QStringLiteral("%1 %2 is switched off but not in the imported scene. Remove it, or "
                                        "re-import the scene that contained it.")
                             .arg(GetLightSourceKindName(key.Kind), FormatLightSourceId(key.SourceId));
            rows.append(row);
        }
    }
    rows.append(worldLights);
    return rows;
}
WorldLightChange
MakeWorldLightToggle(const QVector<WorldLightRow>& rows, const LightSwitchSet& current, qsizetype row, bool isEnabled)
{
    WorldLightChange change;
    change.Switches = current;
    if (row < 0 || row >= rows.size())
    {
        return change;
    }
    const WorldLightRow& target = rows[row];
    if (target.Origin == WorldLightOrigin::WorldLight)
    {
        if (target.IsBakedAlwaysOn == false)
        {
            change.WorldLights.append({target.EntityKey, isEnabled});
        }
        return change;
    }
    QVector<LightSourceKey> disabled;
    for (const LightSourceKey& key : current.Disabled)
    {
        if ((key == target.Key) == false)
        {
            disabled.append(key);
        }
    }
    if (isEnabled == false)
    {
        disabled.append(target.Key);
    }
    change.Switches.Disabled = disabled;
    NormalizeLightSwitches(change.Switches);
    return change;
}
WorldLightChange MakeWorldLightSolo(const QVector<WorldLightRow>& rows, const LightSwitchSet& current, qsizetype row)
{
    WorldLightChange change;
    for (const LightSourceKey& key : current.Disabled)
    {
        bool isPresent = false;
        for (const WorldLightRow& other : rows)
        {
            isPresent = isPresent == true || (IsImported(other.Origin) == true && other.Key == key);
        }
        if (isPresent == false)
        {
            change.Switches.Disabled.append(key); // A missing entry stays until removed.
        }
    }
    for (qsizetype i = 0; i < rows.size(); ++i)
    {
        const WorldLightRow& other = rows[i];
        const bool isSolo = i == row;
        if (IsImported(other.Origin) == true && isSolo == false)
        {
            change.Switches.Disabled.append(other.Key);
        }
        else if (other.Origin == WorldLightOrigin::WorldLight && other.IsBakedAlwaysOn == false &&
                 other.IsEnabled != isSolo)
        {
            change.WorldLights.append({other.EntityKey, isSolo});
        }
    }
    NormalizeLightSwitches(change.Switches);
    return change;
}
WorldLightChange MakeWorldLightsEnabled(const QVector<WorldLightRow>& rows)
{
    WorldLightChange change;
    for (const WorldLightRow& row : rows)
    {
        if (row.Origin == WorldLightOrigin::WorldLight && row.IsEnabled == false)
        {
            change.WorldLights.append({row.EntityKey, true});
        }
    }
    return change;
}
WorldLightChange MakeMissingLightSwitchesRemoved(const QVector<WorldLightRow>& rows, const LightSwitchSet& current)
{
    WorldLightChange change;
    for (const LightSourceKey& key : current.Disabled)
    {
        bool isMissing = false;
        for (const WorldLightRow& row : rows)
        {
            isMissing = isMissing == true || (row.Origin == WorldLightOrigin::Missing && row.Key == key);
        }
        if (isMissing == false)
        {
            change.Switches.Disabled.append(key);
        }
    }
    return change;
}

WorldLightsPanel::WorldLightsPanel(QWidget* parentOrNull) : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("worldLightsPanel"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QLabel* title = new QLabel(QStringLiteral("Lights"), this);
    title->setObjectName(QStringLiteral("worldLightsTitle"));
    layout->addWidget(title);
    mTreeOrNull = new QTreeWidget(this);
    mTreeOrNull->setObjectName(QStringLiteral("worldLights"));
    mTreeOrNull->setAccessibleName(QStringLiteral("World lights"));
    mTreeOrNull->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Kind")});
    mTreeOrNull->header()->setStretchLastSection(false);
    mTreeOrNull->header()->setSectionResizeMode(NAME_COLUMN, QHeaderView::Stretch);
    mTreeOrNull->header()->setSectionResizeMode(KIND_COLUMN, QHeaderView::ResizeToContents);
    mTreeOrNull->setItemsExpandable(false);
    mTreeOrNull->setRootIsDecorated(false);
    mTreeOrNull->setMinimumHeight(MIN_LIST_HEIGHT);
    mTreeOrNull->setToolTip(QStringLiteral("Switch any light on or off. An imported area light can cover several "
                                           "emissive shapes. Switching it off keeps its authored values."));
    layout->addWidget(mTreeOrNull, 1);
    QHBoxLayout* commands = new QHBoxLayout;
    mSoloOrNull = new QPushButton(QStringLiteral("Solo"), this);
    mSoloOrNull->setObjectName(QStringLiteral("worldLightsSolo"));
    mSoloOrNull->setToolTip(QStringLiteral("Keep only the selected light on. Undo is available."));
    mEnableAllOrNull = new QPushButton(QStringLiteral("Enable All"), this);
    mEnableAllOrNull->setObjectName(QStringLiteral("worldLightsEnableAll"));
    mEnableAllOrNull->setToolTip(QStringLiteral("Switch every light back on. Undo is available."));
    mRemoveMissingOrNull = new QPushButton(QStringLiteral("Remove Missing"), this);
    mRemoveMissingOrNull->setObjectName(QStringLiteral("worldLightsRemoveMissing"));
    mRemoveMissingOrNull->setToolTip(QStringLiteral("Forget switches for lights the imported scene no longer has."));
    for (QPushButton* button : {mSoloOrNull, mEnableAllOrNull, mRemoveMissingOrNull})
    {
        button->setProperty("density", QStringLiteral("compact"));
        // A layout never squeezes a command below the height its own text needs.
        button->setMinimumHeight(button->sizeHint().height());
        commands->addWidget(button);
    }
    layout->addLayout(commands);
    connect(mTreeOrNull, &QTreeWidget::itemChanged, this, &WorldLightsPanel::rowChanged);
    connect(mTreeOrNull, &QTreeWidget::itemSelectionChanged, this, &WorldLightsPanel::updateCommands);
    connect(mSoloOrNull, &QPushButton::clicked, this, [this]() {
        const qsizetype row = getSelectedRow();
        if (row < 0 || mRows[row].Origin == WorldLightOrigin::Missing)
        {
            return;
        }
        Q_EMIT LightsChangeRequested(
            MakeWorldLightSolo(mRows, mSwitches, row),
            QStringLiteral("Soloed %1. Always-on baked lights stay on. Undo is available.").arg(mRows[row].Label));
    });
    connect(mEnableAllOrNull, &QPushButton::clicked, this, [this]() {
        Q_EMIT LightsChangeRequested(MakeWorldLightsEnabled(mRows),
                                     QStringLiteral("Every light is on. Undo is available."));
    });
    connect(mRemoveMissingOrNull, &QPushButton::clicked, this, [this]() {
        Q_EMIT LightsChangeRequested(MakeMissingLightSwitchesRemoved(mRows, mSwitches),
                                     QStringLiteral("Switches for missing lights removed. Undo is available."));
    });
    updateCommands();
}
void WorldLightsPanel::SetRows(QVector<WorldLightRow> rows, LightSwitchSet switches)
{
    bool isSameList = rows.size() == mRows.size() && mTreeOrNull->topLevelItemCount() > 0;
    for (qsizetype i = 0; isSameList == true && i < rows.size(); ++i)
    {
        isSameList = IsSameLight(rows[i], mRows[i]);
    }
    // Remember the selected light and the light at the top of the view by identity before the
    // row indices change. The top row survives an empty list, such as during a re-import.
    const qsizetype selected = getSelectedRow();
    const bool hasSelection = selected >= 0;
    const WorldLightRow previous = hasSelection == true ? mRows[selected] : WorldLightRow{};
    if (mTreeOrNull->topLevelItemCount() > 0)
    {
        QTreeWidgetItem* topOrNull = mTreeOrNull->itemAt(0, 0);
        const qsizetype top = topOrNull != nullptr ? mTreeOrNull->indexOfTopLevelItem(topOrNull) : -1;
        mHasTopRow = top >= 0;
        mTopRow = mHasTopRow == true ? mRows[top] : WorldLightRow{};
    }
    mRows = foundation::Move(rows);
    mSwitches = foundation::Move(switches);
    mIsRendering = true;
    if (isSameList == true)
    {
        // A switch or a refresh of the same lights: update the items in place so the list keeps
        // its scroll position, selection and keyboard focus.
        updateItems();
    }
    else
    {
        rebuildItems(hasSelection == true ? &previous : nullptr);
    }
    mIsRendering = false;
    setVisible(mRows.isEmpty() == false);
    updateCommands();
}
void WorldLightsPanel::updateItems()
{
    for (int32 i = 0; i < mTreeOrNull->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = mTreeOrNull->topLevelItem(i);
        const WorldLightRow& row = mRows[i];
        item->setText(NAME_COLUMN, row.Label);
        item->setText(KIND_COLUMN, row.KindLabel);
        item->setToolTip(NAME_COLUMN, row.Detail);
        item->setToolTip(KIND_COLUMN, row.Detail);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (row.Origin != WorldLightOrigin::Missing)
        {
            if (row.IsBakedAlwaysOn == false)
            {
                flags |= Qt::ItemIsUserCheckable;
            }
            item->setCheckState(NAME_COLUMN, row.IsEnabled == true ? Qt::Checked : Qt::Unchecked);
        }
        item->setFlags(flags);
    }
}
void WorldLightsPanel::rebuildItems(const WorldLightRow* selectedOrNull)
{
    mTreeOrNull->clear();
    QTreeWidgetItem* topItemOrNull = nullptr;
    for (const WorldLightRow& row : mRows)
    {
        QTreeWidgetItem* item = new QTreeWidgetItem(mTreeOrNull);
        if (selectedOrNull != nullptr && IsSameLight(row, *selectedOrNull) == true)
        {
            item->setSelected(true);
            mTreeOrNull->setCurrentItem(item, NAME_COLUMN, QItemSelectionModel::NoUpdate);
        }
        if (mHasTopRow == true && IsSameLight(row, mTopRow) == true)
        {
            topItemOrNull = item;
        }
    }
    updateItems();
    if (topItemOrNull != nullptr)
    {
        mTreeOrNull->scrollToItem(topItemOrNull, QAbstractItemView::PositionAtTop);
    }
}
void WorldLightsPanel::rowChanged(QTreeWidgetItem* item)
{
    if (mIsRendering == true || item == nullptr)
    {
        return;
    }
    const qsizetype row = mTreeOrNull->indexOfTopLevelItem(item);
    if (row < 0 || row >= mRows.size() || mRows[row].Origin == WorldLightOrigin::Missing ||
        mRows[row].IsBakedAlwaysOn == true)
    {
        return;
    }
    const bool isEnabled = item->checkState(NAME_COLUMN) == Qt::Checked;
    if (isEnabled == mRows[row].IsEnabled)
    {
        return;
    }
    Q_EMIT LightsChangeRequested(
        MakeWorldLightToggle(mRows, mSwitches, row, isEnabled),
        QStringLiteral("%1 switched %2. Undo is available.")
            .arg(mRows[row].Label, isEnabled == true ? QStringLiteral("on") : QStringLiteral("off")));
}
void WorldLightsPanel::updateCommands()
{
    const qsizetype row = getSelectedRow();
    bool hasDisabled = mSwitches.Disabled.isEmpty() == false;
    bool hasMissing = false;
    for (const WorldLightRow& other : mRows)
    {
        hasDisabled = hasDisabled == true || other.IsEnabled == false;
        hasMissing = hasMissing == true || other.Origin == WorldLightOrigin::Missing;
    }
    mSoloOrNull->setEnabled(row >= 0 && mRows[row].Origin != WorldLightOrigin::Missing && mRows.size() > 1);
    mEnableAllOrNull->setEnabled(hasDisabled);
    mRemoveMissingOrNull->setVisible(hasMissing);
}
qsizetype WorldLightsPanel::getSelectedRow() const
{
    const QList<QTreeWidgetItem*> selected = mTreeOrNull->selectedItems();
    return selected.isEmpty() == true ? -1 : mTreeOrNull->indexOfTopLevelItem(selected.first());
}
} // namespace ludus::editor
