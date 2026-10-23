#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/platform/entropy.h>

#include "internal/world_document.h"

#include <QMap>
#include <QStringList>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QSplitter;
class QToolButton;
QT_END_NAMESPACE
namespace ludus::editor
{
// Desktop, project-owned schema UI. No runtime pools or project code are loaded.
class RenderPanel;
class RenderPreview;
class RendererWorkspace;
class LevelWorkspace;
struct ToolingPaths;
/// Where Play opens the game: a local editor preference, not project data.
enum class PlayMode : foundation::uint8
{
    /// The World viewport becomes the game's window (the default).
    InViewport,
    /// The game opens its own native window; the World viewport keeps the edit view.
    SeparateWindow,
};
/// How WorldWorkspace::PreparePlay resolved a Play request.
enum class PlayWindow : foundation::uint8
{
    /// Launch with the returned hosted-window address: the game plays in the World viewport.
    InViewport,
    /// Launch without a hosted window: the game opens its own window.
    SeparateWindow,
    /// Do not launch. The viewport cannot host the game; the workspace states why and offers a separate window.
    Unavailable,
};
class WorldWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit WorldWorkspace(QWidget* parent = nullptr);
    WorldWorkspace(const ToolingPaths& tooling, QWidget* parent);
    [[nodiscard]] bool ImportScene(const QString& source);
    /// Dry-run source (a .pbrt, .gltf, .glb or .obj file, or a reference directory) and show the
    /// preview panel; ConfirmImportPreview then imports the previewed source.
    [[nodiscard]] bool PreviewImportScene(const QString& source);
    /// Import the source shown in the preview panel.
    [[nodiscard]] bool ConfirmImportPreview();
    [[nodiscard]] QString Path() const
    {
        return Path_;
    }
    /// Renderer of the viewport this World shows, whose device and extraction feed the
    /// render-graph preview: the scene viewport of an imported-scene World, otherwise the
    /// render panel, which draws schema-bound and World-demo Worlds alike. RenderPreviewChanged reports a change of
    /// choice. The result is owned by this workspace; it never starts a device on the caller's behalf.
    [[nodiscard]] RenderPreview& GetRenderPreview() noexcept;
    /// Route both World extraction surfaces through the selected project renderer.
    void UseProjectGraph(RendererWorkspace& inoutWorkspace);
    void SetProject(const QString& root, foundation::uint64 epoch);
    [[nodiscard]] bool Open(const QString& path);
    [[nodiscard]] bool Create(const QString& path);
    [[nodiscard]] bool Save();
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Pending_ || Unsaved_ || Document_.Dirty();
    }
    [[nodiscard]] bool CanSave() const noexcept
    {
        return Document_.Loaded() && SceneBusy() == false;
    }
    [[nodiscard]] bool CanUndo() const noexcept
    {
        return Pending_ == false && SceneBusy() == false && Document_.CanUndo();
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return Pending_ == false && SceneBusy() == false && Document_.CanRedo();
    }
    void StopPreview() noexcept;
    /// @brief Make the World viewport the window of the game about to be launched.
    /// @param outAddress The game's `LUDUS_HOSTED_WINDOW` value, set on success.
    /// @param outReason Why the game must open in its own window instead, set on failure.
    /// @return True when the bound viewport hosts the game (Play in the viewport).
    [[nodiscard]] bool BeginPlayInViewport(QString& outAddress, QString& outReason);
    /// The Play mode chosen next to Play saved World. Read from the editor's preferences at
    /// construction; InViewport when none was saved.
    [[nodiscard]] PlayMode GetPlayMode() const noexcept;
    /// Choose where the next Play opens the game and save the choice in the editor's preferences
    /// (QSettings). A game already running does not move.
    void SetPlayMode(PlayMode mode);
    /// @brief Resolve a Play request before launching the game.
    ///
    /// SeparateWindow never touches the viewport, and the game is launched without a hosted
    /// window. InViewport calls BeginPlayInViewport; when the viewport cannot host the game,
    /// nothing is launched and the workspace shows the reason with a "Play in a separate
    /// window" offer, which emits PlayRequested(PlayMode::SeparateWindow). Play never switches
    /// modes silently.
    /// @param mode Where to open the game; normally GetPlayMode().
    /// @param outAddress The game's `LUDUS_HOSTED_WINDOW` value; set only for PlayWindow::InViewport.
    /// @return How to launch, or Unavailable to launch nothing.
    [[nodiscard]] PlayWindow PreparePlay(PlayMode mode, QString& outAddress);
    /// Why Play in the viewport is unavailable, as the workspace shows it; empty while no refusal is shown.
    [[nodiscard]] QString GetPlayUnavailableText() const;
    /// Label the World viewport while a game runs in its own window, or remove the label.
    void SetPlayingElsewhere(bool isPlayingElsewhere);
    /// Return the viewport to editing and close the game's window, if a game plays in it.
    void EndPlayInViewport() noexcept;
    /// Ask the game playing in the viewport to close its window.
    void RequestPlayClose() noexcept;
    /// Whether a game is playing in the World viewport.
    [[nodiscard]] bool IsPlayingInViewport() const noexcept;
    void ResumePreview();
    [[nodiscard]] bool PreviewRunning() const noexcept;
    [[nodiscard]] QJsonObject Session() const;
    void RestoreSession(const QJsonObject& state);
    void Undo();
    void Redo();
    [[nodiscard]] bool Apply();
    [[nodiscard]] bool Recover();
    [[nodiscard]] const WorldDocument& Document() const noexcept
    {
        return Document_;
    }
    /// Move the entity inspector (name, component fields, Add Component, Apply and Reset)
    /// out of this view into newParent, for a shell Properties tool window. The panel keeps
    /// editing this workspace's draft; this workspace keeps owning its state. Call once.
    /// @return The moved panel; it lives as long as newParent keeps it.
    [[nodiscard]] QWidget* TakePropertiesPanel(QWidget* newParent);
    /// Show or hide everything around the viewport: the file and render controls above it,
    /// the entity hierarchy beside it, the status line below it, and the inspector while it
    /// is still in this view. The viewports are never hidden, reparented or recreated, so
    /// their native surfaces only resize. Showing again restores the splitter sizes that
    /// were in place when the chrome was hidden. Visibility each control derives from the
    /// document (import preview, Play, scene cancellation) is unaffected.
    void SetChromeVisible(bool isVisible);
    /// False while SetChromeVisible(false) is in effect.
    [[nodiscard]] bool IsChromeVisible() const noexcept
    {
        return mIsChromeVisible;
    }
    /// Replace the entropy source that mints World, entity and scene-level IDs,
    /// including the embedded 3D level workspace's. Production keeps
    /// platform::GetSystemEntropySource(); tests inject a controlled or failing source.
    /// @pre No scene operation is running.
    void SetEntropySource(const foundation::identity::EntropySource& entropy) noexcept;
Q_SIGNALS:
    void DocumentChanged();
    void PreviewStarting();
    /// Play saved World was chosen, or the separate-window offer after a refusal.
    /// @param mode Where the game should open.
    void PlayRequested(ludus::editor::PlayMode mode);
    /// GetRenderPreview now returns a different viewport renderer (the shown view changed).
    void RenderPreviewChanged();

private:
    [[nodiscard]] bool LoadProjectSchema(WorldDocument& candidate, QByteArray& schema, bool worldDemo = false);
    [[nodiscard]] bool Inside(const QString& path, bool existing) const;
    [[nodiscard]] bool SceneBusy() const noexcept;
    void RefreshScene();
    void SceneCompleted(bool accepted);
    void SceneVerified(bool accepted);
    void Render();
    void RenderInspector();
    void Changed(const QString& message = {});
    void Pending();
    void PreviewDraft();
    [[nodiscard]] bool ReadDraft(WorldDocument& document, QString& error, QWidget*& invalid);
    void PickTint(const QString& id, const QJsonObject& binding);
    void Snapshot();
    [[nodiscard]] QByteArray RecoveryHeader() const;
    /// Record which viewport renderer the World shows and emit RenderPreviewChanged on change.
    void updateShownRenderPreview(bool isSceneShown);
    RenderPreview* mShownRenderPreviewOrNull = nullptr;
    /// Entity inspector column; TakePropertiesPanel moves it into a shell tool window.
    QWidget* mPropertiesPanel;
    /// File, import and render controls above the viewport; SetChromeVisible hides it.
    QWidget* mHeader;
    /// Entity hierarchy column of the splitter; SetChromeVisible hides it.
    QWidget* mHierarchy;
    /// Splitter sizes captured when the chrome was hidden, restored when it is shown.
    QByteArray mSplitterStateBeforeChromeHidden;
    bool mIsChromeVisible = true;
    WorldDocument Document_;
    foundation::identity::EntropySource mEntropy = platform::GetSystemEntropySource();
    QPushButton* PlayButton_ = nullptr;
    void showPlayUnavailable(const QString& reason);
    void hidePlayUnavailable();
    QComboBox* mPlayModeBox;
    QLabel* mPlayUnavailableLabel;
    QPushButton* mPlayElsewhereButton;
    RenderPanel* Render_ = nullptr;
    LevelWorkspace* Scene_ = nullptr;
    QWidget* Viewport_ = nullptr;
    QWidget* BoundViewport_ = nullptr;
    QSplitter* Split_ = nullptr;
    QTimer DraftTimer_;
    bool DraftPreview_ = false;
    QPushButton* Import_ = nullptr;
    QPushButton* CancelScene_ = nullptr;
    /// Dry-run preview of a pending import, with confirm and discard actions.
    QPlainTextEdit* ImportPreview_ = nullptr;
    QPushButton* ConfirmImport_ = nullptr;
    QPushButton* DiscardImport_ = nullptr;
    QString PreviewedSource_;
    /// Stored import reports of the World's scene.
    QPlainTextEdit* ImportReport_ = nullptr;
    QToolButton* mImportReportToggleOrNull = nullptr;
    void ShowImportPreview(bool isAccepted);
    void ShowImportReports();
    /// Import source into the World; isPreviewed requires it to match the approved preview.
    [[nodiscard]] bool StartSceneImport(const QString& source, bool isPreviewed);
    /// Drop the previewed import and the report panel when the live World is replaced.
    void ClearImportState();
    bool Importing_ = false;
    bool SavePending_ = false;
    bool SaveValidated_ = false;
    QJsonObject ValidatedScene_;
    QByteArray ValidatedWorld_;
    QString SceneError_;
    QString ConversionPath_;
    QByteArray ConversionSchema_;
    foundation::uint64 SceneGeneration_ = 0;

    QString Root_;
    QString Path_;
    QString Selected_;
    QStringList mSelectedEntities;
    QByteArray Disk_;
    QByteArray SchemaDisk_;
    foundation::uint64 Epoch_ = ~foundation::uint64{0};
    foundation::uint64 DocumentGeneration_ = 0;
    bool Pending_ = false;
    bool Unsaved_ = false;
    bool Rendering_ = false;
    bool RecoveryFailed_ = false;
    QMap<QString, QWidget*> Fields_;
    QListWidget* Entities_;
    QLabel* Status_;
    QLabel* PathLabel_;
    QLineEdit* Name_ = nullptr;
    QComboBox* Components_;
    QPushButton* Attach_;
    QPushButton* Add_;
    QPushButton* Duplicate_;
    QPushButton* Remove_;
    QPushButton* Apply_;
    QPushButton* Reset_;
    QPushButton* Recovery_;
    QVBoxLayout* Inspector_;
};
} // namespace ludus::editor
