#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/platform/entropy.h>

#include "design_tokens.h"
#include "internal/render_graph_luminaire_coverage.h"
#include "internal/world_document.h"
#include "internal/world_lighting_coverage.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QPoint;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QSplitter;
class QTabWidget;
class QToolButton;
QT_END_NAMESPACE
namespace ludus::resources
{
struct SceneMaterial;
}

namespace ludus::editor
{
class WorldEntitySearch;
// Desktop, project-owned schema UI. No runtime pools or project code are loaded.
class RenderPanel;
class RenderPreview;
class RendererWorkspace;
class LevelWorkspace;
class InspectorLabelColumn;
class InspectorSection;
class WorldLightsPanel;
class WorldMaterialDocument;
class WorldMaterialsPanel;
class WorldTerrainPanel;
struct WorldLightChange;
struct WorldLightRow;
enum class PlacementValidationResult : foundation::uint8;
struct ToolingPaths;
/// Narrowest the Properties tool window becomes, in device-independent pixels: seven page gutters, room for a label
/// column beside a number field. Below it the inspector would have to clip.
inline constexpr foundation::int32 WORLD_PROPERTIES_MINIMUM_WIDTH = design::SPACING_PAGE * 7;

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
    /// Dry-run source (a .pbrt, .gltf, .glb, .obj or .luscene.json file, or a reference directory) and show the
    /// preview panel; ConfirmImportPreview then imports the previewed source.
    /// environmentFile optionally selects a latitude/longitude image inside that source folder.
    [[nodiscard]] bool PreviewImportScene(const QString& source, const QString& environmentFile = {});
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
    /// Local World, camera, selection and splitter preferences. Hidden chrome records the
    /// normal splitter arrangement that will return when its controls are shown again.
    [[nodiscard]] QJsonObject Session() const;
    /// Restore local session preferences on the GUI thread. An accepted splitter state
    /// also replaces the normal arrangement retained while chrome is hidden; restoring
    /// preferences keeps that compact presentation active.
    void RestoreSession(const QJsonObject& state);
    void Undo();
    void Redo();
    /// Undo or redo to World history position `cursor` in one step, as Undo does; false leaves the World as is.
    [[nodiscard]] bool JumpToHistory(foundation::int32 cursor);
    /// Apply a luminaire repair (RendererWorkspace::MakeRenderWarningRepair) to the open default-schema World as one
    /// undoable edit: create the fitted luminaire, or move and resize the named one, and select it.
    /// @return False, with the reason in the status, when the World cannot be edited, is not a default World or no
    /// longer has that luminaire; the World is then unchanged.
    [[nodiscard]] bool ApplyLuminaireRepair(const RenderGraphLuminaireCoverage& repair);
    /// Apply a World lighting repair (RendererWorkspace::MakeWorldLightingRepair) to the open default-schema World as
    /// one undoable edit: create the sun it describes and select it.
    /// @return False, with the reason in the status, when the World cannot be edited or is not a default World; the
    /// World is then unchanged.
    [[nodiscard]] bool ApplySunRepair(const WorldLightingCoverage& repair);
    [[nodiscard]] bool Apply();
    [[nodiscard]] bool Recover();
    [[nodiscard]] const WorldDocument& Document() const noexcept
    {
        return Document_;
    }
    /// Select one entity of the open World by ID, as a hierarchy click does.
    /// @return False when the World has no such entity (for example an imported scene object).
    [[nodiscard]] bool SelectEntity(const QString& id);
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
    bool eventFilter(QObject* watched, QEvent* event) override;
    [[nodiscard]] bool LoadProjectSchema(WorldDocument& candidate, QByteArray& schema, bool worldDemo = false);
    [[nodiscard]] bool Inside(const QString& path, bool existing) const;
    /// A blocking scene operation runs, or a full assembly waits for a background placement cook;
    /// World edits and the viewport gizmo wait. A placement cook alone keeps them available.
    [[nodiscard]] bool SceneBusy() const noexcept;
    /// SceneBusy(), or a background placement cook occupies the scene operation thread: operations
    /// that start a scene operation themselves (open, import, draft preview, recovery) wait.
    [[nodiscard]] bool isSceneWorkerActive() const noexcept;
    void RefreshScene();
    /// Adopt or report a background placement cook; run a full assembly it deferred.
    void placementValidationFinished(PlacementValidationResult result);
    /// Forget the validated and presented scene so the next RefreshScene assembles the level.
    void invalidateScene();
    void SceneCompleted(bool accepted);
    void SceneVerified(bool accepted);
    void Render();
    void RenderInspector();
    [[nodiscard]] QStringList getStaticColliderTargets() const;
    void initializeTerrainAuthoring();
    void showTerrainAuthoring(const QString& key = {});
    void updateTerrainActions();
    void queueTerrainPreview();
    void prepareTerrainPreview();
    void terrainBoundLoaded(bool isAccepted);
    void finishTerrainPreview(bool isAccepted, const QString& error = {});
    void cancelTerrainRequest();
    void closeTerrainAuthoring(bool shouldRestoreWorld);
    void restoreTerrainWorld();
    void checkTerrainBase();
    void refreshTerrainBase();
    void commitTerrainDraft();
    /// Preview the dragged stroke on the terrain while the button is held; only release commits it.
    void previewTerrainStroke(const QJsonArray& points);
    void queueTerrainStroke(const QJsonArray& points);
    /// Discard a live stroke and show the surface it started from again.
    void abandonTerrainStroke();
    void cancelTerrainBrushGesture();
    /// A brush drag is previewing on the terrain. Workspace refreshes then keep the gesture alive.
    [[nodiscard]] bool isTerrainStrokeLive() const;
    void frameTerrainDraft();
    [[nodiscard]] bool isTerrainAuthoringOpen() const;
    [[nodiscard]] bool canEditTerrain(const QString& key) const;
    [[nodiscard]] bool tryPlaceTerrainDraft(const QString& id, const QJsonObject& placement);
    void applyTerrainInteraction();
    WorldTerrainPanel* mTerrainPanelOrNull = nullptr;
    /// Refresh the light list from the World and the presented scene.
    void renderLights();
    /// Apply a light switch, Solo or Enable All from the light list as one undo step.
    void applyLightChange(const WorldLightChange& change, const QString& message);
    /// Show the properties of a light selected in the light list: select its first World entity, or, for a light
    /// without one, clear the entity selection so the inspector shows the light's own values.
    void selectLight(const WorldLightRow& row);
    /// Append the read-only Light group of the light list's selected light, which has no World entity.
    void addLightGroup(const WorldLightRow& row);
    /// Append the read-only Material group of the selected scene instance, read from the
    /// presented viewport scene; a placeholder until that scene has loaded.
    void addMaterialGroup();
    void addMaterialRepair(QFormLayout& inoutRows);
    /// Offer component in the Add Component menu.
    void addComponentChoice(const QString& label, const QString& component);
    /// Give a long Add Component menu its search field, once every choice is in.
    void finishComponentMenu();
    /// Fill a component section's "⋯" menu: Reset to Defaults, Copy Values and, unless the project requires the
    /// component, Remove Component.
    void addComponentActions(InspectorSection& inoutSection, const QJsonObject& component);
    /// A field's value as text, for telling whether it changed since the inspector was built.
    [[nodiscard]] static QString readFieldState(const QWidget& field);
    void recordAppliedFields();
    [[nodiscard]] foundation::int32 countUnappliedFields() const;
    /// Show the Apply and Reset footer, with how many fields changed, only while there are unapplied edits.
    void updateCommitFooter();
    void addProjectBrdfControls(QFormLayout& inoutRows,
                                const resources::SceneMaterial* materialOrNull,
                                const QJsonObject& placement);
    void assignProjectBrdf(const QString& material);
    [[nodiscard]] bool readProjectBrdfRoughness(QJsonObject& inoutScene, QString& outError, QWidget*& outInvalidOrNull);
    void mapImportedMaterial(const QString& source, const QString& replacement, const QString& label);
    void bindViewportInteraction();
    void frameEntity(const QString& id);
    void showEntityMenu(const QPoint& positionGlobal, const QString& entityId);
    void updateViewportInteraction();
    void exploreFirstPerson();
    void commitViewportPlacement(const QString& id, const QJsonObject& placement);
    /// Undo history name for a viewport placement, from the active gizmo tool ("Move Entity", ...).
    [[nodiscard]] const char* getPlacementEditName() const;
    [[nodiscard]] QJsonObject findEditableProbeVolume(const QString& id) const;
    void commitProbeVolumeEdit(const QJsonObject& entity, const QJsonObject& components);
    void commitProbeSpacing(const QString& id, foundation::float64 spacingMetres);
    void commitProbeFace(const QString& id, foundation::int32 axis, foundation::uint32 count, bool isMinimumFace);
    void commitProbeSize(const QString& id, foundation::int32 axis, foundation::float64 sizeMetres, bool isMinimumFace);
    /// Offer to raise the selected volume's spacing so the probe limit reaches every edge, then apply it.
    void coverProbeVolume(foundation::float64 spacingMetres);
    /// Show the GI Probe Volume's layout-dependent rows: counts for the counts layout, the derived grid and a
    /// spacing repair for the volume layout.
    void configureProbeVolumeFields(QWidget& inoutGroup,
                                    QFormLayout& inoutRows,
                                    const QString& cid,
                                    const QJsonObject& components);
    /// A leading space and the coverage sentence when the probe limit leaves part of the volume unsampled.
    [[nodiscard]] QString describeProbeCoverage(const QJsonObject& components) const;
    /// Store the scene viewport's current perspective view as the level start view (one undo step).
    void saveStartView();
    void Changed(const QString& message = {});
    void markInspectorPending(bool shouldPreview = true);
    void PreviewDraft();
    [[nodiscard]] bool readDraft(WorldDocument& inoutDocument,
                                 QString& outError,
                                 QWidget*& outInvalidOrNull,
                                 bool shouldReadMaterial = true,
                                 bool* outHasInheritedEarthObserverOrNull = nullptr);
    [[nodiscard]] bool readCoatingFields(QJsonObject& inoutScene, QString& outError, QWidget*& outInvalidOrNull);
    [[nodiscard]] bool openMaterialDocument(const QString& entityId, QString& outError);
    [[nodiscard]] bool setSlotCoating(const QString& entityId, const QJsonObject& coating, QString& outError);
    void restoreInheritedCoating();
    void applySharedCoating();
    void PickTint(const QString& id, const QJsonObject& binding);
    void Snapshot();
    /// Create sun as one undoable edit and select it; the status gives the reason when the World cannot be edited.
    [[nodiscard]] bool addSun(const WorldSunPlacement& sun);
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
    /// Light switches under the entity list, in the hierarchy column.
    WorldLightsPanel* mLightsPanel;
    QTabWidget* mWorldLists;
    WorldMaterialsPanel* mMaterialsPanel;
    QPointer<WorldMaterialDocument> mMaterialDocumentOrNull;
    /// Splitter sizes captured when the chrome was hidden, restored when it is shown.
    QByteArray mSplitterStateBeforeChromeHidden;
    bool mIsChromeVisible = true;
    WorldDocument Document_;
    foundation::identity::EntropySource mEntropy = platform::GetSystemEntropySource();
    QPushButton* PlayButton_ = nullptr;
    QPushButton* mExploreFirstPersonButton;
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
    QPushButton* mChooseImportEnvironmentOrNull = nullptr;
    QPushButton* mClearImportEnvironmentOrNull = nullptr;
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
    /// RefreshScene needs a full assembly that waits for the background placement cook to drain.
    bool mIsSceneRefreshDeferred = false;
    /// Encoded World and document generation of the latest placement LevelWorkspace published.
    QByteArray mPlacementWorld;
    foundation::uint64 mPlacementDocumentGeneration = 0;
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
    QTreeWidget* mEntities;
    WorldEntitySearch* mEntitySearch;
    QLabel* Status_;
    QLabel* PathLabel_;
    QLineEdit* Name_ = nullptr;
    QPushButton* Attach_;
    QMenu* mComponentMenu;
    QWidget* mAttachRow;
    QWidget* mCommitFooter;
    QLabel* mPendingSummary;
    InspectorLabelColumn* mLabelColumn;
    /// Each inspector field's value when the inspector was built, by field key.
    QHash<QString, QString> mAppliedFields;
    QMenu* mEntityMenu;
    QAction* mAddEntity;
    QAction* mDuplicateEntities;
    QAction* mDeleteEntities;
    QPushButton* Apply_;
    QPushButton* Reset_;
    QPushButton* Recovery_;
    QVBoxLayout* Inspector_;
};
} // namespace ludus::editor
