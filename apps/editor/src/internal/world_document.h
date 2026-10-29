#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>

#include "internal/document_history.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
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
    static constexpr foundation::int64 MaxBytes = 524288;
    /// Largest world schema accepted from a project or the built-in adapter.
    static constexpr foundation::int64 MAX_SCHEMA_BYTES = 65536;
    /// Most entities one World admits; validation rejects a larger entity list.
    static constexpr foundation::int32 MAX_ENTITIES = 128;
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
    [[nodiscard]] bool ImportScene(const QJsonObject& level, QString& error);
    [[nodiscard]] QJsonObject SceneLevel() const;
    [[nodiscard]] QJsonObject ScenePlacement(const QJsonObject& entity) const;
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
};
} // namespace ludus::editor
