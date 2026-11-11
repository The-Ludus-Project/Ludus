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
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
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
constexpr int32 IMPORT_TAB = 2;
constexpr float64 DEFAULT_BRUSH_RADIUS_METRES = 2;
constexpr int32 BRUSH_DECIMAL_PLACES = 3;
constexpr QLatin1StringView HEIGHTMAP_FIELD("heightmap");
constexpr QLatin1StringView HEIGHTMAP_SHAPE("Heightmap");

QJsonObject GetTerrainRecipe(const WorldDocument& world, const QString& key)
{
    return world.Entity(key).value(QStringLiteral("components")).toObject().value(GetTerrainComponentId()).toObject();
}

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
          Import(new QPushButton(QStringLiteral("Choose Heightmap…"), this)), HeightmapInfo(new QLabel(this)),
          FlipX(new QCheckBox(QStringLiteral("Flip X"), this)), FlipZ(new QCheckBox(QStringLiteral("Flip Z"), this)),
          Sculpt(new QComboBox(this)), PickHeight(new QPushButton(QStringLiteral("Pick from surface"), this)),
          BrushRadius(new QDoubleSpinBox(this)), BrushHeight(new QDoubleSpinBox(this)),
          BrushDelta(new QDoubleSpinBox(this)), BrushStrength(new QDoubleSpinBox(this)),
          ShowSlope(new QCheckBox(QStringLiteral("Show slope at cursor"), this)), mForm(new QFormLayout),
          mPresetRow(new QWidget(this))
    {
        setObjectName(QStringLiteral("worldTerrainPanel"));
        QVBoxLayout* layout = new QVBoxLayout(this);
        Title->setWordWrap(true);
        layout->addWidget(Title);
        Mode->setObjectName(QStringLiteral("worldTerrainMode"));
        Mode->setAccessibleName(QStringLiteral("Terrain source"));
        Mode->addTab(QStringLiteral("Flat"));
        Mode->addTab(QStringLiteral("Generate"));
        Mode->addTab(QStringLiteral("Import"));
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
        Import->setObjectName(QStringLiteral("worldTerrainImportHeightmap"));
        HeightmapInfo->setObjectName(QStringLiteral("worldTerrainHeightmapInfo"));
        HeightmapInfo->setWordWrap(true);
        HeightmapInfo->setTextFormat(Qt::PlainText);
        FlipX->setObjectName(QStringLiteral("worldTerrainFlipX"));
        FlipZ->setObjectName(QStringLiteral("worldTerrainFlipZ"));
#if defined(Q_OS_WASM)
        Import->setEnabled(false);
        Import->setToolTip(QStringLiteral("Import PNG files in the native editor, then open the saved World here."));
#endif
        mForm->addRow(Import);
        mForm->addRow(HeightmapInfo);
        mForm->addRow(FlipX);
        mForm->addRow(FlipZ);
        addField(QStringLiteral("width"), QStringLiteral("Width (m)"));
        addField(QStringLiteral("depth"), QStringLiteral("Depth (m)"));
        addField(QStringLiteral("relief"), QStringLiteral("Relief (m)"));
        mForm->addRow(Advanced);
        addField(QStringLiteral("minimumElevation"), QStringLiteral("Base elevation (m)"));
        addField(QStringLiteral("seed"), QStringLiteral("Seed"));
        addField(QStringLiteral("featureSize"), QStringLiteral("Feature size (m)"));
        addField(QStringLiteral("roughness"), QStringLiteral("Roughness"));
        addField(QStringLiteral("samples"), QStringLiteral("Samples per side"));
        Sculpt->setObjectName(QStringLiteral("worldTerrainSculptBrush"));
        Sculpt->addItem(QStringLiteral("Off"), static_cast<int32>(TerrainBrush::Off));
        Sculpt->addItem(QStringLiteral("Flatten"), static_cast<int32>(TerrainBrush::Flatten));
        Sculpt->addItem(QStringLiteral("Raise / Lower"), static_cast<int32>(TerrainBrush::RaiseLower));
        Sculpt->addItem(QStringLiteral("Smooth"), static_cast<int32>(TerrainBrush::Smooth));
        PickHeight->setObjectName(QStringLiteral("worldTerrainPickHeight"));
        PickHeight->setCheckable(true);
        BrushRadius->setObjectName(QStringLiteral("worldTerrainBrushRadius"));
        BrushRadius->setDecimals(BRUSH_DECIMAL_PLACES);
        BrushRadius->setRange(MIN_TERRAIN_BRUSH_RADIUS_METRES, MAX_TERRAIN_BRUSH_RADIUS_METRES);
        BrushRadius->setValue(DEFAULT_BRUSH_RADIUS_METRES);
        BrushHeight->setObjectName(QStringLiteral("worldTerrainBrushHeight"));
        BrushHeight->setDecimals(BRUSH_DECIMAL_PLACES);
        BrushHeight->setRange(-MAX_TERRAIN_BRUSH_HEIGHT_METRES, MAX_TERRAIN_BRUSH_HEIGHT_METRES);
        BrushDelta->setObjectName(QStringLiteral("worldTerrainBrushDelta"));
        BrushDelta->setDecimals(BRUSH_DECIMAL_PLACES);
        BrushDelta->setRange(-MAX_TERRAIN_BRUSH_HEIGHT_METRES, MAX_TERRAIN_BRUSH_HEIGHT_METRES);
        BrushDelta->setValue(1);
        BrushStrength->setObjectName(QStringLiteral("worldTerrainBrushStrength"));
        BrushStrength->setDecimals(BRUSH_DECIMAL_PLACES);
        constexpr float64 MINIMUM_SMOOTH_STRENGTH = 0.001;
        BrushStrength->setRange(MINIMUM_SMOOTH_STRENGTH, 1);
        constexpr float64 SMOOTH_STRENGTH_STEP = 0.1;
        BrushStrength->setSingleStep(SMOOTH_STRENGTH_STEP);
        BrushStrength->setValue(0.5);
        mForm->addRow(QStringLiteral("Sculpt brush"), Sculpt);
        mForm->addRow(QStringLiteral("Radius (m)"), BrushRadius);
        mForm->addRow(QStringLiteral("Target height (m)"), BrushHeight);
        mForm->addRow(PickHeight);
        mForm->addRow(QStringLiteral("Height change (m)"), BrushDelta);
        mForm->addRow(QStringLiteral("Strength"), BrushStrength);
        ShowSlope->setObjectName(QStringLiteral("worldTerrainShowSlope"));
        mForm->addRow(ShowSlope);
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
        // A dragged stroke previews as soon as the event loop is free; the busy pipeline paces it.
        LiveTimer.setSingleShot(true);
        LiveTimer.setInterval(0);
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
        const QSignalBlocker sculptBlock(Sculpt);
        const QSignalBlocker pickBlock(PickHeight);
        Sculpt->setCurrentIndex(0);
        PickHeight->setChecked(false);
        mForm->setRowVisible(Sculpt, name.isEmpty() == false);
        Sculpt->setEnabled(false);
        SetHeightmap(mInitialRecipe.value(HEIGHTMAP_FIELD).toObject());
        const QString shape = mInitialRecipe.value(QStringLiteral("shape")).toString();
        Mode->setCurrentIndex(shape == HEIGHTMAP_SHAPE          ? IMPORT_TAB
                              : shape == QStringLiteral("Flat") ? FLAT_TAB
                                                                : GENERATE_TAB);
        Preset->clear();
        const QJsonArray choices =
            mDefinitions.value(QStringLiteral("shape")).toObject().value(QStringLiteral("choices")).toArray();
        for (const QJsonValue choice : choices)
        {
            if (choice.toString() != QStringLiteral("Flat") && choice.toString() != HEIGHTMAP_SHAPE)
            {
                Preset->addItem(choice.toString());
            }
        }
        if (shape != QStringLiteral("Flat") && shape != HEIGHTMAP_SHAPE)
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
        const bool isImport = Mode->currentIndex() == IMPORT_TAB;
        const bool isGenerate = Mode->currentIndex() == GENERATE_TAB;
        const bool hasAdvanced = isGenerate == true && Advanced->isChecked() == true;
        mForm->setRowVisible(mPresetRow, isGenerate);
        mForm->setRowVisible(Import, isImport);
        mForm->setRowVisible(HeightmapInfo, isImport);
        mForm->setRowVisible(FlipX, isImport);
        mForm->setRowVisible(FlipZ, isImport);
        mForm->setRowVisible(Fields.value(QStringLiteral("relief")), isFlat == false);
        mForm->setRowVisible(Advanced, isGenerate);
        mForm->setRowVisible(Fields.value(QStringLiteral("minimumElevation")),
                             isFlat == true || isImport == true || hasAdvanced == true);
        const TerrainBrush brush = GetBrush();
        mForm->setRowVisible(BrushRadius, brush != TerrainBrush::Off);
        mForm->setRowVisible(BrushHeight, brush == TerrainBrush::Flatten);
        mForm->setRowVisible(PickHeight, brush == TerrainBrush::Flatten);
        mForm->setRowVisible(BrushDelta, brush == TerrainBrush::RaiseLower);
        mForm->setRowVisible(BrushStrength, brush == TerrainBrush::Smooth);
        mForm->setRowVisible(ShowSlope, brush != TerrainBrush::Off);
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
        const bool isImport = Mode->currentIndex() == IMPORT_TAB;
        if (isImport == true && mHeightmap.isEmpty() == true)
        {
#if defined(Q_OS_WASM)
            Status->setText(
                QStringLiteral("Import the heightmap in the native editor, then reopen the saved World here."));
#else
            Status->setText(QStringLiteral("Choose a grayscale PNG to preview its heightmap."));
#endif
            return false;
        }
        recipe.insert(QStringLiteral("shape"),
                      isImport == true ? QString(HEIGHTMAP_SHAPE)
                      : isFlat == true ? QStringLiteral("Flat")
                                       : Preset->currentText());
        if (isImport == true)
        {
            QJsonObject map = mHeightmap;
            map.insert(QStringLiteral("flipX"), FlipX->isChecked());
            map.insert(QStringLiteral("flipZ"), FlipZ->isChecked());
            recipe.insert(HEIGHTMAP_FIELD, map);
        }
        else
        {
            recipe.remove(HEIGHTMAP_FIELD);
        }
        for (QMap<QString, QLineEdit*>::const_iterator field = Fields.constBegin(); field != Fields.constEnd(); ++field)
        {
            if (field.key() == QStringLiteral("relief") ||
                ((isFlat == true || isImport == true) && field.key() != QStringLiteral("width") &&
                 field.key() != QStringLiteral("depth") && field.key() != QStringLiteral("minimumElevation")))
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
            // Preserve the stored JSON value when the elevation mapping is unchanged.
            if (base != initialBase || relief != initialMaximum - initialBase)
            {
                recipe.insert(QStringLiteral("maximumElevation"), base + relief);
            }
        }
        SetTerrainRecipeVersion(recipe);
        outRecipe = foundation::Move(recipe);
        return true;
    }

    void SetHeightmap(const QJsonObject& heightmap)
    {
        mHeightmap = heightmap;
        const QSignalBlocker blockX(FlipX);
        const QSignalBlocker blockZ(FlipZ);
        FlipX->setChecked(heightmap.value(QStringLiteral("flipX")).toBool());
        FlipZ->setChecked(heightmap.value(QStringLiteral("flipZ")).toBool());
        const QJsonArray rows = heightmap.value(QStringLiteral("rows")).toArray();
        HeightmapInfo->setText(
            rows.isEmpty() == true
                ? QStringLiteral("Choose an 8- or 16-bit grayscale PNG.")
                : QStringLiteral("%1 × %2 samples. Image top-left maps to local −X, −Z before flips.")
                      .arg(rows.first().toArray().size())
                      .arg(rows.size()));
    }

    [[nodiscard]] const QJsonObject& GetCommittedRecipe() const
    {
        return mInitialRecipe;
    }

    [[nodiscard]] LUDUS_INLINE TerrainBrush GetBrush() const
    {
        return static_cast<TerrainBrush>(Sculpt->currentData().toInt());
    }

    [[nodiscard]] float32 GetBrushValue() const
    {
        switch (GetBrush())
        {
            case TerrainBrush::Flatten:
                return static_cast<float32>(BrushHeight->value());
            case TerrainBrush::RaiseLower:
                return static_cast<float32>(BrushDelta->value());
            case TerrainBrush::Smooth:
                return static_cast<float32>(BrushStrength->value());
            case TerrainBrush::Off:
                return 0;
        }
        return 0;
    }

    void SetStrokePending(bool isPending)
    {
        Mode->setEnabled(isPending == false);
        Preset->setEnabled(isPending == false);
        Randomize->setEnabled(isPending == false);
        Advanced->setEnabled(isPending == false);
#if !defined(Q_OS_WASM)
        Import->setEnabled(isPending == false);
#endif
        FlipX->setEnabled(isPending == false);
        FlipZ->setEnabled(isPending == false);
        BrushRadius->setEnabled(isPending == false);
        BrushHeight->setEnabled(isPending == false);
        BrushDelta->setEnabled(isPending == false);
        BrushStrength->setEnabled(isPending == false);
        PickHeight->setEnabled(isPending == false);
        for (QLineEdit* field : Fields)
        {
            field->setEnabled(isPending == false);
        }
    }

    void RebaseRecipe(const QJsonObject& recipe)
    {
        if (recipe.isEmpty() == false)
        {
            mInitialRecipe = recipe;
        }
    }

    void ClearStroke()
    {
        StrokeRecipe = {};
        StrokeBaseScene = {};
        IsStrokeLive = false;
        LiveTimer.stop();
        SetStrokePending(false);
    }

    void AcceptStroke(const QJsonObject& recipe)
    {
        RebaseRecipe(recipe);
        ClearStroke();
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
    QPushButton* Import;
    QLabel* HeightmapInfo;
    QCheckBox* FlipX;
    QCheckBox* FlipZ;
    QComboBox* Sculpt;
    QPushButton* PickHeight;
    QDoubleSpinBox* BrushRadius;
    QDoubleSpinBox* BrushHeight;
    QDoubleSpinBox* BrushDelta;
    QDoubleSpinBox* BrushStrength;
    QCheckBox* ShowSlope;
    QMap<QString, QLineEdit*> Fields;
    QTimer Timer;
    QTimer LiveTimer;

    // The existing renderer and World document own the derived frame and the undo stack.
    QJsonObject Base;
    QJsonObject Transform;
    WorldDocument ReadyWorld;
    QJsonObject ReadyRecipe;
    QJsonObject StrokeRecipe;
    /// The ready committed view a live stroke started from; failure and cancellation show it again.
    RenderScene StrokeBaseScene;
    qsizetype RemovedStrokes = 0;
    WorldDocument Candidate;
    RenderScene PreviousScene;
    /// The scene the viewport presented when the request in flight began.
    QByteArray PriorIdentity;
    QByteArray DrawingIdentity;
    QString Target;
    QString ReadyKey;
    QString CandidateKey;
    uint64 WorldGeneration = 0;
    uint64 Sequence = 0;
    uint64 RequestedSequence = 0;
    uint64 ReadySequence = 0;
    // A live stroke's points advance LiveSequence without invalidating the ready committed view.
    uint64 LiveSequence = 0;
    uint64 RequestedLiveSequence = 0;
    Request Stage = Request::Idle;
    bool IsOpen = false;
    bool IsComparing = false;
    bool HasDraftView = false;
    bool ShouldFrame = false;
    bool IsCancellingScene = false;
    bool ShouldRestore = false;
    bool HasConflict = false;
    /// StrokeRecipe holds the stroke still being dragged. Its previews are shown but never committed.
    bool IsStrokeLive = false;
    /// The request in flight previews a live stroke.
    bool IsRequestLive = false;

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
            if (key == QStringLiteral("relief"))
            {
                inoutRecipe.insert(key, initial);
            }
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
    QJsonObject mHeightmap;
};

void WorldWorkspace::initializeTerrainAuthoring()
{
    WorldTerrainPanel* panel = new WorldTerrainPanel(Split_);
    mTerrainPanelOrNull = panel;
    Split_->setCollapsible(Split_->indexOf(panel), false);
    connect(&panel->Timer, &QTimer::timeout, this, &WorldWorkspace::prepareTerrainPreview);
    connect(&panel->LiveTimer, &QTimer::timeout, this, &WorldWorkspace::prepareTerrainPreview);
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
    connect(panel->FlipX, &QCheckBox::toggled, this, changeSource);
    connect(panel->FlipZ, &QCheckBox::toggled, this, changeSource);
#if !defined(Q_OS_WASM)
    connect(panel->Import, &QPushButton::clicked, this, [this, panel, changeSource]() {
        const uint64 sequence = panel->Sequence;
        const QString path = QFileDialog::getOpenFileName(this,
                                                          QStringLiteral("Import Terrain Heightmap"),
                                                          {},
                                                          QStringLiteral("Grayscale PNG (*.png)"));
        if (path.isEmpty() == true || panel->IsOpen == false || panel->Sequence != sequence)
        {
            return;
        }
        QJsonObject map;
        QString error;
        if (ImportTerrainHeightmap(path, map, error) == false)
        {
            panel->Status->setText(error);
            return;
        }
        panel->SetHeightmap(map);
        changeSource();
    });
#endif
    const auto cancelBrushGesture = [this]() { cancelTerrainBrushGesture(); };
    connect(panel->Sculpt, &QComboBox::currentIndexChanged, this, [this, panel, cancelBrushGesture]() {
        cancelBrushGesture();
        const QSignalBlocker blocker(panel->PickHeight);
        panel->PickHeight->setChecked(false);
        panel->UpdateFields();
        applyTerrainInteraction();
        switch (panel->GetBrush())
        {
            case TerrainBrush::Off:
                panel->Status->setText(QStringLiteral("Choose a brush to sculpt, or change the terrain source."));
                break;
            case TerrainBrush::Flatten:
                panel->Status->setText(
                    QStringLiteral("Drag to flatten to the target height. Release keeps the stroke."));
                break;
            case TerrainBrush::RaiseLower:
                panel->Status->setText(QStringLiteral("Enter a positive amount to raise terrain or a negative amount "
                                                      "to lower it. Drag to sculpt, and release to keep the stroke."));
                break;
            case TerrainBrush::Smooth:
                panel->Status->setText(
                    QStringLiteral("Drag to blend the terrain toward nearby heights. Release keeps the stroke."));
                break;
        }
    });
    connect(panel->PickHeight, &QPushButton::toggled, this, [this, panel](bool isPicking) {
        applyTerrainInteraction();
        if (isPicking == true)
        {
            panel->Status->setText(QStringLiteral("Click the terrain to pick its local height."));
        }
    });
    connect(panel->BrushRadius, &QDoubleSpinBox::valueChanged, this, [this]() { applyTerrainInteraction(); });
    connect(panel->ShowSlope, &QCheckBox::toggled, this, [this]() { applyTerrainInteraction(); });
    connect(panel->BrushHeight, &QDoubleSpinBox::valueChanged, this, cancelBrushGesture);
    connect(panel->BrushDelta, &QDoubleSpinBox::valueChanged, this, cancelBrushGesture);
    connect(panel->BrushStrength, &QDoubleSpinBox::valueChanged, this, cancelBrushGesture);
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
        ViewportObjectInteraction& interaction = viewport->GetObjectInteraction();
        connect(&interaction,
                &ViewportObjectInteraction::TerrainStrokeChanged,
                this,
                &WorldWorkspace::previewTerrainStroke);
        connect(&interaction,
                &ViewportObjectInteraction::TerrainStrokeFinished,
                this,
                &WorldWorkspace::queueTerrainStroke);
        connect(&interaction,
                &ViewportObjectInteraction::TerrainStrokeRejected,
                this,
                [this, panel](const QString& message) {
                    if (panel->IsStrokeLive == true)
                    {
                        abandonTerrainStroke();
                    }
                    panel->Status->setText(message);
                });
        connect(&interaction, &ViewportObjectInteraction::TerrainStrokeCancelled, this, [this, panel]() {
            if (panel->IsStrokeLive == true)
            {
                abandonTerrainStroke();
                panel->Status->setText(QStringLiteral("Stroke cancelled. The surface is unchanged."));
            }
        });
        connect(&interaction, &ViewportObjectInteraction::TerrainSculptRefused, this, [this, panel]() {
            panel->Status->setText(
                Pending_ == true             ? QStringLiteral("Apply or reset the Properties changes, then sculpt.")
                : panel->IsComparing == true ? QStringLiteral("Turn off Compare with committed to sculpt.")
                : panel->HasConflict == true ? QStringLiteral("The World changed. Refresh Draft, then sculpt.")
                : panel->StrokeRecipe.isEmpty() == false || panel->ReadySequence != panel->Sequence
                    ? QStringLiteral("Applying the previous stroke. Sculpt again when the preview is ready.")
                    : QStringLiteral("Apply or cancel the source changes before sculpting."));
        });
        connect(&interaction, &ViewportObjectInteraction::TerrainHeightPicked, this, [panel](float32 height) {
            panel->BrushHeight->setValue(static_cast<float64>(height));
            panel->PickHeight->setChecked(false);
            panel->Status->setText(
                QStringLiteral("Target height %1 m. Drag to flatten.").arg(panel->BrushHeight->text()));
        });
        connect(&interaction,
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

bool WorldWorkspace::isTerrainStrokeLive() const
{
    return isTerrainAuthoringOpen() == true && mTerrainPanelOrNull->IsStrokeLive == true;
}

void WorldWorkspace::cancelTerrainBrushGesture()
{
    Render_->GetViewport().GetObjectInteraction().Cancel();
    Scene_->GetViewport().GetObjectInteraction().Cancel();
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
    panel.ClearStroke();
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
    if (panel.IsStrokeLive == true)
    {
        // A source or comparison change replaces the drag's base surface.
        panel.ClearStroke();
        cancelTerrainBrushGesture();
    }
    ++panel.Sequence;
    panel.Commit->setEnabled(false);
    panel.Compare->setEnabled(false);
    panel.Sculpt->setEnabled(false);
    if (panel.GetBrush() != TerrainBrush::Off)
    {
        applyTerrainInteraction();
    }
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
    if (panel.IsStrokeLive == true && panel.RequestedLiveSequence == panel.LiveSequence)
    {
        // The newest dragged points are already shown or in flight.
        return;
    }
    if (panel.Stage != WorldTerrainPanel::Request::Idle || isSceneWorkerActive() == true)
    {
        panel.Timer.start();
        return;
    }

    QJsonObject recipe = panel.StrokeRecipe;
    if (recipe.isEmpty() == true && panel.ReadRecipe(recipe) == false)
    {
        return;
    }
    panel.RemovedStrokes = RemoveClippedTerrainStrokes(panel.GetCommittedRecipe(), recipe);
    panel.Candidate = Document_;
    panel.CandidateKey = panel.Target;
    QString error;
    if (panel.IsComparing == false &&
        EditWorldTerrain(panel.Candidate, mEntropy, panel.Target, recipe, panel.CandidateKey, error, panel.Transform) ==
            false)
    {
        if (panel.IsStrokeLive == true)
        {
            abandonTerrainStroke();
        }
        else
        {
            panel.ClearStroke();
        }
        panel.Status->setText(error);
        return;
    }
    panel.RequestedSequence = panel.Sequence;
    panel.IsRequestLive = panel.IsStrokeLive;
    panel.RequestedLiveSequence = panel.LiveSequence;
    if (panel.Candidate.SceneLevel().isEmpty() == true)
    {
        panel.Stage = WorldTerrainPanel::Request::LoadingWorld;
        panel.PriorIdentity = Render_->Preview().Scene().Identity;
        // Stroke previews fall back to the surface the stroke started from, not to an earlier partial stroke.
        panel.PreviousScene = panel.StrokeRecipe.isEmpty() == false && panel.StrokeBaseScene.Identity.isEmpty() == false
                                  ? panel.StrokeBaseScene
                                  : Render_->Preview().Scene();
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
    if (PreviewViewport::CanPresent() == false ||
        (Render_->Preview().FrameAvailable() == true && Render_->Preview().Scene().Identity == panel.PriorIdentity))
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
    const bool isLiveRequest = panel.IsRequestLive;
    panel.Stage = WorldTerrainPanel::Request::Idle;
    panel.IsRequestLive = false;
    panel.IsCancellingScene = false;
    if (stage == WorldTerrainPanel::Request::DrawingWorld)
    {
        Render_->Preview().SetNeedsValidationFrame(false);
    }
    const bool isBaseCurrent = panel.IsOpen == true && panel.HasConflict == false &&
                               panel.WorldGeneration == DocumentGeneration_ && panel.Base == Document_.Draft();
    const bool isCurrent = isBaseCurrent == true && panel.RequestedSequence == panel.Sequence;
    if (isCurrent == false)
    {
        if (panel.ShouldRestore == true)
        {
            QTimer::singleShot(0, this, &WorldWorkspace::restoreTerrainWorld);
        }
        else if (isLiveRequest == true && isBaseCurrent == true && panel.StrokeRecipe.isEmpty() == false)
        {
            // Release superseded this partial stroke. Show it while the released stroke prepares.
            if (isAccepted == true && stage == WorldTerrainPanel::Request::DrawingWorld)
            {
                Render_->GetViewport().ShowFrame();
            }
            panel.LiveTimer.start();
        }
        return;
    }
    if (isAccepted == false)
    {
        const bool hasLiveStroke = panel.IsStrokeLive;
        panel.ClearStroke();
        if (hasLiveStroke == true)
        {
            cancelTerrainBrushGesture();
            updateViewportInteraction();
        }
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
    if (isLiveRequest == true)
    {
        // A dragged stroke's preview is never ready to commit; the committed view stays the ready one.
        if (panel.IsStrokeLive == true && panel.RequestedLiveSequence != panel.LiveSequence)
        {
            panel.LiveTimer.start();
        }
        return;
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
    if (panel.IsStrokeLive == true)
    {
        // A drag began while this view prepared; preview its points next.
        panel.LiveTimer.start();
        return;
    }
    if (panel.StrokeRecipe.isEmpty() == false)
    {
        const uint64 sequence = panel.Sequence;
        QTimer::singleShot(0, this, [this, sequence]() {
            const WorldTerrainPanel& current = *mTerrainPanelOrNull;
            if (current.Sequence == sequence && current.StrokeRecipe.isEmpty() == false)
            {
                commitTerrainDraft();
            }
        });
        return;
    }
    panel.Sculpt->setEnabled(panel.Target.isEmpty() == false && panel.HasDraftView == true &&
                             panel.ReadyRecipe == panel.GetCommittedRecipe());
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
        panel.IsRequestLive = false;
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
    panel.ClearStroke();
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
    const bool hasLiveStroke = panel.IsStrokeLive;
    panel.ClearStroke();
    if (hasLiveStroke == true)
    {
        cancelTerrainBrushGesture();
    }
    panel.Sculpt->setEnabled(false);
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
    panel.RebaseRecipe(GetTerrainRecipe(Document_, panel.Target));
    panel.HasConflict = false;
    panel.Refresh->hide();
    queueTerrainPreview();
}

void WorldWorkspace::previewTerrainStroke(const QJsonArray& points)
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    // Only a press on the ready committed view starts a live stroke; later points extend it.
    if (panel.IsStrokeLive == false &&
        (points.size() != 1 || panel.IsOpen == false || panel.Sculpt->isEnabled() == false ||
         panel.GetBrush() == TerrainBrush::Off || panel.ReadySequence != panel.Sequence ||
         panel.ReadyRecipe != panel.GetCommittedRecipe() || panel.Target != Selected_))
    {
        return;
    }
    QJsonObject recipe = panel.ReadyRecipe;
    QString error;
    if (AppendTerrainStroke(recipe,
                            points,
                            static_cast<float32>(panel.BrushRadius->value()),
                            panel.GetBrush(),
                            panel.GetBrushValue(),
                            error) == false)
    {
        // Release reports the same problem; the surface keeps the last previewed points until then.
        panel.Status->setText(error);
        return;
    }
    if (panel.IsStrokeLive == false)
    {
        panel.IsStrokeLive = true;
        if (Document_.SceneLevel().isEmpty() == true)
        {
            panel.StrokeBaseScene = Render_->Preview().Scene();
        }
        panel.SetStrokePending(true);
        panel.Status->setText(QStringLiteral("Sculpting… Release keeps the stroke."));
    }
    panel.StrokeRecipe = Move(recipe);
    ++panel.LiveSequence;
    if (panel.Stage == WorldTerrainPanel::Request::Idle)
    {
        panel.LiveTimer.start();
    }
}

void WorldWorkspace::abandonTerrainStroke()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    const RenderScene base = panel.StrokeBaseScene;
    panel.ClearStroke();
    cancelTerrainBrushGesture();
    if (Document_.SceneLevel().isEmpty() == false || base.Identity.isEmpty() == true)
    {
        // An assembled scene has no retained base view; assemble the committed terrain again.
        queueTerrainPreview();
        return;
    }
    cancelTerrainRequest();
    // The viewport's World draft is the committed document again, matching the ready view.
    Render_->CancelTerrainDraft();
    if (Render_->Preview().Scene().Identity != base.Identity)
    {
        static_cast<void>(Render_->Preview().SetScene(base));
    }
    Render_->GetViewport().ShowFrame();
    updateViewportInteraction();
}

void WorldWorkspace::queueTerrainStroke(const QJsonArray& points)
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    if (panel.IsStrokeLive == true)
    {
        // The surface already shows the stroke. Prepare it once more as the edit to commit.
        QJsonObject recipe = panel.ReadyRecipe;
        QString error;
        if (AppendTerrainStroke(recipe,
                                points,
                                static_cast<float32>(panel.BrushRadius->value()),
                                panel.GetBrush(),
                                panel.GetBrushValue(),
                                error) == false)
        {
            abandonTerrainStroke();
            panel.Status->setText(error);
            return;
        }
        panel.IsStrokeLive = false;
        panel.StrokeRecipe = Move(recipe);
        ++panel.Sequence;
        panel.Commit->setEnabled(false);
        panel.Compare->setEnabled(false);
        panel.Sculpt->setEnabled(false);
        applyTerrainInteraction();
        panel.Status->setText(QStringLiteral("Applying stroke…"));
        // A partial stroke still in flight starts this request when it finishes.
        if (panel.Stage == WorldTerrainPanel::Request::Idle)
        {
            panel.LiveTimer.start();
        }
        return;
    }
    if (panel.IsOpen == false || panel.Sculpt->isEnabled() == false || panel.GetBrush() == TerrainBrush::Off ||
        panel.ReadySequence != panel.Sequence || panel.ReadyRecipe != panel.GetCommittedRecipe() ||
        panel.Target != Selected_)
    {
        return;
    }
    QJsonObject recipe = panel.ReadyRecipe;
    QString error;
    if (AppendTerrainStroke(recipe,
                            points,
                            static_cast<float32>(panel.BrushRadius->value()),
                            panel.GetBrush(),
                            panel.GetBrushValue(),
                            error) == false)
    {
        panel.Status->setText(error);
        return;
    }
    panel.StrokeRecipe = Move(recipe);
    panel.SetStrokePending(true);
    queueTerrainPreview();
}

void WorldWorkspace::commitTerrainDraft()
{
    WorldTerrainPanel& panel = *mTerrainPanelOrNull;
    checkTerrainBase();
    if (panel.IsOpen == false || panel.HasConflict == true || panel.IsComparing == true || panel.IsStrokeLive == true ||
        panel.ReadySequence != panel.Sequence)
    {
        return;
    }
    const bool isStroke = panel.StrokeRecipe.isEmpty() == false;
    if (Pending_ == true || isSceneWorkerActive() == true || IsPlayingInViewport() == true)
    {
        panel.Status->setText(
            isStroke == true
                ? QStringLiteral("Finish pending World edits or stop Play, then refresh the draft to apply the stroke.")
                : QStringLiteral("Finish pending World edits or stop Play before applying this draft."));
        panel.Refresh->setVisible(isStroke);
        return;
    }
    const QJsonArray previousRows =
        panel.GetCommittedRecipe().value(HEIGHTMAP_FIELD).toObject().value(QStringLiteral("rows")).toArray();
    const QJsonArray nextRows =
        panel.ReadyRecipe.value(HEIGHTMAP_FIELD).toObject().value(QStringLiteral("rows")).toArray();
    const bool shouldReplaceHeightmap = previousRows.isEmpty() == false && previousRows != nextRows;
    if (panel.RemovedStrokes != 0 || shouldReplaceHeightmap == true)
    {
        const uint64 sequence = panel.Sequence;
        QString message;
        if (panel.RemovedStrokes != 0)
        {
            message = QStringLiteral("The smaller footprint cuts %1 sculpt strokes. Applying removes those strokes.")
                          .arg(panel.RemovedStrokes);
        }
        if (shouldReplaceHeightmap == true)
        {
            if (message.isEmpty() == false)
            {
                message += QLatin1Char(' ');
            }
            message += QStringLiteral("Applying replaces the embedded %1 × %2 heightmap with the selected source.")
                           .arg(previousRows.first().toArray().size())
                           .arg(previousRows.size());
        }
        QMessageBox confirmation(QMessageBox::Question,
                                 QStringLiteral("Apply Terrain Source Changes"),
                                 message,
                                 QMessageBox::Cancel,
                                 this);
        QPushButton* apply = confirmation.addButton(QStringLiteral("Remove and apply"), QMessageBox::AcceptRole);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        checkTerrainBase();
        if (confirmation.clickedButton() != apply || panel.IsOpen == false || panel.HasConflict == true ||
            panel.Sequence != sequence || Pending_ == true || isSceneWorkerActive() == true ||
            IsPlayingInViewport() == true)
        {
            return;
        }
    }
    QString error;
    if (Document_.Restore(panel.ReadyWorld.Draft(), error, "Apply Terrain Changes") == false)
    {
        panel.ClearStroke();
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
    if (isStroke == true)
    {
        panel.Base = Document_.Draft();
        panel.AcceptStroke(panel.ReadyRecipe);
        panel.HasDraftView = false;
        panel.Sculpt->setEnabled(true);
        panel.Compare->setEnabled(true);
        panel.Status->setText(QStringLiteral("Stroke applied. Undo restores the previous surface."));
    }
    else
    {
        closeTerrainAuthoring(false);
    }
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
    PreviewViewport& otherViewport =
        &viewport == &Render_->GetViewport() ? Scene_->GetViewport() : Render_->GetViewport();
    otherViewport.GetObjectInteraction().SetTerrainBrush({}, 0, false, false, false);
    // The chosen brush stays the viewport tool while a stroke's preview is pending, so clicks in that window
    // are refused instead of selecting the terrain and showing its transform handles.
    const bool hasBrush = panel.IsOpen == true && panel.GetBrush() != TerrainBrush::Off &&
                          IsPlayingInViewport() == false && panel.Target.isEmpty() == false &&
                          panel.Target == Selected_;
    const bool canSculpt =
        hasBrush == true && panel.Sculpt->isEnabled() == true && panel.IsComparing == false && Pending_ == false;
    interaction.SetTerrainBrush(hasBrush == true ? panel.Target : QString{},
                                hasBrush == true ? static_cast<float32>(panel.BrushRadius->value()) : 0,
                                hasBrush == true && panel.PickHeight->isChecked() == true,
                                hasBrush == true && panel.ShowSlope->isChecked() == true,
                                canSculpt);
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
