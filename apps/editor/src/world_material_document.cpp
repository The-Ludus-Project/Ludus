#include "internal/world_material_document.h"

#include "internal/inspector_fields.h"
#include "internal/material_inspection.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <ludus/runtime/resources/scene.h>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 FLOAT32_ROUND_TRIP_DIGITS = 9;

QLabel* MakeMaterialLabel(QWidget* parent)
{
    QLabel* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

QPushButton* MakeMaterialButton(const QString& text, const QString& name, QWidget* parent)
{
    QPushButton* button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setAutoDefault(false);
    return button;
}
} // namespace

void SetCoatingInput(QLineEdit& inoutInput, const QJsonValue& value)
{
    const QString text = QString::number(value.toDouble(), 'g', FLOAT32_ROUND_TRIP_DIGITS);
    inoutInput.setText(text);
    inoutInput.setProperty("initialCoatingText", text);
    inoutInput.setProperty("initialCoatingValue", value.toDouble());
}

bool ReadCoatingInput(const CoatingField& field,
                      const QLineEdit& input,
                      QJsonValue& outValue,
                      bool& outHasChanged,
                      QString& outError)
{
    const QJsonValue initial = input.property("initialCoatingValue").toDouble();
    outValue = initial;
    if (input.text() != input.property("initialCoatingText").toString() &&
        WorldDocument::ParseValue(field.MakeNumberProperty(), input.text(), outValue, outError) == false)
    {
        outError = QStringLiteral("%1: enter a finite value from %2 to %3.")
                       .arg(QString::fromLatin1(field.Label),
                            QString::number(static_cast<foundation::float64>(field.Minimum)),
                            QString::number(static_cast<foundation::float64>(field.Maximum)));
        return false;
    }
    outHasChanged =
        static_cast<foundation::float32>(outValue.toDouble()) != static_cast<foundation::float32>(initial.toDouble());
    return true;
}

QJsonObject
GetPresentedMaterialSlot(const QJsonObject& presentedLevel, const QString& entityId, const QString& resourceId)
{
    const QJsonArray objects = presentedLevel.value(QStringLiteral("objects")).toArray();
    for (const QJsonValue value : objects)
    {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("id")).toString() == entityId &&
            WorldDocument::GetMaterialResourceId(object, presentedLevel) == resourceId)
        {
            return object;
        }
    }
    return {};
}

void DescribeMaterialDocument(const WorldDocument& world,
                              const QJsonObject& presentedLevel,
                              const RenderScene& presentedScene,
                              const QString& entityId,
                              const QString& resourceId,
                              MaterialDocumentState& inoutState)
{
    const QJsonObject entity = world.Entity(entityId);
    const QJsonObject placement = entity.value(QStringLiteral("scene")).toObject();
    const QJsonObject shared = world.GetSharedCoating(resourceId);
    inoutState.SlotName = entity.value(QStringLiteral("name")).toString();
    inoutState.Coating = placement.value(QStringLiteral("coating")).toObject();
    inoutState.HasSlotOverride = inoutState.Coating.isEmpty() == false;
    if (inoutState.HasSlotOverride == false)
    {
        inoutState.Coating = shared;
    }
    const QJsonArray entities = world.Draft().value(QStringLiteral("entities")).toArray();
    for (const QJsonValue value : entities)
    {
        const QJsonObject user = value.toObject();
        const QJsonObject slot = user.value(QStringLiteral("scene")).toObject();
        if (world.GetMaterialResourceId(slot) == resourceId)
        {
            const QString scope = slot.contains(QStringLiteral("coating")) == true ? QStringLiteral("slot override")
                                                                                   : QStringLiteral("inherits");
            inoutState.Uses.append(QStringLiteral("%1 · %2").arg(user.value(QStringLiteral("name")).toString(), scope));
        }
    }
    if (inoutState.Coating.isEmpty() == false ||
        presentedLevel.value(QStringLiteral("shared_coatings")).toObject().contains(resourceId) == true)
    {
        return;
    }
    const resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(presentedScene, entityId);
    if (materialOrNull == nullptr ||
        materialOrNull->Scattering.Model != content::scene_format::MaterialModel::CoatedDiffuse)
    {
        return;
    }
    const QJsonObject presented = GetPresentedMaterialSlot(presentedLevel, entityId, resourceId);
    if (presented.isEmpty() == true || presented.contains(QStringLiteral("coating")) == true)
    {
        return;
    }
    for (const CoatingField& field : COATING_FIELDS)
    {
        inoutState.Coating.insert(QString::fromLatin1(field.Key),
                                  static_cast<foundation::float64>(materialOrNull->Scattering.*field.Member));
    }
}

WorldMaterialDocument::WorldMaterialDocument(QWidget* parent,
                                             const QString& entityId,
                                             foundation::uint64 worldGeneration)
    : QDialog(parent), mEntityId(entityId), mWorldGeneration(worldGeneration), mIdentity(MakeMaterialLabel(this)),
      mPreviewStatus(MakeMaterialLabel(this)), mStatus(MakeMaterialLabel(this)), mUses(new QListWidget(this)),
      mApply(MakeMaterialButton(QStringLiteral("Apply to Slot"), QStringLiteral("materialApply"), this)),
      mReset(MakeMaterialButton(QStringLiteral("Reset Fields"), QStringLiteral("materialReset"), this)),
      mInherit(MakeMaterialButton(QStringLiteral("Use Inherited Coating"), QStringLiteral("materialInherit"), this)),
      mUndo(MakeMaterialButton(QStringLiteral("Undo World Edit"), QStringLiteral("materialUndo"), this)),
      mRedo(MakeMaterialButton(QStringLiteral("Redo World Edit"), QStringLiteral("materialRedo"), this)),
      mSave(MakeMaterialButton(QStringLiteral("Save World"), QStringLiteral("materialSave"), this))
{
    setObjectName(QStringLiteral("worldMaterialDocument"));
    setWindowTitle(QStringLiteral("Material"));
    setAttribute(Qt::WA_DeleteOnClose);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(mIdentity);
    QFormLayout* fields = new QFormLayout;
    ConfigureInspectorForm(*fields);
    for (const CoatingField& field : COATING_FIELDS)
    {
        const QString key = QString::fromLatin1(field.Key);
        QLineEdit* input = new QLineEdit(this);
        input->setObjectName(QStringLiteral("materialCoating:") + key);
        fields->addRow(QString::fromLatin1(field.Label), input);
        mInputs.insert(key, input);
        connect(input, &QLineEdit::textEdited, this, [this]() {
            mHasInputEdits = true;
            mStatus->clear();
            updateActions();
        });
    }
    layout->addLayout(fields);
    QHBoxLayout* editActions = new QHBoxLayout;
    editActions->addWidget(mApply);
    editActions->addWidget(mReset);
    layout->addLayout(editActions);
    layout->addWidget(mInherit);

    QLabel* usesLabel = new QLabel(QStringLiteral("Slots using this material"), this);
    usesLabel->setBuddy(mUses);
    mUses->setObjectName(QStringLiteral("materialUses"));
    mUses->setAccessibleName(usesLabel->text());
    layout->addWidget(usesLabel);
    layout->addWidget(mUses);
    mPreviewStatus->setObjectName(QStringLiteral("materialPreviewStatus"));
    layout->addWidget(mPreviewStatus);
    mStatus->setObjectName(QStringLiteral("materialStatus"));
    layout->addWidget(mStatus);

    QHBoxLayout* worldActions = new QHBoxLayout;
    worldActions->addWidget(mUndo);
    worldActions->addWidget(mRedo);
    worldActions->addWidget(mSave);
    layout->addLayout(worldActions);

    connect(mApply, &QPushButton::clicked, this, &WorldMaterialDocument::applyCoating);
    connect(mReset, &QPushButton::clicked, this, &WorldMaterialDocument::resetInputs);
    connect(mInherit, &QPushButton::clicked, this, [this]() {
        QMessageBox* confirmation = new QMessageBox(
            QMessageBox::Question,
            QStringLiteral("Use inherited coating"),
            QStringLiteral("Remove the coating override from %1 and use its inherited values?").arg(mState.SlotName),
            QMessageBox::Cancel,
            this);
        confirmation->setObjectName(QStringLiteral("materialConfirmInheritance"));
        confirmation->setAttribute(Qt::WA_DeleteOnClose);
        confirmation->setTextFormat(Qt::PlainText);
        QPushButton* apply = confirmation->addButton(QStringLiteral("Remove Override"), QMessageBox::AcceptRole);
        apply->setObjectName(QStringLiteral("materialRemoveOverride"));
        confirmation->setDefaultButton(QMessageBox::Cancel);
        const QJsonObject before = mState.Coating;
        connect(confirmation, &QMessageBox::finished, this, [this, confirmation, apply, before]() {
            if (confirmation->clickedButton() == apply && mInherit->isEnabled() == true && mState.Coating == before)
            {
                Q_EMIT InheritanceRequested();
            }
        });
        confirmation->open();
    });
    connect(mUndo, &QPushButton::clicked, this, &WorldMaterialDocument::UndoRequested);
    connect(mRedo, &QPushButton::clicked, this, &WorldMaterialDocument::RedoRequested);
    connect(mSave, &QPushButton::clicked, this, &WorldMaterialDocument::SaveRequested);
}

bool WorldMaterialDocument::IsEditing(const QString& entityId,
                                      const QString& resourceId,
                                      foundation::uint64 generation) const
{
    return mWorldGeneration == generation && mEntityId == entityId && mState.ResourceId == resourceId;
}

void WorldMaterialDocument::SetState(const MaterialDocumentState& state)
{
    if (mState.Uses != state.Uses)
    {
        mUses->clear();
        mUses->addItems(state.Uses);
    }
    const bool hasWorldStatusChanged = mState.WorldStatus != state.WorldStatus;
    mState = state;
    mIdentity->setText(
        QStringLiteral("%1 · Coated diffuse\n%2 · %3\n%4")
            .arg(state.MaterialName,
                 state.SlotName,
                 state.HasSlotOverride == true ? QStringLiteral("Slot override") : QStringLiteral("Inherited coating"),
                 state.ResourceId));
    mPreviewStatus->setText(state.PreviewStatus);
    if (mHasInputEdits == false && state.Coating != mInputValues && state.Coating.isEmpty() == false)
    {
        resetInputs();
    }
    if (mHasInputEdits == false && hasWorldStatusChanged == true)
    {
        mStatus->setText(state.WorldStatus);
    }
    updateActions();
}

void WorldMaterialDocument::ShowError(const QString& message)
{
    mStatus->setText(message);
}

void WorldMaterialDocument::AcceptEdits()
{
    mHasInputEdits = false;
    mStatus->clear();
}

bool WorldMaterialDocument::HasUnappliedEdits() const noexcept
{
    return mHasInputEdits;
}

void WorldMaterialDocument::reject()
{
    if (mHasInputEdits == false)
    {
        QDialog::reject();
        return;
    }
    QMessageBox* existingOrNull = findChild<QMessageBox*>();
    if (existingOrNull != nullptr && existingOrNull->isVisible() == true)
    {
        existingOrNull->raise();
        return;
    }
    QMessageBox* confirmation =
        new QMessageBox(QMessageBox::Question,
                        QStringLiteral("Discard coating edits?"),
                        QStringLiteral("Close this Material document and discard its unapplied fields?"),
                        QMessageBox::Discard | QMessageBox::Cancel,
                        this);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setDefaultButton(QMessageBox::Cancel);
    connect(confirmation, &QMessageBox::finished, this, [this](foundation::int32 result) {
        if (result == QMessageBox::Discard)
        {
            QDialog::reject();
        }
    });
    confirmation->open();
}

void WorldMaterialDocument::applyCoating()
{
    if (mApply->isEnabled() == false)
    {
        return;
    }
    QJsonObject values;
    bool hasChanged = false;
    for (const CoatingField& field : COATING_FIELDS)
    {
        const QString key = QString::fromLatin1(field.Key);
        QLineEdit* input = mInputs.value(key);
        QJsonValue value;
        bool hasFieldChanged = false;
        QString error;
        if (ReadCoatingInput(field, *input, value, hasFieldChanged, error) == false)
        {
            ShowError(error);
            input->setFocus();
            return;
        }
        values.insert(key, value);
        hasChanged = hasChanged == true || hasFieldChanged == true;
    }
    if (hasChanged == false)
    {
        resetInputs();
        return;
    }
    Q_EMIT CoatingRequested(values);
}

void WorldMaterialDocument::resetInputs()
{
    mInputValues = mState.Coating;
    for (const CoatingField& field : COATING_FIELDS)
    {
        const QString key = QString::fromLatin1(field.Key);
        QLineEdit* input = mInputs.value(key);
        if (mInputValues.contains(key) == true)
        {
            SetCoatingInput(*input, mInputValues.value(key));
        }
        else
        {
            input->clear();
        }
    }
    AcceptEdits();
    updateActions();
}

void WorldMaterialDocument::updateActions()
{
    const bool hasConflict =
        mHasInputEdits == true && mState.Coating.isEmpty() == false && mInputValues != mState.Coating;
    const bool canEdit = mState.CanEdit == true && hasConflict == false;
    for (QLineEdit* input : mInputs)
    {
        input->setEnabled(canEdit);
    }
    mApply->setEnabled(canEdit == true && mHasInputEdits == true);
    mReset->setEnabled(mHasInputEdits);
    mInherit->setEnabled(canEdit == true && mHasInputEdits == false && mState.HasSlotOverride == true);
    mUndo->setEnabled(mHasInputEdits == false && mState.CanUndo == true);
    mRedo->setEnabled(mHasInputEdits == false && mState.CanRedo == true);
    mSave->setEnabled(mHasInputEdits == false && mState.CanSave == true);
    if (hasConflict == true)
    {
        ShowError(QStringLiteral("The coating changed in World. Reset Fields to load its current values."));
    }
}
} // namespace ludus::editor
