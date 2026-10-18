#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>

#include "internal/document_history.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
// Private, declarative authoring boundary. IDs persist as strings; runtime
// EntityId handles and component layout remain owned by each game's adapter.
// Whole candidates validate before publication. No project code is executed.
class WorldDocument final
{
public:
    static constexpr foundation::int64 MaxBytes = 524288;
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
    [[nodiscard]] QJsonObject Component(const QString& id) const;
    [[nodiscard]] QJsonObject Entity(const QString& id) const;
    // New entity IDs are minted from `entropy` (see internal/editor_identity.h).
    // When it is unavailable, the draft, history and `outId` stay unchanged and
    // `outError` explains the failure; there is no fallback source.
    [[nodiscard]] bool CreateEntity(const foundation::identity::EntropySource& entropy,
                                    QString& outError,
                                    const QString& name,
                                    QString& outId);
    [[nodiscard]] bool DuplicateEntity(const foundation::identity::EntropySource& entropy,
                                       QString& outError,
                                       const QString& source,
                                       QString& outId);
    [[nodiscard]] bool RemoveEntity(const QString& id, QString& error);
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
    [[nodiscard]] bool ImportScene(const QJsonObject& level, QString& error);
    [[nodiscard]] QJsonObject SceneLevel() const;
    [[nodiscard]] QJsonObject ScenePlacement(const QJsonObject& entity) const;
    [[nodiscard]] bool Validate(const QJsonObject& candidate, QString& error) const;
    [[nodiscard]] QByteArray Encode() const;
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
