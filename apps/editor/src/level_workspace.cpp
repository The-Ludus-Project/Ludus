#include "internal/level_workspace.h"
#include "internal/item_selection.h"

#include "internal/editor_identity.h"
#include "internal/import_report.h"
#include "internal/preview_viewport.h"
#include <QGuiApplication>
#include <ludus/foundation/base/core.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/platform/entropy.h>

#include <span>
#include <string_view>

namespace ludus::editor
{
namespace
{
#if !defined(Q_OS_WASM)
// Editor CPU budget for decoding scene import textures, excluding the operation thread,
// which helps in Wait. The user waits on an import, but Qt, RHI submission and a preview
// bake (STATIC_LIGHTING_WORKER_COUNT workers) keep running beside it.
constexpr foundation::uint32 SCENE_IMPORT_WORKER_COUNT = 4;
#endif
} // namespace

LevelWorkspace::LevelWorkspace(const ToolingPaths& tooling, QWidget* parent) : QWidget(parent), Tooling_(tooling)
{
    setObjectName(QStringLiteral("levelWorkspace"));
#if !defined(Q_OS_WASM)
    // Without workers (startup failure) imports decode their textures serially.
    mHasImportJobs =
        mImportJobs.Initialize({ .WorkerCount = SCENE_IMPORT_WORKER_COUNT }) == foundation::threading::JobStatus::Ok;
#endif
    auto* layout = new QVBoxLayout(this);
    auto* actions = new QHBoxLayout;
    New_ = new QPushButton(QStringLiteral("New 3D scene"), this);
    Open_ = new QPushButton(QStringLiteral("Open 3D scene"), this);
    Import_ = new QPushButton(QStringLiteral("Import reference scene"), this);
    Save_ = new QPushButton(QStringLiteral("Save scene"), this);
    Cancel_ = new QPushButton(QStringLiteral("Cancel import"), this);
    for (auto* button : {New_, Open_, Import_, Save_, Cancel_})
    {
        actions->addWidget(button);
    }
    layout->addLayout(actions);
    File_ = new QLabel(this);
    Status_ = new QLabel(this);
    Status_->setObjectName(QStringLiteral("levelStatus"));
    Status_->setWordWrap(true);
    layout->addWidget(File_);
    auto* body = new QHBoxLayout;
    Objects_ = new QListWidget(this);
    ConfigureItemSelection(*Objects_);
    Objects_->setObjectName(QStringLiteral("levelObjects"));
    body->addWidget(Objects_, 1);
    Viewport_ = new PreviewViewport(&Preview_, this, false);

    Viewport_->setObjectName(QStringLiteral("levelViewport"));
    body->addWidget(Viewport_, 2);
    layout->addLayout(body, 1);
    auto* inspector = new QHBoxLayout;
    Name_ = new QLineEdit(this);
    Name_->setPlaceholderText(QStringLiteral("Object name"));
    inspector->addWidget(Name_);
    for (auto*& translation : Translation_)
    {
        translation = new QDoubleSpinBox(this);
        translation->setRange(-1000000, 1000000);
        translation->setDecimals(4);
        inspector->addWidget(translation);
        connect(translation, &QDoubleSpinBox::editingFinished, this, &LevelWorkspace::Edit);
    }
    Remove_ = new QPushButton(QStringLiteral("Remove object"), this);
    inspector->addWidget(Remove_);
    layout->addLayout(inspector);
    layout->addWidget(Status_);
    Warnings_ = new QLabel(this);
    Warnings_->setTextFormat(Qt::PlainText);
    Warnings_->setWordWrap(true);
    layout->addWidget(Warnings_);
    connect(New_, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getSaveFileName(this,
                                                       QStringLiteral("New 3D scene"),
                                                       QDir(Root_).filePath(QStringLiteral("scenes/main.lulevel")),
                                                       QStringLiteral("Ludus levels (*.lulevel)"));
        if (path.isEmpty() == false)
        {
            (void)Create(path);
        }
    });
    connect(Open_, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getOpenFileName(this,
                                                       QStringLiteral("Open 3D scene"),
                                                       Root_,
                                                       QStringLiteral("Ludus levels (*.lulevel)"));
        if (path.isEmpty() == false)
        {
            (void)Open(path);
        }
    });
    connect(Import_, &QPushButton::clicked, this, [this]() {
        const QString file =
            QFileDialog::getOpenFileName(this,
                                         QStringLiteral("Import scene"),
                                         {},
                                         QStringLiteral("Scenes (*.pbrt *.gltf *.glb *.obj);;PBRT v4 (*.pbrt);;glTF "
                                                        "2.0 (*.gltf *.glb);;Wavefront OBJ (*.obj)"));
        if (file.isEmpty() == false)
        {
            PreviewSource_ = file;
            (void)PreviewImport(file);
        }
    });
    // Confirm a previewed import: show the dry-run summary, then import on request.
    connect(this, &LevelWorkspace::ImportPreviewFinished, this, [this](bool isAccepted) {
        const QString source = PreviewSource_;
        PreviewSource_.clear();
        if (isAccepted == false || source.isEmpty() == true)
        {
            return;
        }
        QMessageBox* confirm = new QMessageBox(QMessageBox::Question,
                                               QStringLiteral("Import preview"),
                                               DescribeImportPreview(ImportPreview_),
                                               QMessageBox::Ok | QMessageBox::Cancel,
                                               this);
        confirm->setObjectName(QStringLiteral("levelImportPreview"));
        confirm->button(QMessageBox::Ok)->setText(QStringLiteral("Import"));
        confirm->setAttribute(Qt::WA_DeleteOnClose);
        connect(confirm, &QMessageBox::accepted, this, [this, source]() { (void)ImportPreviewed(source); });
        confirm->open();
    });
    connect(Save_, &QPushButton::clicked, this, [this]() { (void)Save(); });
    connect(Cancel_, &QPushButton::clicked, this, &LevelWorkspace::Cancel);
    connect(Objects_, &QListWidget::currentRowChanged, this, &LevelWorkspace::Inspect);
    connect(Name_, &QLineEdit::editingFinished, this, &LevelWorkspace::Edit);
    connect(Remove_, &QPushButton::clicked, this, [this]() {
        const auto index = Objects_->currentRow();
        if (Busy_ || index < 0)
        {
            return;
        }
        auto objects = Level_.value(QStringLiteral("objects")).toArray();
        const auto id = objects[index].toObject().value(QStringLiteral("id")).toString();
        for (const auto& object : objects)
        {
            if (object.toObject().value(QStringLiteral("parent")).toString() == id)
            {
                Status_->setText(QStringLiteral("Remove child objects before removing their parent."));
                return;
            }
        }
        auto candidate = Level_;
        objects.removeAt(index);
        candidate.insert(QStringLiteral("objects"), objects);
        Candidate_ = foundation::Move(candidate);
        (void)Start(QStringLiteral("assemble"));
    });
    connect(&Preview_, &RenderPreview::Changed, this, [this]() {
        if (Busy_ == false)
        {
            Status_->setText(Preview_.Status());
        }
    });
    connect(&Preview_, &RenderPreview::Failed, this, [this](const QString& error) {
        if (AwaitingViewport_)
        {
            Rollback(error);
        }
    });
    connect(&Preview_, &RenderPreview::Stopped, this, [this]() {
        // A viewport that stops before its first frame (no GPU sharing, hidden or
        // failed surface) must not leave the operation waiting forever.
        // A failure emits Stopped during teardown before its specific Failed signal.
        // Defer the generic stop fallback so the failure keeps its diagnostic.
        QTimer::singleShot(0, this, [this]() {
            if (AwaitingViewport_ == true && Preview_.Running() == false)
            {
                Rollback(QStringLiteral("The viewport stopped before rendering the candidate scene."));
            }
        });
    });
    connect(&Preview_, &RenderPreview::FrameReady, this, [this]() {
        if (PublishingCatalog_)
        {
            return;
        }
        if (AwaitingViewport_)
        {
            AwaitingViewport_ = false;
            if (Operation_ == QStringLiteral("import"))
            {
                CommitImport();
                return;
            }
            CommitCandidate();
        }
        Viewport_->ShowFrame();
    });
    connect(&Decode_, &QThread::finished, this, &LevelWorkspace::Publish);
    Render();
}
LevelWorkspace::~LevelWorkspace()
{
    StopPreview();
    disconnect(&Decode_, nullptr, this, nullptr);
    CancelRequested_.store(true);
    WaitForOperation();
    Decode_.quit();
    (void)Decode_.wait();
}
void LevelWorkspace::SetProject(const QString& root, foundation::uint64 epoch)
{
    if (Epoch_ == epoch && Root_ == root)
    {
        return;
    }
    if (Busy_)
    {
        PendingRoot_ = root;
        PendingEpoch_ = epoch;
        ProjectPending_ = true;
        Cancel();
        return;
    }
    StopPreview();
    (void)Preview_.SetScene({});
    Viewport_->ClearFrame(QStringLiteral("Import a reference scene to render it here."));
    Warnings_->clear();
    Scene_ = {};
    Candidate_ = {};
    PreviousScene_ = {};
    Root_ = root;
    Epoch_ = epoch;
    Level_ = {};
    Path_.clear();
    Disk_.clear();
    Dirty_ = false;
    Status_->setText(
        QStringLiteral("New/open a 3D scene, then import a reference scene. Assets are stored in this project."));
    Render();
}
bool LevelWorkspace::Relative(const QString& path, QString& relative) const
{
    if (Root_.isEmpty())
    {
        return false;
    }
    const auto absolute = QFileInfo(path).absoluteFilePath();
    relative = QDir(Root_).relativeFilePath(absolute);
    auto parent = QFileInfo(absolute).absolutePath();
    while (QFileInfo::exists(parent) == false)
    {
        parent = QFileInfo(parent).absolutePath();
    }
    const auto realParent = QFileInfo(parent).canonicalFilePath();
    const auto realRoot = QFileInfo(Root_).canonicalFilePath();
    return relative.startsWith(QStringLiteral("../")) == false && QDir::isAbsolutePath(relative) == false &&
           absolute.endsWith(QStringLiteral(".lulevel")) &&
           (realParent == realRoot || realParent.startsWith(realRoot + QLatin1Char('/')));
}
bool LevelWorkspace::Create(const QString& path)
{
    QString relative;
    if (Busy_)
    {
        return false;
    }
    if (Relative(path, relative) == false || QFileInfo::exists(path))
    {
        Status_->setText(QStringLiteral("Choose a new .lulevel path inside this project, or open the existing scene."));
        return false;
    }
    QString levelId;
    if (TryGenerateUuidText(mEntropy, levelId) == false)
    {
        Status_->setText(DescribeEntropyUnavailable());
        return false;
    }
    if (ConfirmDiscard() == false)
    {
        return false;
    }
    StopPreview();
    (void)Preview_.SetScene({});
    Viewport_->ClearFrame(QStringLiteral("Import a reference scene to render it here."));
    Warnings_->clear();
    Level_ = QJsonObject{{QStringLiteral("version"), 1},
                         {QStringLiteral("id"), levelId},
                         {QStringLiteral("name"), QFileInfo(path).completeBaseName()},
                         {QStringLiteral("profile"), QStringLiteral("resident-reference-v1")},
                         {QStringLiteral("objects"), QJsonArray{}},
                         {QStringLiteral("imports"), QJsonArray{}},
                         {QStringLiteral("camera"),
                          QJsonObject{{QStringLiteral("eye"), QJsonArray{0, 0, 3}},
                                      {QStringLiteral("target"), QJsonArray{0, 0, 0}},
                                      {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                                      {QStringLiteral("fov"), 1.0471975512}}}};
    Path_ = foundation::Move(relative);
    Disk_.clear();
    Dirty_ = true;
    Render();
    return true;
}
bool LevelWorkspace::Open(const QString& path)
{
    QString relative;
    if (Busy_ || Relative(path, relative) == false || ConfirmDiscard() == false)
    {
        return false;
    }
    return Start(QStringLiteral("open"), relative);
}
bool LevelWorkspace::Import(const QString& source)
{
    if (Busy_ || Level_.isEmpty())
    {
        return false;
    }
    const ImportSource split = SplitImportSource(source);
    SourceEntry_ = split.Entry;
    const bool isStarted = Start(QStringLiteral("import"), {}, split.Directory);
    ExpectedSourceDigest_.clear();
    return isStarted;
}
bool LevelWorkspace::ImportPreviewed(const QString& source)
{
    const QString digest = ImportPreview_.value(QStringLiteral("source_digest")).toString();
    if (digest.isEmpty() == true)
    {
        return false;
    }
    ExpectedSourceDigest_ = digest;
    return Import(source);
}
bool LevelWorkspace::PreviewImport(const QString& source)
{
    if (Busy_ == true || Root_.isEmpty() == true || isOperationActive() == true)
    {
        return false;
    }
    const ImportSource split = SplitImportSource(source);
    SourceEntry_ = split.Entry;
    Source_ = split.Directory;
    Operation_ = QStringLiteral("preview-import");
    Error_.clear();
    Cancelling_ = false;
    ImportPreview_ = {};
    if (Run({Operation_, {}, Source_, QJsonValue(QJsonValue::Null)}) == false)
    {
        Status_->setText(QStringLiteral("Could not start the import preview; current scene retained."));
        return false;
    }
    Busy_ = true;
    Status_->setText(QStringLiteral("Planning the import (dry run)… Previous viewport remains available."));
    Render();
    return true;
}
QWidget* LevelWorkspace::TakeViewport(QWidget* parent)
{
    WorldOwned_ = true;
    Dirty_ = false;
    Viewport_->setParent(parent);
    return Viewport_;
}
void LevelWorkspace::ShowSceneFailure(const QString& error)
{
    if (Preview_.FrameAvailable() == false && Viewport_->HasRetainedGraphFrame() == false)
    {
        Viewport_->ClearFrame(QStringLiteral("Scene unavailable: ") + error);
    }
}
QString LevelWorkspace::StatusText() const
{
    return Status_->text();
}
bool LevelWorkspace::VerifyAssets(const QJsonObject& level)
{
    if (Busy_ || level.isEmpty())
    {
        return false;
    }
    VerificationOnly_ = true;
    Candidate_ = level;
    if (Start(QStringLiteral("assemble")) == false)
    {
        VerificationOnly_ = false;
        return false;
    }
    return true;
}
bool LevelWorkspace::Assemble(const QJsonObject& level)
{
    if (Busy_ || level.isEmpty())
    {
        return false;
    }
    VerificationOnly_ = false;
    Candidate_ = level;
    return Start(QStringLiteral("assemble"));
}
bool LevelWorkspace::ImportInto(const QJsonObject& level, const QString& source)
{
    if (Busy_ || level.isEmpty())
    {
        return false;
    }
    // Start(import) copies Level_. Retain the authoritative document for this service operation.
    Level_ = level;
    return Import(source);
}
bool LevelWorkspace::ImportPreviewedInto(const QJsonObject& level, const QString& source)
{
    if (Busy_ == true || level.isEmpty() == true)
    {
        return false;
    }
    Level_ = level;
    return ImportPreviewed(source);
}
bool LevelWorkspace::SetImportEnvironmentFile(const QString& file)
{
    if (Busy_ == true || isOperationActive() == true)
    {
        return false;
    }
    mImportEnvironmentFile = file;
    ExpectedSourceDigest_.clear();
    ImportPreview_ = {};
    return true;
}
void LevelWorkspace::ClearImportState()
{
    PreviewSource_.clear();
    mImportEnvironmentFile.clear();
    ImportPreview_ = {};
    ImportReports_ = {};
    PendingImportReports_ = {};
}
bool LevelWorkspace::Save()
{
    if (CanSave() == false)
    {
        return false;
    }
    return Start(QStringLiteral("save"), Path_);
}
bool LevelWorkspace::Start(const QString& operation, const QString& path, const QString& source)
{
    if (Busy_ == true || Root_.isEmpty() == true || isOperationActive() == true)
    {
        return false;
    }
    if (operation != QStringLiteral("assemble"))
    {
        Candidate_ = Level_;
    }
    Error_.clear();
    Scene_ = {};
    Success_ = false;
    Cancelling_ = false;
    Source_ = source;
    Operation_ = operation;
    CandidatePath_ = path;
    const QJsonValue expected = Disk_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(Disk_);
    if (Run({operation, path, source, expected}) == false)
    {
        Status_->setText(QStringLiteral("Could not start the scene operation; current scene retained."));
        return false;
    }
    Busy_ = true;
    Status_->setText(QStringLiteral("Validating scene assets… Previous viewport remains available."));
    Render();
    return true;
}
namespace
{
std::span<const foundation::uint8> ToSpan(const QByteArray& bytes) noexcept
{
    return {reinterpret_cast<const foundation::uint8*>(bytes.constData()),
            static_cast<foundation::usize>(bytes.size())};
}

std::string_view ToView(const QByteArray& bytes) noexcept
{
    return {bytes.constData(), static_cast<foundation::usize>(bytes.size())};
}

bool IsCancelRequested(void* context) noexcept
{
    return static_cast<const std::atomic<bool>*>(context)->load() == true;
}

QString DescribeStage(std::string_view stage)
{
    if (stage == "import")
    {
        return QStringLiteral("Importing reference scene… Previous viewport remains available.");
    }
    if (stage == "preview")
    {
        return QStringLiteral("Planning the import (dry run)… Nothing is written.");
    }
    if (stage == "commit")
    {
        return QStringLiteral("Committing prepared scene assets…");
    }
    if (stage == "save")
    {
        return QStringLiteral("Saving scene…");
    }
    if (stage == "assemble")
    {
        return QStringLiteral("Assembling scene assets…");
    }
    return {};
}
} // namespace
bool LevelWorkspace::Run(const SceneOperationRequest& request)
{
    const QString& operation = request.Operation;
    const QString& path = request.Path;
    const QString& source = request.Source;
    const QJsonValue& expected = request.Expected;
    using namespace ludus::content::scene_cook;
    EditorSceneOperation kind = EditorSceneOperation::Assemble;
    if (operation == QStringLiteral("open"))
    {
        kind = EditorSceneOperation::Open;
    }
    else if (operation == QStringLiteral("import"))
    {
        kind = EditorSceneOperation::Import;
    }
    else if (operation == QStringLiteral("commit-import"))
    {
        kind = EditorSceneOperation::CommitImport;
    }
    else if (operation == QStringLiteral("preview-import"))
    {
        kind = EditorSceneOperation::PreviewImport;
    }
    else if (operation == QStringLiteral("save"))
    {
        kind = EditorSceneOperation::Save;
    }
    else if (operation != QStringLiteral("assemble"))
    {
        return false;
    }
    // The worker owns copies of every input; the widget only reads the results
    // after the thread has finished.
    QByteArray level = QJsonDocument(Candidate_).toJson(QJsonDocument::Compact);
    if (level.size() > foundation::int64{1024} * 1024)
    {
        return false;
    }
    QByteArray root = Root_.toUtf8();
    QByteArray relative = path.toUtf8();
    QByteArray from = source.toUtf8();
    struct SceneImportInputs final
    {
        QByteArray Entry;
        QByteArray Environment;
    };
    const SceneImportInputs files{SourceEntry_.toUtf8(), mImportEnvironmentFile.toUtf8()};
    QByteArray expectedSource = kind == EditorSceneOperation::Import ? ExpectedSourceDigest_.toUtf8() : QByteArray();
    QByteArray digest = expected.isString() ? expected.toString().toUtf8() : QByteArray();
    QByteArray catalog = kind == EditorSceneOperation::CommitImport ? CandidateCatalog_ : QByteArray();
    const bool hasExpected = expected.isString();
    Response_.clear();
    SceneBytes_.clear();
    OperationError_.clear();
    if (kind != EditorSceneOperation::CommitImport)
    {
        CandidateCatalog_.clear();
    }
    CancelRequested_.store(false);
    const auto work = [this, kind, hasExpected, level, root, relative, from, files, expectedSource, digest, catalog]() {
        EditorSceneProgress progress;
        progress.ContextOrNull = this;
        progress.Report = [](void* context, std::string_view stage) noexcept {
            LevelWorkspace* self = static_cast<LevelWorkspace*>(context);
            const QString text = DescribeStage(stage);
            if (text.isEmpty() == false)
            {
                QMetaObject::invokeMethod(
                    self,
                    [self, text]() {
                        if (self->Busy_ == true && self->Cancelling_ == false)
                        {
                            self->Status_->setText(text);
                        }
                    },
                    Qt::QueuedConnection);
            }
        };
        Cancellation cancel;
        cancel.Requested = &IsCancelRequested;
        cancel.ContextOrNull = &CancelRequested_;
        // SetEntropySource() is refused while an operation runs, so this read does not race.
        const foundation::identity::EntropySource entropy = mEntropy;
        AssetOptions options;
        options.CancelOrNull = &cancel;
        options.EntropyOrNull = &entropy;
        options.SourceEntry = ToView(files.Entry);
        options.EnvironmentFile = ToView(files.Environment);
        options.ExpectedSourceDigest = ToView(expectedSource);
        options.JobsOrNull = mHasImportJobs == true ? &mImportJobs : nullptr;
        EditorSceneRequest request;
        request.Operation = kind;
        request.Root = ToView(root);
        request.Path = ToView(relative);
        request.Source = ToView(from);
        request.Level = kind == EditorSceneOperation::Open ? std::span<const foundation::uint8>() : ToSpan(level);
        request.Catalog = ToSpan(catalog);
        request.HasExpected = hasExpected;
        request.Expected = ToView(digest);
        request.ProgressOrNull = &progress;
        EditorSceneResult result;
        AssetError error;
        if (OperateEditorScene(request, options, result, error))
        {
            Response_ = QByteArray(reinterpret_cast<const char*>(result.Response.GetData()),
                                   static_cast<qsizetype>(result.Response.GetSize()));
            SceneBytes_ = QByteArray(reinterpret_cast<const char*>(result.Scene.GetData()),
                                     static_cast<qsizetype>(result.Scene.GetSize()));
            if (kind == EditorSceneOperation::Import)
            {
                CandidateCatalog_ = QByteArray(reinterpret_cast<const char*>(result.Catalog.GetData()),
                                               static_cast<qsizetype>(result.Catalog.GetSize()));
            }
        }
        else
        {
            const std::string_view message = error.Message.View();
            OperationError_ = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
        }
    };
#if defined(Q_OS_WASM)
    // The browser Editor is single-threaded: run the cook in-process on the GUI thread after
    // the status text paints. Progress text from the cook is applied when the operation ends.
    constexpr int BROWSER_PAINT_DELAY_MILLISECONDS = 50;
    mIsOperationQueued = true;
    QTimer::singleShot(BROWSER_PAINT_DELAY_MILLISECONDS, this, [this, work]() {
        work();
        mIsOperationQueued = false;
        Completed();
    });
#else
    Operate_ = QThread::create(work);
    connect(Operate_, &QThread::finished, this, [this]() {
        Operate_->deleteLater();
        Operate_ = nullptr;
        Completed();
    });
    Operate_->start();
#endif
    return true;
}
void LevelWorkspace::WaitForOperation()
{
    if (Operate_ != nullptr)
    {
        disconnect(Operate_, nullptr, this, nullptr);
        (void)Operate_->wait();
        delete Operate_;
        Operate_ = nullptr;
    }
}
void LevelWorkspace::Completed()
{
    const QJsonObject result = QJsonDocument::fromJson(Response_).object();
    const bool succeeded = OperationError_.isEmpty() && result.value(QStringLiteral("ok")).toBool();
    if (Operation_ == QStringLiteral("preview-import"))
    {
        const bool isAccepted =
            Cancelling_ == false && succeeded == true && result.value(QStringLiteral("schema")).toInt() == 1;
        Busy_ = false;
        if (isAccepted == true)
        {
            ImportPreview_ = result;
            Status_->setText(QStringLiteral("Import preview ready; nothing was written."));
        }
        else
        {
            Status_->setText(Cancelling_ == true                  ? QStringLiteral("Import preview cancelled.")
                             : OperationError_.isEmpty() == false ? OperationError_
                                                                  : QStringLiteral("Import preview failed."));
        }
        Cancelling_ = false;
        Render();
        Q_EMIT ImportPreviewFinished(isAccepted);
        ApplyPendingProject();
        return;
    }
    if (PublishingCatalog_)
    {
        PublishingCatalog_ = false;
        if (result.value(QStringLiteral("schema")).toInt() == 1 && result.value(QStringLiteral("ok")).toBool() &&
            result.value(QStringLiteral("committed")).toBool() && succeeded)
        {
            Cancelling_ = false; // Publication already crossed its atomic commit point.
            Viewport_->ShowFrame();
            CommitCandidate();
        }
        else
        {
            Rollback(OperationError_.isEmpty() ? QStringLiteral("Could not publish the prepared catalog.")
                                               : OperationError_);
        }
        return;
    }
    if (Cancelling_ || succeeded == false || result.value(QStringLiteral("schema")).toInt() != 1)
    {
        Error_ =
            Cancelling_ ? QStringLiteral("Import cancelled; current scene and saved level retained.") : OperationError_;
        if (Error_.isEmpty())
        {
            Error_ = QStringLiteral("Scene operation failed; current scene retained.");
        }
        Publish();
        return;
    }
    PendingImportReports_ = result.value(QStringLiteral("import_reports")).toObject();
    Candidate_ = result.value(QStringLiteral("level")).toObject();
    CandidateRevisions_ = result.value(QStringLiteral("artifacts")).toObject();
    CatalogExpected_ = result.value(QStringLiteral("catalog_expected"));
    QStringList warnings;
    for (const auto& warning : result.value(QStringLiteral("warnings")).toArray())
    {
        warnings.append(warning.toString());
    }
    PendingWarnings_ = warnings.join(QLatin1Char('\n'));
    if (Operation_ == QStringLiteral("save") || Operation_ == QStringLiteral("open"))
    {
        CandidateDisk_ = result.value(QStringLiteral("disk")).toString();
    }
#if defined(Q_OS_WASM)
    QTimer::singleShot(0, this, [this]() {
        decodeCandidate();
        Publish();
    });
#else
    auto* worker = new QObject;
    worker->moveToThread(&Decode_);
    connect(&Decode_, &QThread::started, worker, [this]() {
        decodeCandidate();
        Decode_.quit();
    });
    connect(&Decode_, &QThread::finished, worker, &QObject::deleteLater);
    Decode_.start();
#endif
}
void LevelWorkspace::decodeCandidate()
{
    if (SceneBytes_.size() > static_cast<qsizetype>(content::scene_format::IMPORT_LIMITS.BundleBytes))
    {
        Error_ = QStringLiteral("Cooked scene exceeds 384 MiB.");
    }
    else
    {
        const QByteArray& bytes = SceneBytes_;
        Success_ = bytes.isEmpty() && Candidate_.value(QStringLiteral("objects")).toArray().isEmpty();
        if (Success_ == false)
        {
            Success_ = RenderScene::LoadCooked(bytes, Candidate_, Scene_, Error_, CandidateRevisions_);
        }
        WorldDocument authoredWorld = WorldSnapshot_;
        if (Success_ == true && VerificationOnly_ == false && authoredWorld.Loaded() == true &&
            (Operation_ == QStringLiteral("import") || Operation_ == QStringLiteral("open")))
        {
            // Validate the complete entity/component transaction before any
            // candidate viewport or catalog can be published.
            Success_ = authoredWorld.ImportScene(Candidate_, Error_);
        }
        if (Success_ == true && VerificationOnly_ == false && authoredWorld.Loaded() == true &&
            RenderScene::GetSource(Root_, authoredWorld) == RenderSceneSource::ProjectBindings)
        {
            RenderScene authored;
            Success_ = RenderScene::Load(Root_, authoredWorld, authored, Error_, false) &&
                       RenderScene::Merge(authored, bytes, Candidate_, Scene_, Error_, CandidateRevisions_);
        }
    }
}
void LevelWorkspace::CommitImport()
{
    PublishingCatalog_ = true;
    if (Run({QStringLiteral("commit-import"), {}, Source_, CatalogExpected_}) == false)
    {
        PublishingCatalog_ = false;
        Rollback(QStringLiteral("Could not publish the prepared catalog."));
        return;
    }
    Status_->setText(QStringLiteral("Committing prepared scene assets…"));
}
void LevelWorkspace::CommitCandidate()
{
    Level_ = Candidate_;
    ImportReports_ = PendingImportReports_;
    if (Scene_.Identity.isEmpty())
    {
        Viewport_->ClearFrame(QStringLiteral("No visible scene instances."));
    }
    Warnings_->setText(PendingWarnings_);
    if (Operation_ == QStringLiteral("save") || Operation_ == QStringLiteral("open"))
    {
        Disk_ = CandidateDisk_;
    }
    if (Operation_ == QStringLiteral("open"))
    {
        Path_ = CandidatePath_;
    }
    Dirty_ = WorldOwned_ == false && Operation_ != QStringLiteral("save") && Operation_ != QStringLiteral("open");
    Busy_ = false;
    PreviousScene_ = {};
    Status_->setText(Operation_ == QStringLiteral("save")
                         ? QStringLiteral("Scene saved with verified project-owned assets.")
                         : QStringLiteral("Scene ready."));
    Render();
    Q_EMIT OperationFinished(true);
    ApplyPendingProject();
}
void LevelWorkspace::Rollback(const QString& error)
{
    const bool hasPreviousFrame = Viewport_->HasRetainedGraphFrame();
    AwaitingViewport_ = false;
    PublishingCatalog_ = false;
    Preview_.Stop();
    if (Preview_.SetScene(PreviousScene_) && PreviousScene_.Identity.isEmpty() == false)
    {
        (void)Preview_.Start();
    }
    if (Preview_.Running() == false && hasPreviousFrame == false)
    {
        // Show the refusal in the viewport, not only in the status line.
        Viewport_->ClearFrame(error);
    }
    PreviousScene_ = {};
    Busy_ = false;
    Status_->setText(error + QStringLiteral(" Previous scene and level retained."));
    Render();
    Q_EMIT OperationFinished(false);
    ApplyPendingProject();
}
void LevelWorkspace::Publish()
{
    if (Success_ == false || Cancelling_)
    {
        Busy_ = false;
        Status_->setText(Error_);
        const bool verification = VerificationOnly_;
        VerificationOnly_ = false;
        Render();
        if (verification)
        {
            Q_EMIT AssetsVerified(false);
        }
        else
        {
            Q_EMIT OperationFinished(false);
        }
        ApplyPendingProject();
        return;
    }
    if (VerificationOnly_)
    {
        VerificationOnly_ = false;
        Busy_ = false;
        Status_->setText(QStringLiteral("World scene assets verified."));
        Render();
        Q_EMIT AssetsVerified(true);
        ApplyPendingProject();
        return;
    }
    if (Operation_ == QStringLiteral("save"))
    {
        CommitCandidate();
        return;
    }
    PreviousScene_ = Preview_.Scene();
    const bool existingFrame = Preview_.FrameAvailable() && PreviousScene_.PacketIdentity == Scene_.PacketIdentity &&
                               PreviousScene_.AssetsIdentity == Scene_.AssetsIdentity;
    if (existingFrame == false)
    {
        Viewport_->HoldFrame();
    }
    Q_EMIT PreviewStarting();
    if (Preview_.SetScene(Scene_) == false)
    {
        Rollback(Preview_.Status());
        return;
    }
    if (Scene_.Identity.isEmpty() || existingFrame)
    {
        if (Operation_ == QStringLiteral("import"))
        {
            CommitImport();
        }
        else
        {
            CommitCandidate();
        }
        return;
    }
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (PreviewViewport::CanPresent() == false)
    {
        // Commit without a rendered frame, but say why the viewport stays empty
        // instead of leaving the generic placeholder.
        Viewport_->ClearFrame(QStringLiteral("Scene ready, but this Editor cannot show the GPU viewport here: native "
                                             "GPU sharing needs macOS (Metal), or X11/Wayland with Vulkan "
                                             "(this build uses Qt %1 on platform '%2').")
                                  .arg(QStringLiteral(QT_VERSION_STR), QGuiApplication::platformName()));
        if (Operation_ == QStringLiteral("import"))
        {
            CommitImport();
        }
        else
        {
            CommitCandidate();
        }
        return;
    }
    AwaitingViewport_ = true;
    if (Preview_.Start() == false)
    {
        Rollback(Preview_.Status());
        return;
    }
    Status_->setText(
        QStringLiteral("Preparing candidate viewport; the current level stays active until its first rendered frame."));
#else
    if (Operation_ == QStringLiteral("import"))
    {
        CommitImport();
    }
    else
    {
        CommitCandidate();
    }
    Viewport_->ClearFrame(QStringLiteral("Scene ready. This Editor build has no native rendered viewport "
                                         "(configure with LUDUS_EDITOR_RENDER_PREVIEW)."));
    Status_->setText(QStringLiteral("Scene ready. This Editor build has no native rendered viewport."));
#endif
}
void LevelWorkspace::ApplyPendingProject()
{
    if (ProjectPending_ && Busy_ == false)
    {
        ProjectPending_ = false;
        SetProject(PendingRoot_, PendingEpoch_);
    }
}
void LevelWorkspace::Cancel()
{
    if (Busy_ == false)
    {
        return;
    }
    Cancelling_ = true;
    if (AwaitingViewport_)
    {
        Rollback(QStringLiteral("Import cancelled."));
        return;
    }
    CancelRequested_.store(true);
    Status_->setText(QStringLiteral("Cancelling; waiting for the owned operation to drain…"));
}
bool LevelWorkspace::ConfirmDiscard()
{
    if (Busy_)
    {
        Status_->setText(
            QStringLiteral("Finish or cancel the current scene operation before closing/changing projects."));
        return false;
    }
    if (WorldOwned_ || Dirty_ == false)
    {
        return true;
    }
    const auto choice = QMessageBox::warning(this,
                                             QStringLiteral("Unsaved 3D scene"),
                                             QStringLiteral("Discard unsaved scene edits?"),
                                             QMessageBox::Discard | QMessageBox::Cancel,
                                             QMessageBox::Cancel);
    return choice == QMessageBox::Discard;
}
void LevelWorkspace::SetSelection(const QString& id)
{
    Preview_.SetSelection(id);
}
void LevelWorkspace::StopPreview() noexcept
{
    Preview_.Stop();
}
void LevelWorkspace::Render()
{
    Rendering_ = true;
    const auto selected = Objects_->currentRow();
    Objects_->clear();
    for (const auto& value : Level_.value(QStringLiteral("objects")).toArray())
    {
        Objects_->addItem(value.toObject().value(QStringLiteral("name")).toString());
    }
    if (selected >= 0 && selected < Objects_->count())
    {
        Objects_->setCurrentRow(selected);
    }
    File_->setText(Path_.isEmpty() ? QStringLiteral("No 3D scene open")
                                   : Path_ + (Dirty_ ? QStringLiteral(" *") : QString()));
    New_->setEnabled(Busy_ == false && Root_.isEmpty() == false);
    Open_->setEnabled(Busy_ == false && Root_.isEmpty() == false);
    Import_->setEnabled(Busy_ == false && Level_.isEmpty() == false);
    Save_->setEnabled(CanSave());
    Cancel_->setEnabled(Busy_ && Operation_ != QStringLiteral("save") && PublishingCatalog_ == false);
    Objects_->setEnabled(Busy_ == false);
    Rendering_ = false;
    Inspect();
    Q_EMIT DocumentChanged();
}
void LevelWorkspace::Inspect()
{
    if (Rendering_)
    {
        return;
    }
    Rendering_ = true;
    const auto index = Objects_->currentRow();
    const auto objects = Level_.value(QStringLiteral("objects")).toArray();
    const bool isAvailable = Busy_ == false && index >= 0 && index < objects.size();
    Name_->setEnabled(isAvailable);
    Remove_->setEnabled(isAvailable);
    const QJsonObject object = isAvailable ? objects[index].toObject() : QJsonObject{};
    Name_->setText(object.value(QStringLiteral("name")).toString());
    const auto transform = object.value(QStringLiteral("transform")).toArray();
    for (foundation::int32 i = 0; i < 3; ++i)
    {
        Translation_[i]->setEnabled(isAvailable);
        Translation_[i]->setValue(transform.size() == 12 ? transform[9 + i].toDouble() : 0);
    }
    Rendering_ = false;
}
void LevelWorkspace::Edit()
{
    if (Rendering_ || Busy_ || Objects_->currentRow() < 0 || Name_->text().isEmpty())
    {
        return;
    }
    auto objects = Level_.value(QStringLiteral("objects")).toArray();
    const auto index = Objects_->currentRow();
    auto object = objects[index].toObject();
    auto transform = object.value(QStringLiteral("transform")).toArray();
    object.insert(QStringLiteral("name"), Name_->text());
    for (foundation::int32 i = 0; i < 3; ++i)
    {
        transform[9 + i] = Translation_[i]->value();
    }
    object.insert(QStringLiteral("transform"), transform);
    if (objects[index].toObject() == object)
    {
        return;
    }
    objects[index] = object;
    Candidate_ = Level_;
    Candidate_.insert(QStringLiteral("objects"), objects);
    (void)Start(QStringLiteral("assemble"));
}
} // namespace ludus::editor
