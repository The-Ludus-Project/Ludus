#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/platform/entropy.h>

#include "internal/controller.h"
#include "internal/render_preview.h"
#include "internal/world_document.h"

#include <QByteArray>
#include <QJsonObject>
#include <QThread>
#include <QWidget>

#include <atomic>

QT_BEGIN_NAMESPACE
class QLabel;
class QListWidget;
class QLineEdit;
class QDoubleSpinBox;
class QPushButton;
QT_END_NAMESPACE
namespace ludus::editor
{
// Project-owned 3D level surface. Ludus::SceneCook owns cooking, immutable
// catalogs and save conflicts; it runs in process on a worker thread and no
// project game code is executed.
class PreviewViewport;
/// How LevelWorkspace::PublishPlacement answered.
enum class PlacementPublication : foundation::uint8
{
    /// The viewport presents the level now; its authoritative cook validates it in the background.
    Published,
    /// The presented scene already shows the level.
    Unchanged,
    /// More than scene placement changed, nothing is presented yet, or deriving failed: Assemble the level.
    RequiresAssembly,
};
/// Validation of the scene LevelWorkspace::PublishPlacement presented.
enum class PlacementValidation : foundation::uint8
{
    /// The presented scene is the last completed cook's, or no scene is presented.
    Ready,
    /// A derived scene is presented; its cook waits for the operation thread.
    Pending,
    /// The cook of the presented scene runs on the operation thread.
    Running,
};
/// How one background placement cook ended (LevelWorkspace::PlacementValidationFinished).
enum class PlacementValidationResult : foundation::uint8
{
    /// The cook confirmed the presented placement, or replaced it with its own scene.
    Accepted,
    /// The cook failed; the last validated scene is presented again and the error is in StatusText().
    Rejected,
    /// A newer placement, a cancellation or a project change made this cook obsolete; nothing changed.
    Superseded,
};
class LevelWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit LevelWorkspace(const ToolingPaths& tooling, QWidget* parent = nullptr);
    ~LevelWorkspace() override;
    void SetProject(const QString& root, foundation::uint64 epoch);
    [[nodiscard]] bool Create(const QString& path);
    [[nodiscard]] bool Open(const QString& path);
    /// Import a source directory (pbrt-reference-v1) or a .pbrt, .gltf, .glb or .obj
    /// file (routed by Ludus::SceneCook to its best-effort importer).
    [[nodiscard]] bool Import(const QString& source);
    /// Import source after the user approved its preview: fails with a conflict,
    /// asking for a new preview, unless the sources still match ImportPreview().
    [[nodiscard]] bool ImportPreviewed(const QString& source);
    /// Plan importing source as a dry run on the worker thread (cancellable, with
    /// progress). Nothing is written; ImportPreviewFinished reports the result.
    [[nodiscard]] bool PreviewImport(const QString& source);
    /// The last successful PreviewImport response (profile, report, reference_rejection).
    [[nodiscard]] QJsonObject ImportPreview() const
    {
        return ImportPreview_;
    }
    /// Stored import reports of the current level, keyed by import id (best-effort imports only).
    [[nodiscard]] QJsonObject ImportReports() const
    {
        return ImportReports_;
    }
    [[nodiscard]] bool Save();
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Dirty_;
    }
    [[nodiscard]] bool CanSave() const noexcept
    {
        return Busy_ == false && Level_.isEmpty() == false;
    }
    /// Select a stable World/scene object ID for viewport framing; empty clears it.
    void SetSelection(const QString& id);
    void StopPreview() noexcept;
    void ResumePreview()
    {
        Q_EMIT PreviewStarting();
        (void)Preview_.Start();
    }
    [[nodiscard]] QJsonObject Navigation() const
    {
        return Preview_.Navigation();
    }
    void RestoreNavigation(const QJsonObject& state)
    {
        Preview_.RestoreNavigation(state);
    }
    [[nodiscard]] bool CanCancel() const noexcept
    {
        return Busy_ && Operation_ != QStringLiteral("save") && PublishingCatalog_ == false;
    }
    void CancelOperation()
    {
        Cancel();
    }
    /// A blocking scene operation runs. A background placement cook is not one; see CanStartOperation.
    [[nodiscard]] bool Busy() const noexcept
    {
        return Busy_;
    }
    /// No scene operation, including a background placement cook, occupies the operation thread,
    /// so Assemble, VerifyAssets, imports and SetWorldSnapshot may be called.
    [[nodiscard]] bool CanStartOperation() const noexcept
    {
        return Busy_ == false && isOperationActive() == false && mIsValidationRunning == false;
    }
    /// @brief Present level at once when it differs from the presented scene only in scene-object placement.
    ///
    /// The derived scene carries the cook's own object transforms (DerivePlacedScene). The authoritative
    /// cook of the level then runs on the operation thread without making Busy() true, so placement
    /// edits, selection and Undo continue; a newer publication supersedes an older cook, and the result
    /// arrives through PlacementValidationFinished. Returning to the last validated placement presents
    /// that cook's scene again and needs no validation. GUI thread; World-owned workspaces only.
    /// @param level The World's SceneLevel().
    /// @param world The World that level came from; its other content must equal the presented World's.
    /// @return Published, Unchanged, or RequiresAssembly when the caller must Assemble (nothing changed).
    [[nodiscard]] PlacementPublication PublishPlacement(const QJsonObject& level, const WorldDocument& world);
    /// Validation state of the presented scene; Ready unless PublishPlacement presented a derived one.
    [[nodiscard]] PlacementValidation GetPlacementValidation() const noexcept
    {
        return mPlacementValidation;
    }
    /// Stop the background placement cook so a full assembly can replace the presented scene. The derived
    /// scene stays presented and Pending; PlacementValidationFinished(Superseded) reports the drained cook.
    void CancelPlacementValidation() noexcept;
    /// Forget the presented and validated placements (the World replaced or removed its scene), and stop
    /// a background placement cook. The viewport keeps its current frame; the next level is assembled.
    void ClearPlacementPresentation() noexcept;
    [[nodiscard]] QWidget* TakeViewport(QWidget* parent);
    /// World composed with the next Assemble or VerifyAssets. @pre CanStartOperation().
    void SetWorldSnapshot(const WorldDocument& world)
    {
        LUDUS_ASSERT(mIsValidationRunning == false);
        WorldSnapshot_ = world;
    }
    /// Replace the entropy source that mints new level IDs and that SceneCook
    /// operations receive. Production keeps platform::GetSystemEntropySource();
    /// tests inject a controlled or failing source.
    /// @pre No scene operation is running; its worker reads the source.
    LUDUS_INLINE void SetEntropySource(const foundation::identity::EntropySource& entropy) noexcept
    {
        LUDUS_ASSERT(isOperationActive() == false);
        mEntropy = entropy;
    }
    [[nodiscard]] bool VerifyAssets(const QJsonObject& level);
    [[nodiscard]] bool Assemble(const QJsonObject& level);
    [[nodiscard]] bool ImportInto(const QJsonObject& level, const QString& source);
    /// ImportInto with the approved-preview check of ImportPreviewed.
    [[nodiscard]] bool ImportPreviewedInto(const QJsonObject& level, const QString& source);
    /// Select a source-root-relative latitude/longitude environment for subsequent imports.
    /// Refuses changes while a worker runs; selecting a new map invalidates the preview token.
    [[nodiscard]] bool SetImportEnvironmentFile(const QString& file);
    /// Forget the previewed import and the shown import reports (document replaced).
    void ClearImportState();
    [[nodiscard]] QString StatusText() const;
    /// Shows why the scene could not be assembled when no previous frame is retained.
    void ShowSceneFailure(const QString& error);
    [[nodiscard]] bool PreviewRunning() const noexcept
    {
        return Preview_.Running();
    }
    [[nodiscard]] const RenderScene& PreviewScene() const noexcept
    {
        return Preview_.Scene();
    }
    /// Viewport renderer of this level; it owns the device and extraction that render-graph
    /// previews borrow while the level is the World's shown view.
    [[nodiscard]] LUDUS_INLINE RenderPreview& GetRenderPreview() noexcept
    {
        return Preview_;
    }

    [[nodiscard]] QJsonObject Document() const
    {
        return Level_;
    }
    /// Embedded viewport; borrowed for project render-graph binding.
    [[nodiscard]] LUDUS_INLINE PreviewViewport& GetViewport() noexcept
    {
        return *Viewport_;
    }
Q_SIGNALS:
    void DocumentChanged();
    void PreviewStarting();
    void OperationFinished(bool accepted);
    void AssetsVerified(bool accepted);
    void ImportPreviewFinished(bool accepted);
    /// A background placement cook drained; see PlacementValidationResult. Emitted on the GUI thread
    /// before the next placement cook may start.
    void PlacementValidationFinished(ludus::editor::PlacementValidationResult result);

private:
    bool Start(const QString& operation, const QString& path = {}, const QString& source = {});
    void Completed();
    /// Inputs for one scene operation, grouped so call sites name each field.
    struct SceneOperationRequest
    {
        QString Operation;
        QString Path;
        QString Source;
        QJsonValue Expected;
    };
    bool Run(const SceneOperationRequest& request);
    void WaitForOperation();
    void Publish();
    void CommitCandidate();
    void CommitImport();
    void Rollback(const QString& error);
    void Render();
    void Inspect();
    void Edit();
    void Cancel();
    void ApplyPendingProject();
    QString PendingRoot_;
    foundation::uint64 PendingEpoch_ = 0;
    bool ProjectPending_ = false;
    bool Relative(const QString& path, QString& relative) const;
    ToolingPaths Tooling_;
    QString Root_, Path_, Operation_, CandidatePath_, Disk_, CandidateDisk_, PendingWarnings_;
    foundation::uint64 Epoch_ = ~foundation::uint64{0};
    QJsonObject Level_, Candidate_, CandidateRevisions_;
    QJsonValue CatalogExpected_;
    QString Source_;
    /// Scene entry file (.pbrt, .gltf, .glb or .obj) inside Source_; empty keeps the reference profile.
    QString SourceEntry_;
    QString mImportEnvironmentFile;
    /// source_digest of the approved preview for the next import; empty skips the check.
    QString ExpectedSourceDigest_;
    /// File picked by the Import button while its preview runs.
    QString PreviewSource_;
    QJsonObject ImportPreview_;
    QJsonObject ImportReports_;
    QJsonObject PendingImportReports_;
    bool PublishingCatalog_ = false;
    QByteArray Response_, SceneBytes_, CandidateCatalog_;
    QString OperationError_;
    QThread* Operate_ = nullptr;
    /// Browser builds run scene operations on the GUI thread from a queued timer.
    bool mIsOperationQueued = false;
    foundation::identity::EntropySource mEntropy = platform::GetSystemEntropySource();
    /// Texture decoding for scene imports (SCENE_IMPORT_WORKER_COUNT workers). Initialized
    /// by the constructor on native builds; only the operation thread submits to it, one
    /// operation at a time, and the destructor joins it after WaitForOperation().
    foundation::threading::JobSystem mImportJobs;
    bool mHasImportJobs = false;
    [[nodiscard]] bool isOperationActive() const noexcept
    {
        return Operate_ != nullptr || mIsOperationQueued == true;
    }
    /// Decode the cooked candidate and validate it against the World (worker or browser GUI thread).
    void decodeCandidate();
    /// Start the cook of the presented derived scene when it is Pending and the operation thread is free.
    void startPlacementValidation();
    /// Settle a drained placement cook (GUI thread) and report it.
    void finishPlacementValidation();
    /// Present scene in the viewport and remember it as the presented scene; failure keeps both.
    [[nodiscard]] bool presentScene(const RenderScene& scene);
    /// Record the scene a completed publishing operation presented as presented and validated.
    void acceptCommittedScene();
    PlacementValidation mPlacementValidation = PlacementValidation::Ready;
    /// A placement cook occupies the operation thread (including its decode).
    bool mIsValidationRunning = false;
    /// The running placement cook was cancelled or superseded by a project change.
    bool mIsValidationCancelled = false;
    /// Advances whenever PublishPlacement changes the presented scene; a cook validates one value.
    foundation::uint64 mPlacementGeneration = 0;
    foundation::uint64 mValidationGeneration = 0;
    /// The scene, level and placement-free World draft the viewport presents.
    RenderScene mPresentedScene;
    QJsonObject mPresentedLevel;
    QJsonObject mPresentedWorld;
    /// The last completed cook's scene, level and placement-free World draft.
    RenderScene mValidatedScene;
    QJsonObject mValidatedLevel;
    QJsonObject mValidatedWorld;
    /// The World of the presented derived scene, composed by its cook.
    WorldDocument mPlacementWorld;
    std::atomic<bool> CancelRequested_{false};
    QThread Decode_;
    WorldDocument WorldSnapshot_;
    RenderScene Scene_;
    RenderScene PreviousScene_;
    bool AwaitingViewport_ = false;
    QString Error_;
    RenderPreview Preview_;
    bool WorldOwned_ = false;
    bool VerificationOnly_ = false;
    bool Busy_ = false, Dirty_ = false, Cancelling_ = false, Success_ = false, Rendering_ = false;
    QLabel *Status_, *File_, *Warnings_;
    PreviewViewport* Viewport_;
    QListWidget* Objects_;
    QLineEdit* Name_;
    QDoubleSpinBox* Translation_[3];
    QPushButton *New_, *Open_, *Import_, *Save_, *Cancel_, *Remove_;
};
} // namespace ludus::editor
