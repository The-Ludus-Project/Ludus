#include "internal/world_workspace.h"

#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/viewport_object_interaction.h"
#include "internal/world_terrain.h"

#include <ludus/foundation/base/core.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 TERRAIN_PREVIEW_DELAY_MILLISECONDS = 100;
constexpr int32 FLAT_TAB = 0;
constexpr int32 GENERATE_TAB = 1;

QString FormatTerrainNumber(const QString& key, float64 value)
{
    if (key == QStringLiteral("seed") || key == QStringLiteral("samples"))
    {
        return QString::number(static_cast<int32>(value));
    }
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}
} // namespace

class WorldTerrainPanel final : public QWidget
{
public:
    enum class Request : uint8
    {
        Idle,
        LoadingWorld,
        DrawingWorld,
        AssemblingScene
    };

    explicit WorldTerrainPanel(QWidget* parent)
        : QWidget(parent), Title(new QLabel(this)), Mode(new QTabBar(this)), Preset(new QComboBox(this)),
          Randomize(new QPushButton(QStringLiteral("Randomize"), this)),
          Advanced(new QCheckBox(QStringLiteral("Advanced"), this)), Status(new QLabel(this)),
          Compare(new QCheckBox(QStringLiteral("Compare with committed"), this)),
          Frame(new QPushButton(QStringLiteral("Frame Preview"), this)),
          Refresh(new QPushButton(QStringLiteral("Refresh Draft"), this)),
          Cancel(new QPushButton(QStringLiteral("Cancel"), this)), Commit(new QPushButton(this)),
          mForm(new QFormLayout), mPresetRow(new QWidget(this))
    {
        setObjectName(QStringLiteral("worldTerrainPanel"));
        QVBoxLayout* layout = new QVBoxLayout(this);
        Title->setWordWrap(true);
        layout->addWidget(Title);
        Mode->setObjectName(QStringLiteral("worldTerrainMode"));
        Mode->setAccessibleName(QStringLiteral("Terrain source"));
        Mode->addTab(QStringLiteral("Flat"));
        Mode->addTab(QStringLiteral("Generate"));
        layout->addWidget(Mode);
        QScrollArea* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        QWidget* fields = new QWidget(scroll);
        QVBoxLayout* fieldLayout = new QVBoxLayout(fields);
        fieldLayout->addLayout(mForm);
        fieldLayout->addStretch();
        scroll->setWidget(fields);
        layout->addWidget(scroll, 1);
        QHBoxLayout* presetLayout = new QHBoxLayout(mPresetRow);
        presetLayout->setContentsMargins(0, 0, 0, 0);
        Preset->setObjectName(QStringLiteral("worldTerrainField:shape"));
        Preset->setAccessibleName(QStringLiteral("Terrain preset"));
        Randomize->setObjectName(QStringLiteral("worldTerrainRandomize"));
        presetLayout->addWidget(Preset, 1);
        presetLayout->addWidget(Randomize);
        mForm->addRow(QStringLiteral("Preset"), mPresetRow);
        addField(QStringLiteral("width"), QStringLiteral("Width (m)"));
        addField(QStringLiteral("depth"), QStringLiteral("Depth (m)"));
        addField(QStringLiteral("relief"), QStringLiteral("Relief (m)"));
        mForm->addRow(Advanced);
        addField(QStringLiteral("minimumElevation"), QStringLiteral("Base elevation (m)"));
        addField(QStringLiteral("seed"), QStringLiteral("Seed"));
        addField(QStringLiteral("featureSize"), QStringLiteral("Feature size (m)"));
        addField(QStringLiteral("roughness"), QStringLiteral("Roughness"));
        addField(QStringLiteral("samples"), QStringLiteral("Samples per side"));
        Status->setObjectName(QStringLiteral("worldTerrainStatus"));
        Status->setWordWrap(true);
        Status->setTextFormat(Qt::PlainText);
        layout->addWidget(Status);
        Compare->setObjectName(QStringLiteral("worldTerrainCompare"));
        Frame->setObjectName(QStringLiteral("worldTerrainFrame"));
        Refresh->setObjectName(QStringLiteral("worldTerrainRefresh"));
        Cancel->setObjectName(QStringLiteral("worldTerrainCancel"));
        Commit->setObjectName(QStringLiteral("worldTerrainCommit"));
        layout->addWidget(Compare);
        layout->addWidget(Frame);
        layout->addWidget(Refresh);
        QHBoxLayout* footer = new QHBoxLayout;
        footer->addWidget(Cancel);
        footer->addWidget(Commit);
        layout->addLayout(footer);
        Timer.setSingleShot(true);
        Timer.setInterval(TERRAIN_PREVIEW_DELAY_MILLISECONDS);
        hide();
    }

    void Begin(const QJsonObject& definition, const QJsonObject& recipe, const QString& name)
    {
        mDefinitions = {};
        mInitialRecipe = recipe;
        for (const QJsonValue entry : definition.value(QStringLiteral("properties")).toArray())
        {
            const QJsonObject property = entry.toObject();
            const QString key = property.value(QStringLiteral("id")).toString();
            mDefinitions.insert(key, property);
            if (mInitialRecipe.contains(key) == false)
            {
                mInitialRecipe.insert(key, property.value(QStringLiteral("default")));
            }
        }
        const QSignalBlocker modeBlock(Mode);
        const QSignalBlocker presetBlock(Preset);
        const QSignalBlocker compareBlock(Compare);
        const QSignalBlocker advancedBlock(Advanced);
        const QString shape = mInitialRecipe.value(QStringLiteral("shape")).toString();
        Mode->setCurrentIndex(shape == QStringLiteral("Flat") ? FLAT_TAB : GENERATE_TAB);
        Preset->clear();
        const QJsonArray choices =
            mDefinitions.value(QStringLiteral("shape")).toObject().value(QStringLiteral("choices")).toArray();
        for (const QJsonValue choice : choices)
        {
            if (choice.toString() != QStringLiteral("Flat"))
            {
                Preset->addItem(choice.toString());
            }
        }
        if (shape != QStringLiteral("Flat"))
        {
            Preset->setCurrentText(shape);
        }
        Advanced->setChecked(false);
        Compare->setChecked(false);
        for (QMap<QString, QLineEdit*>::const_iterator field = Fields.constBegin(); field != Fields.constEnd(); ++field)
        {
            const QSignalBlocker blocker(field.value());
            if (field.key() != QStringLiteral("relief"))
            {
                field.value()->setText(FormatTerrainNumber(field.key(), mInitialRecipe.value(field.key()).toDouble()));
                field.value()->setToolTip(
                    mDefinitions.value(field.key()).toObject().value(QStringLiteral("help")).toString());
            }
        }
        SetNumber(QStringLiteral("relief"),
                  mInitialRecipe.value(QStringLiteral("maximumElevation")).toDouble() -
                      mInitialRecipe.value(QStringLiteral("minimumElevation")).toDouble());
        Fields.value(QStringLiteral("relief"))
            ->setToolTip(QStringLiteral("The configured elevation range in metres above the base."));
        Title->setText(name.isEmpty() == true ? QStringLiteral("Add Terrain")
                                              : QStringLiteral("Terrain · %1 · Source").arg(name));
        Commit->setText(name.isEmpty() == true ? QStringLiteral("Create Terrain")
                                               : QStringLiteral("Apply Source Changes"));
        Compare->setVisible(name.isEmpty() == false);
        Compare->setEnabled(false);
        Frame->setEnabled(false);
        Refresh->hide();
        UpdateFields();
        show();
    }

    void UpdateFields()
    {
        const bool isFlat = Mode->currentIndex() == FLAT_TAB;
        const bool hasAdvanced = isFlat == false && Advanced->isChecked() == true;
        mForm->setRowVisible(mPresetRow, isFlat == false);
        mForm->setRowVisible(Fields.value(QStringLiteral("relief")), isFlat == false);
        mForm->setRowVisible(Advanced, isFlat == false);
        mForm->setRowVisible(Fields.value(QStringLiteral("minimumElevation")), isFlat == true || hasAdvanced == true);
        for (const QString& key : {QStringLiteral("seed"),
                                   QStringLiteral("featureSize"),
                                   QStringLiteral("roughness"),
                                   QStringLiteral("samples")})
        {
            mForm->setRowVisible(Fields.value(key), hasAdvanced);
        }
    }

    [[nodiscard]] bool ReadRecipe(QJsonObject& outRecipe)
    {
        QJsonObject recipe = mInitialRecipe;
        const bool isFlat = Mode->currentIndex() == FLAT_TAB;
        recipe.insert(QStringLiteral("shape"), isFlat == true ? QStringLiteral("Flat") : Preset->currentText());
        for (QMap<QString, QLineEdit*>::const_iterator field = Fields.constBegin(); field != Fields.constEnd(); ++field)
        {
            if (field.key() == QStringLiteral("relief") ||
                (isFlat == true && field.key() != QStringLiteral("width") && field.key() != QStringLiteral("depth") &&
                 field.key() != QStringLiteral("minimumElevation")))
            {
                continue;
            }
            if (readField(field.key(), mDefinitions.value(field.key()).toObject(), recipe) == false)
            {
                return false;
            }
        }
        const float64 base = recipe.value(QStringLiteral("minimumElevation")).toDouble();
        const QJsonObject upper = mDefinitions.value(QStringLiteral("maximumElevation")).toObject();
        if (isFlat == true)
        {
            if (recipe.value(QStringLiteral("maximumElevation")).toDouble() < base)
            {
                recipe.insert(QStringLiteral("maximumElevation"), base);
            }
        }
        else
        {
            const QJsonObject reliefDefinition{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("label"), QStringLiteral("Relief")},
                {QStringLiteral("min"), 0},
                {QStringLiteral("max"), upper.value(QStringLiteral("max")).toDouble() - base}};
            if (readField(QStringLiteral("relief"), reliefDefinition, recipe) == false)
            {
                return false;
            }
            const float64 relief = recipe.take(QStringLiteral("relief")).toDouble();
            const float64 initialBase = mInitialRecipe.value(QStringLiteral("minimumElevation")).toDouble();
            const float64 initialMaximum = mInitialRecipe.value(QStringLiteral("maximumElevation")).toDouble();
            // Preserve the exact saved upper bound when opening a legacy recipe.
            recipe.insert(QStringLiteral("maximumElevation"),
                          base == initialBase && relief == initialMaximum - initialBase ? initialMaximum
                                                                                        : base + relief);
        }
        outRecipe = foundation::Move(recipe);
        return true;
    }

    void RandomizeSeed()
    {
        const uint32 maximum = static_cast<uint32>(
            mDefinitions.value(QStringLiteral("seed")).toObject().value(QStringLiteral("max")).toDouble());
        const uint32 count = maximum + 1;
        const uint32 seed = QRandomGenerator::global()->bounded(count);
        const uint32 current = Fields.value(QStringLiteral("seed"))->text().toUInt();
        SetNumber(QStringLiteral("seed"), seed == current ? (seed + 1) % count : seed);
    }

    void SetNumber(const QString& key, float64 number)
    {
        QLineEdit* field = Fields.value(key);
        LUDUS_REQUIRE(field != nullptr);
        const QSignalBlocker blocker(field);
        field->setText(FormatTerrainNumber(key, number));
    }

    [[nodiscard]] float64 ClampExtent(const QString& key, float64 extent) const
    {
        const QJsonObject definition = mDefinitions.value(key).toObject();
        const float64 minimum = definition.value(QStringLiteral("min")).toDouble();
        const float64 maximum = definition.value(QStringLiteral("max")).toDouble();
        return extent < minimum ? minimum : extent > maximum ? maximum : extent;
    }

    QLabel* Title;
    QTabBar* Mode;
    QComboBox* Preset;
    QPushButton* Randomize;
    QCheckBox* Advanced;
    QLabel* Status;
    QCheckBox* Compare;
    QPushButton* Frame;
    QPushButton* Refresh;
    QPushButton* Cancel;
    QPushButton* Commit;
    QMap<QString, QLineEdit*> Fields;
    QTimer Timer;

    // The existing renderer and World document own the derived frame and the undo stack.
    QJsonObject Base;
    QJsonObject Transform;
    WorldDocument ReadyWorld;
    QJsonObject ReadyRecipe;
    WorldDocument Candidate;
    RenderScene PreviousScene;
    QByteArray DrawingIdentity;
    QString Target;
    QString ReadyKey;
    QString CandidateKey;
    uint64 WorldGeneration = 0;
    uint64 Sequence = 0;
    uint64 RequestedSequence = 0;
    uint64 ReadySequence = 0;
    Request Stage = Request::Idle;
    bool IsOpen = false;
    bool IsComparing = false;
    bool HasDraftView = false;
    bool ShouldFrame = false;
    bool IsCancellingScene = false;
    bool ShouldRestore = false;
    bool HasConflict = false;

private:
    void addField(const QString& key, const QString& label)
    {
        QLineEdit* field = new QLineEdit(this);
        field->setObjectName(QStringLiteral("worldTerrainField:") + key);
        mForm->addRow(label, field);
        Fields.insert(key, field);
    }

    [[nodiscard]] bool readField(const QString& key, const QJsonObject& definition, QJsonObject& inoutRecipe)
    {
        const QLineEdit* field = Fields.value(key);
        LUDUS_REQUIRE(field != nullptr);
        const float64 initial = key == QStringLiteral("relief")
                                    ? mInitialRecipe.value(QStringLiteral("maximumElevation")).toDouble() -
                                          mInitialRecipe.value(QStringLiteral("minimumElevation")).toDouble()
                                    : mInitialRecipe.value(key).toDouble();
        const QJsonValue minimum = definition.value(QStringLiteral("min"));
        const QJsonValue maximum = definition.value(QStringLiteral("max"));
        // World number entry parses float32. Untouched fields keep the stored JSON precision.
        if (field->text() == FormatTerrainNumber(key, initial) && initial >= minimum.toDouble() &&
            initial <= maximum.toDouble())
        {
            inoutRecipe.insert(key, initial);
            return true;
        }
        QJsonValue value;
        QString error;
        if (WorldDocument::ParseValue(definition, field->text(), value, error) == false)
        {
            if (field->isHidden() == true && Mode->currentIndex() == GENERATE_TAB)
            {
                Advanced->setChecked(true);
            }
            Status->setText(QStringLiteral("%1 Enter a value from %2 to %3.")
                                .arg(error, minimum.toVariant().toString(), maximum.toVariant().toString()));
            return false;
        }
        inoutRecipe.insert(key, value);
        return true;
    }

    QFormLayout* mForm;
    QWidget* mPresetRow;
    QJsonObject mDefinitions;
    QJsonObject mInitialRecipe;
};

void WorldWorkspace::initializeTerrainAuthoring()
{
    WorldTerrainPanel* panel = new WorldTerrainPanel(Split_);
    mTerrainPanelOrNull = panel;
    Split_->setCollapsible(Split_->indexOf(panel), false);
    connect(&panel->Timer, &QTimer::timeout, this, &WorldWorkspace::prepareTerrainPreview);
    connect(panel->Cancel, &QPushButton::clicked, this, [this]() { closeTerrainAuthoring(true); });
    connect(panel->Commit, &QPushButton::clicked, this, &WorldWorkspace::commitTerrainDraft);
    connect(panel->Frame, &QPushButton::clicked, this, &WorldWorkspace::frameTerrainDraft);
    connect(panel->Refresh, &QPushButton::clicked, this, &WorldWorkspace::refreshTerrainBase);
    const auto changeSource = [this, panel]() {
        const QSignalBlocker compareBlock(panel->Compare);
        panel->Compare->setChecked(false);
        panel->IsComparing = false;
        queueTerrainPreview();
    };
    connect(panel->Mode, &QTabBar::currentChanged, this, [panel, changeSource]() {
        panel->UpdateFields();
        changeSource();
    });
    connect(panel->Advanced, &QCheckBox::toggled, panel, &WorldTerrainPanel::UpdateFields);
    connect(panel->Preset, &QComboBox::currentIndexChanged, this, changeSource);
    connect(panel->Randomize, &QPushButton::clicked, this, [panel, changeSource]() {
        panel->RandomizeSeed();
        changeSource();
    });
    for (QLineEdit* field : panel->Fields)
    {
        connect(field, &QLineEdit::textChanged, this, changeSource);
    }
    connect(panel->Compare, &QCheckBox::toggled, this, [this, panel](bool isChecked) {
        panel->IsComparing = isChecked;
        queueTerrainPreview();
    });

    for (PreviewViewport* viewport : {&Render_->GetViewport(), &Scene_->GetViewport()})
    {
        viewport->SetTerrainActions();
        connect(&viewport->GetAddTerrainAction(), &QAction::triggered, this, [this]() { showTerrainAuthoring(); });
        connect(&viewport->GetEditTerrainAction(), &QAction::triggered, this, [this]() {
            showTerrainAuthoring(Selected_);
        });
        connect(&viewport->GetObjectInteraction(),
                &ViewportObjectInteraction::TerrainExtentCommitted,
                this,
                [panel, changeSource](const QString& key, float64 extent) {
                    panel->SetNumber(key, panel->ClampExtent(key, extent));
                    changeSource();
                });
    }
    mEntityMenu->addSeparator();
    mEntityMenu->addAction(&Render_->GetViewport().GetAddTerrainAction());
    mEntityMenu->addAction(&Render_->GetViewport().GetEditTerrainAction());
    connect(Render_, &RenderPanel::Loaded, this, &WorldWorkspace::terrainBoundLoaded);
    connect(&Render_->Preview(), &RenderPreview::FrameReady, this, [this, panel]() {
        if (panel->Stage == WorldTerrainPanel::Request::DrawingWorld &&
            panel->DrawingIdentity == Render_->Preview().Scene().Identity)
        {
            finishTerrainPreview(true);
        }
    });
    connect(&Render_->Preview(), &RenderPreview::Stopped, this, [this, panel]() {
        QTimer::singleShot(0, this, [this, panel]() {
            if (panel->Stage == WorldTerrainPanel::Request::DrawingWorld && Render_->Preview().Running() == false)
            {
                finishTerrainPreview(false, QStringLiteral("The viewport stopped. Resume it and refresh the draft."));
            }
        });
    });
    const auto failedFrame = [this, panel](const QString& error) {
        if (panel->Stage == WorldTerrainPanel::Request::DrawingWorld)
        {
            finishTerrainPreview(false, error);
        }
    };
    connect(&Render_->Preview(), &RenderPreview::Failed, this, failedFrame);
    connect(&Render_->Preview(), &RenderPreview::BuiltInViewFailed, this, failedFrame);
    connect(Scene_, &LevelWorkspace::OperationFinished, this, [this, panel](bool isAccepted) {
        if (panel->Stage == WorldTerrainPanel::Request::AssemblingScene)
        {
            finishTerrainPreview(isAccepted, Scene_->StatusText());
        }
    });
}

bool WorldWorkspace::isTerrainAuthoringOpen() const
{
    return mTerrainPanelOrNull != nullptr && mTerrainPanelOrNull->IsOpen == true;
}

bool WorldWorkspace::canEditTerrain(const QString& key) const
{
    return key.isEmpty() == false && Document_.HasBuiltInTerrain() == true &&
           Document_.Entity(key).value(QStringLiteral("components")).toObject().contains(GetTerrainComponentId());
}

void WorldWorkspace::updateTerrainActions()
{
    if (mTerrainPanelOrNull == nullptr)
    {
        return;
    }
    const bool canOpen = Document_.Loaded() == true && Document_.HasBuiltInTerrain() == true && Pending_ == false &&
                         isSceneWorkerActive() == false && IsPlayingInViewport() == false;
    const bool isTerrainSelected = mSelectedEntities.size() == 1 && canEditTerrain(Selected_) == true;
    for (PreviewViewport* viewport : {&Render_->GetViewport(), &Scene_->GetViewport()})
    {
        viewport->GetAddTerrainAction().setEnabled(canOpen == true && mAddEntity->isEnabled() == true);
        viewport->GetEditTerrainAction().setVisible(isTerrainSelected);
        viewport->GetEditTerrainAction().setEnabled(canOpen == true && isTerrainSelected == true);
    }
}

void WorldWorkspace::showTerrainAuthoring(const QString& key)
{
    if (Document_.Loaded() == false || Document_.HasBuiltInTerrain() == false || Pending_ == true ||
        IsPlayingInViewport() == true || isSceneWorkerActive() == true ||
        (key.isEmpty() == true ? mAddEntity->isEnabled() == false : canEditTerrain(key) == false))
    {
        Changed(QStringLiteral("Open an editable default World and finish pending edits before opening Terrain."));
        return;
    }
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.IsOpen == true)
    {
        panel.Status->setText(QStringLiteral("Apply or cancel this terrain draft before starting another."));
        panel.Mode->setFocus();
        return;
    }
    DraftTimer_.stop();
    const QJsonObject entity = Document_.Entity(key);
    panel.Target = key;
    panel.Base = Document_.Draft();
    panel.WorldGeneration = DocumentGeneration_;
    panel.Transform = {};
    panel.IsOpen = true;
    panel.IsComparing = false;
    panel.HasConflict = false;
    panel.ShouldFrame = key.isEmpty() == true && panel.Base.value(QStringLiteral("entities")).toArray().isEmpty();
    panel.Begin(Document_.Component(GetTerrainComponentId()),
                entity.value(QStringLiteral("components")).toObject().value(GetTerrainComponentId()).toObject(),
                entity.value(QStringLiteral("name")).toString());
    queueTerrainPreview();
}

void WorldWorkspace::queueTerrainPreview()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.IsOpen == false)
    {
        return;
    }
    ++panel.Sequence;
    panel.Commit->setEnabled(false);
    panel.Compare->setEnabled(false);
    checkTerrainBase();
    if (panel.HasConflict == true)
    {
        return;
    }
    cancelTerrainRequest();
    panel.Status->setText(QStringLiteral("Updating preview…"));
    panel.Timer.start();
}

void WorldWorkspace::prepareTerrainPreview()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.IsOpen == false || panel.HasConflict == true)
    {
        return;
    }
    checkTerrainBase();
    if (panel.HasConflict == true)
    {
        return;
    }
    if (Pending_ == true)
    {
        panel.Status->setText(QStringLiteral("Apply or reset the Properties changes, then refresh this draft."));
        panel.Refresh->show();
        return;
    }
    if (panel.Stage != WorldTerrainPanel::Request::Idle || isSceneWorkerActive() == true)
    {
        panel.Timer.start();
        return;
    }

    QJsonObject recipe;
    if (panel.ReadRecipe(recipe) == false)
    {
        return;
    }
    panel.Candidate = Document_;
    panel.CandidateKey = panel.Target;
    QString error;
    if (panel.IsComparing == false &&
        EditWorldTerrain(panel.Candidate, mEntropy, panel.Target, recipe, panel.CandidateKey, error, panel.Transform) ==
            false)
    {
        panel.Status->setText(error);
        return;
    }
    panel.RequestedSequence = panel.Sequence;
    if (panel.Candidate.SceneLevel().isEmpty() == true)
    {
        panel.Stage = WorldTerrainPanel::Request::LoadingWorld;
        panel.PreviousScene = Render_->Preview().Scene();
        Render_->PreviewTerrainDraft(panel.Candidate);
        return;
    }

    panel.Stage = WorldTerrainPanel::Request::AssemblingScene;
    DraftPreview_ = true;
    SceneGeneration_ = DocumentGeneration_;
    Scene_->SetWorldSnapshot(panel.Candidate);
    if (Scene_->Assemble(panel.Candidate.SceneLevel()) == false)
    {
        DraftPreview_ = false;
        finishTerrainPreview(false, QStringLiteral("Could not prepare the scene. Wait for the World, then retry."));
    }
}

void WorldWorkspace::terrainBoundLoaded(bool isAccepted)
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.Stage != WorldTerrainPanel::Request::LoadingWorld)
    {
        return;
    }
    if (isAccepted == false)
    {
        finishTerrainPreview(false, Render_->GetLoadError());
        return;
    }
    if (PreviewViewport::CanPresent() == false || (Render_->Preview().FrameAvailable() == true &&
                                                   Render_->Preview().Scene().Identity == panel.PreviousScene.Identity))
    {
        finishTerrainPreview(true);
        return;
    }
    if (Render_->Preview().Running() == false)
    {
        finishTerrainPreview(false, Render_->Preview().Status());
        return;
    }
    panel.Stage = WorldTerrainPanel::Request::DrawingWorld;
    panel.DrawingIdentity = Render_->Preview().Scene().Identity;
    Render_->Preview().SetNeedsValidationFrame(true);
}

void WorldWorkspace::finishTerrainPreview(bool isAccepted, const QString& error)
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    const WorldTerrainPanel::Request stage = panel.Stage;
    panel.Stage = WorldTerrainPanel::Request::Idle;
    panel.IsCancellingScene = false;
    if (stage == WorldTerrainPanel::Request::DrawingWorld)
    {
        Render_->Preview().SetNeedsValidationFrame(false);
    }
    const bool isCurrent = panel.IsOpen == true && panel.HasConflict == false &&
                           panel.RequestedSequence == panel.Sequence && panel.WorldGeneration == DocumentGeneration_ &&
                           panel.Base == Document_.Draft();
    if (isCurrent == false)
    {
        if (panel.ShouldRestore == true)
        {
            QTimer::singleShot(0, this, &WorldWorkspace::restoreTerrainWorld);
        }
        return;
    }
    if (isAccepted == false)
    {
        if (stage == WorldTerrainPanel::Request::DrawingWorld || stage == WorldTerrainPanel::Request::LoadingWorld)
        {
            Render_->CancelTerrainDraft();
            if (panel.PreviousScene.Identity != Render_->Preview().Scene().Identity)
            {
                static_cast<void>(Render_->Preview().SetScene(panel.PreviousScene));
            }
            Render_->GetViewport().ShowFrame();
        }
        panel.Status->setText(error.isEmpty() == true
                                  ? QStringLiteral("Preview failed. Resume the viewport and refresh the draft.")
                                  : error);
        panel.Refresh->show();
        return;
    }
    if (stage == WorldTerrainPanel::Request::DrawingWorld)
    {
        Render_->GetViewport().ShowFrame();
    }
    panel.ReadySequence = panel.RequestedSequence;
    panel.ReadyKey = panel.CandidateKey;
    panel.HasDraftView = panel.IsComparing == false;
    panel.ReadyWorld = foundation::Move(panel.Candidate);
    panel.ReadyRecipe = panel.ReadyWorld.Entity(panel.ReadyKey)
                            .value(QStringLiteral("components"))
                            .toObject()
                            .value(GetTerrainComponentId())
                            .toObject();
    panel.PreviousScene = {};
    panel.Compare->setEnabled(panel.Target.isEmpty() == false);
    panel.Frame->setEnabled(true);
    panel.Commit->setEnabled(panel.HasDraftView);
    panel.Refresh->hide();
    panel.Status->setText(PreviewViewport::CanPresent() == false
                              ? QStringLiteral("Recipe validated. This session cannot display the GPU viewport.")
                          : panel.IsComparing == true ? QStringLiteral("Showing committed terrain.")
                          : PreviewViewport::GetPresentation() == ViewportPresentation::BrowserCanvas &&
                                  panel.Target.isEmpty() == true
                              ? QStringLiteral("Terrain preview ready. Use the fields to resize in the browser.")
                              : QStringLiteral("Terrain preview ready."));
    updateViewportInteraction();
    if (panel.ShouldFrame == true)
    {
        panel.ShouldFrame = false;
        frameTerrainDraft();
    }
}

void WorldWorkspace::cancelTerrainRequest()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.Stage == WorldTerrainPanel::Request::AssemblingScene)
    {
        if (panel.IsCancellingScene == false)
        {
            panel.IsCancellingScene = true;
            Scene_->CancelOperation();
        }
        return;
    }
    if (panel.Stage != WorldTerrainPanel::Request::Idle)
    {
        Render_->CancelTerrainDraft();
        if (panel.Stage == WorldTerrainPanel::Request::DrawingWorld)
        {
            Render_->Preview().SetNeedsValidationFrame(false);
            static_cast<void>(Render_->Preview().SetScene(panel.PreviousScene));
        }
        panel.Stage = WorldTerrainPanel::Request::Idle;
        Render_->GetViewport().ShowFrame();
    }
}

void WorldWorkspace::closeTerrainAuthoring(bool shouldRestoreWorld)
{
    if (isTerrainAuthoringOpen() == false)
    {
        return;
    }
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    panel.IsOpen = false;
    panel.hide();
    panel.Timer.stop();
    ++panel.Sequence;
    panel.ShouldRestore = shouldRestoreWorld;
    cancelTerrainRequest();
    Render_->CancelTerrainDraft();
    panel.HasDraftView = false;
    panel.ReadyWorld = {};
    panel.ReadyRecipe = {};
    panel.Candidate = {};
    panel.PreviousScene = {};
    if (panel.Stage == WorldTerrainPanel::Request::Idle)
    {
        restoreTerrainWorld();
    }
    updateViewportInteraction();
}

void WorldWorkspace::restoreTerrainWorld()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.ShouldRestore == false)
    {
        return;
    }
    panel.ShouldRestore = false;
    if (panel.WorldGeneration != DocumentGeneration_)
    {
        return;
    }
    if (Document_.SceneLevel().isEmpty() == true)
    {
        Render_->WorldChanged();
    }
    else
    {
        ValidatedWorld_.clear();
        RefreshScene();
    }
    if (Pending_ == true)
    {
        DraftTimer_.start();
    }
}

void WorldWorkspace::checkTerrainBase()
{
    if (isTerrainAuthoringOpen() == false)
    {
        return;
    }
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.HasConflict == true || (panel.WorldGeneration == DocumentGeneration_ && panel.Base == Document_.Draft()))
    {
        return;
    }
    panel.HasConflict = true;
    ++panel.Sequence;
    panel.Timer.stop();
    panel.HasDraftView = false;
    panel.Frame->setEnabled(false);
    panel.Compare->setEnabled(false);
    panel.Commit->setEnabled(false);
    panel.Refresh->setVisible(panel.Target.isEmpty() == true || canEditTerrain(panel.Target) == true);
    panel.Status->setText(
        panel.Refresh->isHidden() == true
            ? QStringLiteral("This terrain no longer exists. Cancel to close the draft.")
            : QStringLiteral("The World changed. Refresh Draft to keep your settings on the current World."));
    panel.ShouldRestore = panel.Stage == WorldTerrainPanel::Request::AssemblingScene;
    cancelTerrainRequest();
}

void WorldWorkspace::refreshTerrainBase()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (Pending_ == true || isSceneWorkerActive() == true)
    {
        panel.Status->setText(QStringLiteral("Finish pending World operations or reset Properties, then refresh."));
        return;
    }
    if (panel.Target.isEmpty() == false && canEditTerrain(panel.Target) == false)
    {
        checkTerrainBase();
        return;
    }
    panel.Base = Document_.Draft();
    panel.WorldGeneration = DocumentGeneration_;
    panel.HasConflict = false;
    panel.Refresh->hide();
    queueTerrainPreview();
}

void WorldWorkspace::commitTerrainDraft()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    checkTerrainBase();
    if (panel.IsOpen == false || panel.HasConflict == true || panel.IsComparing == true ||
        panel.ReadySequence != panel.Sequence)
    {
        return;
    }
    if (Pending_ == true || isSceneWorkerActive() == true || IsPlayingInViewport() == true)
    {
        panel.Status->setText(QStringLiteral("Finish pending World edits or stop Play before applying this draft."));
        return;
    }
    QString error;
    if (Document_.Restore(panel.ReadyWorld.Draft(), error) == false)
    {
        panel.Status->setText(error);
        return;
    }
    Selected_ = panel.ReadyKey;
    mSelectedEntities = {Selected_};
    if (Document_.SceneLevel().isEmpty() == false)
    {
        ValidatedScene_ = Scene_->Document();
        ValidatedWorld_ = Document_.Encode();
    }
    closeTerrainAuthoring(false);
    Render();
    Snapshot();
    Changed(QStringLiteral("Terrain applied. Save World to keep it. Undo is available."));
}

void WorldWorkspace::frameTerrainDraft()
{
    const WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    RenderPreview& preview = GetRenderPreview();
    preview.SetSelection(panel.ReadyKey);
    if (preview.FrameSelection() == false)
    {
        panel.Status->setText(
            QStringLiteral("No drawable terrain to frame. Refresh the draft after the viewport starts."));
    }
}

void WorldWorkspace::applyTerrainInteraction()
{
    if (mTerrainPanelOrNull == nullptr)
    {
        return;
    }
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    const bool hasDraft = panel.IsOpen == true && panel.HasDraftView == true && panel.HasConflict == false;
    PreviewViewport& viewport =
        Document_.SceneLevel().isEmpty() == true ? Render_->GetViewport() : Scene_->GetViewport();
    viewport.SetTerrainPreviewLabel(hasDraft);
    ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
    if (hasDraft == true && panel.Target.isEmpty() == true)
    {
        interaction.SetWorld(panel.ReadyWorld, panel.ReadyKey, true);
        interaction.SetTerrainFootprint(panel.ReadyRecipe);
        GetRenderPreview().SetSelection(panel.ReadyKey);
        return;
    }
    interaction.SetTerrainFootprint({});
}

bool WorldWorkspace::tryPlaceTerrainDraft(const QString& id, const QJsonObject& placement)
{
    if (isTerrainAuthoringOpen() == false || mTerrainPanelOrNull->Target.isEmpty() == false ||
        mTerrainPanelOrNull->ReadyKey != id)
    {
        return false;
    }
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    const QJsonArray position = placement.value(QStringLiteral("position")).toArray();
    const QJsonArray rotation = placement.value(QStringLiteral("rotation")).toArray();
    const QJsonArray scale = placement.value(QStringLiteral("scale")).toArray();
    for (int32 axis = 0; axis < ViewportObjectInteraction::AXIS_COUNT; ++axis)
    {
        const QString coordinate(QChar('x' + axis));
        panel.Transform.insert(coordinate, position[axis]);
        panel.Transform.insert(QStringLiteral("r") + coordinate, rotation[axis]);
        panel.Transform.insert(QStringLiteral("s") + coordinate, scale[axis]);
    }
    queueTerrainPreview();
    return true;
}
} // namespace ludus::editor
