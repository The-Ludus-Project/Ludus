#include "internal/world_workspace.h"
#include "internal/import_report.h"

#include "internal/controller.h"
#include "internal/editor_files.h"
#include "internal/editor_identity.h"
#include "internal/level_workspace.h"
#include "internal/render_panel.h"
#include "internal/world_demo_adapter.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

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
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
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
QString Display(const QJsonValue& value)
{
    if (value.isDouble())
    {
        return QString::number(value.toDouble(), 'g', 17);
    }
    return value.toString();
}
} // namespace
WorldWorkspace::WorldWorkspace(QWidget* parent) : WorldWorkspace(ToolingPaths{}, parent) {}
WorldWorkspace::WorldWorkspace(const ToolingPaths& tooling, QWidget* parent)
    : QWidget(parent), DemoPreview_(&DemoDocument_, this)
{
    setObjectName(QStringLiteral("worldWorkspace"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(QStringLiteral("World — entities and project-defined components"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    intro->setToolTip(
        QStringLiteral("Create or open a world, import a scene, and edit placement and components together. "
                       "Projects may supply authoring/world.schema.json for gameplay fields. Runtime "
                       "binding belongs to the game."));
    QGridLayout* files = new QGridLayout;
    auto* create = new QPushButton(QStringLiteral("New World…"), this);
    create->setObjectName(QStringLiteral("worldNew"));
    auto* open = new QPushButton(QStringLiteral("Open World…"), this);
    open->setObjectName(QStringLiteral("worldOpen"));
    Recovery_ = new QPushButton(QStringLiteral("Restore Recovery"), this);
    Recovery_->setObjectName(QStringLiteral("worldRecover"));
    files->addWidget(create, 0, 0);
    files->addWidget(open, 0, 1);
    files->addWidget(Recovery_, 0, 2);
    Import_ = new QPushButton(QStringLiteral("Import scene…"), this);
    Import_->setObjectName(QStringLiteral("worldImportScene"));
    files->addWidget(Import_, 0, 3);
    CancelScene_ = new QPushButton(QStringLiteral("Cancel scene operation"), this);
    CancelScene_->setObjectName(QStringLiteral("worldCancelScene"));
    files->addWidget(CancelScene_, 1, 0, 1, 2);
    connect(CancelScene_, &QPushButton::clicked, this, [this]() {
        DemoPreview_.Stop();
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
                                         QStringLiteral("Scenes (*.pbrt *.gltf *.glb *.obj);;PBRT v4 (*.pbrt);;glTF "
                                                        "2.0 (*.gltf *.glb);;Wavefront OBJ (*.obj)"));
        if (source.isEmpty() == false)
        {
            (void)PreviewImportScene(source);
        }
    });
#endif
    PreviewButton_ = new QPushButton(QStringLiteral("Open rendered preview"), this);
    PreviewButton_->setObjectName(QStringLiteral("worldPreview"));
    PlayButton_ = new QPushButton(QStringLiteral("Play saved World"), this);
    PlayButton_->setObjectName(QStringLiteral("worldPlay"));
    files->addWidget(PreviewButton_, 1, 2);
    files->addWidget(PlayButton_, 1, 3);
    connect(PreviewButton_, &QPushButton::clicked, this, [this]() { (void)StartPreview(); });
    connect(PlayButton_, &QPushButton::clicked, this, [this]() {
        if (Save() && Dirty() == false && SceneBusy() == false)
        {
            StopPreview();
            Q_EMIT PlayRequested();
        }
    });
    connect(&DemoPreview_, &ScenePreview::Changed, this, &WorldWorkspace::WorldDemoChanged);
    files->setColumnStretch(4, 1);
    layout->addLayout(files);
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
    previewActions->addWidget(ConfirmImport_);
    previewActions->addWidget(DiscardImport_);
    previewActions->addStretch();
    previewRow->addWidget(ImportPreview_, 1);
    previewRow->addLayout(previewActions);
    layout->addLayout(previewRow);
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
    PathLabel_ = new QLabel(this);
    PathLabel_->setTextFormat(Qt::PlainText);
    PathLabel_->setWordWrap(true);
    PathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QHBoxLayout* documentRow = new QHBoxLayout;
    documentRow->addWidget(PathLabel_, 1);
    mImportReportToggleOrNull = new QToolButton(this);
    mImportReportToggleOrNull->setObjectName(QStringLiteral("worldImportReportToggle"));
    mImportReportToggleOrNull->setText(QStringLiteral("Import report"));
    mImportReportToggleOrNull->setCheckable(true);
    mImportReportToggleOrNull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mImportReportToggleOrNull->setArrowType(Qt::RightArrow);
    mImportReportToggleOrNull->hide();
    documentRow->addWidget(mImportReportToggleOrNull);
    layout->addLayout(documentRow);
    connect(mImportReportToggleOrNull, &QToolButton::toggled, this, [this](bool isExpanded) {
        ImportReport_->setVisible(isExpanded);
        mImportReportToggleOrNull->setArrowType(isExpanded == true ? Qt::DownArrow : Qt::RightArrow);
    });
    Render_ = new RenderPanel(this);
    layout->addWidget(Render_);
    connect(Render_, &RenderPanel::Starting, this, &WorldWorkspace::PreviewStarting);
    connect(Render_, &RenderPanel::TintRequested, this, &WorldWorkspace::PickTint);
    auto* split = new QSplitter(this);
    Split_ = split;
    split->setObjectName(QStringLiteral("worldSplitter"));
    layout->addWidget(split, 1);
    QScrollArea* hierarchyScroll = new QScrollArea(split);
    hierarchyScroll->setObjectName(QStringLiteral("worldHierarchyScroll"));
    hierarchyScroll->setWidgetResizable(true);
    hierarchyScroll->setMinimumWidth(130);
    QWidget* left = new QWidget(hierarchyScroll);
    QVBoxLayout* hierarchy = new QVBoxLayout(left);
    // A docked workspace may be shorter than the hierarchy's controls. Scroll
    // the contents instead of compressing styled button text below its hint.
    hierarchy->setSizeConstraint(QLayout::SetMinimumSize);
    hierarchyScroll->setWidget(left);
    Entities_ = new QListWidget(left);
    Entities_->setObjectName(QStringLiteral("worldEntities"));
    Entities_->setAccessibleName(QStringLiteral("World entities"));
    hierarchy->addWidget(Entities_, 1);
    auto* commands = new QVBoxLayout;
    Add_ = new QPushButton(QStringLiteral("Add Entity"), left);
    Add_->setObjectName(QStringLiteral("worldAdd"));
    Duplicate_ = new QPushButton(QStringLiteral("Duplicate"), left);
    Duplicate_->setObjectName(QStringLiteral("worldDuplicate"));
    Remove_ = new QPushButton(QStringLiteral("Delete"), left);
    Remove_->setObjectName(QStringLiteral("worldDelete"));
    commands->addWidget(Add_);
    commands->addWidget(Duplicate_);
    commands->addWidget(Remove_);
    hierarchy->addLayout(commands);
    BoundViewport_ = Render_->TakeViewport(split);
    Scene_ = new LevelWorkspace(tooling, this);
    Scene_->hide();
    Viewport_ = Scene_->TakeViewport(split);
    Viewport_->setObjectName(QStringLiteral("worldSceneViewport"));
    Viewport_->hide();
    connect(Scene_, &LevelWorkspace::OperationFinished, this, &WorldWorkspace::SceneCompleted);
    connect(Scene_, &LevelWorkspace::AssetsVerified, this, &WorldWorkspace::SceneVerified);
    connect(Scene_, &LevelWorkspace::ImportPreviewFinished, this, &WorldWorkspace::ShowImportPreview);
    connect(Scene_, &LevelWorkspace::DocumentChanged, this, [this]() { Changed(); });
    connect(Scene_, &LevelWorkspace::PreviewStarting, this, [this]() {
        Render_->Stop();
        // The candidate scene commits only after its first rendered frame, and the
        // native surface is created only once its widget is shown. Show the scene
        // viewport beside the current World view; Render() hides the old view after
        // the commit, or hides the candidate again if the import fails.
        Viewport_->show();
        Q_EMIT PreviewStarting();
    });
    auto* scroll = new QScrollArea(split);
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
    auto* edits = new QHBoxLayout;
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
    edits->addWidget(Components_, 1);
    edits->addWidget(Attach_);
    edits->addWidget(Apply_);
    edits->addWidget(Reset_);
    layout->addLayout(edits);
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
    connect(Entities_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (Rendering_ == false && Pending_ == false)
        {
            Selected_ = item == nullptr ? QString() : item->data(Qt::UserRole).toString();
            RenderInspector();
            Changed();
        }
    });
    connect(Add_, &QPushButton::clicked, this, [this]() {
        QString id, error;
        if (Document_.CreateEntity(mEntropy, error, QStringLiteral("Entity"), id) == false)
        {
            Changed(error);
            return;
        }
        Selected_ = foundation::Move(id);
        Render();
        Snapshot();
        Changed(QStringLiteral("Entity created. Give it a meaningful name in the inspector."));
    });
    connect(Duplicate_, &QPushButton::clicked, this, [this]() {
        QString id, error;
        if (Document_.DuplicateEntity(mEntropy, error, Selected_, id) == false)
        {
            Changed(error);
            return;
        }
        Selected_ = foundation::Move(id);
        Render();
        Snapshot();
        Changed(QStringLiteral("Duplicate created with a new stable ID."));
    });
    connect(Remove_, &QPushButton::clicked, this, [this]() {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("Delete Entity"),
            QStringLiteral("Delete %1? Undo can restore it. References from other entities must be cleared first.")
                .arg(Document_.Entity(Selected_).value(QStringLiteral("name")).toString()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
        {
            return;
        }
        QString error;
        if (Document_.RemoveEntity(Selected_, error) == false)
        {
            Changed(error);
            return;
        }
        Selected_.clear();
        Render();
        Snapshot();
        Changed(QStringLiteral("Entity deleted. Undo is available."));
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
    ValidatedScene_ = {};
    ValidatedWorld_.clear();
    SceneError_.clear();
    Importing_ = false;
    SavePending_ = false;
    SaveValidated_ = false;
    ConversionPath_.clear();
    Root_ = canonical;
    Epoch_ = epoch;
    Path_.clear();
    Selected_.clear();
    Disk_.clear();
    SchemaDisk_.clear();
    Pending_ = false;
    Unsaved_ = false;
    ++DocumentGeneration_;
    Document_.Clear();
    DemoDocument_.Clear();
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
            schema = QByteArrayLiteral("{\"version\":1,\"id\":\"ludus.world.default.v1\",\"label\":\"World\","
                                       "\"components\":[],\"assets\":[]}");
        }
        return candidate.LoadSchema(schema, error);
    }
    if (Inside(path, true) == false || Read(path, schema, 65536) == false)
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
    if (SceneBusy())
    {
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
    if (Inside(path, true) == false || Read(path, bytes, WorldDocument::MaxBytes) == false)
    {
        Changed(QStringLiteral("Open a regular world file inside the project's source folder (at most 512 KiB)."));
        return false;
    }
    const auto incoming = QJsonDocument::fromJson(bytes).object();
    const bool legacy =
        incoming.contains(QStringLiteral("schema")) == false && incoming.contains(QStringLiteral("settings"));
    const bool demo =
        legacy || incoming.value(QStringLiteral("schema")) == QJsonValue(QStringLiteral("ludus.world-demo.v1"));
    if (LoadProjectSchema(candidate, schema, demo) == false)
    {
        return false;
    }
    const auto destination = legacy ? QFileInfo(path).absolutePath() + QLatin1Char('/') +
                                          QFileInfo(path).completeBaseName() + QStringLiteral(".world.json")
                                    : path;
    if ((legacy && Inside(destination, false) == false) ||
        (legacy ? ImportWorldDemo(candidate, mEntropy, bytes, error) : candidate.Load(bytes, error)) == false)
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
        Read(path, bytes, WorldDocument::MaxBytes) == false ||
        (legacy ? ImportWorldDemo(candidate, mEntropy, bytes, error) : candidate.Load(bytes, error)) == false)
    {
        Changed(QStringLiteral("Incoming world/schema changed while opening. Retry Open World."));
        return false;
    }
    RecoveryFailed_ = false;
    StopPreview();
    ValidatedScene_ = {};
    ValidatedWorld_.clear();
    SceneError_.clear();
    ++DocumentGeneration_;
    Document_ = foundation::Move(candidate);
    ClearImportState();
    Render_->SetWorld(Root_, &Document_);
    SchemaDisk_ = foundation::Move(schema);
    Disk_ = legacy ? QByteArray() : bytes;
    Path_ = QFileInfo(destination).absoluteFilePath();
    Selected_.clear();
    Pending_ = false;
    Unsaved_ = legacy;
    Render();
    RefreshWorldDemo();
    RefreshScene();
    Changed(QStringLiteral("World opened. Select an entity or add one; recovery is restored only when requested."));
    return true;
}
bool WorldWorkspace::Create(const QString& path)
{
    if (SceneBusy())
    {
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
    ValidatedScene_ = {};
    ValidatedWorld_.clear();
    SceneError_.clear();
    ++DocumentGeneration_;
    Document_ = foundation::Move(candidate);
    ClearImportState();
    Render_->SetWorld(Root_, &Document_);
    SchemaDisk_ = foundation::Move(schema);
    Disk_.clear();
    Path_ = QFileInfo(path).absoluteFilePath();
    Selected_.clear();
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
    return Scene_ != nullptr && Scene_->Busy();
}
bool WorldWorkspace::ImportScene(const QString& source)
{
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
    if (SceneBusy() == true)
    {
        Changed(QStringLiteral("A scene operation is still running; wait for it or cancel it before importing."));
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
bool WorldWorkspace::PreviewImportScene(const QString& source)
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
    if (SceneBusy() == true)
    {
        Changed(QStringLiteral("A scene operation is still running; wait for it or cancel it before importing."));
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
void WorldWorkspace::RefreshScene()
{
    const auto level = Document_.SceneLevel();
    if (level.isEmpty())
    {
        Scene_->StopPreview();
        ValidatedScene_ = {};
    }
    else if (SceneBusy() == false && (level != ValidatedScene_ || Document_.Encode() != ValidatedWorld_))
    {
        SceneGeneration_ = DocumentGeneration_;
        Scene_->SetWorldSnapshot(Document_);
        (void)Scene_->Assemble(level);
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
            ValidatedScene_ = {};
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
        ++DocumentGeneration_;
        Render_->SetWorld(Root_, &Document_);
    }
    ValidatedScene_ = Scene_->Document();
    if (importing && conversion.isEmpty())
    {
        QString error;
        if (Document_.ImportScene(Scene_->Document(), error) == false)
        {
            ValidatedScene_ = {};
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
                      : QStringLiteral("World viewport updated; scene assets validated."));
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
    for (const auto value : Document_.Draft().value(QStringLiteral("entities")).toArray())
    {
        const auto entity = value.toObject();
        auto* item = new QListWidgetItem(entity.value(QStringLiteral("name")).toString(), Entities_);
        const auto id = entity.value(QStringLiteral("id")).toString();
        item->setData(Qt::UserRole, id);
        item->setToolTip(id);
        if (id == Selected_)
        {
            Entities_->setCurrentItem(item);
        }
    }
    if (Document_.Entity(Selected_).isEmpty())
    {
        Selected_.clear();
    }
    Rendering_ = false;
    RenderInspector();
    const bool scene = Document_.Draft().contains(QStringLiteral("scene"));
    Viewport_->setVisible(scene);
    const bool demo = IsWorldDemo(Document_);
    BoundViewport_->setVisible(scene == false && demo == false);
    Render_->setVisible(scene == false && demo == false);
    PreviewButton_->setVisible(demo);
    PlayButton_->setVisible(demo);
}
void WorldWorkspace::RenderInspector()
{
    Scene_->SetSelection(Selected_);
    Render_->SetSelection(Selected_, Pending_);
    world_demo::Name selected;
    const auto selectedBytes = Selected_.toLatin1();
    if (selectedBytes.size() < 32)
    {
        for (foundation::usize i = 0; i < static_cast<foundation::usize>(selectedBytes.size()); ++i)
        {
            selected.Text[i] = selectedBytes.constData()[i];
        }
    }
    DemoPreview_.Select(selected);
    Fields_.clear();
    Name_ = nullptr;
    while (auto* item = Inspector_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    const auto entity = Document_.Entity(Selected_);
    Components_->clear();
    if (entity.isEmpty())
    {
        auto* hint = new QLabel(
            Document_.Loaded()
                ? QStringLiteral("Select an entity. Add Entity starts with the components your project requires.")
                : QStringLiteral("No world open."));
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
    connect(Name_, &QLineEdit::textEdited, this, [this]() { Pending(); });
    auto* id = new QLabel(QStringLiteral("Stable ID: ") + Selected_);
    id->setWordWrap(true);
    id->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Inspector_->addWidget(id);
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
            for (const auto& key : {QStringLiteral("position"), QStringLiteral("rotation"), QStringLiteral("scale")})
            {
                const auto values = scene.value(key).toArray();
                for (foundation::int32 axis = 0; axis < 3; ++axis)
                {
                    const auto fieldKey = QStringLiteral("scene/") + key + QString::number(axis);
                    auto* field = new QLineEdit(Display(values[axis]), group);
                    field->setObjectName(QStringLiteral("worldField:") + fieldKey);
                    Fields_.insert(fieldKey, field);
                    connect(field, &QLineEdit::textEdited, this, [this]() { Pending(); });
                    const auto unit = key == QStringLiteral("position")   ? QStringLiteral(" (m)")
                                      : key == QStringLiteral("rotation") ? QStringLiteral(" (°)")
                                                                          : QString();
                    rows->addRow(key + QStringLiteral(" ") + QString(QChar('X' + axis)) + unit, field);
                }
            }
        }
        auto* assets = new QLabel(QStringLiteral("Mesh: %1\nMaterial: %2")
                                      .arg(scene.value(QStringLiteral("mesh")).toString(),
                                           scene.value(QStringLiteral("material")).toString()),
                                  group);
        assets->setWordWrap(true);
        assets->setTextFormat(Qt::PlainText);
        rows->addRow(assets);
        Inspector_->addWidget(group);
    }
    const auto attached = entity.value(QStringLiteral("components")).toObject();
    for (const auto definition : Document_.Schema().value(QStringLiteral("components")).toArray())
    {
        const auto component = definition.toObject();
        const auto cid = component.value(QStringLiteral("id")).toString();
        const auto label = component.value(QStringLiteral("label")).toString();
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
        for (const auto value : component.value(QStringLiteral("properties")).toArray())
        {
            const auto p = value.toObject();
            const auto pid = p.value(QStringLiteral("id")).toString();
            const auto type = p.value(QStringLiteral("type")).toString();
            const auto current = values.value(pid);
            QWidget* field = nullptr;
            if (type == QStringLiteral("boolean"))
            {
                auto* check = new QCheckBox(group);
                check->setChecked(current.toBool());
                connect(check, &QCheckBox::toggled, this, [this]() { Pending(); });
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
                        const auto key = entry.value(QStringLiteral("id")).toString();
                        choices->addItem(entry.value(type == QStringLiteral("entity") ? QStringLiteral("name")
                                                                                      : QStringLiteral("label"))
                                                 .toString() +
                                             QStringLiteral(" — ") + key,
                                         key);
                    }
                }
                choices->setCurrentIndex(choices->findData(current.toString()));
                connect(choices, &QComboBox::currentIndexChanged, this, [this]() { Pending(); });
                field = choices;
            }
            else
            {
                auto* text = new QLineEdit(Display(current), group);
                connect(text, &QLineEdit::textEdited, this, [this]() { Pending(); });
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
            rows->addRow(title, field);
            Fields_.insert(FieldKey(cid, pid), field);
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
        group->setMinimumHeight(rows->totalHeightForWidth(360));
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
                Pending();
                Changed(QStringLiteral(
                    "Tint chosen. Apply Changes validates all fields together; Reset Fields cancels it."));
            });
    picker->open();
}
void WorldWorkspace::Pending()
{
    DemoPreview_.Stop(); // A focused inspector buffer may not race a keyboard gesture.
    Pending_ = true;
    DraftTimer_.start();
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
    const bool busy = SceneBusy() || DemoDocument_.Previewing();
    if (Name_ != nullptr)
    {
        Name_->setEnabled(busy == false || DraftPreview_);
    }
    for (auto* field : Fields_)
    {
        field->setEnabled(busy == false || DraftPreview_);
    }
    Entities_->setEnabled(Pending_ == false && busy == false);
    Import_->setEnabled(Document_.Loaded() && IsWorldDemo(Document_) == false && Pending_ == false && busy == false);
    PreviewButton_->setEnabled(Document_.Loaded() && busy == false);
    PlayButton_->setEnabled(Document_.Loaded() && busy == false);
    CancelScene_->setVisible(busy);
    CancelScene_->setEnabled(Scene_->CanCancel());
    Add_->setEnabled(Document_.Loaded() && Pending_ == false && Entities_->count() < 128 && busy == false);
    Duplicate_->setEnabled(Selected_.isEmpty() == false && Pending_ == false && Entities_->count() < 128 &&
                           busy == false);
    Remove_->setEnabled(Selected_.isEmpty() == false && Pending_ == false && busy == false);
    Attach_->setEnabled(Selected_.isEmpty() == false && Pending_ == false && Components_->count() > 0 && busy == false);
    Components_->setEnabled(Pending_ == false && busy == false);
    Apply_->setEnabled(Pending_ && busy == false);
    Reset_->setEnabled(Pending_ && busy == false);
    Recovery_->setEnabled(Document_.Loaded() && busy == false && Dirty() == false && Unsaved_ == false &&
                          QFileInfo::exists(Path_ + QStringLiteral(".ludus-recovery")));
    PathLabel_->setText(Path_.isEmpty()
                            ? QStringLiteral("No world open")
                            : (Dirty() ? QStringLiteral("● ") : QString()) + QDir(Root_).relativeFilePath(Path_));
    Render_->SetSelection(Selected_, Pending_);
    Q_EMIT DocumentChanged();
}
bool WorldWorkspace::ReadDraft(WorldDocument& document, QString& error, QWidget*& invalid)
{
    auto components = Document_.Entity(Selected_).value(QStringLiteral("components")).toObject();
    for (auto c = components.begin(); c != components.end(); ++c)
    {
        auto values = c.value().toObject();
        for (const auto definition : Document_.Component(c.key()).value(QStringLiteral("properties")).toArray())
        {
            const auto p = definition.toObject();
            const auto pid = p.value(QStringLiteral("id")).toString();
            auto* field = Fields_.value(FieldKey(c.key(), pid));
            QJsonValue candidate;
            if (auto* check = qobject_cast<QCheckBox*>(field))
            {
                candidate = check->isChecked();
            }
            else if (auto* choice = qobject_cast<QComboBox*>(field))
            {
                candidate = choice->currentData().toString();
            }
            else if (auto* text = qobject_cast<QLineEdit*>(field))
            {
                // Preserve unchanged JSON numbers exactly rather than round them
                // through the float32 text converter during an unrelated edit.
                if (text->text() == Display(values.value(pid)))
                {
                    candidate = values.value(pid);
                }
                else if (WorldDocument::ParseValue(p, text->text(), candidate, error) == false)
                {
                    invalid = text;
                    return false;
                }
            }
            else
            {
                error = QStringLiteral("Inspector no longer matches the selected entity.");
                return false;
            }
            values.insert(pid, candidate);
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
                    error = QStringLiteral("Scene inspector is stale.");
                    return false;
                }
                if (field->text() == Display(values[axis]))
                {
                    continue;
                }
                const QJsonObject property{{QStringLiteral("type"), QStringLiteral("number")},
                                           {QStringLiteral("label"), key},
                                           {QStringLiteral("min"),
                                            key == QStringLiteral("scale")      ? 0.000001
                                            : key == QStringLiteral("rotation") ? -360
                                                                                : -1000000},
                                           {QStringLiteral("max"), key == QStringLiteral("rotation") ? 360 : 1000000}};
                QJsonValue value;
                if (WorldDocument::ParseValue(property, field->text(), value, error) == false)
                {
                    invalid = field;
                    return false;
                }
                values[axis] = value;
            }
            scene.insert(key, values);
        }
    }
    if (document.Edit(Selected_, components, Name_->text(), error, scene.isEmpty() ? nullptr : &scene) == false)
    {
        return false;
    }
    return true;
}
void WorldWorkspace::PreviewDraft()
{
    if (Document_.Loaded() == false)
    {
        return;
    }
    if (SceneBusy())
    {
        DraftTimer_.start();
        return;
    }
    if (IsWorldDemo(Document_))
    {
        return;
    }
    auto draft = Document_;
    QString error;
    QWidget* invalid = nullptr;
    if (Pending_ && ReadDraft(draft, error, invalid) == false)
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
QJsonObject WorldWorkspace::Session() const
{
    return {{QStringLiteral("world"), Path_.isEmpty() ? QString() : QDir(Root_).relativeFilePath(Path_)},
            {QStringLiteral("selection"), Selected_},
            {QStringLiteral("splitter"), QString::fromLatin1(Split_->saveState().toBase64())},
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
        (void)Split_->restoreState(bytes);
    }
    const auto selected = state.value(QStringLiteral("selection")).toString();
    if (Document_.Entity(selected).isEmpty() == false)
    {
        Selected_ = selected;
        Render();
    }
}
bool WorldWorkspace::Apply()
{
    if (SceneBusy() || DemoDocument_.Previewing())
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
    if (ReadDraft(candidate, error, invalid) == false)
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
        if (SceneBusy() == false)
        {
            SceneGeneration_ = DocumentGeneration_;
            Scene_->SetWorldSnapshot(Document_);
            (void)Scene_->VerifyAssets(level);
        }
        if (SceneBusy() == false)
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
                  : (Read(Path_, current, WorldDocument::MaxBytes) == false || current != Disk_)))
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
    RefreshWorldDemo();
    RefreshScene();
    Render_->WorldChanged();
    RecoveryFailed_ = Document_.Dirty() && Unsaved_ == false;
    if (Unsaved_ == false && Document_.Dirty() == false && Inside(Path_, true))
    {
        QByteArray old;
        if (Read(Path_ + QStringLiteral(".ludus-recovery"), old, WorldDocument::MaxBytes + 256) &&
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
    if (lock.tryLock(0) == false || Read(Path_, disk, WorldDocument::MaxBytes) == false || disk != Disk_)
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
    if (SceneBusy() || Dirty() || Unsaved_ || Inside(Path_, true) == false ||
        Read(Path_, current, WorldDocument::MaxBytes) == false || current != Disk_ ||
        LoadProjectSchema(definition, schema, IsWorldDemo(Document_)) == false || schema != SchemaDisk_ ||
        Read(Path_ + QStringLiteral(".ludus-recovery"), envelope, WorldDocument::MaxBytes + 256) == false ||
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
    Render();
    RefreshWorldDemo();
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
        return requested && SceneBusy() == false && SavePending_ == false && Dirty() == false;
    }
    if (answer != QMessageBox::Discard)
    {
        return false;
    }
    Pending_ = false;
    Document_.Discard();
    DemoPreview_.Stop();
    RefreshWorldDemo();
    RefreshScene();
    Render_->WorldChanged();
    // Keep a new world marked unsaved until its caller closes/replaces it.
    // Discard only author-owned recovery for the same baseline, never symlinks.
    QByteArray envelope;
    if (Read(Path_ + QStringLiteral(".ludus-recovery"), envelope, WorldDocument::MaxBytes + 256) &&
        envelope.startsWith(RecoveryHeader()))
    {
        (void)QFile::remove(Path_ + QStringLiteral(".ludus-recovery"));
    }
    Render();
    Changed(QStringLiteral("Changes discarded."));
    return true;
}
RenderPreview& WorldWorkspace::GetRenderPreview() noexcept
{
    return Render_->Preview();
}
bool WorldWorkspace::StartPreview()
{
    if (IsWorldDemo(Document_) == false || Apply() == false)
    {
        return false;
    }
    RefreshWorldDemo();
    world_demo::Level projection;
    QString error;
    if (ProjectWorldDemo(Document_, projection, error) == false)
    {
        Changed(error);
        return false;
    }
    if (DemoDocument_.Loaded() == false)
    {
        Changed(QStringLiteral("The World preview projection could not be prepared. Authoring edits were retained."));
        return false;
    }
    Render_->Stop();
    Scene_->StopPreview();
    Q_EMIT PreviewStarting();
    return DemoPreview_.Start();
}
void WorldWorkspace::RefreshWorldDemo()
{
    if (IsWorldDemo(Document_) == false || DemoSyncing_)
    {
        return;
    }
    world_demo::Level projection;
    QString error;
    if (ProjectWorldDemo(Document_, projection, error) == false)
    {
        DemoPreview_.Stop();
        DemoDocument_.Clear();
        return;
    }
    foundation::core::Array<char> bytes;
    if (world_demo::WriteLevel(projection, bytes) == false)
    {
        DemoPreview_.Stop();
        DemoDocument_.Clear();
        return;
    }
    DemoSyncing_ = true;
    const auto result = DemoDocument_.Load(bytes.GetData(), bytes.GetSize());
    DemoSyncing_ = false;
    if (result.Error != world_demo::LevelError::None)
    {
        DemoPreview_.Stop();
        DemoDocument_.Clear();
    }
}
void WorldWorkspace::WorldDemoChanged()
{
    if (DemoSyncing_ || IsWorldDemo(Document_) == false)
    {
        return;
    }
    const auto selected = QString::fromLatin1(DemoPreview_.Selected().Text);
    if (DemoDocument_.Previewing() == false && DemoDocument_.Dirty())
    {
        QString error;
        if (Pending_ == false && CommitWorldDemoGesture(Document_, DemoDocument_.Draft(), error))
        {
            Snapshot();
        }
        else
        {
            RefreshWorldDemo();
            Changed(error);
            return;
        }
    }
    if (Pending_ == false && DemoDocument_.Previewing() == false)
    {
        Selected_ = selected;
        Render();
    }
    Changed(DemoPreview_.Status());
}
bool WorldWorkspace::PreviewRunning() const noexcept
{
    return DemoPreview_.Running() || Render_->Preview().Running() || Scene_->PreviewRunning();
}
void WorldWorkspace::ResumePreview()
{
    if (Document_.Loaded() == false || SceneBusy())
    {
        return;
    }
    if (IsWorldDemo(Document_))
    {
        (void)StartPreview();
    }
    else if (Document_.SceneLevel().isEmpty())
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
    DemoPreview_.Stop();
    Render_->Stop();
    Scene_->StopPreview();
}
void WorldWorkspace::Undo()
{
    if (Pending_ == false && DemoDocument_.Previewing() == false && SceneBusy() == false && Document_.Undo())
    {
        Render();
        Snapshot();
        Changed(QStringLiteral("World edit undone."));
    }
}
void WorldWorkspace::Redo()
{
    if (Pending_ == false && DemoDocument_.Previewing() == false && SceneBusy() == false && Document_.Redo())
    {
        Render();
        Snapshot();
        Changed(QStringLiteral("World edit redone."));
    }
}
} // namespace ludus::editor
