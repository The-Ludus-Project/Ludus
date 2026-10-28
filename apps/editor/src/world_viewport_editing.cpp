#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/viewport_object_interaction.h"
#include "internal/world_workspace.h"

#include <QComboBox>
#include <QJsonArray>
#include <QKeySequence>
#include <QPushButton>
#include <QShortcut>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 AXIS_COUNT = 3;
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
        connect(remove, &QShortcut::activated, Remove_, &QPushButton::click);
        QShortcut* duplicate = new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), viewport);
        duplicate->setContext(Qt::WidgetWithChildrenShortcut);
        connect(duplicate, &QShortcut::activated, Duplicate_, &QPushButton::click);
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
        connect(&interaction,
                &ViewportObjectInteraction::PlacementCommitted,
                this,
                &WorldWorkspace::commitViewportPlacement);
    }
}
void WorldWorkspace::updateViewportInteraction()
{
    const bool canEdit =
        Document_.Loaded() == true && Pending_ == false && SceneBusy() == false && mSelectedEntities.size() == 1;
    Render_->GetViewport().GetObjectInteraction().SetWorld(Document_, Selected_, canEdit);
    Scene_->GetViewport().GetObjectInteraction().SetWorld(Document_, Selected_, canEdit);
}
void WorldWorkspace::commitViewportPlacement(const QString& id, const QJsonObject& placement)
{
    if (id != Selected_ || Pending_ == true || SceneBusy() == true || mSelectedEntities.size() != 1)
    {
        return;
    }
    const QJsonObject entity = Document_.Entity(id);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
    const QJsonObject sceneBinding = Document_.Schema().value(QStringLiteral("scene_transform")).toObject();
    const QStringList keys{QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")};
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
        const QStringList prefixes{QString(), QStringLiteral("r"), QStringLiteral("s")};
        for (int32 kind = 0; kind < AXIS_COUNT; ++kind)
        {
            const QJsonArray axes = placement.value(keys[kind]).toArray();
            const QJsonArray properties = binding.value(keys[kind]).toArray();
            for (int32 axis = 0; axis < AXIS_COUNT; ++axis)
            {
                const QString property = isScene == true ? properties[axis].toString()
                                                         : binding.value(prefixes[kind] + QChar('x' + axis)).toString();
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
} // namespace ludus::editor
