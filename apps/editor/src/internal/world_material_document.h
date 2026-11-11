#pragma once

#include <ludus/foundation/base/types.h>

#include <QDialog>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

namespace ludus::editor
{
struct CoatingField;
struct RenderScene;
class WorldDocument;

/// Set up a coating field with a round-trippable initial value for the inspector or Material document.
void SetCoatingInput(QLineEdit& inoutInput, const QJsonValue& value);

/// Parse one field with the coating schema. Keep its original precision if its text is unchanged.
[[nodiscard]] bool ReadCoatingInput(const CoatingField& field,
                                    const QLineEdit& input,
                                    QJsonValue& outValue,
                                    bool& outHasChanged,
                                    QString& outError);

struct MaterialDocumentState final
{
    QString SlotName;
    QString MaterialName;
    QString ResourceId;
    QStringList Uses;
    QJsonObject Coating;
    QString PreviewStatus;
    QString WorldStatus;
    bool HasSlotOverride = false;
    bool CanEdit = false;
    bool CanUndo = false;
    bool CanRedo = false;
    bool CanSave = false;
};

/// Find an entity's slot in the presented level, returning an empty object when the resource differs.
[[nodiscard]] QJsonObject
GetPresentedMaterialSlot(const QJsonObject& presentedLevel, const QString& entityId, const QString& resourceId);

/// Read the applied slot and its users. Imported defaults are taken from a matching presented binding.
void DescribeMaterialDocument(const WorldDocument& world,
                              const QJsonObject& presentedLevel,
                              const RenderScene& presentedScene,
                              const QString& entityId,
                              const QString& resourceId,
                              MaterialDocumentState& inoutState);

/// The editable view of one World slot. World owns the applied values, history, save and preview.
class WorldMaterialDocument final : public QDialog
{
    Q_OBJECT
public:
    WorldMaterialDocument(QWidget* parent, const QString& entityId, foundation::uint64 worldGeneration);
    [[nodiscard]] bool
    IsEditing(const QString& entityId, const QString& resourceId, foundation::uint64 generation) const;
    void SetState(const MaterialDocumentState& state);
    void ShowError(const QString& message);
    void AcceptEdits();
    [[nodiscard]] bool HasUnappliedEdits() const noexcept;

Q_SIGNALS:
    void CoatingRequested(const QJsonObject& values);
    void InheritanceRequested();
    void UndoRequested();
    void RedoRequested();
    void SaveRequested();

protected:
    void reject() override;

private:
    void applyCoating();
    void resetInputs();
    void updateActions();

    QString mEntityId;
    foundation::uint64 mWorldGeneration;
    MaterialDocumentState mState;
    QJsonObject mInputValues;
    QMap<QString, QLineEdit*> mInputs;
    QLabel* mIdentity;
    QLabel* mPreviewStatus;
    QLabel* mStatus;
    QListWidget* mUses;
    QPushButton* mApply;
    QPushButton* mReset;
    QPushButton* mInherit;
    QPushButton* mUndo;
    QPushButton* mRedo;
    QPushButton* mSave;
    bool mHasInputEdits = false;
};
} // namespace ludus::editor
