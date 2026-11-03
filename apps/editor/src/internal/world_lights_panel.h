#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/light_switches.h"
#include "internal/world_document.h"

#include <QString>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace ludus::editor
{
struct RenderScene;

// The World workspace's light list: imported emitter groups, imported analytic lights and
// authored light entities, each with an on/off switch, plus Solo and Enable all
// (docs/development/editor-workspace.md#switching-lights). The rows and every edit are
// computed by the free functions below; the widget only shows them and reports requests.

/// Where a light row comes from.
enum class WorldLightOrigin : foundation::uint8
{
    /// An emitter group of the imported scene (switched through `disabled_lights`).
    ImportedEmitter,
    /// An analytic light of the imported scene (switched through `disabled_lights`).
    ImportedLight,
    /// An authored light entity (switched through its bound `enabled` property).
    WorldLight,
    /// A `disabled_lights` entry the presented scene does not contain.
    Missing,
};

/// One switchable light.
struct WorldLightRow final
{
    WorldLightOrigin Origin = WorldLightOrigin::ImportedEmitter;
    /// Imported and missing rows: the switched source.
    LightSourceKey Key;
    /// World lights: the entity key.
    QString EntityKey;
    QString Label;
    /// Radiance or intensity, object count and source ID, for the tooltip.
    QString Detail;
    bool IsEnabled = true;
};

/// The light rows of `world` presented as `scene`, imported rows first in definition order.
/// Imported rows need the scene's cooked definition; without it only World light rows appear.
[[nodiscard]] QVector<WorldLightRow> CollectWorldLightRows(const WorldDocument& world, const RenderScene& scene);

/// A requested light edit: the complete new switch set and the authored lights to rewrite.
struct WorldLightChange final
{
    LightSwitchSet Switches;
    QVector<WorldDocument::WorldLightEnable> WorldLights;
};
/// Turn rows[row] on or off, keeping every other light.
[[nodiscard]] WorldLightChange
MakeWorldLightToggle(const QVector<WorldLightRow>& rows, const LightSwitchSet& current, qsizetype row, bool isEnabled);
/// Keep only rows[row] on: every other imported source is switched off and every other World light disabled.
/// Missing entries are kept.
[[nodiscard]] WorldLightChange
MakeWorldLightSolo(const QVector<WorldLightRow>& rows, const LightSwitchSet& current, qsizetype row);
/// Turn every light on and forget every switch, missing entries included.
[[nodiscard]] WorldLightChange MakeWorldLightsEnabled(const QVector<WorldLightRow>& rows);
/// Forget the switches the presented scene does not contain.
[[nodiscard]] WorldLightChange MakeMissingLightSwitchesRemoved(const QVector<WorldLightRow>& rows,
                                                               const LightSwitchSet& current);

/// Checkable light list with Solo, Enable All and Remove Missing. GUI thread.
class WorldLightsPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit WorldLightsPanel(QWidget* parentOrNull = nullptr);
    /// Show `rows` for a World whose current switch set is `switches`; the selected row is kept by identity.
    void SetRows(QVector<WorldLightRow> rows, LightSwitchSet switches);
    [[nodiscard]] LUDUS_INLINE const QVector<WorldLightRow>& GetRows() const noexcept
    {
        return mRows;
    }

Q_SIGNALS:
    /// The user asked for `change`; `message` describes it for the status line.
    void LightsChangeRequested(const ludus::editor::WorldLightChange& change, const QString& message);

private:
    void rowChanged(QTreeWidgetItem* item);
    void updateCommands();
    [[nodiscard]] qsizetype getSelectedRow() const;
    QVector<WorldLightRow> mRows;
    LightSwitchSet mSwitches;
    QTreeWidget* mTreeOrNull = nullptr;
    QPushButton* mSoloOrNull = nullptr;
    QPushButton* mEnableAllOrNull = nullptr;
    QPushButton* mRemoveMissingOrNull = nullptr;
    bool mIsRendering = false;
};
} // namespace ludus::editor
