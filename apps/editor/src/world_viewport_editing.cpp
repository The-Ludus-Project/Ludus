#include "internal/builtin_world.h"
#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/viewport_object_interaction.h"
#include "internal/world_workspace.h"

#include <ludus/gameplay/camera/camera.hpp>

#include <QAction>
#include <QComboBox>
#include <QJsonArray>
#include <QKeySequence>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QShortcut>
#include <QString>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 AXIS_COUNT = ViewportObjectInteraction::AXIS_COUNT;
}
void WorldWorkspace::bindViewportInteraction()
{
    for (PreviewViewport* viewport : {&Render_->GetViewport(), &Scene_->GetViewport()})
    {
        QComboBox* toolOrNull = viewport->findChild<QComboBox*>(QStringLiteral("viewportObjectTool"));
        if (toolOrNull != nullptr)
        {
            toolOrNull->show();
        }
        QShortcut* remove = new QShortcut(QKeySequence::Delete, viewport);
        remove->setContext(Qt::WidgetWithChildrenShortcut);
        connect(remove, &QShortcut::activated, mDeleteEntities, &QAction::trigger);
        QShortcut* duplicate = new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), viewport);
        duplicate->setContext(Qt::WidgetWithChildrenShortcut);
        connect(duplicate, &QShortcut::activated, mDuplicateEntities, &QAction::trigger);
        viewport->EnableWorldContextMenu();
        connect(viewport, &PreviewViewport::WorldContextMenuRequested, this, &WorldWorkspace::showEntityMenu);
        ViewportObjectInteraction& interaction = viewport->GetObjectInteraction();
        connect(&interaction, &ViewportObjectInteraction::SelectionRequested, this, [this](const QString& id) {
            if (Pending_ == true || SceneBusy() == true)
            {
                return;
            }
            if (id.isEmpty() == false)
            {
                static_cast<void>(SelectEntity(id));
            }
            else
            {
                Selected_.clear();
                mSelectedEntities.clear();
                Render();
                Changed();
            }
        });
        connect(&interaction, &ViewportObjectInteraction::FrameRequested, this, &WorldWorkspace::frameEntity);
        connect(&interaction,
                &ViewportObjectInteraction::PlacementCommitted,
                this,
                &WorldWorkspace::commitViewportPlacement);
    }
    // Only the scene viewport shows a level, so only it can store a start view.
    connect(&Scene_->GetViewport().GetStartViewSaveAction(), &QAction::triggered, this, &WorldWorkspace::saveStartView);
}
void WorldWorkspace::frameEntity(const QString& id)
{
    RenderPreview& preview = GetRenderPreview();
    if (Pending_ == true || SceneBusy() == true || IsPlayingInViewport() == true ||
        preview.IsExploringFirstPerson() == true || preview.IsRayPathInspection() == true)
    {
        return;
    }
    if (preview.FrameEntity(id) == false)
    {
        Changed(QStringLiteral("Cannot frame this entity. Choose an entity with a visible mesh, or use Frame all."));
    }
}
void WorldWorkspace::showEntityMenu(const QPoint& positionGlobal, const QString& entityId)
{
    if (Document_.Loaded() == false)
    {
        return;
    }
    if (Pending_ == false && SceneBusy() == false && entityId.isEmpty() == false &&
        mSelectedEntities.contains(entityId) == false)
    {
        static_cast<void>(SelectEntity(entityId));
    }
    mEntityMenu->popup(positionGlobal);
}
void WorldWorkspace::updateViewportInteraction()
{
    if (isTerrainStrokeLive() == true)
    {
        // SetWorld cancels gestures, and a live stroke's scene preparation reports the World as busy.
        // The stroke's release, cancellation or failure refreshes the interaction.
        return;
    }
    const bool canEdit =
        Document_.Loaded() == true && Pending_ == false && SceneBusy() == false && mSelectedEntities.size() == 1;
    Render_->GetViewport().GetObjectInteraction().SetWorld(Document_, Selected_, canEdit);
    Scene_->GetViewport().GetObjectInteraction().SetWorld(Document_, Selected_, canEdit);
    QAction& saveStartView = Scene_->GetViewport().GetStartViewSaveAction();
    saveStartView.setVisible(Document_.Loaded() == true && Document_.Draft().contains(QStringLiteral("scene")) == true);
    saveStartView.setEnabled(Pending_ == false && SceneBusy() == false);
    applyTerrainInteraction();
    const bool hasSelectedPlayer =
        mSelectedEntities.size() == 1 && HasWorldFirstPersonPlayer(Document_, Selected_) == true;
    mExploreFirstPersonButton->setVisible(hasSelectedPlayer);
    mExploreFirstPersonButton->setEnabled(canEdit == true && hasSelectedPlayer == true);
}
void WorldWorkspace::exploreFirstPerson()
{
    if (isTerrainAuthoringOpen() == true)
    {
        Changed(QStringLiteral("Apply or cancel the Terrain draft before exploring the World."));
        return;
    }
    gameplay::camera::CameraSample spawn;
    float32 speed = 0;
    QString error;
    if (ReadWorldFirstPersonPlayer(Document_, Selected_, spawn, speed, error) == false)
    {
        Changed(error);
        return;
    }
    if (PreviewViewport::CanPresent() == false)
    {
        Changed(QStringLiteral("Enable the rendered World viewport to explore from the player."));
        return;
    }
    if (GetRenderPreview().Running() == false)
    {
        Changed(QStringLiteral("Resume the World viewport, then choose Explore as Player."));
        return;
    }
    const bool hasImportedScene = Document_.Draft().contains(QStringLiteral("scene"));
    PreviewViewport& viewport = hasImportedScene == true ? Scene_->GetViewport() : Render_->GetViewport();
    if (viewport.BeginFirstPerson(spawn, speed, error) == false)
    {
        Changed(error);
        return;
    }
    Changed(QStringLiteral("Hold right mouse to look, WASD to walk, Shift to run, and Space to jump. "
                           "Escape returns to the editor view."));
}
void WorldWorkspace::commitViewportPlacement(const QString& id, const QJsonObject& placement)
{
    if (tryPlaceTerrainDraft(id, placement) == true)
    {
        return;
    }
    if (id != Selected_ || Pending_ == true || SceneBusy() == true || mSelectedEntities.size() != 1)
    {
        return;
    }
    const QJsonObject entity = Document_.Entity(id);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
    const QJsonObject sceneBinding = Document_.Schema().value(QStringLiteral("scene_transform")).toObject();
    const ViewportObjectInteraction::TransformPropertyNames keys{QStringLiteral("position"),
                                                                 QStringLiteral("rotation"),
                                                                 QStringLiteral("scale")};
    if (scene.isEmpty() == false && sceneBinding.isEmpty() == true)
    {
        for (const QString& key : keys)
        {
            scene.insert(key, placement.value(key));
        }
    }
    else
    {
        const bool isScene = scene.isEmpty() == false;
        const QJsonObject binding = isScene == true ? sceneBinding : GetRenderPreview().Scene().TransformBinding;
        const QString component = binding.value(QStringLiteral("component")).toString();
        if (component.isEmpty() == true || components.contains(component) == false)
        {
            return;
        }
        QJsonObject values = components.value(component).toObject();
        const QJsonObject properties = binding.value(QStringLiteral("properties")).toObject();
        const ViewportObjectInteraction::TransformPropertyNames prefixes{QString(),
                                                                         QStringLiteral("r"),
                                                                         QStringLiteral("s")};
        for (usize kind = 0; kind < keys.GetSize(); ++kind)
        {
            const QJsonArray axes = placement.value(keys[kind]).toArray();
            const QJsonArray sceneProperties = binding.value(keys[kind]).toArray();
            for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
            {
                const QString property = isScene == true
                                             ? sceneProperties[axis].toString()
                                             : properties.value(prefixes[kind] + QChar('x' + axis)).toString();
                if (property.isEmpty() == true)
                {
                    return;
                }
                values.insert(property, axes[axis]);
            }
        }
        components.insert(component, values);
    }
    QString error;
    if (Document_.Edit(id,
                       components,
                       entity.value(QStringLiteral("name")).toString(),
                       error,
                       scene.isEmpty() == true ? nullptr : &scene) == false)
    {
        Changed(error);
        return;
    }
    RenderInspector();
    Snapshot();
    Changed(QStringLiteral("Object transformed. Undo restores its placement; Save World keeps the edit."));
}
void WorldWorkspace::saveStartView()
{
    if (Pending_ == true || SceneBusy() == true)
    {
        Changed(
            QStringLiteral("Apply or reset the pending edits and wait for the scene before setting the start view."));
        return;
    }
    PreviewStartView view;
    if (Scene_->GetRenderPreview().TryDescribeStartView(view) == false)
    {
        Changed(QStringLiteral("Switch the viewport to Perspective to set the start view; orthographic views have no "
                               "field of view."));
        return;
    }
    QString error;
    if (Document_.SetSceneStartView(MakeStartViewJson(view), error) == false)
    {
        Changed(error);
        return;
    }
    Snapshot();
    Changed(QStringLiteral("Start view set. The World opens here; Undo restores the previous start view and Save "
                           "World keeps it."));
}
} // namespace ludus::editor
