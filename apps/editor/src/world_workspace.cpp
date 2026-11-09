#include "internal/world_workspace.h"
#include "design_tokens.h"
#include "internal/builtin_world.h"
#include "internal/import_report.h"
#include "internal/inspector_fields.h"
#include "internal/item_selection.h"
#include "internal/preview_viewport.h"
#include "internal/render_scene.h"
#include "internal/world_celestial_light.h"
#include "internal/world_entity_search.h"

#include "internal/controller.h"
#include "internal/editor_files.h"
#include "internal/editor_icons.h"
#include "internal/editor_identity.h"
#include "internal/level_workspace.h"
#include "internal/material_inspection.h"
#include "internal/render_panel.h"
#include "internal/state_view.h"
#include "internal/world_demo_adapter.h"
#include "internal/world_lights_panel.h"
#include "internal/world_terrain.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/runtime/resources/scene.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QColorSpace>
#include <QColorTransform>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QLockFile>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStringList>
#include <QStringView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>

namespace ludus::editor
{
namespace
{
/// Room in a recovery envelope for its header line and SHA-256 hex digest line.
constexpr foundation::int64 MAX_RECOVERY_HEADER_BYTES = 256;
/// The entity list never shrinks below a few rows, and entities take more of the column than lights (parts of 5).
/// Inclusive limits of a scene instance's saved position (metres), rotation (degrees) and scale.
/// Axes in a transform or colour row: X, Y and Z, or red, green and blue.
constexpr qsizetype VECTOR_COMPONENT_COUNT = 3;
constexpr foundation::float64 SCENE_POSITION_LIMIT = 1000000;
constexpr foundation::float64 SCENE_ROTATION_LIMIT = 360;
constexpr foundation::float64 SCENE_SCALE_MINIMUM = 0.000001;
constexpr foundation::int32 ENTITY_LIST_MINIMUM_HEIGHT = 96;
constexpr foundation::int32 ENTITY_SECTION_STRETCH = 3;
constexpr foundation::int32 LIGHT_SECTION_STRETCH = 2;
bool Read(const QString& path, QByteArray& output, foundation::int64 limit)
{
    const QFileInfo info(path);
    if (info.isFile() == false || info.isSymLink() || info.canonicalFilePath() != info.absoluteFilePath())
    {
        return false;
    }
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false || file.size() <= 0 || file.size() > limit)
    {
        return false;
    }
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != file.size() || bytes.size() > limit)
    {
        return false;
    }
    output = bytes;
    return true;
}
bool Replace(const QString& path, const QByteArray& bytes)
{
    if (QFileInfo(path).isSymLink())
    {
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QString FieldKey(const QString& component, const QString& property)
{
    return component + QLatin1Char('/') + property;
}
constexpr foundation::int32 WORLD_PROPERTY_LAYOUT_WIDTH = 360;
void ConfigureCelestialLightFields(QGroupBox& inoutGroup, QFormLayout& inoutRows, const QMap<QString, QWidget*>& fields)
{
    const QString component = QStringLiteral("light");
    QComboBox* kindOrNull = qobject_cast<QComboBox*>(fields.value(FieldKey(component, QStringLiteral("kind"))));
    QComboBox* roleOrNull = qobject_cast<QComboBox*>(fields.value(FieldKey(component, WORLD_CELESTIAL_ROLE)));
    QComboBox* driverOrNull = qobject_cast<QComboBox*>(fields.value(FieldKey(component, WORLD_CELESTIAL_DRIVER)));
    if (kindOrNull == nullptr || roleOrNull == nullptr || driverOrNull == nullptr)
    {
        return;
    }

    struct CelestialRow final
    {
        WorldCelestialFieldGroup Group;
        QWidget* Field;
    };
    QVector<CelestialRow> celestialRows;
    const QString componentPrefix = FieldKey(component, QString());
    for (const QString& key : fields.keys())
    {
        if (key.startsWith(componentPrefix) == true)
        {
            const WorldCelestialFieldGroup group =
                GetWorldCelestialFieldGroup(QStringView(key).sliced(componentPrefix.size()));
            QWidget* fieldOrNull = fields.value(key);
            if (group != WorldCelestialFieldGroup::Other && fieldOrNull != nullptr)
            {
                celestialRows.append({group, fieldOrNull});
            }
        }
    }
    const auto updateRows = [&inoutGroup, &inoutRows, kindOrNull, roleOrNull, driverOrNull, celestialRows]() {
        const bool isDirectional = kindOrNull->currentData().toString() == QStringLiteral("directional");
        const bool hasDriver = isDirectional == true && roleOrNull->currentData().toString() == WORLD_CELESTIAL_DYNAMIC;
        const QString driver = driverOrNull->currentData().toString();
        const bool hasObserver = hasDriver == true && driver != WORLD_CELESTIAL_AUTHORED;
        const bool hasOrbit = hasObserver == true && driver == WORLD_CELESTIAL_ORBIT;
        const bool hasEarth = hasObserver == true && hasOrbit == false;
        for (const CelestialRow& row : celestialRows)
        {
            const bool isVisible = (row.Group == WorldCelestialFieldGroup::Role && isDirectional == true) ||
                                   (row.Group == WorldCelestialFieldGroup::Driver && hasDriver == true) ||
                                   (row.Group == WorldCelestialFieldGroup::Observer && hasObserver == true) ||
                                   (row.Group == WorldCelestialFieldGroup::Earth && hasEarth == true) ||
                                   (row.Group == WorldCelestialFieldGroup::Orbit && hasOrbit == true);
            inoutRows.setRowVisible(row.Field, isVisible);
        }
        inoutGroup.setMinimumHeight(inoutRows.totalHeightForWidth(WORLD_PROPERTY_LAYOUT_WIDTH));
    };
    QObject::connect(kindOrNull, &QComboBox::currentIndexChanged, roleOrNull, updateRows);
    QObject::connect(roleOrNull, &QComboBox::currentIndexChanged, roleOrNull, updateRows);
    QObject::connect(driverOrNull, &QComboBox::currentIndexChanged, roleOrNull, updateRows);
    updateRows();
}
QString Display(const QJsonValue& value)
{
    if (value.isDouble())
    {
        return QString::number(value.toDouble(), 'g', 17);
    }
    return value.toString();
}
/// Editor preference holding the Play mode; project data never records it.
[[nodiscard]] LUDUS_INLINE QString GetPlayModeKey()
{
    return QStringLiteral("play/mode");
}
[[nodiscard]] LUDUS_INLINE QString GetViewportPlayModeValue()
{
    return QStringLiteral("viewport");
}
[[nodiscard]] LUDUS_INLINE QString GetSeparateWindowPlayModeValue()
{
    return QStringLiteral("window");
}
/// What the World document is for; the path label's tooltip, below the full path.
QString GetWorldIntroduction()
{
    return QStringLiteral("Create or open a world, import a scene, and edit placement and components together. "
                          "Projects may supply authoring/world.schema.json for gameplay fields. Runtime binding "
                          "belongs to the game.");
}
} // namespace
WorldWorkspace::WorldWorkspace(QWidget* parent) : WorldWorkspace(ToolingPaths{}, parent) {}
WorldWorkspace::WorldWorkspace(const ToolingPaths& tooling, QWidget* parent)
    : QWidget(parent), mExploreFirstPersonButton(new QPushButton(QStringLiteral("Explore as Player"), this))
{
    setObjectName(QStringLiteral("worldWorkspace"));
    auto* layout = new QVBoxLayout(this);
    // Everything above the viewport shares one container, so focusing the viewport hides it
    // without disturbing the visibility each control derives from the document.
    mHeader = new QWidget(this);
    mHeader->setObjectName(QStringLiteral("worldHeader"));
    QVBoxLayout* header = new QVBoxLayout(mHeader);
    header->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mHeader);
#if defined(Q_OS_WASM)
    // The browser note says where Worlds live; it stays visible instead of a tooltip.
    QLabel* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setToolTip(GetWorldIntroduction());
    header->addWidget(intro);
#endif
    // One row: the open document's path, then its commands. Rarely available commands
    // (Restore Recovery, Cancel scene operation, Play) appear only while they apply.
    QHBoxLayout* files = new QHBoxLayout;
    PathLabel_ = new QLabel(this);
    PathLabel_->setObjectName(QStringLiteral("worldPath"));
    PathLabel_->setTextFormat(Qt::PlainText);
    PathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    PathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    files->addWidget(PathLabel_, 1);
    const auto compact = [](QPushButton* button) {
        button->setProperty("density", QStringLiteral("compact"));
        return button;
    };
    QPushButton* create = compact(new QPushButton(QStringLiteral("New World…"), this));
    create->setObjectName(QStringLiteral("worldNew"));
    QPushButton* open = compact(new QPushButton(QStringLiteral("Open World…"), this));
    open->setObjectName(QStringLiteral("worldOpen"));
    Recovery_ = compact(new QPushButton(QStringLiteral("Restore Recovery"), this));
    Recovery_->setObjectName(QStringLiteral("worldRecover"));
    Recovery_->setToolTip(QStringLiteral("Replace the open World with the unsaved edits recovered after the editor "
                                         "stopped unexpectedly. Undo restores the saved World."));
    files->addWidget(create);
    files->addWidget(open);
    files->addWidget(Recovery_);
    Import_ = compact(new QPushButton(QStringLiteral("Import scene…"), this));
    Import_->setObjectName(QStringLiteral("worldImportScene"));
    files->addWidget(Import_);
    CancelScene_ = compact(new QPushButton(QStringLiteral("Cancel scene operation"), this));
    CancelScene_->setObjectName(QStringLiteral("worldCancelScene"));
    files->addWidget(CancelScene_);
    connect(CancelScene_, &QPushButton::clicked, this, [this]() {
        Scene_->CancelOperation();
        // The Level status label is hidden inside the World workspace; mirror the cancel state here.
        Changed(Scene_->StatusText());
    });
#if defined(Q_OS_WASM)
    // Browser: copy the chosen scene into MEMFS first; the session copy is then cooked in process.
    QMenu* importChoices = new QMenu(Import_);
    const auto importBrowserScene = [this](bool isFolder) {
        OpenBrowserSceneImport(this, isFolder, [this](const QString& source) { (void)PreviewImportScene(source); });
    };
    connect(importChoices->addAction(QStringLiteral("Scene folder…")),
            &QAction::triggered,
            this,
            [importBrowserScene]() { importBrowserScene(true); });
    connect(importChoices->addAction(QStringLiteral("Scene files or .zip…")),
            &QAction::triggered,
            this,
            [importBrowserScene]() { importBrowserScene(false); });
    Import_->setMenu(importChoices);
#else
    connect(Import_, &QPushButton::clicked, this, [this]() {
        const QString source =
            QFileDialog::getOpenFileName(this,
                                         QStringLiteral("Import scene"),
                                         {},
                                         QStringLiteral("Scenes (*.pbrt *.gltf *.glb *.obj *.luscene.json);;PBRT v4 "
                                                        "(*.pbrt);;glTF 2.0 (*.gltf *.glb);;Wavefront OBJ "
                                                        "(*.obj);;Ludus scene (*.luscene.json)"));
        if (source.isEmpty() == false)
        {
            (void)PreviewImportScene(source);
        }
    });
#endif
    PlayButton_ = compact(new QPushButton(QStringLiteral("Play saved World"), this));
    PlayButton_->setObjectName(QStringLiteral("worldPlay"));
    files->addWidget(PlayButton_);
    connect(PlayButton_, &QPushButton::clicked, this, [this]() {
        // The launcher decides whether the viewport stays alive as the game's window.
        if (Save() && Dirty() == false && SceneBusy() == false)
        {
            hidePlayUnavailable();
            Q_EMIT PlayRequested(GetPlayMode());
        }
    });
    mPlayModeBox = new QComboBox(this);
    mPlayModeBox->setObjectName(QStringLiteral("worldPlayMode"));
    mPlayModeBox->setAccessibleName(QStringLiteral("Play mode"));
    mPlayModeBox->setToolTip(QStringLiteral("Where Play saved World opens the game. The choice is an editor "
                                            "preference and applies to the next Play."));
    mPlayModeBox->addItem(QStringLiteral("In viewport"), static_cast<int>(PlayMode::InViewport));
    mPlayModeBox->addItem(QStringLiteral("In separate window"), static_cast<int>(PlayMode::SeparateWindow));
    const bool isSeparateWindowSaved =
        QSettings().value(GetPlayModeKey()).toString() == GetSeparateWindowPlayModeValue();
    mPlayModeBox->setCurrentIndex(mPlayModeBox->findData(
        static_cast<int>(isSeparateWindowSaved == true ? PlayMode::SeparateWindow : PlayMode::InViewport)));
    files->addWidget(mPlayModeBox);
    connect(mPlayModeBox, &QComboBox::activated, this, [this]() {
        SetPlayMode(static_cast<PlayMode>(mPlayModeBox->currentData().toInt()));
    });
    mPlayUnavailableLabel = new QLabel(this);
    mPlayUnavailableLabel->setObjectName(QStringLiteral("worldPlayUnavailable"));
    mPlayUnavailableLabel->setTextFormat(Qt::PlainText);
    mPlayUnavailableLabel->setWordWrap(true);
    mPlayUnavailableLabel->hide();
    QHBoxLayout* playNotice = new QHBoxLayout;
    playNotice->addWidget(mPlayUnavailableLabel, 1);
    mPlayElsewhereButton = new QPushButton(QStringLiteral("Play in a separate window"), this);
    mPlayElsewhereButton->setObjectName(QStringLiteral("worldPlayElsewhere"));
    mPlayElsewhereButton->setToolTip(QStringLiteral("Open the game in its own window this time. The Play mode "
                                                    "preference is unchanged."));
    mPlayElsewhereButton->hide();
    playNotice->addWidget(mPlayElsewhereButton);
    connect(mPlayElsewhereButton, &QPushButton::clicked, this, [this]() {
        if (Save() && Dirty() == false && SceneBusy() == false)
        {
            hidePlayUnavailable();
            Q_EMIT PlayRequested(PlayMode::SeparateWindow);
        }
    });
    mImportReportToggleOrNull = new QToolButton(this);
    mImportReportToggleOrNull->setObjectName(QStringLiteral("worldImportReportToggle"));
    mImportReportToggleOrNull->setText(QStringLiteral("Import report"));
    mImportReportToggleOrNull->setCheckable(true);
    mImportReportToggleOrNull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mImportReportToggleOrNull->setArrowType(Qt::RightArrow);
    mImportReportToggleOrNull->hide();
    files->addWidget(mImportReportToggleOrNull);
    header->addLayout(files);
    header->addLayout(playNotice);
    QHBoxLayout* previewRow = new QHBoxLayout;
    ImportPreview_ = new QPlainTextEdit(this);
    ImportPreview_->setObjectName(QStringLiteral("worldImportPreview"));
    ImportPreview_->setAccessibleName(QStringLiteral("Import preview"));
    ImportPreview_->setReadOnly(true);
    ImportPreview_->setMaximumHeight(160);
    ImportPreview_->hide();
    ConfirmImport_ = new QPushButton(QStringLiteral("Import previewed scene"), this);
    ConfirmImport_->setObjectName(QStringLiteral("worldImportConfirm"));
    ConfirmImport_->hide();
    DiscardImport_ = new QPushButton(QStringLiteral("Discard preview"), this);
    DiscardImport_->setObjectName(QStringLiteral("worldImportDiscard"));
    DiscardImport_->hide();
    QVBoxLayout* previewActions = new QVBoxLayout;
    mChooseImportEnvironmentOrNull = new QPushButton(QStringLiteral("Choose environment…"), this);
    mChooseImportEnvironmentOrNull->setObjectName(QStringLiteral("worldImportEnvironment"));
    mChooseImportEnvironmentOrNull->setToolTip(
        QStringLiteral("Use a latitude/longitude lighting image inside the scene's source folder."));
    mChooseImportEnvironmentOrNull->hide();
    mClearImportEnvironmentOrNull = new QPushButton(QStringLiteral("Use source lighting"), this);
    mClearImportEnvironmentOrNull->setObjectName(QStringLiteral("worldImportSourceLighting"));
    mClearImportEnvironmentOrNull->hide();
    connect(mChooseImportEnvironmentOrNull, &QPushButton::clicked, this, [this]() {
        const QString source = PreviewedSource_;
        const QString environment = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Choose lighting environment"),
            QFileInfo(source).absolutePath(),
            QStringLiteral("Lighting images (*.hdr *.exr *.pfm *.png *.jpg *.jpeg *.tga)"));
        if (environment.isEmpty() == false)
        {
            (void)PreviewImportScene(source, environment);
        }
    });
    connect(mClearImportEnvironmentOrNull, &QPushButton::clicked, this, [this]() {
        const QString source = PreviewedSource_;
        (void)PreviewImportScene(source);
    });
    previewActions->addWidget(mChooseImportEnvironmentOrNull);
    previewActions->addWidget(mClearImportEnvironmentOrNull);
    previewActions->addWidget(ConfirmImport_);
    previewActions->addWidget(DiscardImport_);
    previewActions->addStretch();
    previewRow->addWidget(ImportPreview_, 1);
    previewRow->addLayout(previewActions);
    header->addLayout(previewRow);
    connect(ConfirmImport_, &QPushButton::clicked, this, [this]() { (void)ConfirmImportPreview(); });
    connect(DiscardImport_, &QPushButton::clicked, this, [this]() {
        PreviewedSource_.clear();
        ShowImportPreview(false);
        Changed(QStringLiteral("Import preview discarded; nothing was written."));
    });
    ImportReport_ = new QPlainTextEdit(this);
    ImportReport_->setObjectName(QStringLiteral("worldImportReport"));
    ImportReport_->setAccessibleName(QStringLiteral("Import report"));
    ImportReport_->setReadOnly(true);
    ImportReport_->setMinimumHeight(100);
    ImportReport_->setMaximumHeight(160);
    ImportReport_->hide();
    connect(mImportReportToggleOrNull, &QToolButton::toggled, this, [this](bool isExpanded) {
        ImportReport_->setVisible(isExpanded);
        mImportReportToggleOrNull->setArrowType(isExpanded == true ? Qt::DownArrow : Qt::RightArrow);
    });
    Render_ = new RenderPanel(this);
    header->addWidget(Render_);
    connect(Render_, &RenderPanel::Starting, this, [this]() {
        updateShownRenderPreview(false);
        Q_EMIT PreviewStarting();
    });
    connect(Render_, &RenderPanel::TintRequested, this, &WorldWorkspace::PickTint);
    connect(Render_, &RenderPanel::PlayStateChanged, this, [this]() {
        // The game's images were refused after launch: Play ended, and the offer stands.
        if (Render_->IsPlaying() == false && Render_->GetPlayUnavailableReason().isEmpty() == false)
        {
            showPlayUnavailable(Render_->GetPlayUnavailableReason());
        }
    });
    auto* split = new QSplitter(this);
    Split_ = split;
    split->setObjectName(QStringLiteral("worldSplitter"));
    layout->addWidget(split, 1);
    QScrollArea* hierarchyScroll = new QScrollArea(split);
    mHierarchy = hierarchyScroll;
    hierarchyScroll->setObjectName(QStringLiteral("worldHierarchyScroll"));
    hierarchyScroll->setWidgetResizable(true);
    hierarchyScroll->setMinimumWidth(130);
    QWidget* left = new QWidget(hierarchyScroll);
    QVBoxLayout* hierarchy = new QVBoxLayout(left);
    // A docked workspace may be shorter than the hierarchy's controls. Scroll
    // the contents instead of compressing styled button text below its hint.
    hierarchy->setSizeConstraint(QLayout::SetMinimumSize);
    hierarchyScroll->setWidget(left);
    // Entities and lights share the column's height in a splitter, so a short entity list does not leave a tall
    // empty box above a cramped one, and the divider lets either grow.
    QSplitter* sections = new QSplitter(Qt::Vertical, left);
    sections->setObjectName(QStringLiteral("worldHierarchySections"));
    sections->setChildrenCollapsible(false);
    hierarchy->addWidget(sections, 1);
    QWidget* entityColumn = new QWidget(sections);
    QVBoxLayout* entityLayout = new QVBoxLayout(entityColumn);
    entityLayout->setContentsMargins(0, 0, 0, 0);
    Entities_ = new QListWidget(entityColumn);
    ConfigureItemSelection(*Entities_);
    Entities_->setObjectName(QStringLiteral("worldEntities"));
    Entities_->setAccessibleName(QStringLiteral("World entities"));
    Entities_->setToolTip(QStringLiteral("Right-click to add, duplicate, or delete entities."));
    Entities_->setMinimumHeight(ENTITY_LIST_MINIMUM_HEIGHT);
    mEntitySearch = new WorldEntitySearch(*Entities_, entityColumn);
    entityLayout->addWidget(mEntitySearch);
    entityLayout->addWidget(Entities_, 1);
    mEntityMenu = new QMenu(this);
    mEntityMenu->setObjectName(QStringLiteral("worldEntityContextMenu"));
    mAddEntity = mEntityMenu->addAction(QStringLiteral("Add Entity"));
    mAddEntity->setObjectName(QStringLiteral("worldAdd"));
    mDuplicateEntities = mEntityMenu->addAction(QStringLiteral("Duplicate"));
    mDuplicateEntities->setObjectName(QStringLiteral("worldDuplicate"));
    mDeleteEntities = mEntityMenu->addAction(QStringLiteral("Delete"));
    mDeleteEntities->setObjectName(QStringLiteral("worldDelete"));
    Entities_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(Entities_, &QWidget::customContextMenuRequested, this, [this](const QPoint& positionList) {
        const QListWidgetItem* itemOrNull = Entities_->itemAt(positionList);
        showEntityMenu(Entities_->viewport()->mapToGlobal(positionList),
                       itemOrNull == nullptr ? QString() : itemOrNull->data(Qt::UserRole).toString());
    });
    QToolButton* addObject = new QToolButton(entityColumn);
    addObject->setObjectName(QStringLiteral("worldAddObject"));
    addObject->setText(QStringLiteral("Add World Preset"));
    addObject->setToolTip(QStringLiteral("Create geometry, lights, a player spawn or an aperture in a default World. "
                                         "Project-owned schemas use Add Entity and their declared components."));
    addObject->setPopupMode(QToolButton::InstantPopup);
    QMenu* objectMenu = new QMenu(addObject->text(), addObject);
    mEntityMenu->addSeparator();
    mEntityMenu->addMenu(objectMenu);
    addObject->setMenu(objectMenu);
    addObject->setMinimumHeight(addObject->sizeHint().height());
    entityLayout->addWidget(addObject);
    mExploreFirstPersonButton->setObjectName(QStringLiteral("worldExploreFirstPerson"));
    mExploreFirstPersonButton->setToolTip(
        QStringLiteral("Hold the right mouse button to look, WASD to walk, Shift to run, and Space to jump. "
                       "Press Escape to return to the editor view."));
    mExploreFirstPersonButton->setMinimumHeight(mExploreFirstPersonButton->sizeHint().height());
    entityLayout->addWidget(mExploreFirstPersonButton);
    connect(mExploreFirstPersonButton, &QPushButton::clicked, this, &WorldWorkspace::exploreFirstPerson);
    for (const QString& preset : {QStringLiteral("Cube"),
                                  QStringLiteral("Plane"),
                                  QStringLiteral("Sphere"),
                                  QStringLiteral("Point Light"),
                                  QStringLiteral("Spot Light"),
                                  QStringLiteral("Directional Light"),
                                  QStringLiteral("Sun"),
                                  QStringLiteral("Luminaire"),
                                  QStringLiteral("First Person Player"),
                                  QStringLiteral("Aperture Slit")})
    {
        QAction* action = objectMenu->addAction(preset);
        action->setObjectName(QStringLiteral("worldCreate") + QString(preset).remove(QLatin1Char(' ')));
        connect(objectMenu, &QMenu::aboutToShow, action, [this, action]() {
            action->setEnabled(mAddEntity->isEnabled() == true && IsBuiltInWorld(Document_) == true);
            action->setToolTip(
                QStringLiteral("Create an object in a default World. Project schemas define their own components."));
        });
        connect(action, &QAction::triggered, this, [this, preset]() {
            QString id;
            QString error;
            if (mAddEntity->isEnabled() == false)
            {
                return;
            }
            if (preset == QStringLiteral("Sun"))
            {
                // Aimed like the imported scene's disabled sun when it has one (FitWorldSun).
                const bool hasScene = Document_.Draft().contains(QStringLiteral("scene"));
                const RenderScene& scene = hasScene == true ? Scene_->PreviewScene() : Render_->Preview().Scene();
                static_cast<void>(addSun(FitWorldSun(SummarizeWorldLighting(scene, true))));
                return;
            }
            if (CreateWorldObject(Document_, mEntropy, preset, id, error) == false)
            {
                Changed(error);
                return;
            }
            Selected_ = id;
            mSelectedEntities = {id};
            Render();
            Snapshot();
            Changed(preset +
                    QStringLiteral(" created. Edit its Transform and properties in the inspector. Undo is available."));
        });
    }

    objectMenu->addSeparator();
    QAction* generateTerrain = objectMenu->addAction(QStringLiteral("Add Terrain…"));
    generateTerrain->setObjectName(QStringLiteral("worldGenerateTerrain"));
    connect(objectMenu, &QMenu::aboutToShow, generateTerrain, [this, generateTerrain]() {
        generateTerrain->setEnabled(mAddEntity->isEnabled() == true && Document_.HasBuiltInTerrain() == true &&
                                    isSceneWorkerActive() == false);
    });
    connect(generateTerrain, &QAction::triggered, this, [this]() { showTerrainAuthoring(); });

    mLightsPanel = new WorldLightsPanel(sections);
    sections->setStretchFactor(0, ENTITY_SECTION_STRETCH);
    sections->setStretchFactor(1, LIGHT_SECTION_STRETCH);
    connect(mLightsPanel, &WorldLightsPanel::LightsChangeRequested, this, &WorldWorkspace::applyLightChange);

    BoundViewport_ = Render_->TakeViewport(split);
    Scene_ = new LevelWorkspace(tooling, this);
    Scene_->hide();
    Viewport_ = Scene_->TakeViewport(split);
    Viewport_->setObjectName(QStringLiteral("worldSceneViewport"));
    Viewport_->hide();
    connect(Scene_, &LevelWorkspace::OperationFinished, this, &WorldWorkspace::SceneCompleted);
    connect(Scene_, &LevelWorkspace::AssetsVerified, this, &WorldWorkspace::SceneVerified);
    connect(Scene_, &LevelWorkspace::PlacementValidationFinished, this, &WorldWorkspace::placementValidationFinished);
    connect(Scene_, &LevelWorkspace::ImportPreviewFinished, this, &WorldWorkspace::ShowImportPreview);
    connect(Scene_, &LevelWorkspace::DocumentChanged, this, [this]() { Changed(); });
    connect(Scene_, &LevelWorkspace::PreviewStarting, this, [this]() {
        Render_->Stop();
        // The candidate scene commits only after its first rendered frame, and the
        // native surface is created only once its widget is shown. Show the scene
        // viewport beside the current World view; Render() hides the old view after
        // the commit, or hides the candidate again if the import fails.
        Viewport_->show();
        updateShownRenderPreview(true);
        Q_EMIT PreviewStarting();
    });
    QWidget* inspectorPanel = new QWidget(split);
    inspectorPanel->setObjectName(QStringLiteral("worldProperties"));
    mPropertiesPanel = inspectorPanel;
    QVBoxLayout* inspectorPanelLayout = new QVBoxLayout(inspectorPanel);
    inspectorPanelLayout->setContentsMargins(0, 0, 0, 0);
    QScrollArea* scroll = new QScrollArea(inspectorPanel);
    inspectorPanelLayout->addWidget(scroll, 1);
    scroll->setObjectName(QStringLiteral("worldInspectorScroll"));
    scroll->setWidgetResizable(true);
    QWidget* inspector = new QWidget(scroll);
    QVBoxLayout* inspectorLayout = new QVBoxLayout(inspector);
    inspectorLayout->setSizeConstraint(QLayout::SetMinimumSize);
    // Diagnostics belong to the scrollable inspector, so opening a long report
    // cannot take height away from the viewport or hierarchy.
    inspectorLayout->addWidget(ImportReport_);
    QWidget* fields = new QWidget(inspector);
    Inspector_ = new QVBoxLayout(fields);
    Inspector_->setContentsMargins(0, 0, 0, 0);
    Inspector_->setSizeConstraint(QLayout::SetMinimumSize);
    inspectorLayout->addWidget(fields, 1);
    scroll->setWidget(inspector);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 3);
    split->setStretchFactor(3, 1);
    split->setSizes({240, 520, 520, 300});
    QVBoxLayout* edits = new QVBoxLayout;
    Components_ = new QComboBox(this);
    Components_->setObjectName(QStringLiteral("worldComponentChoice"));
    Components_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    Components_->setMinimumContentsLength(12);
    Components_->setAccessibleName(QStringLiteral("Available components"));
    Attach_ = new QPushButton(QStringLiteral("Add Component"), this);
    Attach_->setObjectName(QStringLiteral("worldAttach"));
    Apply_ = new QPushButton(QStringLiteral("Apply Changes"), this);
    Apply_->setObjectName(QStringLiteral("worldApply"));
    Reset_ = new QPushButton(QStringLiteral("Reset Fields"), this);
    Reset_->setObjectName(QStringLiteral("worldReset"));
    // The component choice sits beside Add Component; Apply and Reset share the row below,
    // so the narrow Properties column keeps its fields in view.
    QHBoxLayout* attach = new QHBoxLayout;
    attach->addWidget(Components_, 1);
    attach->addWidget(Attach_);
    QHBoxLayout* commit = new QHBoxLayout;
    commit->addWidget(Apply_);
    commit->addWidget(Reset_);
    for (QPushButton* button : {Attach_, Apply_, Reset_})
    {
        button->setProperty("density", QStringLiteral("compact"));
    }
    edits->addLayout(attach);
    edits->addLayout(commit);
    inspectorPanelLayout->addLayout(edits);
    Status_ = new QLabel(this);
    Status_->setObjectName(QStringLiteral("worldStatus"));
    Status_->setTextFormat(Qt::PlainText);
    Status_->setWordWrap(true);
    Status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(Status_);
    const auto pick = [this](bool creating) {
        if (Root_.isEmpty())
        {
            Changed(QStringLiteral("Open a project first."));
            return;
        }
        auto* picker = new QFileDialog(this,
                                       creating ? QStringLiteral("New World") : QStringLiteral("Open World"),
                                       Root_,
                                       QStringLiteral("World documents (*.world.json *.lulevel *.json)"));
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setFileMode(creating ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
        picker->setAcceptMode(creating ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
        picker->setDefaultSuffix(QStringLiteral("world.json"));
        const auto epoch = Epoch_;
        connect(picker, &QFileDialog::fileSelected, this, [this, creating, epoch](const QString& path) {
            if (Epoch_ != epoch)
            {
                Changed(QStringLiteral("Project changed while choosing a file. Choose again."));
                return;
            }
            if (creating)
            {
                (void)Create(path);
            }
            else
            {
                (void)Open(path);
            }
        });
        picker->open();
    };
    connect(open, &QPushButton::clicked, this, [pick]() { pick(false); });
    connect(create, &QPushButton::clicked, this, [pick]() { pick(true); });
    const auto updateSelection = [this]() {
        if (Rendering_ == true || Pending_ == true)
        {
            return;
        }
        mSelectedEntities.clear();
        for (const QListWidgetItem* item : Entities_->selectedItems())
        {
            mSelectedEntities.append(item->data(Qt::UserRole).toString());
        }
        const QListWidgetItem* currentOrNull = Entities_->currentItem();
        Selected_ = currentOrNull != nullptr && currentOrNull->isSelected() == true
                        ? currentOrNull->data(Qt::UserRole).toString()
                        : mSelectedEntities.value(0);
        RenderInspector();
        Changed();
    };
    connect(mEntitySearch, &WorldEntitySearch::SelectionChanged, this, updateSelection);
    connect(Entities_, &QListWidget::currentItemChanged, this, updateSelection);
    connect(Entities_, &QListWidget::itemSelectionChanged, this, updateSelection);
    const auto bindCommand = [this](const QKeySequence& key, QAction* action) {
        QShortcut* shortcut = new QShortcut(key, Entities_);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, action, &QAction::trigger);
    };
    bindCommand(QKeySequence(Qt::CTRL | Qt::Key_D), mDuplicateEntities);
    QList<QKeySequence> deleteKeys = QKeySequence::keyBindings(QKeySequence::Delete);
#if defined(Q_OS_MACOS)
    const QKeySequence backspace(Qt::Key_Backspace);
    if (deleteKeys.contains(backspace) == false)
    {
        deleteKeys.append(backspace);
    }
#endif
    for (const QKeySequence& key : deleteKeys)
    {
        bindCommand(key, mDeleteEntities);
    }
    const auto rename = [this]() {
        if (Entities_->isEnabled() == true && mSelectedEntities.size() == 1 && Name_ != nullptr)
        {
            Name_->setFocus();
            Name_->selectAll();
        }
    };
    QShortcut* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), Entities_);
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this, rename);
#if defined(Q_OS_MACOS)
    QShortcut* renameReturn = new QShortcut(QKeySequence(Qt::Key_Return), Entities_);
    renameReturn->setContext(Qt::WidgetShortcut);
    connect(renameReturn, &QShortcut::activated, this, rename);
#endif
    connect(mAddEntity, &QAction::triggered, this, [this]() {
        QString id;
        QString error;
        if (Document_.CreateEntity(mEntropy, error, QStringLiteral("Entity"), id) == false)
        {
            Changed(error);
            return;
        }
        Selected_ = foundation::Move(id);
        mSelectedEntities = {Selected_};
        Render();
        Snapshot();
        Changed(QStringLiteral("Entity created. Give it a meaningful name in the inspector."));
    });
    connect(mDuplicateEntities, &QAction::triggered, this, [this]() {
        QStringList copies;
        QString error;
        if (Document_.DuplicateEntities(mEntropy, mSelectedEntities, copies, error) == false)
        {
            Changed(error);
            return;
        }
        mSelectedEntities = copies;
        Selected_ = copies.value(0);
        Render();
        Snapshot();
        Changed(QStringLiteral("Selected entities duplicated with new stable IDs. Undo is available."));
    });
    connect(mDeleteEntities, &QAction::triggered, this, [this]() {
        const QMessageBox::StandardButton answer =
            QMessageBox::question(this,
                                  QStringLiteral("Delete Entities"),
                                  QStringLiteral("Delete %1 selected entities? Undo can restore them. References from "
                                                 "surviving entities must be cleared first.")
                                      .arg(mSelectedEntities.size()),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
        {
            return;
        }
        QString error;
        if (Document_.RemoveEntities(mSelectedEntities, error) == false)
        {
            Changed(error);
            return;
        }
        Selected_.clear();
        mSelectedEntities.clear();
        Render();
        Snapshot();
        Changed(QStringLiteral("Selected entities deleted. Undo is available."));
    });
    connect(Attach_, &QPushButton::clicked, this, [this]() {
        QString error;
        if (Document_.Attach(Selected_, error, Components_->currentData().toString()) == false)
        {
            Changed(error);
            return;
        }
        Render();
        Snapshot();
        Changed(QStringLiteral("Component added with project defaults."));
    });
    connect(Apply_, &QPushButton::clicked, this, [this]() { (void)Apply(); });
    connect(Reset_, &QPushButton::clicked, this, [this]() {
        DraftTimer_.stop();
        Pending_ = false;
        PreviewDraft();
        Render();
        Changed(QStringLiteral("Fields reset to the last applied edit."));
    });
    connect(Recovery_, &QPushButton::clicked, this, [this]() {
        if (ConfirmDiscard())
        {
            (void)Recover();
        }
    });
#if defined(Q_OS_WASM)
    intro->setText(QStringLiteral("Browser Worlds live in this session's in-memory project; export the project to keep "
                                  "them. Import scene… copies a scene folder, .zip or .pbrt with its files into the "
                                  "session and cooks it here. Play and runtime binding require the native editor."));
#endif
    initializeTerrainAuthoring();
    bindViewportInteraction();
    DraftTimer_.setSingleShot(true);
    DraftTimer_.setInterval(180);
    connect(&DraftTimer_, &QTimer::timeout, this, &WorldWorkspace::PreviewDraft);
    Render();
    Changed(QStringLiteral("Open a project to begin authoring its world."));
}
void WorldWorkspace::SetProject(const QString& root, foundation::uint64 epoch)
{
    const auto canonical = root.isEmpty() ? QString() : QFileInfo(root).canonicalFilePath();
    if (Root_ == canonical && Epoch_ == epoch)
    {
        return;
    }
    DraftTimer_.stop();
    DraftPreview_ = false;
    StopPreview();
    Render_->Preview().RestoreNavigation({});
    Scene_->RestoreNavigation({});
    Scene_->SetProject(canonical, epoch);
    ClearImportState();
    invalidateScene();
    SceneError_.clear();
    Importing_ = false;
    SavePending_ = false;
    SaveValidated_ = false;
    ConversionPath_.clear();
    Root_ = canonical;
    Epoch_ = epoch;
    Path_.clear();
    Selected_.clear();
    mSelectedEntities.clear();
    Disk_.clear();
    SchemaDisk_.clear();
    Pending_ = false;
    Unsaved_ = false;
    ++DocumentGeneration_;
    Document_.Clear();
    Render_->SetWorld(Root_, &Document_);
    RecoveryFailed_ = false;
    Render();
    Changed(Root_.isEmpty() ? QStringLiteral("Open a project to begin.")
                            : QStringLiteral("Create or open a World, then import a scene or add gameplay entities."));
}
bool WorldWorkspace::Inside(const QString& path, bool existing) const
{
    if (Root_.isEmpty())
    {
        return false;
    }
    const QFileInfo info(path);
    const auto absolute = info.absoluteFilePath();
    if (absolute.startsWith(Root_ + QLatin1Char('/')) == false || info.isSymLink())
    {
        return false;
    }
    if (existing)
    {
        return info.isFile() && info.canonicalFilePath() == absolute;
    }
    const QFileInfo parent(info.absolutePath());
    return info.exists() == false && parent.isDir() && parent.canonicalFilePath() == parent.absoluteFilePath();
}
bool WorldWorkspace::LoadProjectSchema(WorldDocument& candidate, QByteArray& schema, bool worldDemo)
{
    const auto path = QDir(Root_).filePath(QStringLiteral("authoring/world.schema.json"));
    QString error;
    if (QFileInfo::exists(path) == false && QFileInfo(path).isSymLink() == false)
    {
        if (worldDemo)
        {
            QFile builtin(QStringLiteral(":/world-demo/world.schema.json"));
            if (builtin.open(QIODevice::ReadOnly) == false)
            {
                Changed(QStringLiteral("The World demo adapter schema is unavailable."));
                return false;
            }
            schema = builtin.readAll();
        }
        else
        {
            if (ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == false)
            {
                Changed(error);
                return false;
            }
        }
        return candidate.LoadSchema(schema, error);
    }
    if (Inside(path, true) == false || Read(path, schema, WorldDocument::MAX_SCHEMA_BYTES) == false)
    {
        Changed(QStringLiteral("Unreadable or unsafe authoring/world.schema.json. Repair the declared schema before "
                               "opening or creating a World."));
        return false;
    }
    if (candidate.LoadSchema(schema, error) == false)
    {
        Changed(QStringLiteral("Schema: ") + error);
        return false;
    }
    return true;
}
bool WorldWorkspace::Open(const QString& path)
{
    if (isSceneWorkerActive() == true)
    {
        Changed(QStringLiteral("Wait for the scene operation to finish before opening a World."));
        return false;
    }
    if (path.endsWith(QStringLiteral(".lulevel")))
    {
        if (Inside(path, true) == false)
        {
            return false;
        }
        const auto destination = path.chopped(8) + QStringLiteral(".world.json");
        WorldDocument candidate;
        QByteArray schema;
        QString error;
        if (Inside(destination, false) == false || LoadProjectSchema(candidate, schema) == false ||
            candidate.New(error) == false || ConfirmDiscard() == false)
        {
            Changed(QStringLiteral("Choose a level without an existing converted World document."));
            return false;
        }
        ConversionPath_ = destination;
        ConversionSchema_ = foundation::Move(schema);
        SceneGeneration_ = DocumentGeneration_;
        Importing_ = true;
        Scene_->SetWorldSnapshot(candidate);
        if (Scene_->Open(path) == false)
        {
            Importing_ = false;
            ConversionPath_.clear();
            return false;
        }
        Changed(QStringLiteral("Converting the level to a World document; the original level is preserved."));
        return true;
    }
    WorldDocument candidate;
    QByteArray schema, bytes;
    QString error;
    if (Inside(path, true) == false || Read(path, bytes, WorldDocument::MAX_BYTES) == false)
    {
        Changed(QStringLiteral("Open a regular world file inside the project's source folder (at most 512 KiB)."));
        return false;
    }
    const auto incoming = QJsonDocument::fromJson(bytes).object();
    const bool isLegacy =
        incoming.contains(QStringLiteral("schema")) == false && incoming.contains(QStringLiteral("settings"));
    const bool demo =
        isLegacy || incoming.value(QStringLiteral("schema")) == QJsonValue(QStringLiteral("ludus.world-demo.v1"));
    if (LoadProjectSchema(candidate, schema, demo) == false)
    {
        return false;
    }
    const QString destination = isLegacy ? QFileInfo(path).absolutePath() + QLatin1Char('/') +
                                               QFileInfo(path).completeBaseName() + QStringLiteral(".world.json")
                                         : path;
    if ((isLegacy && Inside(destination, false) == false) ||
        (isLegacy ? ImportWorldDemo(candidate, mEntropy, bytes, error) : candidate.Load(bytes, error)) == false)
    {
        Changed(error);
        return false;
    }
    if (ConfirmDiscard() == false)
    {
        return false;
    }
    // Save in the confirmation can change the same file being reopened.
    // Validate the current bytes again before replacing the live document.
    QByteArray currentSchema;
    WorldDocument currentDefinition;
    if (LoadProjectSchema(currentDefinition, currentSchema, demo) == false || currentSchema != schema ||
        Read(path, bytes, WorldDocument::MAX_BYTES) == false ||
        (isLegacy ? ImportWorldDemo(candidate, mEntropy, bytes, error) : candidate.Load(bytes, error)) == false)
    {
        Changed(QStringLiteral("Incoming world/schema changed while opening. Retry Open World."));
        return false;
    }
    RecoveryFailed_ = false;
    StopPreview();
    invalidateScene();
    SceneError_.clear();
    ++DocumentGeneration_;
    Document_ = foundation::Move(candidate);
    ClearImportState();
    Render_->SetWorld(Root_, &Document_);
    SchemaDisk_ = foundation::Move(schema);
    Disk_ = isLegacy ? QByteArray() : bytes;
    Path_ = QFileInfo(destination).absoluteFilePath();
    Selected_.clear();
    mSelectedEntities.clear();
    Pending_ = false;
    Unsaved_ = isLegacy;
    Render();
    RefreshScene();
    Changed(QStringLiteral("World opened. Select an entity or add one; recovery is restored only when requested."));
    return true;
}
bool WorldWorkspace::Create(const QString& path)
{
    if (isSceneWorkerActive() == true)
    {
        Changed(QStringLiteral("Wait for the scene operation to finish before creating a World."));
        return false;
    }
    if (Inside(path, false) == false)
    {
        Changed(QStringLiteral(
            "Choose a new file inside the project's source folder. Existing files are never replaced by New World."));
        return false;
    }
    WorldDocument candidate;
    QByteArray schema;
    QString error;
    if (LoadProjectSchema(candidate, schema, IsWorldDemo(Document_)) == false || candidate.New(error) == false)
    {
        if (error.isEmpty() == false)
        {
            Changed(error);
        }
        return false;
    }
    if (ConfirmDiscard() == false)
    {
        return false;
    }
    RecoveryFailed_ = false;
    StopPreview();
    invalidateScene();
    SceneError_.clear();
    ++DocumentGeneration_;
    Document_ = foundation::Move(candidate);
    ClearImportState();
    Render_->SetWorld(Root_, &Document_);
    SchemaDisk_ = foundation::Move(schema);
    Disk_.clear();
    Path_ = QFileInfo(path).absoluteFilePath();
    Selected_.clear();
    mSelectedEntities.clear();
    Pending_ = false;
    Unsaved_ = true;
    Render();
    Changed(QStringLiteral("New world. Add entities, then Save World to create the file."));
    return true;
}
void WorldWorkspace::SetEntropySource(const foundation::identity::EntropySource& entropy) noexcept
{
    mEntropy = entropy;
    Scene_->SetEntropySource(entropy);
}
bool WorldWorkspace::SceneBusy() const noexcept
{
    return Scene_ != nullptr && (Scene_->Busy() == true || mIsSceneRefreshDeferred == true);
}
bool WorldWorkspace::isSceneWorkerActive() const noexcept
{
    return SceneBusy() == true || (Scene_ != nullptr && Scene_->CanStartOperation() == false);
}
bool WorldWorkspace::ImportScene(const QString& source)
{
    if (Scene_->SetImportEnvironmentFile({}) == false)
    {
        return false;
    }
    return StartSceneImport(source, false);
}
bool WorldWorkspace::StartSceneImport(const QString& source, bool isPreviewed)
{
    if (Document_.Loaded() == false)
    {
        Changed(QStringLiteral("Create or open a World before importing a scene."));
        return false;
    }
    if (Pending_ == true)
    {
        Changed(QStringLiteral("Apply or reset the inspector edits before importing a scene."));
        return false;
    }
    if (isSceneWorkerActive() == true)
    {
        Changed(QStringLiteral("A scene operation is still running; wait for it or cancel it before importing."));
        return false;
    }
    if (Document_.HasUuidRecords() == true)
    {
        Changed(QStringLiteral("Version-2 World records do not support scene import yet; the World was preserved."));
        return false;
    }
    auto level = Document_.SceneLevel();
    if (level.isEmpty())
    {
        QString levelId;
        if (TryGenerateUuidText(mEntropy, levelId) == false)
        {
            Changed(DescribeEntropyUnavailable());
            return false;
        }
        level = QJsonObject{{QStringLiteral("version"), 1},
                            {QStringLiteral("id"), levelId},
                            {QStringLiteral("name"), QFileInfo(Path_).completeBaseName()},
                            {QStringLiteral("profile"), QStringLiteral("resident-reference-v1")},
                            {QStringLiteral("objects"), QJsonArray{}},
                            {QStringLiteral("imports"), QJsonArray{}},
                            {QStringLiteral("camera"),
                             QJsonObject{{QStringLiteral("eye"), QJsonArray{0, 0, 3}},
                                         {QStringLiteral("target"), QJsonArray{0, 0, 0}},
                                         {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                                         {QStringLiteral("fov"), 1.0471975512}}}};
    }
    SceneGeneration_ = DocumentGeneration_;
    Importing_ = true;
    Scene_->SetWorldSnapshot(Document_);
    const bool isStarted =
        isPreviewed == true ? Scene_->ImportPreviewedInto(level, source) : Scene_->ImportInto(level, source);
    if (isStarted == false)
    {
        Importing_ = false;
        const QString status = Scene_->StatusText();
        Changed(status.isEmpty() == false ? status : QStringLiteral("Could not start the scene import."));
        return false;
    }
    Changed(QStringLiteral("Importing scene assets into World…"));
    return true;
}
// Ordered scene and optional environment paths match the import request.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool WorldWorkspace::PreviewImportScene(const QString& source, const QString& environmentFile)
{
    if (Document_.Loaded() == false)
    {
        Changed(QStringLiteral("Create or open a World before importing a scene."));
        return false;
    }
    if (Pending_ == true)
    {
        Changed(QStringLiteral("Apply or reset the inspector edits before importing a scene."));
        return false;
    }
    if (isSceneWorkerActive() == true)
    {
        Changed(QStringLiteral("A scene operation is still running; wait for it or cancel it before importing."));
        return false;
    }
    // The cook defers scene source apertures only for a default World, so plan against this one.
    Scene_->SetWorldSnapshot(Document_);
    const QFileInfo sourceInfo(source);
    const QDir directory(sourceInfo.isDir() == true ? source : sourceInfo.absolutePath());
    const QString relativeEnvironment =
        environmentFile.isEmpty() == true ? QString{} : directory.relativeFilePath(environmentFile);
    if (Scene_->SetImportEnvironmentFile(relativeEnvironment) == false)
    {
        Changed(QStringLiteral("Wait for the current scene operation before choosing its environment."));
        return false;
    }
    PreviewedSource_.clear();
    ShowImportPreview(false);
    if (Scene_->PreviewImport(source) == false)
    {
        Changed(Scene_->StatusText());
        return false;
    }
    PreviewedSource_ = source;
    Changed(QStringLiteral("Planning the import (dry run)…"));
    return true;
}
bool WorldWorkspace::ConfirmImportPreview()
{
    const QString source = PreviewedSource_;
    if (source.isEmpty() == true || Scene_->ImportPreview().isEmpty() == true)
    {
        Changed(QStringLiteral("The import preview is no longer available; choose Import scene… again."));
        return false;
    }
    PreviewedSource_.clear();
    ShowImportPreview(false);
    return StartSceneImport(source, true);
}
void WorldWorkspace::ClearImportState()
{
    PreviewedSource_.clear();
    Scene_->ClearImportState();
    ShowImportPreview(false);
    ImportReport_->clear();
    ImportReport_->hide();
    mImportReportToggleOrNull->setChecked(false);
    mImportReportToggleOrNull->hide();
}
void WorldWorkspace::ShowImportPreview(bool isAccepted)
{
    const bool isShown = isAccepted == true && PreviewedSource_.isEmpty() == false;
    if (isAccepted == false && PreviewedSource_.isEmpty() == false)
    {
        PreviewedSource_.clear();
        Changed(Scene_->StatusText());
    }
    ImportPreview_->setPlainText(isShown == true ? DescribeImportPreview(Scene_->ImportPreview()) : QString());
    ImportPreview_->setVisible(isShown);
    ConfirmImport_->setVisible(isShown);
    mChooseImportEnvironmentOrNull->setVisible(isShown);
    mClearImportEnvironmentOrNull->setVisible(isShown);
    DiscardImport_->setVisible(isShown);
    if (isShown == true)
    {
        Changed(QStringLiteral("Import preview ready; review it, then import or discard."));
    }
}
void WorldWorkspace::ShowImportReports()
{
    const QJsonObject reports = Scene_->ImportReports();
    const QString text = DescribeImportReports(QJsonObject{{QStringLiteral("import_reports"), reports}});
    ImportReport_->setPlainText(text);
    mImportReportToggleOrNull->setVisible(text.isEmpty() == false);
    ImportReport_->setVisible(text.isEmpty() == false && mImportReportToggleOrNull->isChecked() == true);
}
void WorldWorkspace::invalidateScene()
{
    ValidatedScene_ = {};
    ValidatedWorld_.clear();
    mIsSceneRefreshDeferred = false;
    Scene_->ClearPlacementPresentation();
}
void WorldWorkspace::RefreshScene()
{
    const QJsonObject level = Document_.SceneLevel();
    if (level.isEmpty())
    {
        Scene_->StopPreview();
        invalidateScene();
        return;
    }
    if (SceneBusy() == true)
    {
        return;
    }
    // A placement-only edit is presented at once; its cook then validates it in the background.
    const PlacementPublication publication = Scene_->PublishPlacement(level, Document_);
    if (publication == PlacementPublication::Published)
    {
        mPlacementWorld = Document_.Encode();
        mPlacementDocumentGeneration = DocumentGeneration_;
        return;
    }
    if (publication == PlacementPublication::Unchanged ||
        (level == ValidatedScene_ && Document_.Encode() == ValidatedWorld_))
    {
        return;
    }
    // Switching imported lights changes no cooked asset: the presented scene is re-derived at once.
    const PlacementPublication switched = Scene_->PublishLightSwitches(level, Document_);
    if (switched == PlacementPublication::Published)
    {
        if (level == ValidatedScene_)
        {
            ValidatedWorld_ = Document_.Encode();
        }
        renderLights();
        return;
    }
    if (switched == PlacementPublication::Unchanged)
    {
        return;
    }
    if (Scene_->CanStartOperation() == false)
    {
        // The placement cook still occupies the operation thread: stop it and assemble once it drains.
        mIsSceneRefreshDeferred = true;
        Scene_->CancelPlacementValidation();
        return;
    }
    SceneGeneration_ = DocumentGeneration_;
    Scene_->SetWorldSnapshot(Document_);
    (void)Scene_->Assemble(level);
}
void WorldWorkspace::placementValidationFinished(PlacementValidationResult result)
{
    QString message;
    if (mIsSceneRefreshDeferred == true)
    {
        // The edit that deferred this refresh needs a full assembly, which supersedes the placement cook.
        mIsSceneRefreshDeferred = false;
        RefreshScene();
    }
    else if (result != PlacementValidationResult::Superseded && mPlacementDocumentGeneration == DocumentGeneration_)
    {
        if (result == PlacementValidationResult::Accepted)
        {
            SceneError_.clear();
            ValidatedScene_ = Scene_->Document();
            ValidatedWorld_ = mPlacementWorld;
            message = QStringLiteral("Scene assets validated.");
        }
        else
        {
            SceneError_ = Scene_->StatusText();
            message = SceneError_;
        }
    }
    Changed(message);
    // A save waits for the presented placement's validation; a newer placement's cook starts after this.
    if (SavePending_ == true && Scene_->Busy() == false &&
        Scene_->GetPlacementValidation() == PlacementValidation::Ready)
    {
        SavePending_ = false;
        (void)Save();
    }
}
void WorldWorkspace::SceneCompleted(bool accepted)
{
    if (DraftPreview_)
    {
        DraftPreview_ = false;
        // This packet represents only inspector buffers; Apply/Reset still own the document.
        if (accepted == false)
        {
            Changed(Scene_->StatusText());
        }
        else
        {
            Changed();
        }
        return;
    }

    if (SceneGeneration_ != DocumentGeneration_)
    {
        return;
    }
    const bool importing = Importing_;
    Importing_ = false;
    const auto conversion = ConversionPath_;
    ConversionPath_.clear();
    if (accepted == false)
    {
        SceneError_ = Scene_->StatusText();
        Render();
        // Without a previous frame to retain, show why in whichever viewport the World displays.
        if (Document_.Draft().contains(QStringLiteral("scene")) == false)
        {
            Render_->ShowSceneFailure(SceneError_);
        }
        else
        {
            Scene_->ShowSceneFailure(SceneError_);
        }
        Changed(SceneError_);
        if (SavePending_)
        {
            SavePending_ = false;
            (void)Save();
        }
        return;
    }
    SceneError_.clear();
    ShowImportReports();
    if (conversion.isEmpty() == false)
    {
        WorldDocument candidate;
        QString error;
        WorldDocument currentDefinition;
        QByteArray currentSchema;
        if (Inside(conversion, false) == false || LoadProjectSchema(currentDefinition, currentSchema) == false ||
            currentSchema != ConversionSchema_ || candidate.LoadSchema(ConversionSchema_, error) == false ||
            candidate.New(error) == false || candidate.ImportScene(Scene_->Document(), error) == false)
        {
            invalidateScene();
            RefreshScene();
            Changed(error.isEmpty() ? QStringLiteral("Conversion inputs changed; previous World retained.") : error);
            return;
        }
        Document_ = foundation::Move(candidate);
        SchemaDisk_ = ConversionSchema_;
        Disk_.clear();
        Path_ = conversion;
        Unsaved_ = true;
        Pending_ = false;
        Selected_.clear();
        mSelectedEntities.clear();
        ++DocumentGeneration_;
        Render_->SetWorld(Root_, &Document_);
    }
    ValidatedScene_ = Scene_->Document();
    if (importing && conversion.isEmpty())
    {
        QString error;
        if (Document_.ImportScene(Scene_->Document(), error, Scene_->ImportedEntities()) == false)
        {
            invalidateScene();
            RefreshScene();
            Changed(error);
            return;
        }
        Render();
        Snapshot();
    }
    ValidatedWorld_ = Document_.Encode();
    Render();
    Changed(importing ? QStringLiteral("Scene imported. Select its entities to edit placement and gameplay components.")
                      : QStringLiteral("Scene assets validated."));
    if (SavePending_)
    {
        SavePending_ = false;
        (void)Save();
    }
}
void WorldWorkspace::SceneVerified(bool accepted)
{
    if (SceneGeneration_ != DocumentGeneration_)
    {
        return;
    }
    const bool saving = SavePending_;
    SavePending_ = false;
    if (accepted == false)
    {
        Changed(Scene_->StatusText());
        return;
    }
    if (saving)
    {
        SaveValidated_ = true;
        (void)Save();
    }
}
void WorldWorkspace::Render()
{
    Rendering_ = true;
    Entities_->clear();
    for (const QJsonValue value : Document_.Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        QListWidgetItem* item = new QListWidgetItem(entity.value(QStringLiteral("name")).toString(), Entities_);
        const QString id = Document_.GetEntityKey(entity);
        const QString label = Document_.GetEntityLabel(entity);
        item->setData(Qt::UserRole, id);
        item->setToolTip(label == id ? id : label + QLatin1Char('\n') + id);
        if (mSelectedEntities.contains(id) == true)
        {
            item->setSelected(true);
        }
        if (id == Selected_)
        {
            Entities_->setCurrentItem(item, QItemSelectionModel::NoUpdate);
        }
    }
    mEntitySearch->Refresh();
    QStringList retained;
    for (const QListWidgetItem* item : Entities_->selectedItems())
    {
        retained.append(item->data(Qt::UserRole).toString());
    }
    mSelectedEntities = retained;
    if (mSelectedEntities.contains(Selected_) == false)
    {
        Selected_ = mSelectedEntities.value(0);
    }
    Rendering_ = false;
    RenderInspector();
    renderLights();
    const bool scene = Document_.Draft().contains(QStringLiteral("scene"));
    Viewport_->setVisible(scene);
    // World-demo Worlds render in the same bound viewport as every other scene-less World.
    BoundViewport_->setVisible(scene == false);
    Render_->setVisible(scene == false);
    const bool isPlayable = IsWorldDemo(Document_);
    PlayButton_->setVisible(isPlayable);
    // The box only configures Play saved World; without the button it would be an unexplained control.
    mPlayModeBox->setVisible(isPlayable);
    updateShownRenderPreview(scene);
}
void WorldWorkspace::renderLights()
{
    if (Document_.Loaded() == false)
    {
        mLightsPanel->SetRows({}, {});
        return;
    }
    // A scene-less World lists its authored lights from the bound viewport's extraction.
    const bool hasScene = Document_.Draft().contains(QStringLiteral("scene"));
    const RenderScene& scene = hasScene == true ? Scene_->PreviewScene() : Render_->Preview().Scene();
    mLightsPanel->SetRows(CollectWorldLightRows(Document_, scene), Document_.GetLightSwitches());
}
void WorldWorkspace::applyLightChange(const WorldLightChange& change, const QString& message)
{
    QString error;
    const bool hasScene = Document_.Draft().contains(QStringLiteral("scene"));
    const RenderScene& scene = hasScene == true ? Scene_->PreviewScene() : Render_->Preview().Scene();
    if (SceneBusy() == true || Pending_ == true ||
        Document_.SetLightSwitches(change.Switches, scene.LightBinding, change.WorldLights, error) == false)
    {
        renderLights();
        Changed(error.isEmpty() == true ? QStringLiteral("Lights cannot change while the scene is busy.") : error);
        return;
    }
    Render();
    Snapshot();
    Changed(message);
}
void WorldWorkspace::updateShownRenderPreview(bool isSceneShown)
{
    RenderPreview* shown = isSceneShown == true ? &Scene_->GetRenderPreview() : &Render_->Preview();
    shown->SetSurfaceBakeWorldPath(Path_);
    if (shown != mShownRenderPreviewOrNull)
    {
        mShownRenderPreviewOrNull = shown;
        Q_EMIT RenderPreviewChanged();
    }
}
void WorldWorkspace::addMaterialGroup()
{
    QGroupBox* group = new QGroupBox(QStringLiteral("Material"));
    group->setObjectName(QStringLiteral("worldMaterial"));
    QFormLayout* rows = new QFormLayout(group);
    const RenderScene& presented = Scene_->PreviewScene();
    const resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(presented, Selected_);
    const QJsonObject scene = Document_.Entity(Selected_).value(QStringLiteral("scene")).toObject();
    const QJsonObject localCoating = scene.value(QStringLiteral("coating")).toObject();
    const QJsonObject sharedCoating = Document_.GetSharedCoating(Document_.GetInheritedMaterialId(scene));
    const QJsonObject coating = localCoating.isEmpty() == true ? sharedCoating : localCoating;
    const bool canEditCoating = materialOrNull != nullptr &&
                                materialOrNull->Scattering.Model == content::scene_format::MaterialModel::CoatedDiffuse;
    QVector<MaterialPropertyRow> properties;
    if (DescribeSceneObjectMaterial(presented, Selected_, properties) == false)
    {
        QLabel* pending =
            new QLabel(QStringLiteral("Material values appear once the viewport has loaded the scene."), group);
        pending->setWordWrap(true);
        rows->addRow(pending);
    }
    for (const MaterialPropertyRow& property : properties)
    {
        if (canEditCoating == true && property.IsCoatingParameter == true)
        {
            continue;
        }
        QLabel* value = new QLabel(property.Value, group);
        value->setObjectName(QStringLiteral("worldMaterial:") + property.Label);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (property.Swatch.isValid() == false && property.Thumbnail.isNull() == true)
        {
            rows->addRow(property.Label, value);
            continue;
        }
        // A colour or texture row leads with what it looks like, then its numbers.
        QWidget* row = new QWidget(group);
        QHBoxLayout* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        if (property.Swatch.isValid() == true)
        {
            rowLayout->addWidget(new ColourChip(property.Swatch, row));
        }
        if (property.Thumbnail.isNull() == false)
        {
            QLabel* thumbnail = new QLabel(row);
            thumbnail->setObjectName(QStringLiteral("worldMaterialThumbnail:") + property.Label);
            thumbnail->setPixmap(QPixmap::fromImage(property.Thumbnail));
            thumbnail->setAccessibleName(property.Label + QStringLiteral(" preview"));
            rowLayout->addWidget(thumbnail);
        }
        rowLayout->addWidget(value, 1);
        rows->addRow(property.Label, row);
    }
    if (materialOrNull != nullptr &&
        materialOrNull->Scattering.Model == content::scene_format::MaterialModel::Unresolved)
    {
        addMaterialRepair(*rows);
    }
    if (canEditCoating == true)
    {
        QLabel* help = new QLabel(
            localCoating.isEmpty() == true
                ? QStringLiteral("This slot inherits its coating. Apply Changes creates a slot-only instance. "
                                 "Apply to Shared Coating edits the inherited instance for all its slots.")
                : QStringLiteral("This slot has its own coating instance. Use Inherited Coating to rejoin "
                                 "the shared instance."),
            group);
        help->setWordWrap(true);
        rows->addRow(help);
        constexpr foundation::int32 FLOAT32_ROUND_TRIP_DIGITS = 9;
        for (const CoatingField& property : COATING_FIELDS)
        {
            const QString key = QString::fromLatin1(property.Key);
            const QJsonValue initial =
                coating.contains(key) == true
                    ? coating.value(key)
                    : QJsonValue(static_cast<foundation::float64>(materialOrNull->Scattering.*property.Member));
            QLineEdit* field =
                new QLineEdit(QString::number(initial.toDouble(), 'g', FLOAT32_ROUND_TRIP_DIGITS), group);
            const QString fieldKey = QStringLiteral("scene/coating/") + key;
            field->setObjectName(QStringLiteral("worldField:") + fieldKey);
            field->setProperty("initialCoatingValue", initial.toDouble());
            field->setProperty("initialCoatingText", field->text());
            Fields_.insert(fieldKey, field);
            connect(field, &QLineEdit::textEdited, this, [this]() { markInspectorPending(false); });
            rows->addRow(QString::fromLatin1(property.Label), field);
        }
        if (localCoating.isEmpty() == true)
        {
            QPushButton* shared = new QPushButton(QStringLiteral("Apply to Shared Coating"), group);
            shared->setObjectName(QStringLiteral("worldApplySharedCoating"));
            shared->setEnabled(Pending_ == true && SceneBusy() == false);
            connect(shared, &QPushButton::clicked, this, &WorldWorkspace::applySharedCoating);
            rows->addRow(shared);
        }
    }
    if (localCoating.isEmpty() == false && scene.contains(QStringLiteral("project_brdf")) == false)
    {
        QPushButton* restore = new QPushButton(QStringLiteral("Use Inherited Coating"), group);
        restore->setObjectName(QStringLiteral("worldRestoreCoating"));
        connect(restore, &QPushButton::clicked, this, &WorldWorkspace::restoreInheritedCoating);
        rows->addRow(restore);
    }
    addProjectBrdfControls(*rows, materialOrNull, scene);
    Inspector_->addWidget(group);
}

void WorldWorkspace::addProjectBrdfControls(QFormLayout& inoutRows,
                                            const resources::SceneMaterial* materialOrNull,
                                            const QJsonObject& placement)
{
    using content::scene_format::MaterialModel;
    QWidget* group = inoutRows.parentWidget();
    const QJsonObject binding = placement.value(QStringLiteral("project_brdf")).toObject();
    const bool isProjectBrdf =
        materialOrNull != nullptr && materialOrNull->Scattering.Model == MaterialModel::ProjectBrdf;
    if (isProjectBrdf == true)
    {
        const QJsonValue initial = binding.value(QStringLiteral("roughness"));
        constexpr foundation::int32 FLOAT32_ROUND_TRIP_DIGITS = 9;
        const QString text =
            QString::number(initial.toDouble(static_cast<foundation::float64>(materialOrNull->Roughness)),
                            'g',
                            FLOAT32_ROUND_TRIP_DIGITS);
        QLineEdit* field = new QLineEdit(text, group);
        const QString key = QStringLiteral("scene/project_brdf/roughness");
        field->setObjectName(QStringLiteral("worldField:") + key);
        field->setProperty("initialRoughnessText", text);
        Fields_.insert(key, field);
        connect(field, &QLineEdit::textEdited, this, [this]() { markInspectorPending(false); });
        inoutRows.addRow(QStringLiteral("Project BRDF roughness"), field);
    }
    if (binding.isEmpty() == false)
    {
        QPushButton* restore = new QPushButton(QStringLiteral("Use Inherited Material"), group);
        restore->setObjectName(QStringLiteral("worldRestoreProjectBrdf"));
        connect(restore, &QPushButton::clicked, this, [this]() { assignProjectBrdf({}); });
        inoutRows.addRow(restore);
    }
    const RenderScene& scene = Scene_->PreviewScene();
    if (scene.Definition == nullptr)
    {
        return;
    }
    QHash<QString, QString> bindings;
    const QJsonArray entities = Document_.Draft().value(QStringLiteral("entities")).toArray();
    for (const QJsonValue value : entities)
    {
        const QJsonObject entity = value.toObject();
        const QJsonObject slot = entity.value(QStringLiteral("scene")).toObject();
        if (slot.isEmpty() == false)
        {
            const QJsonObject assigned = slot.value(QStringLiteral("project_brdf")).toObject();
            bindings.insert(
                entity.value(QStringLiteral("id")).toString(),
                assigned.value(QStringLiteral("material")).toString(Document_.GetInheritedMaterialId(slot)));
        }
    }
    QComboBox* choices = new QComboBox(group);
    choices->setObjectName(QStringLiteral("worldProjectBrdfChoice"));
    choices->setAccessibleName(QStringLiteral("Project BRDF"));
    const QString current =
        binding.value(QStringLiteral("material")).toString(Document_.GetInheritedMaterialId(placement));
    QSet<QString> listed;
    for (const PreviewObject& object : scene.Objects)
    {
        const QString identity = bindings.value(object.Id);
        if (identity.isEmpty() == true || listed.contains(identity) == true ||
            (isProjectBrdf == true && current == identity) || object.Surface < 0 ||
            static_cast<foundation::usize>(object.Surface) >= scene.Definition->Materials.GetSize())
        {
            continue;
        }
        const resources::SceneMaterial& material =
            scene.Definition->Materials[static_cast<foundation::usize>(object.Surface)];
        if (material.Scattering.Model != MaterialModel::ProjectBrdf)
        {
            continue;
        }
        const std::string_view name = material.Name.View();
        const QString display =
            QStringLiteral("%1 (%2)").arg(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())),
                                          object.Name);
        choices->addItem(display, identity);
        listed.insert(identity);
    }
    if (choices->count() > 0)
    {
        inoutRows.addRow(QStringLiteral("Project BRDF"), choices);
        QPushButton* assign = new QPushButton(QStringLiteral("Assign Project BRDF"), group);
        assign->setObjectName(QStringLiteral("worldAssignProjectBrdf"));
        connect(assign, &QPushButton::clicked, this, [this, choices]() {
            assignProjectBrdf(choices->currentData().toString());
        });
        inoutRows.addRow(assign);
    }
    else
    {
        delete choices;
    }
}

void WorldWorkspace::assignProjectBrdf(const QString& material)
{
    if (Pending_ == true || SceneBusy() == true)
    {
        return;
    }
    const QJsonObject entity = Document_.Entity(Selected_);
    QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
    if (material.isEmpty() == true)
    {
        scene.remove(QStringLiteral("project_brdf"));
    }
    else
    {
        scene.insert(QStringLiteral("project_brdf"), QJsonObject{{QStringLiteral("material"), material}});
    }
    QString error;
    if (Document_.Edit(Selected_,
                       entity.value(QStringLiteral("components")).toObject(),
                       entity.value(QStringLiteral("name")).toString(),
                       error,
                       &scene) == false)
    {
        Changed(error);
        return;
    }
    Snapshot();
    Changed(material.isEmpty() == true ? QStringLiteral("Inherited material restored. Undo is available.")
                                       : QStringLiteral("Project BRDF assigned to this slot. Undo is available."));
}

void WorldWorkspace::addMaterialRepair(QFormLayout& inoutRows)
{
    QWidget* group = inoutRows.parentWidget();
    QLabel* explanation = new QLabel(
        QStringLiteral("This imported PBRT material has no Ludus shading implementation. Map it to a material "
                       "already assigned to a supported slot in this World."),
        group);
    explanation->setWordWrap(true);
    inoutRows.addRow(explanation);
    const QString source =
        Document_.ScenePlacement(Document_.Entity(Selected_)).value(QStringLiteral("material")).toString();
    const QJsonObject mappings = Document_.Draft()
                                     .value(QStringLiteral("scene"))
                                     .toObject()
                                     .value(QStringLiteral("material_mappings"))
                                     .toObject();
    QComboBox* replacements = new QComboBox(group);
    replacements->setObjectName(QStringLiteral("worldMaterialReplacement"));
    replacements->setAccessibleName(QStringLiteral("Replacement material"));
    const RenderScene& scene = Scene_->PreviewScene();
    QHash<QString, QString> bindings;
    const QJsonArray entities = Document_.Draft().value(QStringLiteral("entities")).toArray();
    for (const QJsonValue value : entities)
    {
        const QJsonObject entity = value.toObject();
        const QJsonObject placement = entity.value(QStringLiteral("scene")).toObject();
        if (placement.isEmpty() == false && placement.contains(QStringLiteral("project_brdf")) == false)
        {
            bindings.insert(entity.value(QStringLiteral("id")).toString(),
                            placement.value(QStringLiteral("material")).toString());
        }
    }
    QSet<QString> listed;
    for (const PreviewObject& object : scene.Objects)
    {
        const QString identity = bindings.value(object.Id);
        if (identity.isEmpty() == true || identity == source || mappings.contains(identity) == true ||
            listed.contains(identity) == true)
        {
            continue;
        }
        if (scene.Definition == nullptr || object.Surface < 0 ||
            static_cast<foundation::usize>(object.Surface) >= scene.Definition->Materials.GetSize())
        {
            continue;
        }
        const resources::SceneMaterial& material =
            scene.Definition->Materials[static_cast<foundation::usize>(object.Surface)];
        if (material.Scattering.Model == content::scene_format::MaterialModel::Unresolved)
        {
            continue;
        }
        const std::string_view name = material.Name.View();
        const QString display =
            name.empty() == true
                ? object.Name
                : QStringLiteral("%1 (%2)").arg(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())),
                                                object.Name);
        replacements->addItem(display, identity);
        listed.insert(identity);
    }
    if (replacements->count() == 0)
    {
        replacements->addItem(QStringLiteral("Import a supported material to map this slot."));
        replacements->setEnabled(false);
    }
    inoutRows.addRow(QStringLiteral("Replacement"), replacements);
    QPushButton* map = new QPushButton(QStringLiteral("Map to existing material"), group);
    map->setObjectName(QStringLiteral("worldMapMaterial"));
    map->setEnabled(replacements->isEnabled() == true && Pending_ == false && SceneBusy() == false);
    connect(map, &QPushButton::clicked, this, [this, replacements, source]() {
        mapImportedMaterial(source, replacements->currentData().toString(), replacements->currentText());
    });
    inoutRows.addRow(map);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the source, replacement path and label are distinct.
void WorldWorkspace::mapImportedMaterial(const QString& source, const QString& replacement, const QString& label)
{
    if (Pending_ == true || SceneBusy() == true || source.isEmpty() == true || replacement.isEmpty() == true)
    {
        Changed(QStringLiteral("Finish the current World edit before mapping a material."));
        return;
    }
    QStringList affected;
    for (const QJsonValue value : Document_.Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        const QJsonObject placement = entity.value(QStringLiteral("scene")).toObject();
        if (placement.value(QStringLiteral("material")).toString() == source)
        {
            affected.append(entity.value(QStringLiteral("name")).toString() + QStringLiteral(" · ") +
                            entity.value(QStringLiteral("id")).toString());
        }
    }
    const QByteArray before = Document_.Encode();
    QMessageBox confirmation(QMessageBox::Question,
                             QStringLiteral("Map shared material"),
                             QStringLiteral("Use %1 for %2 World slots and for future imports of this source material?")
                                 .arg(label)
                                 .arg(affected.size()),
                             QMessageBox::Cancel,
                             this);
    confirmation.setTextFormat(Qt::PlainText);
    constexpr qsizetype INLINE_AFFECTED_SLOT_LIMIT = 6;
    const QString affectedSlots = affected.join(QLatin1Char('\n'));
    if (affected.size() <= INLINE_AFFECTED_SLOT_LIMIT)
    {
        confirmation.setInformativeText(affectedSlots);
    }
    else
    {
        confirmation.setDetailedText(affectedSlots);
    }
    QPushButton* apply = confirmation.addButton(QStringLiteral("Map and apply"), QMessageBox::AcceptRole);
    apply->setObjectName(QStringLiteral("worldConfirmMaterialMapping"));
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != apply)
    {
        return;
    }
    if (Document_.Encode() != before || Pending_ == true || SceneBusy() == true)
    {
        Changed(QStringLiteral("The World changed while confirming. Review the affected slots and map again."));
        return;
    }
    QString error;
    if (Document_.MapSceneMaterial(source, replacement, error) == false)
    {
        Changed(error);
        return;
    }
    Snapshot();
    Changed(QStringLiteral("Material mapped for %1 slots. Undo is available.").arg(affected.size()));
}

void WorldWorkspace::restoreInheritedCoating()
{
    if (Pending_ == true || SceneBusy() == true)
    {
        return;
    }
    const QJsonObject entity = Document_.Entity(Selected_);
    QJsonObject scene = entity.value(QStringLiteral("scene")).toObject();
    if (scene.contains(QStringLiteral("coating")) == false)
    {
        return;
    }
    scene.remove(QStringLiteral("coating"));
    QString error;
    if (Document_.Edit(Selected_,
                       entity.value(QStringLiteral("components")).toObject(),
                       entity.value(QStringLiteral("name")).toString(),
                       error,
                       &scene) == false)
    {
        Changed(error);
        return;
    }
    Snapshot();
    Changed(QStringLiteral("Inherited coating restored to this slot. Undo is available."));
}

void WorldWorkspace::applySharedCoating()
{
    if (Pending_ == false || SceneBusy() == true)
    {
        return;
    }
    const QJsonObject placement = Document_.Entity(Selected_).value(QStringLiteral("scene")).toObject();
    const QString material = Document_.GetInheritedMaterialId(placement);
    if (material.isEmpty() == true || placement.contains(QStringLiteral("coating")) == true)
    {
        return;
    }
    WorldDocument candidate = Document_;
    QString error;
    QWidget* invalidOrNull = nullptr;
    QJsonObject edit;
    if (readCoatingFields(edit, error, invalidOrNull) == false ||
        readDraft(candidate, error, invalidOrNull, false) == false)
    {
        if (invalidOrNull != nullptr)
        {
            invalidOrNull->setFocus();
        }
        Changed(error);
        return;
    }
    const QJsonObject coating = edit.value(QStringLiteral("coating")).toObject();
    if (coating.isEmpty() == true || candidate.Draft() != Document_.Draft())
    {
        Changed(QStringLiteral("Change coating values only, or apply or reset your other edits first."));
        return;
    }

    QStringList affected;
    for (const QJsonValue value : Document_.Draft().value(QStringLiteral("entities")).toArray())
    {
        const QJsonObject entity = value.toObject();
        const QJsonObject slot = entity.value(QStringLiteral("scene")).toObject();
        if (Document_.GetInheritedMaterialId(slot) == material && slot.contains(QStringLiteral("coating")) == false &&
            slot.contains(QStringLiteral("project_brdf")) == false)
        {
            affected.append(entity.value(QStringLiteral("name")).toString() + QStringLiteral(" · ") +
                            entity.value(QStringLiteral("id")).toString());
        }
    }
    const QJsonObject before = Document_.Draft();
    QMessageBox confirmation(
        QMessageBox::Question,
        QStringLiteral("Edit shared coating"),
        QStringLiteral("Apply these values to %1 inherited slots and future imports of this material?")
            .arg(affected.size()),
        QMessageBox::Cancel,
        this);
    confirmation.setTextFormat(Qt::PlainText);
    constexpr qsizetype INLINE_SLOT_LIMIT = 6;
    const QString affectedSlots = affected.join(QLatin1Char('\n'));
    if (affected.size() <= INLINE_SLOT_LIMIT)
    {
        confirmation.setInformativeText(affectedSlots);
    }
    else
    {
        confirmation.setDetailedText(affectedSlots);
    }
    QPushButton* apply = confirmation.addButton(QStringLiteral("Apply to Shared Coating"), QMessageBox::AcceptRole);
    apply->setObjectName(QStringLiteral("worldConfirmSharedCoating"));
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != apply)
    {
        return;
    }
    if (Document_.Draft() != before || SceneBusy() == true)
    {
        Changed(QStringLiteral("The World changed while confirming. Review the slots and apply again."));
        return;
    }
    if (Document_.SetSharedCoating(material, coating, error) == false)
    {
        Changed(error);
        return;
    }
    DraftTimer_.stop();
    Pending_ = false;
    Snapshot();
    Changed(QStringLiteral("Shared coating applied to %1 slots. Undo is available.").arg(affected.size()));
}

void WorldWorkspace::RenderInspector()
{
    Scene_->SetSelection(Selected_);
    Render_->SetSelection(Selected_, Pending_);
    Fields_.clear();
    Name_ = nullptr;
    while (auto* item = Inspector_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    Components_->clear();
    if (mSelectedEntities.size() > 1)
    {
        QLabel* summary = new QLabel(QStringLiteral("%1 entities selected. Duplicate or Delete acts on the whole "
                                                    "selection. Select one entity to edit its properties.")
                                         .arg(mSelectedEntities.size()));
        summary->setWordWrap(true);
        Inspector_->addWidget(summary);
        Inspector_->addStretch();
        return;
    }
    const auto entity = Document_.Entity(Selected_);
    if (entity.isEmpty())
    {
        if (Document_.Loaded() == false)
        {
            StateView* empty = new StateView;
            StateViewContent state;
            state.Icon = EditorIcon::World;
            state.Message = QStringLiteral("No world open.");
            state.ShortcutHint = QStringLiteral("Open a world from the Project Explorer.");
            empty->SetContent(state);
            Inspector_->addWidget(empty);
            Inspector_->addStretch();
            return;
        }
        auto* hint = new QLabel(
            QStringLiteral("Select an entity. Add Entity starts with the components your project requires."));
        hint->setTextFormat(Qt::PlainText);
        hint->setWordWrap(true);
        Inspector_->addWidget(hint);
        Inspector_->addStretch();
        return;
    }
    auto* form = new QWidget;
    auto* nameLayout = new QFormLayout(form);
    Name_ = new QLineEdit(entity.value(QStringLiteral("name")).toString(), form);
    Name_->setObjectName(QStringLiteral("worldEntityName"));
    Name_->setAccessibleName(QStringLiteral("Entity name"));
    nameLayout->addRow(QStringLiteral("Name"), Name_);
    Inspector_->addWidget(form);
    connect(Name_, &QLineEdit::textEdited, this, [this]() { markInspectorPending(); });
    if (Document_.HasUuidRecords() == true)
    {
        QLabel* label = new QLabel(QStringLiteral("Label: %1").arg(Document_.GetEntityLabel(entity)));
        label->setObjectName(QStringLiteral("worldEntityLabel"));
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        Inspector_->addWidget(label);
    }
    // A widget, not a bare layout, so RenderInspector's clean-up deletes the rows with it.
    QWidget* identifierBlock = new QWidget;
    QFormLayout* identifierForm = new QFormLayout(identifierBlock);
    identifierForm->setContentsMargins(0, 0, 0, 0);
    IdentifierRow* identifier = new IdentifierRow(Selected_, nullptr);
    identifier->setObjectName(QStringLiteral("worldEntityIdentifier"));
    identifierForm->addRow(Document_.HasUuidRecords() == true ? QStringLiteral("UUID") : QStringLiteral("Stable ID"),
                           identifier);
    Inspector_->addWidget(identifierBlock);
    if (entity.value(QStringLiteral("scene")).isObject())
    {
        auto* group = new QGroupBox(QStringLiteral("Scene instance"));
        auto* rows = new QFormLayout(group);
        const auto scene = entity.value(QStringLiteral("scene")).toObject();
        auto* help =
            new QLabel(QStringLiteral("Position is local to the parent. Rotation (X, Y, Z) and scale adjust the "
                                      "imported basis; zero rotation and unit scale preserve the source geometry."),
                       group);
        help->setWordWrap(true);
        rows->addRow(help);
        if (Document_.Schema().contains(QStringLiteral("scene_transform")) == false)
        {
            struct SceneVector final
            {
                QString Key;
                QString Title;
                QString Unit;
                QString StepUnit;
                foundation::float64 Minimum;
                foundation::float64 Maximum;
            };
            const QVector<SceneVector> vectors{{QStringLiteral("position"),
                                                QStringLiteral("Position"),
                                                QStringLiteral("m"),
                                                QStringLiteral("m"),
                                                -SCENE_POSITION_LIMIT,
                                                SCENE_POSITION_LIMIT},
                                               {QStringLiteral("rotation"),
                                                QStringLiteral("Rotation"),
                                                QStringLiteral("°"),
                                                QStringLiteral("°"),
                                                -SCENE_ROTATION_LIMIT,
                                                SCENE_ROTATION_LIMIT},
                                               {QStringLiteral("scale"),
                                                QStringLiteral("Scale"),
                                                QString(),
                                                QStringLiteral("×"),
                                                SCENE_SCALE_MINIMUM,
                                                SCENE_POSITION_LIMIT}};
            for (const SceneVector& vector : vectors)
            {
                const QJsonArray values = scene.value(vector.Key).toArray();
                QVector<VectorComponent> components;
                for (foundation::int32 axis = 0; axis < 3; ++axis)
                {
                    const QString fieldKey = QStringLiteral("scene/") + vector.Key + QString::number(axis);
                    auto* field = new QLineEdit(Display(values[axis]), group);
                    field->setObjectName(QStringLiteral("worldField:") + fieldKey);
                    field->setAccessibleName(vector.Title + QStringLiteral(" ") + QString(QChar('X' + axis)));
                    Fields_.insert(fieldKey, field);
                    connect(field, &QLineEdit::textEdited, this, [this]() { markInspectorPending(); });
                    components.append(
                    {
                        .Name = QString(QChar('X' + axis)),
                        .FieldOrNull = field,
                        .Settings =
                        {
                            .Minimum = vector.Minimum,
                            .Maximum = vector.Maximum,
                            .Step = GetScrubStep(vector.StepUnit, vector.Minimum, vector.Maximum),
                        },
                    });
                }
                rows->addRow(vector.Title,
                             MakeVectorRow(components, vector.Unit, group, [this]() { markInspectorPending(); }));
            }
        }
        rows->addRow(QStringLiteral("Mesh"), new IdentifierRow(scene.value(QStringLiteral("mesh")).toString(), group));
        rows->addRow(QStringLiteral("Material"),
                     new IdentifierRow(scene.value(QStringLiteral("material")).toString(), group));
        Inspector_->addWidget(group);
        addMaterialGroup();
    }
    const auto attached = entity.value(QStringLiteral("components")).toObject();
    for (const auto definition : Document_.Schema().value(QStringLiteral("components")).toArray())
    {
        const auto component = definition.toObject();
        const auto cid = component.value(QStringLiteral("id")).toString();
        const auto label = component.value(QStringLiteral("label")).toString();
        if (Document_.HasBuiltInTerrain() == true && cid == GetTerrainComponentId())
        {
            if (attached.contains(cid) == true)
            {
                QGroupBox* group = new QGroupBox(label);
                QVBoxLayout* layout = new QVBoxLayout(group);
                QPushButton* editGeneration = new QPushButton(QStringLiteral("Edit Terrain…"), group);
                editGeneration->setObjectName(QStringLiteral("worldEditTerrainGeneration"));
                layout->addWidget(editGeneration);
                connect(editGeneration, &QPushButton::clicked, this, [this, key = Selected_]() {
                    showTerrainAuthoring(key);
                });
                Inspector_->addWidget(group);
            }
            continue;
        }
        if (attached.contains(cid) == false)
        {
            Components_->addItem(label, cid);
            continue;
        }
        auto* group = new QGroupBox(label);
        auto* rows = new QFormLayout(group);
        const auto help = component.value(QStringLiteral("help")).toString();
        auto* hint = new QLabel(help, group);
        hint->setTextFormat(Qt::PlainText);
        hint->setWordWrap(true);
        const auto height = hint->heightForWidth(360);
        if (height > 0)
        {
            hint->setMinimumHeight(height);
        }
        rows->addRow(hint);
        const auto values = attached.value(cid).toObject();
        // Three numbers that belong together (a transform's position, rotation or scale, or a colour's red,
        // green and blue) share one compact row; their fields keep the names and signals they always had.
        struct VectorGroup final
        {
            QString Title;
            QString Unit;
            QString StepUnit;
            QStringList Members;
            QVector<VectorComponent> Components;
            QVector<QString> AxisNames;
            bool IsColour = false;
        };
        QVector<VectorGroup> vectorGroups;
        const QJsonObject binding = Document_.Schema().value(QStringLiteral("scene_transform")).toObject();
        if (binding.value(QStringLiteral("component")).toString() == cid)
        {
            const QVector<QPair<QString, QPair<QString, QString>>> kinds{
                {QStringLiteral("position"), {QStringLiteral("Position"), QStringLiteral("m")}},
                {QStringLiteral("rotation"), {QStringLiteral("Rotation"), QStringLiteral("°")}},
                {QStringLiteral("scale"), {QStringLiteral("Scale"), QStringLiteral("×")}}};
            for (const QPair<QString, QPair<QString, QString>>& kind : kinds)
            {
                const QJsonArray ids = binding.value(kind.first).toArray();
                if (ids.size() != VECTOR_COMPONENT_COUNT)
                {
                    continue;
                }
                vectorGroups.append(
                {
                    .Title = kind.second.first,
                    .Unit = kind.first == QStringLiteral("scale") ? QString() : kind.second.second,
                    .StepUnit = kind.second.second,
                    .Members = {ids[0].toString(), ids[1].toString(), ids[2].toString()},
                    .Components = {},
                    .AxisNames = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")},
                });
            }
        }
        const QStringList colourMembers{QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")};
        bool hasColour = true;
        for (const QString& member : colourMembers)
        {
            bool hasMember = false;
            for (const QJsonValue candidate : component.value(QStringLiteral("properties")).toArray())
            {
                hasMember = hasMember == true ||
                            (candidate.toObject().value(QStringLiteral("id")).toString() == member &&
                             candidate.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("number"));
            }
            hasColour = hasColour == true && hasMember == true;
        }
        if (hasColour == true)
        {
            vectorGroups.append(
            {
                .Title = QStringLiteral("Colour"),
                .Unit = QString(),
                .StepUnit = QString(),
                .Members = colourMembers,
                .Components = {},
                .AxisNames = {QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B")},
                .IsColour = true,
            });
        }
        for (const auto value : component.value(QStringLiteral("properties")).toArray())
        {
            const auto p = value.toObject();
            const auto pid = p.value(QStringLiteral("id")).toString();
            const auto type = p.value(QStringLiteral("type")).toString();
            const QJsonValue authored = values.value(pid);
            const QJsonValue current = authored.isUndefined() == true ? p.value(QStringLiteral("default")) : authored;
            QWidget* field = nullptr;
            if (type == QStringLiteral("boolean"))
            {
                auto* check = new QCheckBox(group);
                check->setChecked(current.toBool());
                connect(check, &QCheckBox::toggled, this, [this]() { markInspectorPending(); });
                field = check;
            }
            else if (type == QStringLiteral("enum") || type == QStringLiteral("entity") ||
                     type == QStringLiteral("asset"))
            {
                auto* choices = new QComboBox(group);
                choices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                choices->setMinimumContentsLength(12);
                if (type == QStringLiteral("enum"))
                {
                    for (const auto choice : p.value(QStringLiteral("choices")).toArray())
                    {
                        choices->addItem(choice.toString(), choice.toString());
                    }
                }
                else
                {
                    choices->addItem(QStringLiteral("None"), QString());
                    const auto candidates = type == QStringLiteral("entity")
                                                ? Document_.Draft().value(QStringLiteral("entities")).toArray()
                                                : Document_.Schema().value(QStringLiteral("assets")).toArray();
                    for (const auto candidate : candidates)
                    {
                        const auto entry = candidate.toObject();
                        if (type == QStringLiteral("asset") &&
                            entry.value(QStringLiteral("kind")) != p.value(QStringLiteral("asset_kind")))
                        {
                            continue;
                        }
                        const bool isEntity = type == QStringLiteral("entity");
                        // Entity references store the entity key (a version-2 UUID) and show its label.
                        const QString key = isEntity == true ? Document_.GetEntityKey(entry)
                                                             : entry.value(QStringLiteral("id")).toString();
                        const QString shown = isEntity == true ? Document_.GetEntityLabel(entry) : key;
                        choices->addItem(
                            entry.value(isEntity == true ? QStringLiteral("name") : QStringLiteral("label"))
                                    .toString() +
                                QStringLiteral(" — ") + shown,
                            key);
                    }
                }
                choices->setCurrentIndex(choices->findData(current.toString()));
                connect(choices, &QComboBox::currentIndexChanged, this, [this]() { markInspectorPending(); });
                field = choices;
            }
            else
            {
                auto* text = new QLineEdit(Display(current), group);
                connect(text, &QLineEdit::textEdited, this, [this]() { markInspectorPending(); });
                field = text;
            }
            field->setObjectName(QStringLiteral("worldField:") + FieldKey(cid, pid));
            field->setProperty("worldType", type);
            field->setAccessibleName(p.value(QStringLiteral("label")).toString());
            QString tooltip = p.value(QStringLiteral("help")).toString();
            if (type == QStringLiteral("number") || type == QStringLiteral("integer"))
            {
                tooltip += QStringLiteral("\nRange: %1 to %2")
                               .arg(Display(p.value(QStringLiteral("min"))), Display(p.value(QStringLiteral("max"))));
            }
            field->setToolTip(tooltip);
            auto title = p.value(QStringLiteral("label")).toString();
            const auto unit = p.value(QStringLiteral("unit")).toString();
            if (unit.isEmpty() == false)
            {
                title += QStringLiteral(" (%1)").arg(unit);
            }
            Fields_.insert(FieldKey(cid, pid), field);
            QLineEdit* textFieldOrNull = qobject_cast<QLineEdit*>(field);
            VectorGroup* groupOrNull = nullptr;
            for (VectorGroup& candidate : vectorGroups)
            {
                if (textFieldOrNull != nullptr && candidate.Members.contains(pid) == true)
                {
                    groupOrNull = &candidate;
                }
            }
            if (groupOrNull == nullptr)
            {
                rows->addRow(title, field);
                continue;
            }
            const foundation::float64 minimum = p.value(QStringLiteral("min")).toDouble();
            const foundation::float64 maximum = p.value(QStringLiteral("max")).toDouble();
            const qsizetype axis = groupOrNull->Members.indexOf(pid);
            groupOrNull->Components.append(
            {
                .Name = groupOrNull->AxisNames.at(axis),
                .FieldOrNull = textFieldOrNull,
                .Settings =
                {
                    .Minimum = minimum,
                    .Maximum = maximum,
                    .Step = GetScrubStep(groupOrNull->StepUnit, minimum, maximum),
                },
            });
            if (groupOrNull->Components.size() < groupOrNull->Members.size())
            {
                continue;
            }
            QWidget* vectorRow =
                MakeVectorRow(groupOrNull->Components, QString(), group, [this]() { markInspectorPending(); });
            if (groupOrNull->IsColour == true)
            {
                QWidget* colourRow = new QWidget(group);
                QHBoxLayout* colourLayout = new QHBoxLayout(colourRow);
                colourLayout->setContentsMargins(0, 0, 0, 0);
                ColourSwatch* swatch = new ColourSwatch(*groupOrNull->Components.at(0).FieldOrNull,
                                                        *groupOrNull->Components.at(1).FieldOrNull,
                                                        *groupOrNull->Components.at(2).FieldOrNull,
                                                        colourRow);
                connect(swatch, &ColourSwatch::Picked, this, [this]() { markInspectorPending(); });
                colourLayout->addWidget(swatch);
                colourLayout->addWidget(vectorRow, 1);
                vectorRow = colourRow;
            }
            // The caption sits above the fields, with its unit, so the three fields share the panel's full
            // width instead of what is left beside a label column.
            QWidget* block = new QWidget(group);
            block->setObjectName(QStringLiteral("inspectorVectorBlock"));
            QVBoxLayout* blockLayout = new QVBoxLayout(block);
            blockLayout->setContentsMargins(0, 0, 0, 0);
            blockLayout->setSpacing(design::SPACING_EXTRA_SMALL);
            QLabel* caption = new QLabel(groupOrNull->Unit.isEmpty() == true
                                             ? groupOrNull->Title
                                             : QStringLiteral("%1 (%2)").arg(groupOrNull->Title, groupOrNull->Unit),
                                         block);
            caption->setForegroundRole(QPalette::PlaceholderText);
            blockLayout->addWidget(caption);
            blockLayout->addWidget(vectorRow);
            rows->addRow(block);
        }
        if (IsBuiltInWorld(Document_) == true && cid == QStringLiteral("light"))
        {
            QCheckBox* staticTransformOrNull =
                qobject_cast<QCheckBox*>(Fields_.value(FieldKey(cid, QStringLiteral("staticTransform"))));
            QCheckBox* bakeOrNull =
                qobject_cast<QCheckBox*>(Fields_.value(FieldKey(cid, QStringLiteral("bakeAlwaysOn"))));
            QCheckBox* enabledOrNull =
                qobject_cast<QCheckBox*>(Fields_.value(FieldKey(cid, QStringLiteral("enabled"))));
            QComboBox* kindOrNull = qobject_cast<QComboBox*>(Fields_.value(FieldKey(cid, QStringLiteral("kind"))));
            QComboBox* roleOrNull = qobject_cast<QComboBox*>(Fields_.value(FieldKey(cid, WORLD_CELESTIAL_ROLE)));
            if (staticTransformOrNull != nullptr && bakeOrNull != nullptr && enabledOrNull != nullptr)
            {
                QLabel* suggestion = new QLabel(
                    QStringLiteral("Static light. Enable Bake always-on lighting to precompute its diffuse light."),
                    group);
                suggestion->setObjectName(QStringLiteral("worldLightBakeSuggestion"));
                suggestion->setWordWrap(true);
                rows->addRow(suggestion);
                const auto refresh =
                    [staticTransformOrNull, bakeOrNull, enabledOrNull, kindOrNull, roleOrNull, suggestion, rows]() {
                        const bool isStatic = staticTransformOrNull->isChecked();
                        const bool isDynamicCelestial =
                            kindOrNull != nullptr &&
                            kindOrNull->currentData().toString() == QStringLiteral("directional") &&
                            roleOrNull != nullptr && roleOrNull->currentData().toString() == WORLD_CELESTIAL_DYNAMIC;
                        if (isStatic == false)
                        {
                            bakeOrNull->setChecked(false);
                        }
                        const bool hasOptedIn = bakeOrNull->isChecked();
                        rows->setRowVisible(bakeOrNull,
                                            isStatic == true && (isDynamicCelestial == false || hasOptedIn == true));
                        bakeOrNull->setEnabled(enabledOrNull->isChecked());
                        enabledOrNull->setEnabled(hasOptedIn == false);
                        suggestion->setVisible(isStatic == true && isDynamicCelestial == false && hasOptedIn == false);
                    };
                connect(staticTransformOrNull, &QCheckBox::toggled, group, refresh);
                connect(bakeOrNull, &QCheckBox::toggled, group, refresh);
                connect(enabledOrNull, &QCheckBox::toggled, group, refresh);
                if (kindOrNull != nullptr)
                {
                    connect(kindOrNull, &QComboBox::currentIndexChanged, group, refresh);
                }
                if (roleOrNull != nullptr)
                {
                    connect(roleOrNull, &QComboBox::currentIndexChanged, group, refresh);
                }
                refresh();
            }
        }
        if (component.value(QStringLiteral("required")).toBool())
        {
            rows->addRow(new QLabel(QStringLiteral("Required by this project"), group));
        }
        else
        {
            auto* remove = new QPushButton(QStringLiteral("Remove Component"), group);
            remove->setObjectName(QStringLiteral("worldDetach:") + cid);
            connect(remove, &QPushButton::clicked, this, [this, cid]() {
                if (Pending_)
                {
                    Changed(QStringLiteral("Apply or reset fields before removing a component."));
                    return;
                }
                QString error;
                if (Document_.Detach(Selected_, error, cid) == false)
                {
                    Changed(error);
                    return;
                }
                Render();
                Snapshot();
                Changed(QStringLiteral("Component removed. Undo is available."));
            });
            rows->addRow(remove);
        }
        if (Document_.HasBuiltInSchema() == true && cid == QStringLiteral("light"))
        {
            ConfigureCelestialLightFields(*group, *rows, Fields_);
        }
        else
        {
            group->setMinimumHeight(rows->totalHeightForWidth(WORLD_PROPERTY_LAYOUT_WIDTH));
        }
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        Inspector_->addWidget(group);
    }
    Inspector_->addStretch();
}
void WorldWorkspace::PickTint(const QString& id, const QJsonObject& binding)
{
    if (Pending_ || id != Selected_ || Document_.Entity(id).isEmpty())
    {
        return;
    }
    const auto component = binding.value(QStringLiteral("component")).toString();
    const auto properties = binding.value(QStringLiteral("properties")).toObject();
    const auto values = Document_.Entity(id).value(QStringLiteral("components")).toObject().value(component).toObject();
    const auto channel = [&values, &properties](const QString& key) {
        return static_cast<foundation::float32>(
            foundation::math::Clamp(values.value(properties.value(key).toString()).toDouble(),
                                    foundation::float64{0},
                                    foundation::float64{1}));
    };
    // Thanks to The Qt Company, "QColorTransform Class", Qt 6 GUI:
    // https://doc.qt.io/qt-6/qcolortransform.html . Private presentation converts
    // display-sRGB picker colors to the renderer's linear tint; engine values
    // remain plain RGB, and no independent transfer approximation is introduced.
    const auto linearToDisplay =
        QColorSpace(QColorSpace::SRgbLinear).transformationToColorSpace(QColorSpace(QColorSpace::SRgb));
    const auto displayToLinear =
        QColorSpace(QColorSpace::SRgb).transformationToColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    const auto initial = linearToDisplay.map(
        QColor::fromRgbF(channel(QStringLiteral("r")), channel(QStringLiteral("g")), channel(QStringLiteral("b"))));
    auto* picker = new QColorDialog(initial, this);
    picker->setObjectName(QStringLiteral("worldTintPicker"));
    picker->setWindowTitle(QStringLiteral("Tint — ") + Document_.Entity(id).value(QStringLiteral("name")).toString());
    picker->setAttribute(Qt::WA_DeleteOnClose);
    const auto generation = DocumentGeneration_;
    const auto draft = Document_.Draft();
    connect(picker,
            &QColorDialog::colorSelected,
            this,
            [this, id, component, properties, generation, draft, initial, displayToLinear](const QColor& display) {
                if (DocumentGeneration_ != generation || id != Selected_ || Pending_ || Document_.Draft() != draft)
                {
                    Changed(QStringLiteral(
                        "World or selection changed while choosing tint. Choose again; fields were preserved."));
                    return;
                }
                if (display.rgba64() == initial.rgba64())
                {
                    Changed(QStringLiteral("Tint is unchanged."));
                    return;
                }
                QLineEdit* fields[3]{};
                const QString names[]{QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")};
                for (foundation::usize i = 0; i < 3; ++i)
                {
                    fields[i] = qobject_cast<QLineEdit*>(
                        Fields_.value(FieldKey(component, properties.value(names[i]).toString())));
                    if (fields[i] == nullptr)
                    {
                        Changed(QStringLiteral("Tint mapping no longer matches the inspector. Fields were preserved."));
                        return;
                    }
                }
                const auto linear = displayToLinear.map(display);
                if (linear.isValid() == false)
                {
                    return;
                }
                const foundation::float32 channels[]{linear.redF(), linear.greenF(), linear.blueF()};
                for (foundation::usize i = 0; i < 3; ++i)
                {
                    fields[i]->setText(QString::number(static_cast<foundation::float64>(channels[i]), 'g', 17));
                }
                markInspectorPending();
                Changed(QStringLiteral(
                    "Tint chosen. Apply Changes validates all fields together; Reset Fields cancels it."));
            });
    picker->open();
}
void WorldWorkspace::markInspectorPending(bool shouldPreview)
{
    Pending_ = true;
    if (shouldPreview == true)
    {
        DraftTimer_.start();
    }
    Changed(QStringLiteral(
        "Unapplied fields. Apply Changes validates them together; Reset Fields keeps the last applied edit."));
}
void WorldWorkspace::Changed(const QString& message)
{
    if (message.isEmpty() == false)
    {
        Status_->setText(
            message +
            (RecoveryFailed_
                 ? QStringLiteral(" Recovery could not be written; Save World is required to preserve this edit.")
                 : QString()));
    }
    const bool isBusy = SceneBusy();
    if (Name_ != nullptr)
    {
        Name_->setEnabled(isBusy == false || DraftPreview_);
    }
    for (auto* field : Fields_)
    {
        field->setEnabled(isBusy == false || DraftPreview_);
    }
    Entities_->setEnabled(Pending_ == false && isBusy == false);
    mEntitySearch->setEnabled(Entities_->isEnabled());
    mLightsPanel->setEnabled(Pending_ == false && isBusy == false);
    const bool isWorkerActive = isSceneWorkerActive();
    Import_->setEnabled(Document_.Loaded() && IsWorldDemo(Document_) == false && Pending_ == false &&
                        isWorkerActive == false);
    PlayButton_->setEnabled(Document_.Loaded() && isBusy == false);
    CancelScene_->setVisible(isBusy);
    CancelScene_->setEnabled(Scene_->CanCancel());
    mAddEntity->setEnabled(Document_.Loaded() == true && Pending_ == false &&
                           Entities_->count() < WorldDocument::MAX_ENTITIES && isBusy == false);
    mDuplicateEntities->setEnabled(Selected_.isEmpty() == false && Pending_ == false &&
                                   Entities_->count() + mSelectedEntities.size() <= WorldDocument::MAX_ENTITIES &&
                                   isBusy == false);
    mDeleteEntities->setEnabled(Selected_.isEmpty() == false && Pending_ == false && isBusy == false);
    Attach_->setEnabled(mSelectedEntities.size() == 1 && Pending_ == false && Components_->count() > 0 &&
                        isBusy == false);
    Components_->setEnabled(Pending_ == false && isBusy == false);
    Apply_->setEnabled(Pending_ && isBusy == false);
    Reset_->setEnabled(Pending_ && isBusy == false);
    QPushButton* mapOrNull = findChild<QPushButton*>(QStringLiteral("worldMapMaterial"));
    const QComboBox* replacementsOrNull = findChild<QComboBox*>(QStringLiteral("worldMaterialReplacement"));
    if (mapOrNull != nullptr && replacementsOrNull != nullptr)
    {
        mapOrNull->setEnabled(replacementsOrNull->isEnabled() == true && Pending_ == false && isBusy == false);
    }
    QPushButton* sharedCoatingOrNull = findChild<QPushButton*>(QStringLiteral("worldApplySharedCoating"));
    if (sharedCoatingOrNull != nullptr)
    {
        sharedCoatingOrNull->setEnabled(Pending_ == true && isBusy == false);
    }
    QPushButton* restoreCoatingOrNull = findChild<QPushButton*>(QStringLiteral("worldRestoreCoating"));
    if (restoreCoatingOrNull != nullptr)
    {
        restoreCoatingOrNull->setEnabled(Pending_ == false && isBusy == false);
    }
    for (const QString& name : {QStringLiteral("worldAssignProjectBrdf"), QStringLiteral("worldRestoreProjectBrdf")})
    {
        QPushButton* buttonOrNull = findChild<QPushButton*>(name);
        if (buttonOrNull != nullptr)
        {
            buttonOrNull->setEnabled(Pending_ == false && isBusy == false);
        }
    }
    const bool hasRecovery = Document_.Loaded() && Dirty() == false && Unsaved_ == false &&
                             QFileInfo::exists(Path_ + QStringLiteral(".ludus-recovery"));
    Recovery_->setVisible(hasRecovery);
    Recovery_->setEnabled(hasRecovery == true && isWorkerActive == false);
    PathLabel_->setText(Path_.isEmpty()
                            ? QStringLiteral("No world open")
                            : (Dirty() ? QStringLiteral("● ") : QString()) + QDir(Root_).relativeFilePath(Path_));
    PathLabel_->setToolTip((Path_.isEmpty() ? QString() : QDir::toNativeSeparators(Path_) + QStringLiteral("\n\n")) +
                           GetWorldIntroduction());
    Render_->SetSelection(Selected_, Pending_);
    updateViewportInteraction();
    updateTerrainActions();
    Q_EMIT DocumentChanged();
}
bool WorldWorkspace::readProjectBrdfRoughness(QJsonObject& inoutScene, QString& outError, QWidget*& outInvalidOrNull)
{
    QLineEdit* fieldOrNull = qobject_cast<QLineEdit*>(Fields_.value(QStringLiteral("scene/project_brdf/roughness")));
    if (fieldOrNull == nullptr || fieldOrNull->text() == fieldOrNull->property("initialRoughnessText").toString())
    {
        return true;
    }
    QJsonValue roughness;
    if (WorldDocument::ParseValue(MakeProjectBrdfRoughnessProperty(), fieldOrNull->text(), roughness, outError) ==
        false)
    {
        outInvalidOrNull = fieldOrNull;
        return false;
    }
    QJsonObject binding = inoutScene.value(QStringLiteral("project_brdf")).toObject();
    if (binding.isEmpty() == true)
    {
        binding.insert(QStringLiteral("material"), Document_.GetInheritedMaterialId(inoutScene));
    }
    binding.insert(QStringLiteral("roughness"), roughness);
    inoutScene.insert(QStringLiteral("project_brdf"), binding);
    return true;
}

bool WorldWorkspace::readCoatingFields(QJsonObject& inoutScene, QString& outError, QWidget*& outInvalidOrNull)
{
    QJsonObject coating;
    bool hasChanged = false;
    for (const CoatingField& property : COATING_FIELDS)
    {
        const QString key = QString::fromLatin1(property.Key);
        QLineEdit* fieldOrNull = qobject_cast<QLineEdit*>(Fields_.value(QStringLiteral("scene/coating/") + key));
        if (fieldOrNull == nullptr)
        {
            return true;
        }
        const QJsonValue initial = fieldOrNull->property("initialCoatingValue").toDouble();
        QJsonValue value = initial;
        if (fieldOrNull->text() != fieldOrNull->property("initialCoatingText").toString() &&
            WorldDocument::ParseValue(property.MakeNumberProperty(), fieldOrNull->text(), value, outError) == false)
        {
            outError = QStringLiteral("%1: enter a finite value from %2 to %3.")
                           .arg(QString::fromLatin1(property.Label),
                                QString::number(static_cast<foundation::float64>(property.Minimum)),
                                QString::number(static_cast<foundation::float64>(property.Maximum)));
            outInvalidOrNull = fieldOrNull;
            return false;
        }
        hasChanged = hasChanged == true || static_cast<foundation::float32>(value.toDouble()) !=
                                               static_cast<foundation::float32>(initial.toDouble());
        coating.insert(key, value);
    }
    if (hasChanged == true)
    {
        inoutScene.insert(QStringLiteral("coating"), coating);
    }
    return true;
}

bool WorldWorkspace::readDraft(WorldDocument& inoutDocument,
                               QString& outError,
                               QWidget*& outInvalidOrNull,
                               bool shouldReadMaterial)
{
    auto components = Document_.Entity(Selected_).value(QStringLiteral("components")).toObject();
    for (auto c = components.begin(); c != components.end(); ++c)
    {
        auto values = c.value().toObject();
        for (const auto definition : Document_.Component(c.key()).value(QStringLiteral("properties")).toArray())
        {
            const auto p = definition.toObject();
            const auto pid = p.value(QStringLiteral("id")).toString();
            QWidget* fieldOrNull = Fields_.value(FieldKey(c.key(), pid));
            if (fieldOrNull != nullptr && fieldOrNull->isHidden() == true && Document_.HasBuiltInSchema() == true &&
                c.key() == QStringLiteral("light") && IsWorldCelestialProperty(pid) == true)
            {
                // A hidden driver's draft may be incomplete. Keep its last accepted settings.
                continue;
            }
            const QJsonValue authored = values.value(pid);
            const QJsonValue current = authored.isUndefined() == true ? p.value(QStringLiteral("default")) : authored;
            QJsonValue candidate;
            if (QCheckBox* checkOrNull = qobject_cast<QCheckBox*>(fieldOrNull); checkOrNull != nullptr)
            {
                candidate = checkOrNull->isChecked();
            }
            else if (QComboBox* choiceOrNull = qobject_cast<QComboBox*>(fieldOrNull); choiceOrNull != nullptr)
            {
                candidate = choiceOrNull->currentData().toString();
            }
            else if (QLineEdit* textOrNull = qobject_cast<QLineEdit*>(fieldOrNull); textOrNull != nullptr)
            {
                // Preserve unchanged JSON numbers exactly rather than round them
                // through the float32 text converter during an unrelated edit.
                if (textOrNull->text() == Display(current))
                {
                    candidate = current;
                }
                else if (WorldDocument::ParseValue(p, textOrNull->text(), candidate, outError) == false)
                {
                    outInvalidOrNull = textOrNull;
                    return false;
                }
            }
            else
            {
                outError = QStringLiteral("Inspector no longer matches the selected entity.");
                return false;
            }
            // Leave absent optional fields absent until the user authors a non-default value.
            if (authored.isUndefined() == false || candidate != current)
            {
                values.insert(pid, candidate);
            }
        }
        c.value() = values;
    }
    auto scene = Document_.Entity(Selected_).value(QStringLiteral("scene")).toObject();
    if (scene.isEmpty() == false && Document_.Schema().contains(QStringLiteral("scene_transform")) == false)
    {
        for (const auto& key : {QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
        {
            auto values = scene.value(key).toArray();
            for (foundation::int32 axis = 0; axis < 3; ++axis)
            {
                auto* field =
                    qobject_cast<QLineEdit*>(Fields_.value(QStringLiteral("scene/") + key + QString::number(axis)));
                if (field == nullptr)
                {
                    outError = QStringLiteral("Scene inspector is stale.");
                    return false;
                }
                if (field->text() == Display(values[axis]))
                {
                    continue;
                }
                const QJsonObject property{
                    {QStringLiteral("type"), QStringLiteral("number")},
                    {QStringLiteral("label"), key},
                    {QStringLiteral("min"),
                     key == QStringLiteral("scale")      ? SCENE_SCALE_MINIMUM
                     : key == QStringLiteral("rotation") ? -SCENE_ROTATION_LIMIT
                                                         : -SCENE_POSITION_LIMIT},
                    {QStringLiteral("max"),
                     key == QStringLiteral("rotation") ? SCENE_ROTATION_LIMIT : SCENE_POSITION_LIMIT}};
                QJsonValue value;
                if (WorldDocument::ParseValue(property, field->text(), value, outError) == false)
                {
                    outInvalidOrNull = field;
                    return false;
                }
                values[axis] = value;
            }
            scene.insert(key, values);
        }
    }
    if (shouldReadMaterial == true && (readCoatingFields(scene, outError, outInvalidOrNull) == false ||
                                       readProjectBrdfRoughness(scene, outError, outInvalidOrNull) == false))
    {
        return false;
    }
    if (inoutDocument
            .Edit(Selected_, components, Name_->text(), outError, scene.isEmpty() == true ? nullptr : &scene) == false)
    {
        return false;
    }
    return true;
}
void WorldWorkspace::PreviewDraft()
{
    if (Document_.Loaded() == false || isTerrainAuthoringOpen() == true)
    {
        return;
    }
    if (isSceneWorkerActive() == true)
    {
        DraftTimer_.start();
        return;
    }
    auto draft = Document_;
    QString error;
    QWidget* invalid = nullptr;
    if (Pending_ && readDraft(draft, error, invalid, false) == false)
    {
        return;
    }
    if (draft.SceneLevel().isEmpty())
    {
        Render_->PreviewDraft(draft);
    }
    else
    {
        DraftPreview_ = true;
        SceneGeneration_ = DocumentGeneration_;
        Scene_->SetWorldSnapshot(draft);
        if (Scene_->Assemble(draft.SceneLevel()) == false)
        {
            DraftPreview_ = false;
        }
    }
}
QWidget* WorldWorkspace::TakePropertiesPanel(QWidget* newParent)
{
    LUDUS_REQUIRE(newParent != nullptr && mPropertiesPanel->parentWidget() == Split_);
    mPropertiesPanel->setParent(newParent);
    return mPropertiesPanel;
}
void WorldWorkspace::SetChromeVisible(bool isVisible)
{
    if (isVisible == mIsChromeVisible)
    {
        return;
    }
    mIsChromeVisible = isVisible;
    if (isVisible == false)
    {
        mSplitterStateBeforeChromeHidden = Split_->saveState();
    }
    mHeader->setVisible(isVisible);
    mHierarchy->setVisible(isVisible);
    Status_->setVisible(isVisible);
    // A Properties tool window owns the inspector once taken; only an embedded one is chrome.
    if (mPropertiesPanel->parentWidget() == Split_)
    {
        mPropertiesPanel->setVisible(isVisible);
    }
    if (isVisible == true)
    {
        (void)Split_->restoreState(mSplitterStateBeforeChromeHidden);
        mSplitterStateBeforeChromeHidden.clear();
    }
}
QJsonObject WorldWorkspace::Session() const
{
    // Sessions saved while the viewport is focused record the arrangement it returns to.
    const QByteArray splitter = mIsChromeVisible == true ? Split_->saveState() : mSplitterStateBeforeChromeHidden;
    return {{QStringLiteral("world"), Path_.isEmpty() ? QString() : QDir(Root_).relativeFilePath(Path_)},
            {QStringLiteral("selection"), Selected_},
            {QStringLiteral("selections"), QJsonArray::fromStringList(mSelectedEntities)},
            {QStringLiteral("splitter"), QString::fromLatin1(splitter.toBase64())},
            {QStringLiteral("boundCamera"), Render_->Preview().Navigation()},
            {QStringLiteral("sceneCamera"), Scene_->Navigation()}};
}
void WorldWorkspace::RestoreSession(const QJsonObject& state)
{
    const auto path = state.value(QStringLiteral("world")).toString();
    if (path.isEmpty() == false && QDir::isRelativePath(path))
    {
        const auto absolute = QDir(Root_).filePath(path);
        if (Inside(absolute, true))
        {
            (void)Open(absolute);
        }
    }
    Render_->Preview().RestoreNavigation(state.value(QStringLiteral("boundCamera")).toObject());
    Scene_->RestoreNavigation(state.value(QStringLiteral("sceneCamera")).toObject());
    const auto encoded = state.value(QStringLiteral("splitter")).toString().toLatin1();
    const auto bytes = QByteArray::fromBase64(encoded);
    if (bytes.size() <= 4096 && bytes.toBase64() == encoded)
    {
        if (Split_->restoreState(bytes) == true && mIsChromeVisible == false)
        {
            mSplitterStateBeforeChromeHidden = bytes;
        }
    }
    const QString selected = state.value(QStringLiteral("selection")).toString();
    mSelectedEntities.clear();
    for (const QJsonValue value : state.value(QStringLiteral("selections")).toArray())
    {
        const QString id = value.toString();
        if (Document_.Entity(id).isEmpty() == false && mSelectedEntities.contains(id) == false)
        {
            mSelectedEntities.append(id);
        }
    }
    if (mSelectedEntities.isEmpty() == true && Document_.Entity(selected).isEmpty() == false)
    {
        mSelectedEntities.append(selected);
    }
    Selected_ = mSelectedEntities.contains(selected) == true ? selected : mSelectedEntities.value(0);
    Render();
    Changed();
}
bool WorldWorkspace::SelectEntity(const QString& id)
{
    if (Document_.Entity(id).isEmpty() == true)
    {
        return false;
    }
    mEntitySearch->RevealEntity(id);
    mSelectedEntities = {id};
    Selected_ = id;
    Render();
    Changed();
    return true;
}
bool WorldWorkspace::Apply()
{
    if (SceneBusy())
    {
        return false;
    }
    if (Pending_ == false)
    {
        return Document_.Loaded();
    }
    WorldDocument candidate = Document_;
    QString error;
    QWidget* invalid = nullptr;
    if (readDraft(candidate, error, invalid) == false)
    {
        if (invalid != nullptr)
        {
            invalid->setFocus();
        }
        Changed(error);
        return false;
    }
    DraftTimer_.stop();
    Document_ = candidate;
    Pending_ = false;
    // Update labels in place so Save preserves the focused field's text buffer.
    for (foundation::int32 i = 0; i < Entities_->count(); ++i)
    {
        auto* item = Entities_->item(i);
        item->setText(Document_.Entity(item->data(Qt::UserRole).toString()).value(QStringLiteral("name")).toString());
    }
    for (auto* field : Fields_)
    {
        auto* choice = qobject_cast<QComboBox*>(field);
        if (choice == nullptr || field->property("worldType").toString() != QStringLiteral("entity"))
        {
            continue;
        }
        for (foundation::int32 i = 1; i < choice->count(); ++i)
        {
            const auto id = choice->itemData(i).toString();
            choice->setItemText(i,
                                Document_.Entity(id).value(QStringLiteral("name")).toString() + QStringLiteral(" — ") +
                                    id);
        }
    }
    Snapshot();
    Changed(QStringLiteral("Changes applied. Save World writes them to the project."));
    return true;
}
bool WorldWorkspace::Save()
{
    if (Document_.Loaded() == false || Apply() == false)
    {
        return false;
    }
    if (IsWorldDemo(Document_))
    {
        world_demo::Level projection;
        QString error;
        if (ProjectWorldDemo(Document_, projection, error) == false)
        {
            Changed(error);
            return false;
        }
    }
    const auto level = Document_.SceneLevel();
    const bool validated = SaveValidated_;
    SaveValidated_ = false;
    if (level.isEmpty() == false && validated == false)
    {
        if (isSceneWorkerActive() == false)
        {
            SceneGeneration_ = DocumentGeneration_;
            Scene_->SetWorldSnapshot(Document_);
            (void)Scene_->VerifyAssets(level);
        }
        // A background placement cook finishes first; placementValidationFinished then saves.
        if (isSceneWorkerActive() == false)
        {
            Changed(QStringLiteral("Scene validation could not start; World edits remain open."));
            return false;
        }
        SavePending_ = true;
        Changed(QStringLiteral("Saving World after scene asset validation finishes…"));
        return true;
    }
    QByteArray schema;
    WorldDocument definition;
    if (LoadProjectSchema(definition, schema, IsWorldDemo(Document_)) == false || schema != SchemaDisk_)
    {
        Changed(QStringLiteral("The project schema changed. Save was refused; keep this world open and reconcile the "
                               "schema before retrying."));
        return false;
    }
    if (Inside(Path_, Unsaved_ == false) == false ||
        QFileInfo(Path_ + QStringLiteral(".ludus-editor-lock")).isSymLink())
    {
        Changed(QStringLiteral("The world location changed or already exists. Save was refused."));
        return false;
    }
    QLockFile lock(Path_ + QStringLiteral(".ludus-editor-lock"));
    QByteArray current;
    if (lock.tryLock(0) == false ||
        (Unsaved_ ? QFileInfo::exists(Path_)
                  : (Read(Path_, current, WorldDocument::MAX_BYTES) == false || current != Disk_)))
    {
        Changed(QStringLiteral(
            "The world changed on disk or is locked. Your edits remain open; resolve the conflict before saving."));
        return false;
    }
    const auto bytes = Document_.Encode();
    if (Replace(Path_, bytes) == false)
    {
        Changed(QStringLiteral("Could not atomically save the world. Your edits remain open."));
        return false;
    }
    Disk_ = bytes;
    Unsaved_ = false;
    Document_.AcknowledgeSave();
    RecoveryFailed_ = false;
    (void)QFile::remove(Path_ + QStringLiteral(".ludus-recovery"));
    RenderPreview& preview = GetRenderPreview();
    preview.SetSurfaceBakeWorldPath(Path_);
    if (preview.SaveSurfaceBake() == false)
    {
        Changed(QStringLiteral("World saved. Surface charts could not be saved beside the World. Check the folder's "
                               "permissions and available space, then save again."));
        return true;
    }
    Changed(SceneError_.isEmpty() ? QStringLiteral("World saved.")
                                  : QStringLiteral("World saved. Previous viewport retained: ") + SceneError_);
    return true;
}
QByteArray WorldWorkspace::RecoveryHeader() const
{
    return QByteArrayLiteral("LUDUS-WORLD-RECOVERY-1\n") +
           QCryptographicHash::hash(SchemaDisk_ + '\n' + Disk_, QCryptographicHash::Sha256).toHex() + '\n';
}
void WorldWorkspace::Snapshot()
{
    checkTerrainBase();
    RefreshScene();
    Render_->WorldChanged();
    RecoveryFailed_ = Document_.Dirty() && Unsaved_ == false;
    if (Unsaved_ == false && Document_.Dirty() == false && Inside(Path_, true))
    {
        QByteArray old;
        if (Read(Path_ + QStringLiteral(".ludus-recovery"),
                 old,
                 WorldDocument::MAX_BYTES + MAX_RECOVERY_HEADER_BYTES) &&
            old.startsWith(RecoveryHeader()))
        {
            (void)QFile::remove(Path_ + QStringLiteral(".ludus-recovery"));
        }
    }
    if (Unsaved_ || Document_.Dirty() == false || Inside(Path_, true) == false)
    {
        return;
    }
    const auto recovery = Path_ + QStringLiteral(".ludus-recovery");
    const auto lockPath = Path_ + QStringLiteral(".ludus-editor-lock");
    if (QFileInfo(lockPath).isSymLink())
    {
        return;
    }
    QLockFile lock(lockPath);
    QByteArray disk;
    if (lock.tryLock(0) == false || Read(Path_, disk, WorldDocument::MAX_BYTES) == false || disk != Disk_)
    {
        return;
    }
    const auto bytes = Document_.Encode();
    RecoveryFailed_ = Replace(recovery,
                              RecoveryHeader() + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() +
                                  '\n' + bytes) == false;
}
bool WorldWorkspace::Recover()
{
    QByteArray envelope, current, schema;
    const auto header = RecoveryHeader();
    WorldDocument definition;
    if (isSceneWorkerActive() || Dirty() || Unsaved_ || Inside(Path_, true) == false ||
        Read(Path_, current, WorldDocument::MAX_BYTES) == false || current != Disk_ ||
        LoadProjectSchema(definition, schema, IsWorldDemo(Document_)) == false || schema != SchemaDisk_ ||
        Read(Path_ + QStringLiteral(".ludus-recovery"),
             envelope,
             WorldDocument::MAX_BYTES + MAX_RECOVERY_HEADER_BYTES) == false ||
        envelope.startsWith(header) == false)
    {
        Changed(
            QStringLiteral("Recovery is missing or belongs to a different saved world/schema. Nothing was changed."));
        return false;
    }
    const auto bytes = envelope.mid(header.size() + 65);
    if (envelope.mid(header.size(), 65) != QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() + '\n')
    {
        Changed(QStringLiteral("Recovery is damaged. Nothing was changed."));
        return false;
    }
    WorldDocument candidate;
    QString error;
    if (candidate.LoadSchema(SchemaDisk_, error) == false || candidate.Load(bytes, error) == false)
    {
        Changed(error);
        return false;
    }
    // Recovery is one undoable edit against the saved document.
    if (Document_.Restore(candidate.Draft(), error) == false)
    {
        Changed(error);
        return false;
    }
    Pending_ = false;
    Selected_.clear();
    mSelectedEntities.clear();
    Render();
    checkTerrainBase();
    RefreshScene();
    Render_->WorldChanged();
    Changed(QStringLiteral("Recovery restored for review. Save World to publish it."));
    return true;
}
bool WorldWorkspace::ConfirmDiscard()
{
    if (SceneBusy())
    {
        Changed(QStringLiteral("Wait for the scene operation to finish before closing or changing documents."));
        return false;
    }
    if (Dirty() == false)
    {
        return true;
    }
    const auto answer =
        QMessageBox::warning(this,
                             QStringLiteral("Unsaved World"),
                             QStringLiteral("Save changes to %1? Unapplied fields must pass validation before saving.")
                                 .arg(QFileInfo(Path_).fileName()),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Cancel);
    if (answer == QMessageBox::Save)
    {
        const bool requested = Save();
        return requested && isSceneWorkerActive() == false && SavePending_ == false && Dirty() == false;
    }
    if (answer != QMessageBox::Discard)
    {
        return false;
    }
    Pending_ = false;
    Document_.Discard();
    checkTerrainBase();
    RefreshScene();
    Render_->WorldChanged();
    // Keep a new world marked unsaved until its caller closes/replaces it.
    // Discard only author-owned recovery for the same baseline, never symlinks.
    QByteArray envelope;
    if (Read(Path_ + QStringLiteral(".ludus-recovery"),
             envelope,
             WorldDocument::MAX_BYTES + MAX_RECOVERY_HEADER_BYTES) &&
        envelope.startsWith(RecoveryHeader()))
    {
        (void)QFile::remove(Path_ + QStringLiteral(".ludus-recovery"));
    }
    Render();
    Changed(QStringLiteral("Changes discarded."));
    return true;
}
void WorldWorkspace::UseProjectGraph(RendererWorkspace& inoutWorkspace)
{
    Render_->GetViewport().UseProjectGraph(inoutWorkspace);
    Scene_->GetViewport().UseProjectGraph(inoutWorkspace);
}
RenderPreview& WorldWorkspace::GetRenderPreview() noexcept
{
    return mShownRenderPreviewOrNull != nullptr ? *mShownRenderPreviewOrNull : Render_->Preview();
}
bool WorldWorkspace::BeginPlayInViewport(QString& outAddress, QString& outReason)
{
    // Only the bound viewport of a World without an imported scene hosts Play today.
    if (Document_.Loaded() == false || Document_.SceneLevel().isEmpty() == false || Render_->isHidden() == true)
    {
        outReason = QStringLiteral("Play in the viewport needs this World shown in the World viewport.");
        return false;
    }
    if (Render_->Preview().Running() == false)
    {
        outReason = QStringLiteral("Play in the viewport needs the World viewport running; it is paused.");
        return false;
    }
    return Render_->BeginPlay(outAddress, outReason);
}
PlayMode WorldWorkspace::GetPlayMode() const noexcept
{
    return static_cast<PlayMode>(mPlayModeBox->currentData().toInt());
}
void WorldWorkspace::SetPlayMode(PlayMode mode)
{
    mPlayModeBox->setCurrentIndex(mPlayModeBox->findData(static_cast<int>(mode)));
    QSettings().setValue(GetPlayModeKey(),
                         mode == PlayMode::SeparateWindow ? GetSeparateWindowPlayModeValue()
                                                          : GetViewportPlayModeValue());
    hidePlayUnavailable();
}
PlayWindow WorldWorkspace::PreparePlay(PlayMode mode, QString& outAddress)
{
    hidePlayUnavailable();
    if (mode == PlayMode::SeparateWindow)
    {
        return PlayWindow::SeparateWindow;
    }
    QString address;
    QString reason;
    if (BeginPlayInViewport(address, reason) == false)
    {
        showPlayUnavailable(reason);
        return PlayWindow::Unavailable;
    }
    outAddress = foundation::Move(address);
    return PlayWindow::InViewport;
}
QString WorldWorkspace::GetPlayUnavailableText() const
{
    return mPlayUnavailableLabel->isHidden() == true ? QString() : mPlayUnavailableLabel->text();
}
void WorldWorkspace::SetPlayingElsewhere(bool isPlayingElsewhere)
{
    Render_->SetPlayingElsewhere(isPlayingElsewhere);
}
void WorldWorkspace::showPlayUnavailable(const QString& reason)
{
    mPlayUnavailableLabel->setText(reason + QStringLiteral(" The game was not started. Play it in a separate window "
                                                           "now, or choose In separate window to always do so."));
    mPlayUnavailableLabel->show();
    mPlayElsewhereButton->show();
}
void WorldWorkspace::hidePlayUnavailable()
{
    mPlayUnavailableLabel->hide();
    mPlayUnavailableLabel->clear();
    mPlayElsewhereButton->hide();
}
void WorldWorkspace::EndPlayInViewport() noexcept
{
    Render_->EndPlay();
}
void WorldWorkspace::RequestPlayClose() noexcept
{
    Render_->RequestPlayClose();
}
bool WorldWorkspace::IsPlayingInViewport() const noexcept
{
    return Render_->IsPlaying();
}
bool WorldWorkspace::PreviewRunning() const noexcept
{
    return Render_->Preview().Running() || Scene_->PreviewRunning();
}
void WorldWorkspace::ResumePreview()
{
    if (Document_.Loaded() == false || SceneBusy())
    {
        return;
    }
    if (Document_.SceneLevel().isEmpty())
    {
        (void)Render_->Refresh(true);
    }
    else
    {
        Scene_->ResumePreview();
    }
}
void WorldWorkspace::StopPreview() noexcept
{
    closeTerrainAuthoring(false);
    Render_->GetViewport().EndFirstPerson();
    Scene_->GetViewport().EndFirstPerson();
    Render_->Stop();
    Scene_->StopPreview();
}
bool WorldWorkspace::ApplyLuminaireRepair(const RenderGraphLuminaireCoverage& repair)
{
    if (repair.Repair == RenderGraphLuminaireRepair::None || Pending_ == true || SceneBusy() == true ||
        mAddEntity->isEnabled() == false)
    {
        Changed(QStringLiteral("The World cannot be edited now; try the luminaire repair again when it is ready."));
        return false;
    }
    QString id;
    QString error;
    if (repair.Repair == RenderGraphLuminaireRepair::AddLuminaire)
    {
        if (CreateWorldLuminaire(Document_, mEntropy, repair.Placement, id, error) == false)
        {
            Changed(error);
            return false;
        }
    }
    else
    {
        QJsonObject entity;
        for (const QJsonValue value : Document_.Draft().value(QStringLiteral("entities")).toArray())
        {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("components")).toObject().contains(QStringLiteral("luminaire")) ==
                    true &&
                ComputeRenderSceneSourceId(Document_.GetEntityKey(candidate)) == repair.LuminaireSourceId)
            {
                entity = candidate;
            }
        }
        if (entity.isEmpty() == true || IsBuiltInWorld(Document_) == false)
        {
            Changed(QStringLiteral("The luminaire to fit is no longer in this default World."));
            return false;
        }
        id = Document_.GetEntityKey(entity);
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
        transform.insert(QStringLiteral("x"), static_cast<foundation::float64>(repair.Placement.PositionWorld.X));
        transform.insert(QStringLiteral("y"), static_cast<foundation::float64>(repair.Placement.PositionWorld.Y));
        transform.insert(QStringLiteral("z"), static_cast<foundation::float64>(repair.Placement.PositionWorld.Z));
        transform.insert(QStringLiteral("rx"), static_cast<foundation::float64>(repair.Placement.RotationDegrees.X));
        transform.insert(QStringLiteral("ry"), static_cast<foundation::float64>(repair.Placement.RotationDegrees.Y));
        transform.insert(QStringLiteral("rz"), static_cast<foundation::float64>(repair.Placement.RotationDegrees.Z));
        QJsonObject luminaire = components.value(QStringLiteral("luminaire")).toObject();
        luminaire.insert(QStringLiteral("halfExtent"),
                         static_cast<foundation::float64>(repair.Placement.HalfExtentWorld));
        components.insert(QStringLiteral("transform"), transform);
        components.insert(QStringLiteral("luminaire"), luminaire);
        if (Document_.Edit(id, components, entity.value(QStringLiteral("name")).toString(), error) == false)
        {
            Changed(error);
            return false;
        }
    }
    Selected_ = id;
    mSelectedEntities = {id};
    Render();
    Snapshot();
    Changed(repair.Repair == RenderGraphLuminaireRepair::AddLuminaire
                ? QStringLiteral("Luminaire added. Move it with the viewport gizmo or edit it in the inspector. Undo "
                                 "is available.")
                : QStringLiteral("Luminaire fitted to the World. Undo is available."));
    return true;
}
bool WorldWorkspace::ApplySunRepair(const WorldLightingCoverage& repair)
{
    if (repair.Repair != WorldLightingRepair::AddSun)
    {
        Changed(QStringLiteral("This World needs no sun."));
        return false;
    }
    return addSun(repair.Sun);
}
bool WorldWorkspace::addSun(const WorldSunPlacement& sun)
{
    if (Pending_ == true || SceneBusy() == true || mAddEntity->isEnabled() == false)
    {
        Changed(QStringLiteral("The World cannot be edited now; try adding the sun again when it is ready."));
        return false;
    }
    QString id;
    QString error;
    if (CreateWorldSun(Document_, mEntropy, sun, id, error) == false)
    {
        Changed(error);
        return false;
    }
    Selected_ = id;
    mSelectedEntities = {id};
    Render();
    Snapshot();
    Changed(QStringLiteral("Sun added. Turn it with the viewport gizmo or set its intensity in the inspector. Undo is "
                           "available."));
    return true;
}
void WorldWorkspace::Undo()
{
    if (Pending_ == false && SceneBusy() == false && Document_.Undo())
    {
        Render();
        Snapshot();
        Changed(QStringLiteral("World edit undone."));
    }
}
void WorldWorkspace::Redo()
{
    if (Pending_ == false && SceneBusy() == false && Document_.Redo())
    {
        Render();
        Snapshot();
        Changed(QStringLiteral("World edit redone."));
    }
}
} // namespace ludus::editor
