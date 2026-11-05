#include "internal/world_workspace.h"

#include "internal/level_workspace.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/world_terrain.h"

#include <ludus/foundation/base/core.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>

namespace ludus::editor
{
namespace
{
class TerrainDialog final : public QDialog
{
public:
    enum class PreviewState : foundation::uint8
    {
        Editing,
        Preparing,
        Ready,
    };

    TerrainDialog(const QJsonObject& definition, const QJsonObject& recipe, QWidget* parent)
        : QDialog(parent), mFields(new QWidget(this)), mStatus(new QLabel(this)),
          mButtons(new QDialogButtonBox(QDialogButtonBox::Cancel, this))
    {
        setObjectName(QStringLiteral("worldTerrainGenerator"));
        setWindowTitle(recipe.isEmpty() == true ? QStringLiteral("Generate Terrain")
                                                : QStringLiteral("Edit Generation"));
        setAttribute(Qt::WA_DeleteOnClose);
        QVBoxLayout* layout = new QVBoxLayout(this);
        QLabel* description = new QLabel(
            QStringLiteral("Shape a heightfield in local XZ with elevation along Y. Preview it in the World viewport."),
            this);
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addWidget(mFields);
        QFormLayout* form = new QFormLayout(mFields);
        for (const QJsonValue value : definition.value(QStringLiteral("properties")).toArray())
        {
            const QJsonObject property = value.toObject();
            const QString key = property.value(QStringLiteral("id")).toString();
            const QString label = property.value(QStringLiteral("label")).toString();
            const QString unit = property.value(QStringLiteral("unit")).toString();
            const QJsonValue initial =
                recipe.value(key).isUndefined() == true ? property.value(QStringLiteral("default")) : recipe.value(key);
            if (key == QStringLiteral("version"))
            {
                mVersion = initial;
                continue;
            }
            QWidget* fieldOrNull = nullptr;
            if (property.value(QStringLiteral("type")).toString() == QStringLiteral("enum"))
            {
                QComboBox* choices = new QComboBox(mFields);
                for (const QJsonValue choice : property.value(QStringLiteral("choices")).toArray())
                {
                    choices->addItem(choice.toString());
                }
                choices->setCurrentText(initial.toString());
                fieldOrNull = choices;
            }
            else
            {
                fieldOrNull = new QLineEdit(initial.toVariant().toString(), mFields);
            }
            fieldOrNull->setObjectName(QStringLiteral("worldTerrainField:") + key);
            fieldOrNull->setToolTip(property.value(QStringLiteral("help")).toString());
            form->addRow(unit.isEmpty() == true ? label : QStringLiteral("%1 (%2)").arg(label, unit), fieldOrNull);
            mInputs.append({property, initial, initial.toVariant().toString(), fieldOrNull});
        }
        mStatus->setObjectName(QStringLiteral("worldTerrainStatus"));
        mStatus->setWordWrap(true);
        mStatus->setTextFormat(Qt::PlainText);
        layout->addWidget(mStatus);
        PreviewButton = mButtons->addButton(QStringLiteral("Preview"), QDialogButtonBox::ActionRole);
        PreviewButton->setObjectName(QStringLiteral("worldTerrainPreview"));
        CommitButton = mButtons->addButton(recipe.isEmpty() == true ? QStringLiteral("Create Terrain")
                                                                    : QStringLiteral("Apply Generation"),
                                           QDialogButtonBox::AcceptRole);
        CommitButton->setObjectName(QStringLiteral("worldTerrainCommit"));
        PreviewButton->setDefault(true);
        connect(mButtons, &QDialogButtonBox::rejected, this, &TerrainDialog::reject);
        layout->addWidget(mButtons);
    }

    [[nodiscard]] bool ReadRecipe(QJsonObject& outRecipe)
    {
        QJsonObject recipe{{QStringLiteral("version"), mVersion}};
        for (const Input& input : mInputs)
        {
            const QComboBox* choiceOrNull = qobject_cast<const QComboBox*>(input.Field);
            const QString text =
                choiceOrNull != nullptr ? choiceOrNull->currentText() : static_cast<QLineEdit*>(input.Field)->text();
            QJsonValue value = input.Initial;
            QString error;
            if (text != input.InitialText && WorldDocument::ParseValue(input.Definition, text, value, error) == false)
            {
                if (input.Definition.contains(QStringLiteral("min")) == true)
                {
                    error += QStringLiteral(" Enter a value from %1 to %2.")
                                 .arg(input.Definition.value(QStringLiteral("min")).toVariant().toString(),
                                      input.Definition.value(QStringLiteral("max")).toVariant().toString());
                }
                SetStatus(error);
                input.Field->setFocus();
                return false;
            }
            recipe.insert(input.Definition.value(QStringLiteral("id")).toString(), value);
        }
        outRecipe = foundation::Move(recipe);
        return true;
    }

    void SetPreviewState(PreviewState state)
    {
        mPreviewState = state;
        const bool canEdit = state != PreviewState::Preparing;
        mFields->setEnabled(canEdit);
        PreviewButton->setEnabled(canEdit);
        CommitButton->setEnabled(canEdit);
    }
    void SetStatus(const QString& status)
    {
        mStatus->setText(status);
    }
    void ShowPreviewReady()
    {
        SetStatus(QStringLiteral("Candidate preview ready. Apply it to keep the terrain."));
    }
    void reject() override
    {
        if (IsPreparing() == true)
        {
            mShouldClose = true;
            SetStatus(QStringLiteral("Discarding the candidate when the preview operation finishes…"));
            return;
        }
        QDialog::reject();
    }

    // The draft owns the exact history entry and entity ID used by the preview.
    WorldDocument Candidate;
    QJsonObject Recipe;
    QString CandidateKey;
    QPushButton* PreviewButton;
    QPushButton* CommitButton;
    bool HasPreview = false;
    bool ShouldCommit = false;
    [[nodiscard]] LUDUS_INLINE bool IsPreparing() const noexcept
    {
        return mPreviewState == PreviewState::Preparing;
    }
    [[nodiscard]] LUDUS_INLINE bool IsReady() const noexcept
    {
        return mPreviewState == PreviewState::Ready;
    }
    [[nodiscard]] LUDUS_INLINE bool ShouldClose() const noexcept
    {
        return mShouldClose;
    }

private:
    struct Input final
    {
        QJsonObject Definition;
        QJsonValue Initial;
        QString InitialText;
        QWidget* Field;
    };
    QVector<Input> mInputs;
    QWidget* mFields;
    QLabel* mStatus;
    QDialogButtonBox* mButtons;
    QJsonValue mVersion;
    PreviewState mPreviewState = PreviewState::Editing;
    bool mShouldClose = false;
};
} // namespace

void WorldWorkspace::showTerrainGenerator(const QString& key)
{
    const QString component = GetTerrainComponentId();
    const QJsonObject definition = Document_.Component(component);
    if (Document_.Loaded() == false || Document_.HasBuiltInTerrain() == false ||
        (key.isEmpty() == true && Add_->isEnabled() == false) || Pending_ == true || isSceneWorkerActive() == true)
    {
        Changed(QStringLiteral("Open a default World and finish pending World operations before generating terrain."));
        return;
    }
    const QJsonObject savedRecipe =
        Document_.Entity(key).value(QStringLiteral("components")).toObject().value(component).toObject();
    if (key.isEmpty() == false && savedRecipe.isEmpty() == true)
    {
        Changed(QStringLiteral("Select a terrain entity before editing its generation settings."));
        return;
    }

    TerrainDialog* dialog = new TerrainDialog(definition, savedRecipe, this);
    const foundation::uint64 generation = DocumentGeneration_;
    const QJsonObject base = Document_.Draft();
    const bool hasScene = Document_.SceneLevel().isEmpty() == false;
    const QJsonObject navigation = GetRenderPreview().Navigation();
    const auto commit = [this, dialog, generation, base, hasScene]() {
        if (DocumentGeneration_ != generation || Document_.Draft() != base || Pending_ == true)
        {
            dialog->SetStatus(QStringLiteral("The World changed. Reopen Generate Terrain to use the current World."));
            return;
        }
        Document_ = foundation::Move(dialog->Candidate);
        Selected_ = dialog->CandidateKey;
        mSelectedEntities = {Selected_};
        if (hasScene == true)
        {
            ValidatedScene_ = Scene_->Document();
            ValidatedWorld_ = Document_.Encode();
        }
        dialog->accept();
        Render();
        Snapshot();
        Changed(QStringLiteral("Terrain applied. Save World to keep it. Undo is available."));
    };
    const auto finishPreview = [this, dialog, commit](bool isAccepted, const QString& error) {
        if (dialog->IsPreparing() == false)
        {
            return;
        }
        dialog->SetPreviewState(isAccepted == true ? TerrainDialog::PreviewState::Ready
                                                   : TerrainDialog::PreviewState::Editing);
        dialog->HasPreview = dialog->HasPreview == true || isAccepted == true;
        if (dialog->ShouldClose() == true)
        {
            dialog->reject();
        }
        else if (isAccepted == false)
        {
            dialog->SetStatus(error);
        }
        else
        {
            RenderPreview& preview = GetRenderPreview();
            preview.SetSelection(dialog->CandidateKey);
            static_cast<void>(preview.FrameSelection());
            if (dialog->ShouldCommit == true)
            {
                commit();
            }
            else
            {
                dialog->ShowPreviewReady();
            }
        }
    };
    if (hasScene == true)
    {
        connect(Scene_, &LevelWorkspace::OperationFinished, dialog, [this, finishPreview](bool isAccepted) {
            finishPreview(isAccepted, Scene_->StatusText());
        });
    }
    else
    {
        connect(Render_, &RenderPanel::Loaded, dialog, [this, finishPreview](bool isAccepted) {
            finishPreview(isAccepted, Render_->GetLoadError());
        });
    }
    connect(dialog,
            &QDialog::finished,
            this,
            [this, dialog, generation, hasScene, navigation](foundation::int32 result) {
                if (result != QDialog::Rejected || dialog->Candidate.Loaded() == false ||
                    DocumentGeneration_ != generation)
                {
                    return;
                }
                if (dialog->HasPreview == true)
                {
                    RenderPreview& preview = GetRenderPreview();
                    preview.SetSelection(Selected_);
                    preview.RestoreNavigation(navigation);
                }
                if (hasScene == false)
                {
                    Render_->WorldChanged();
                }
                else if (dialog->HasPreview == true)
                {
                    // The level is unchanged, so force an assembly of the real World.
                    ValidatedWorld_.clear();
                    RefreshScene();
                }
            });

    const auto prepare = [this, dialog, generation, base, key, hasScene, commit](bool shouldCommit) {
        if (DocumentGeneration_ != generation || Document_.Draft() != base || Pending_ == true ||
            isSceneWorkerActive() == true)
        {
            dialog->SetStatus(
                QStringLiteral("The World is busy or changed. Reopen Generate Terrain once it is ready."));
            return;
        }
        QJsonObject recipe;
        if (dialog->ReadRecipe(recipe) == false)
        {
            return;
        }
        if (dialog->IsReady() == true && recipe == dialog->Recipe)
        {
            if (shouldCommit == true)
            {
                commit();
            }
            else
            {
                dialog->ShowPreviewReady();
            }
            return;
        }

        WorldDocument candidate = Document_;
        QString candidateKey;
        QString error;
        if (EditWorldTerrain(candidate, mEntropy, key, recipe, candidateKey, error) == false)
        {
            dialog->SetStatus(error);
            return;
        }
        dialog->Recipe = foundation::Move(recipe);
        dialog->Candidate = foundation::Move(candidate);
        dialog->CandidateKey = foundation::Move(candidateKey);
        dialog->ShouldCommit = shouldCommit;
        dialog->SetPreviewState(TerrainDialog::PreviewState::Preparing);
        dialog->SetStatus(QStringLiteral("Preparing terrain preview…"));
        if (hasScene == false)
        {
            Render_->PreviewDraft(dialog->Candidate, true);
            return;
        }
        DraftPreview_ = true;
        SceneGeneration_ = generation;
        Scene_->SetWorldSnapshot(dialog->Candidate);
        if (Scene_->Assemble(dialog->Candidate.SceneLevel()) == false)
        {
            DraftPreview_ = false;
            dialog->SetPreviewState(TerrainDialog::PreviewState::Editing);
            const QString sceneError = Scene_->StatusText();
            dialog->SetStatus(sceneError.isEmpty() == false
                                  ? sceneError
                                  : QStringLiteral("Preview could not start. Try again once the World is ready."));
        }
    };
    connect(dialog->PreviewButton, &QPushButton::clicked, dialog, [prepare]() { prepare(false); });
    connect(dialog->CommitButton, &QPushButton::clicked, dialog, [prepare]() { prepare(true); });
    dialog->move(mHierarchy->mapToGlobal(QPoint{}));
    dialog->open();
}
} // namespace ludus::editor
