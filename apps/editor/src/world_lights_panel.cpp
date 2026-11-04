#include "internal/world_lights_panel.h"

#include "internal/render_scene.h"

#include <ludus/foundation/base/core.h>
#include <ludus/runtime/resources/scene.h>

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::int32;
using foundation::uint64;
using foundation::usize;

/// Item data role holding a child item's row index into WorldLightsPanel::GetRows().
constexpr int32 ROW_ROLE = Qt::UserRole;
/// Significant digits of radiance, intensity and power in row tooltips.
constexpr int32 DISPLAY_SIGNIFICANT_DIGITS = 4;
/// Smallest list height that still shows a section and a few rows.
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
QString GetSectionTitle(WorldLightOrigin origin)
{
    switch (origin)
    {
        case WorldLightOrigin::ImportedEmitter:
            return QStringLiteral("Imported emitters");
        case WorldLightOrigin::ImportedLight:
            return QStringLiteral("Imported lights");
        case WorldLightOrigin::WorldLight:
            return QStringLiteral("World lights");
        case WorldLightOrigin::Missing:
            return QStringLiteral("Missing from the scene");
    }
    return {};
}
/// The component a World binds as its light, and that component's `enabled` property.
struct WorldLightBinding final
{
    QString Component;
    QString EnabledProperty;
};
WorldLightBinding ReadLightBinding(const RenderScene& scene)
{
    return {
        scene.LightBinding.value(QStringLiteral("component")).toString(),
        scene.LightBinding.value(QStringLiteral("properties")).toObject().value(QStringLiteral("enabled")).toString()};
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
            row.Label = entity.value(QStringLiteral("name")).toString();
            row.Detail = QStringLiteral("World light entity %1").arg(world.GetEntityLabel(entity));
            row.IsEnabled = component.toObject().value(binding.EnabledProperty).toBool();
            authoredSources.insert(ComputeEntitySourceId(row.EntityKey));
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
            row.Label = GetLightKindLabel(light.Kind);
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
        change.WorldLights.append({target.EntityKey, isEnabled});
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
        else if (other.Origin == WorldLightOrigin::WorldLight && other.IsEnabled != isSolo)
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
    mTreeOrNull->setHeaderHidden(true);
    mTreeOrNull->setRootIsDecorated(false);
    mTreeOrNull->setMinimumHeight(MIN_LIST_HEIGHT);
    mTreeOrNull->setToolTip(QStringLiteral("Switch lights on or off. Imported emitters are grouped as the importer "
                                           "merged them; a switched-off light keeps its authored values."));
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
        Q_EMIT LightsChangeRequested(MakeWorldLightSolo(mRows, mSwitches, row),
                                     QStringLiteral("Only %1 is on. Undo is available.").arg(mRows[row].Label));
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
        const QTreeWidgetItem* topOrNull = mTreeOrNull->itemAt(0, 0);
        mHasTopRow = topOrNull != nullptr && topOrNull->data(0, ROW_ROLE).isValid() == true;
        mTopRow = mHasTopRow == true ? mRows[static_cast<qsizetype>(topOrNull->data(0, ROW_ROLE).toLongLong())]
                                     : WorldLightRow{};
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
    for (QTreeWidgetItemIterator it(mTreeOrNull); *it != nullptr; ++it)
    {
        QTreeWidgetItem* item = *it;
        if (item->data(0, ROW_ROLE).isValid() == false)
        {
            continue;
        }
        const WorldLightRow& row = mRows[static_cast<qsizetype>(item->data(0, ROW_ROLE).toLongLong())];
        item->setText(0, row.Label);
        item->setToolTip(0, row.Detail);
        if (row.Origin != WorldLightOrigin::Missing)
        {
            item->setCheckState(0, row.IsEnabled == true ? Qt::Checked : Qt::Unchecked);
        }
    }
}
void WorldLightsPanel::rebuildItems(const WorldLightRow* selectedOrNull)
{
    mTreeOrNull->clear();
    QTreeWidgetItem* topItemOrNull = nullptr;
    for (const WorldLightOrigin origin : {WorldLightOrigin::ImportedEmitter,
                                          WorldLightOrigin::ImportedLight,
                                          WorldLightOrigin::Missing,
                                          WorldLightOrigin::WorldLight})
    {
        QTreeWidgetItem* sectionOrNull = nullptr;
        for (qsizetype i = 0; i < mRows.size(); ++i)
        {
            const WorldLightRow& row = mRows[i];
            if (row.Origin != origin)
            {
                continue;
            }
            if (sectionOrNull == nullptr)
            {
                sectionOrNull = new QTreeWidgetItem(mTreeOrNull, {GetSectionTitle(origin)});
                sectionOrNull->setFlags(Qt::ItemIsEnabled);
            }
            QTreeWidgetItem* item = new QTreeWidgetItem(sectionOrNull, {row.Label});
            item->setData(0, ROW_ROLE, QVariant::fromValue(static_cast<qint64>(i)));
            item->setToolTip(0, row.Detail);
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (row.Origin != WorldLightOrigin::Missing)
            {
                flags |= Qt::ItemIsUserCheckable;
                item->setCheckState(0, row.IsEnabled == true ? Qt::Checked : Qt::Unchecked);
            }
            item->setFlags(flags);
            if (selectedOrNull != nullptr && IsSameLight(row, *selectedOrNull) == true)
            {
                item->setSelected(true);
                mTreeOrNull->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            }
            if (mHasTopRow == true && IsSameLight(row, mTopRow) == true)
            {
                topItemOrNull = item;
            }
        }
    }
    mTreeOrNull->expandAll();
    if (topItemOrNull != nullptr)
    {
        mTreeOrNull->scrollToItem(topItemOrNull, QAbstractItemView::PositionAtTop);
    }
}
void WorldLightsPanel::rowChanged(QTreeWidgetItem* item)
{
    if (mIsRendering == true || item == nullptr || item->data(0, ROW_ROLE).isValid() == false)
    {
        return;
    }
    const qsizetype row = static_cast<qsizetype>(item->data(0, ROW_ROLE).toLongLong());
    if (row < 0 || row >= mRows.size() || mRows[row].Origin == WorldLightOrigin::Missing)
    {
        return;
    }
    const bool isEnabled = item->checkState(0) == Qt::Checked;
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
    if (selected.isEmpty() == true || selected.first()->data(0, ROW_ROLE).isValid() == false)
    {
        return -1;
    }
    const qsizetype row = static_cast<qsizetype>(selected.first()->data(0, ROW_ROLE).toLongLong());
    return row >= 0 && row < mRows.size() ? row : -1;
}
} // namespace ludus::editor
