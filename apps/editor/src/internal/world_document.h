#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/transform.hpp>

#include <ludus/content/scene_format/limits.h>

#include "internal/document_history.h"
#include "internal/light_switches.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ludus::editor
{
inline constexpr QLatin1StringView WORLD_RASTER_SHADOWS{"rasterShadows"};
// Private, declarative authoring boundary. IDs persist as strings; runtime
// EntityId handles and component layout remain owned by each game's adapter.
// Whole candidates validate before publication. No project code is executed.
//
// Version-1 Worlds name entities by a scoped `id`. Version-2 records add a
// document AssetId and name each entity by its SourceObjectId `uuid`, keeping the
// version-1 ID as a readable `label`
// (docs/architecture/object-handles-uuid.md#stage-3-uuid-authoring-records).
// Each document keeps the version it was opened with: a version-1 World is never
// converted implicitly. Every entity-key parameter and output below is the
// version-2 `uuid` or the version-1 `id` (see GetEntityKey()).
class WorldDocument final
{
public:
    /// Most entities a World admits beside the objects of its imported scene.
    static constexpr foundation::int32 MAX_AUTHORED_ENTITIES = 128;
    /// Most entities one World admits; validation rejects a larger entity list. Every object of an
    /// import-profile scene (the cooked bundle's limit, e.g. Bistro's one object per material) fits
    /// beside the authored entities.
    static constexpr foundation::int32 MAX_ENTITIES =
        static_cast<foundation::int32>(content::scene_format::IMPORT_LIMITS.Objects) + MAX_AUTHORED_ENTITIES;
    /// Encoded bytes the World budget reserves for each admitted entity.
    static constexpr foundation::int64 MAX_ENTITY_BYTES = 4096;
    /// Largest encoded World document, compact JSON with its terminator.
    static constexpr foundation::int64 MAX_BYTES = MAX_ENTITIES * MAX_ENTITY_BYTES;
    /// Largest world schema accepted from a project or the built-in adapter.
    static constexpr foundation::int64 MAX_SCHEMA_BYTES = 65536;
    [[nodiscard]] bool LoadSchema(const QByteArray& bytes, QString& error);
    [[nodiscard]] bool Load(const QByteArray& bytes, QString& error);
    [[nodiscard]] bool New(QString& error);
    void Clear();
    [[nodiscard]] bool Loaded() const noexcept
    {
        return Loaded_;
    }
    [[nodiscard]] bool HasSchema() const noexcept
    {
        return Schema_.isEmpty() == false;
    }
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Loaded_ && Draft_ != Saved_;
    }
    [[nodiscard]] bool CanUndo() const noexcept
    {
        return History_.CanUndo();
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return History_.CanRedo();
    }
    [[nodiscard]] bool Undo() noexcept
    {
        return History_.Undo(Draft_);
    }
    [[nodiscard]] bool Redo() noexcept
    {
        return History_.Redo(Draft_);
    }
    [[nodiscard]] const QJsonObject& Draft() const noexcept
    {
        return Draft_;
    }
    [[nodiscard]] const QJsonObject& Schema() const noexcept
    {
        return Schema_;
    }
    /// Whether this document uses the exact bundled schema, including its optional extensions.
    [[nodiscard]] LUDUS_INLINE bool HasBuiltInSchema() const noexcept
    {
        return mIsBuiltInSchema;
    }
    /// Whether this document uses the bundled default schema with terrain authoring enabled.
    [[nodiscard]] LUDUS_INLINE bool HasBuiltInTerrain() const noexcept
    {
        return mIsBuiltInSchema;
    }
    /// Whether this document uses the bundled default schema with aperture authoring enabled
    /// (the `aperture` component, internal/world_aperture.h).
    [[nodiscard]] LUDUS_INLINE bool HasBuiltInAperture() const noexcept
    {
        return mIsBuiltInSchema;
    }
    /// True when the open World is a version-2 record with a document AssetId and entity UUIDs.
    [[nodiscard]] bool HasUuidRecords() const;
    /// Key naming `entity` in this API and in entity-typed properties: its version-2 `uuid` or version-1 `id`.
    [[nodiscard]] QString GetEntityKey(const QJsonObject& entity) const;
    /// Readable scoped label of `entity`: its version-2 `label` or version-1 `id`.
    [[nodiscard]] QString GetEntityLabel(const QJsonObject& entity) const;
    /// Key of the entity labelled `label`, or empty when no entity has that label.
    [[nodiscard]] QString FindEntityKey(const QString& label) const;
    [[nodiscard]] QJsonObject Component(const QString& id) const;
    [[nodiscard]] QJsonObject Entity(const QString& id) const;
    /// New entity keys are minted from `entropy` (see internal/editor_identity.h).
    /// A version-2 entity receives a fresh SourceObjectId, checked against every
    /// identity of the World, and a unique label derived from its name. When the
    /// source is unavailable, or every candidate collides, the draft, history and
    /// `outId` stay unchanged and `outError` explains the failure; there is no
    /// fallback source. Initial components replace required defaults and validate
    /// atomically with creation; unknown components/fields reject the entire edit.
    /// Duplication points the copy's self-references at the copy.
    [[nodiscard]] bool CreateEntity(const foundation::identity::EntropySource& entropy,
                                    QString& outError,
                                    const QString& name,
                                    QString& outId,
                                    const QJsonObject& initialComponents = {});
    [[nodiscard]] bool DuplicateEntity(const foundation::identity::EntropySource& entropy,
                                       QString& outError,
                                       const QString& source,
                                       QString& outId);
    /// Duplicate a group as one undo step, remapping references within the copied group.
    /// Failure preserves the document, history and outIds, including entropy/capacity failure.
    [[nodiscard]] bool DuplicateEntities(const foundation::identity::EntropySource& entropy,
                                         const QStringList& sources,
                                         QStringList& outIds,
                                         QString& outError);
    /// Remove a group as one undo step. Surviving references reject the entire edit.
    [[nodiscard]] bool RemoveEntities(const QStringList& ids, QString& outError);
    [[nodiscard]] bool RemoveEntity(const QString& id, QString& error);
    // Renaming changes only the display name; the label and UUID are preserved.
    [[nodiscard]] bool RenameEntity(const QString& id, QString& error, const QString& name);
    [[nodiscard]] bool Attach(const QString& entity, QString& error, const QString& component);
    [[nodiscard]] bool Detach(const QString& entity, QString& error, const QString& component);
    // Apply one inspector transaction, including the display name. Outputs and
    // history remain unchanged on failure. Unknown fields are rejected.
    [[nodiscard]] bool Edit(const QString& entity,
                            const QJsonObject& components,
                            const QString& name,
                            QString& error,
                            const QJsonObject* scene = nullptr);
    // Import is one history transaction. Cooked object IDs become entity IDs.
    // Version-2 records do not support the scene extension yet and are refused.
    /// @param generatedEntities Complete authored entity records the import adds beside the cooked
    /// objects in the same transaction, such as MakeImportedApertures' apertures. A record whose ID
    /// an entity already has is skipped, so a reimport keeps that entity and its edits.
    [[nodiscard]] bool
    ImportScene(const QJsonObject& level, QString& outError, const QJsonArray& generatedEntities = {});
    [[nodiscard]] QJsonObject SceneLevel() const;
    /// Map all current and future slots of a logical source material in one World history entry.
    [[nodiscard]] bool MapSceneMaterial(const QString& source, const QString& replacement, QString& outError);
    /// Resolve an imported slot's inherited material through the World repair mapping.
    [[nodiscard]] QString GetInheritedMaterialId(const QJsonObject& placement) const;
    /// Shared parameters of this resolved material, or an empty object for imported defaults.
    [[nodiscard]] QJsonObject GetSharedCoating(const QString& material) const;
    /// Set the World-owned instance shared by inherited slots of this material as one undo step.
    [[nodiscard]] bool SetSharedCoating(const QString& material, const QJsonObject& coating, QString& outError);
    /// Replace the scene level's start view (`scene.camera`: eye, target, up, fov) as one undo
    /// step. Fails without a scene level or when the World's validation rejects the camera;
    /// failure preserves the draft and history, and an unchanged camera adds no history.
    [[nodiscard]] bool SetSceneStartView(const QJsonObject& camera, QString& outError);
    /// Imported light sources this World switches off (the root `disabled_lights` list), in
    /// canonical order. Empty for Worlds without a scene level or without disabled sources.
    [[nodiscard]] LightSwitchSet GetLightSwitches() const;
    /// Enabled state to write to one authored light entity's bound `enabled` property.
    struct WorldLightEnable final
    {
        QString EntityKey;
        bool IsEnabled = true;
    };
    /// Replace the switched-off imported light sources and the enabled state of authored light
    /// entities as one undo step (one light switch, Solo or Enable all). An empty set removes
    /// `disabled_lights`, so a World without switches keeps its earlier bytes.
    /// @param switches Imported sources to switch off; nonempty only with a scene level, and at most
    /// GetMaxDisabledLightSources().
    /// @param lightBinding The World's light binding ({component, properties}, RenderScene::LightBinding);
    /// read only when worldLights is nonempty.
    /// @param worldLights Authored light entities whose bound `enabled` property is written.
    /// @param outError Why the edit was refused. Failure (no scene, unknown entity, an entity without the
    /// bound component, or World validation) preserves the draft and history; an unchanged World adds no
    /// history.
    [[nodiscard]] bool SetLightSwitches(const LightSwitchSet& switches,
                                        const QJsonObject& lightBinding,
                                        const QVector<WorldLightEnable>& worldLights,
                                        QString& outError);
    [[nodiscard]] QJsonObject ScenePlacement(const QJsonObject& entity) const;
    /// Mesh-to-parent transform of a scene placement (basis, then Euler-degree rotation Rz*Ry*Rx and scale,
    /// then position), in float32. SceneLevel stores this basis with the authored position, and the viewport
    /// gizmo composes it with the parent's cooked transform; for an object without a parent it equals the
    /// cooked object transform bit for bit. A parent composes in float64 when cooked, so a child may differ
    /// from the gizmo's float32 composition in its last bits.
    /// @param placement ScenePlacement() fields: basis (nine numbers), position, rotation and scale (three each).
    [[nodiscard]] static foundation::math::Affine3 MakeSceneMeshToParent(const QJsonObject& placement) noexcept;
    /// The draft with every scene instance's position, rotation and scale removed, including
    /// values held by the schema's scene_transform component binding. Two drafts that compare
    /// equal here differ at most in scene placement, which SceneLevel turns into object transforms.
    [[nodiscard]] QJsonObject MakeDraftWithoutScenePlacements() const;
    [[nodiscard]] bool Validate(const QJsonObject& candidate, QString& error) const;
    [[nodiscard]] QByteArray Encode() const;
    // Restored candidates must keep the open World's version and document identity.
    [[nodiscard]] bool Restore(const QJsonObject& candidate, QString& error)
    {
        return Commit(candidate, error);
    }
    void AcknowledgeSave();
    void Discard();
    [[nodiscard]] static bool
    ParseValue(const QJsonObject& property, const QString& text, QJsonValue& value, QString& error);

private:
    [[nodiscard]] bool Commit(const QJsonObject& candidate, QString& error);
    [[nodiscard]] bool ReplaceEntity(const QJsonObject& entity, QString& error);
    QJsonObject Schema_;
    QJsonObject Draft_;
    QJsonObject Saved_;
    DocumentHistory<QJsonObject, 32> History_;
    bool Loaded_ = false;
    bool mIsBuiltInSchema = false;
};
} // namespace ludus::editor
