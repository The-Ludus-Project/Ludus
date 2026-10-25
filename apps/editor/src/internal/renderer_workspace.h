#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>

#include "internal/controller.h"
#include "internal/external_code_editor.h"
#include "internal/render_graph_build_settings.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_code_document.h"
#include "internal/render_graph_creation.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_source.h"
#include "internal/render_graph_source_watch.h"
#include "internal/viewport_frame_pump.h"

#include <QByteArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QDialog;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphQuickAdd;
struct RenderGraphTemplate;
struct RenderGraphResolutionSelection;
class CodeEditor;
class CodeDocumentController;
class CodeFormatter;
class RenderGraphPreview;
class RenderGraphWorker;
class RenderPreview;
class TextureViewer;
// RG3 Renderer workspace: edits one render-graph package through the typed
// RenderGraphDocument, and runs validation, builds and runtime cooking through
// the shared `ludus render-graph` backend on explicit immutable draft snapshots.
// Saved sources change only on Save. Diagnostics map to authored nodes and source
// lines. RG3b adds a node canvas over the same document operations and an explicit
// trusted preview of the cooked runtime generation on the World viewport device.
// A project without graphs/Main/main.lurender.json shows the engine default read-only;
// the first edit turns it into a project draft that Save creates under graphs/Main/.
//
// In the editor shell this is the render-graph canvas document: a source tree beside
// the Canvas, Source and Plan views. It owns no command buttons; the shell's renderGraph.*
// commands call its operations. Its node inspector and preview are panels the shell
// moves into the Properties and Render Preview tool windows (TakeInspectorPanel and
// TakePreviewPanel), and Tools > Options edits its build settings.
// See docs/development/render-graph-authoring.md#edit-and-reload-graphs-rg3,
// docs/development/render-graph-authoring.md#node-canvas-and-preview-rg3b,
// docs/development/render-graph-authoring.md#start-customizing-the-renderer and
// docs/development/editor-workspace.md#workspace-layout.
class RendererWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit RendererWorkspace(const ToolingPaths& tooling, QWidget* parentOrNull = nullptr);
    ~RendererWorkspace() override;
    RendererWorkspace(const RendererWorkspace&) = delete;
    RendererWorkspace& operator=(const RendererWorkspace&) = delete;

    /// Bind a project root and epoch; a different root closes the document and cancels jobs.
    /// The root is canonicalized, so a symlinked path binds the same project and its build
    /// tree; an empty or missing root closes the project. A bound project then shows its
    /// graphs/Main/main.lurender.json, or the engine default when it has none (or it cannot
    /// be loaded; the status line says why). Binding writes nothing.
    void SetProject(const QString& root, foundation::uint64 epoch);
    /// Open a root .lurender.json inside the project; its directory is the package root.
    [[nodiscard]] bool OpenGraph(const QString& path);
    /// Create and open a built-in raster, ReSTIR DI or visibility offscreen Renderer template in the selected project's
    /// graphs/name directory. Completion is asynchronous through JobFinished.
    /// Existing files and the current draft are retained if creation fails.
    [[nodiscard]] bool CreateGraph(const QString& name, const QString& templateIdentity = QStringLiteral("raster"));
    /// Create an empty library and reference in the selected configuration, or rebind the
    /// selected Subgraph. Opens its Default configuration; Save publishes the draft.
    /// Failure/cancellation preserves sources. GUI thread only.
    [[nodiscard]] bool CreateSubgraph(const QString& name);
    /// Prompt for an asset name and create/open the library in the current package.
    [[nodiscard]] bool PromptCreateSubgraph();
    /// True when a configuration is selected and no external source conflict exists.
    [[nodiscard]] bool CanCreateSubgraph() const;
    /// Create/select an empty configuration in the selected graph file. One undo step;
    /// Save publishes it. Invalid/duplicate names or external source conflicts preserve the draft.
    [[nodiscard]] bool CreateConfiguration(const QString& name);
    /// Add or fill an empty reusable ReSTIR DI configuration in the selected graph file.
    /// Includes settings, shaders and automatically sized reservoirs in one undoable draft;
    /// Save publishes. Failures preserve sources and display a repair. GUI thread only.
    [[nodiscard]] bool ApplyRestirTemplate(const QString& configuration);
    /// Apply one built-in library template through the same undo/save path as the canvas picker.
    /// templateIdentity selects restir-di, visibility-buffer, visibility-secondary-rays, tiled-vbuffer-nee
    /// or compute-ray-tracing.
    /// Failure preserves sources and reports a repair. The optional budget request reports a
    /// required increase or supplies explicit approval; GUI thread only; Save publishes.
    [[nodiscard]] bool ApplyConfigurationTemplate(const QString& configuration,
                                                  const QString& templateIdentity,
                                                  RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull = nullptr);
    /// Delete the selected configuration in one undo step and select the asset default.
    /// Last configurations, referenced configurations and source conflicts preserve the
    /// draft and report the repair in the status line. Save publishes; GUI thread only.
    [[nodiscard]] bool RemoveSelectedConfiguration();
    /// Copy the selected authored-node group to the system clipboard; writes no project files.
    /// Requires one configuration and no external source conflict. Failure retains clipboard data.
    [[nodiscard]] bool CopySelectedNodes();
    /// Paste the clipboard group into the displayed configuration at a scene position.
    /// Unique IDs, internal connections and layout are one undo step. Failure preserves the draft.
    [[nodiscard]] bool PasteNodes(QPointF position);
    /// Remove selected authored nodes and their incident connections in one undo step.
    void RemoveSelectedNodes();
    /// True with a loaded document, no job and no external source conflict. GUI thread only.
    [[nodiscard]] bool CanRemoveUnusedShaders() const;
    /// Remove unused shader registrations and exclusive sources in one undoable draft edit.
    /// Checks the whole loaded package; reports counts or repair details. Save publishes deletions.
    [[nodiscard]] bool RemoveUnusedShaders();
    /// Shader sources mapped by the root manifest to the selected pipeline's program references
    /// or a legacy Raster's technique.
    /// Empty for resource/scene nodes or missing mappings. Never guesses from filenames.
    [[nodiscard]] QStringList GetSelectedShaderSources() const;
    /// True when a project is bound and no job or creation is running, so New Graph and
    /// Open Graph can start.
    [[nodiscard]] bool CanCreateGraph() const;
    /// Ask to save a dirty draft, then choose a graph name/template and start CreateGraph. False
    /// when CanCreateGraph() is false, a prompt is cancelled or creation cannot start.
    /// templateIdentity preselects a catalog graph; empty or unknown keeps the first template.
    [[nodiscard]] bool PromptNewGraph(const QString& templateIdentity = {});
    /// Open the Quick Add palette for the canvas configuration. A chosen node is added at
    /// scenePosition in one undoable edit; a chosen snippet opens Apply Configuration Template
    /// with it selected. False, with a status message, when no configuration is editable.
    [[nodiscard]] bool ShowQuickAdd(QPointF scenePosition);
    /// Ask to save a dirty draft, then choose a root .lurender.json in the project and open
    /// it. False when CanCreateGraph() is false, a prompt is cancelled or the graph cannot open.
    [[nodiscard]] bool PromptOpenGraph();
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool IsDirty() const;
    [[nodiscard]] bool CanSave() const;
    /// Save the draft. An unedited engine default is not project content: nothing is written
    /// and the result is true. An edited default creates its package under graphs/Main/.
    /// Finishes the active inspector edit. Successful saves schedule a saved-source preview
    /// when Live Preview is enabled; validation/build failures preserve the working view.
    [[nodiscard]] bool Save();
    /// Open the selected saved project source in the chosen native code editor.
    /// Pending edits require an explicit save; cancellation preserves all source text.
    [[nodiscard]] bool OpenSourceInCodeEditor();
    /// Adopt external saves if there are no conflicting local edits; writes nothing.
    [[nodiscard]] bool ReloadSourcesFromDisk();
    /// True while the engine default is shown and Save has not created a project copy yet.
    [[nodiscard]] bool IsBuiltIn() const;
    /// Explicitly copy the shown engine default, with any draft edits, into graphs/Main/ in
    /// the project and continue editing it there. False when the default is not shown or the
    /// copy fails; an existing file at the destination is never replaced.
    [[nodiscard]] bool CustomizeBuiltIn();
    /// True with a loaded graph, no running renderer job and no external source conflict.
    [[nodiscard]] bool CanEditResolutionSettings() const;
    /// Update an exposed provider pair in one graph undo step; does not save files.
    [[nodiscard]] bool SetResolutionSettings(const RenderGraphResolutionSelection& selection,
                                             const QJsonValue& extent,
                                             const QJsonArray& scale,
                                             QString& outError);
    /// Add providers to an ordinary root configuration, preserving existing dimensions.
    /// Rejects ambiguous layouts without mutation. GUI thread only; Save remains explicit.
    [[nodiscard]] bool EnableResolutionSettings(QString& outError);
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Undo();
    void Redo();

    /// True when a document is open and no job or creation is running, so Validate, Build,
    /// Build & Preview and Cook Runtime can start.
    [[nodiscard]] bool CanBuild() const;
    /// Validate and dump the current draft snapshot. False when a job is active or nothing is open.
    [[nodiscard]] bool Validate();
    /// Build the draft snapshot's sibling .lushader.json into the project's render-graph build tree.
    [[nodiscard]] bool Build();
    /// Recompile/cook the immutable draft and preview only a current successful result. An
    /// unedited engine default previews the editor's prebuilt generation without a build.
    [[nodiscard]] bool BuildAndPreview();
    /// Authored configurations of the active root graph.
    [[nodiscard]] QStringList GetConfigurations() const;
    /// Effective authored configuration, resolving the asset default.
    [[nodiscard]] QString GetConfiguration() const;
    /// Choose an authored root configuration; unknown names are ignored.
    void SetConfiguration(const QString& configuration);
    /// Keep the project graph live in the World viewport, including external shader saves.
    void EnableWorldRendering();
    /// Use an existing shared native worker endpoint; empty retains the offline compiler path.
    void SetWorkerSocket(const QString& endpoint);
    /// Cook the last published build into a runtime generation for atomic activation.
    [[nodiscard]] bool CookRuntime();
    /// Cancel the running job or graph creation, and turn Live Preview off.
    void CancelJob();
    [[nodiscard]] bool IsJobActive() const;
    /// Turn background rebuilds of saved sources on or off. Turning it on schedules the
    /// saved version; successful saves and external refreshes coalesce into the newest
    /// saved build. Unsaved edits never trigger a build. Failures keep the working view.
    /// CancelJob and a project change turn it off. Emits
    /// LivePreviewChanged when the state changes.
    void SetLivePreview(bool isEnabled);
    [[nodiscard]] bool IsLivePreviewEnabled() const;
    /// Explicitly opt into advanced shader path overrides; two empty paths restore project
    /// detection. The validator and SPIRV-Cross overrides are kept.
    void SetBuildTools(const QString& helper, const QString& compiler);
    /// Replace the build settings that later builds use (Tools > Options). The worker socket
    /// is stored as an editor preference; tool overrides last until the project changes.
    void SetBuildSettings(const RenderGraphBuildSettings& settings);
    [[nodiscard]] const RenderGraphBuildSettings& GetBuildSettings() const noexcept
    {
        return mBuildSettings;
    }
    /// Shader tools the last successful build without overrides resolved from the project's
    /// SDK, or empty paths before such a build. Cleared when the project changes.
    [[nodiscard]] const RenderGraphBuildTools& GetResolvedBuildTools() const noexcept
    {
        return mResolvedTools;
    }
    /// SDK prefix of the last successful build without overrides, or empty.
    [[nodiscard]] const QString& GetResolvedSdk() const noexcept
    {
        return mResolvedSdk;
    }
    /// Publish a finished job result. Results for another request, project epoch or a
    /// closed document are ignored, so stale jobs never replace current diagnostics.
    void AcceptResult(const RenderGraphJobResult& result);

    [[nodiscard]] RenderGraphDocument& GetDocument() noexcept
    {
        return mDocument;
    }
    [[nodiscard]] const RenderGraphJobResult& GetLastResult() const noexcept
    {
        return mLastResult;
    }
    [[nodiscard]] bool SelectNode(const QString& file, const QString& configuration, const QString& node);
    /// Selected node as `file#configuration/node`, or empty.
    [[nodiscard]] QString GetSelection() const;
    /// Rows of the last published result, with structured validation batches shown once.
    /// Zero before any result. The shell's Error List shows
    /// the same rows in the same order (MakeRenderGraphErrorEntries).
    [[nodiscard]] foundation::int32 GetDiagnosticCount() const;
    /// Navigate to a diagnostic row's authored node (selecting it on the Canvas) or source
    /// line and column (showing it in the Source view). False for a row without an authored location.
    [[nodiscard]] bool ActivateDiagnostic(foundation::int32 row);
    [[nodiscard]] QString GetSourceFile() const;
    [[nodiscard]] foundation::int32 GetSourceLine() const;
    /// One-based Unicode scalar column of the Source view caret.
    [[nodiscard]] foundation::int32 GetSourceColumn() const;
    /// Canonical `<project>/out/render-graph/<package>/<graph>.lurgrt` path that Build and
    /// Cook Runtime write. Empty when nothing is open or the package lies outside the project.
    [[nodiscard]] QString GetRuntimePackagePath() const;
    /// Generation Preview Runtime loads: the editor's prebuilt engine default while the
    /// unedited default is shown (when this build has one), otherwise GetRuntimePackagePath().
    [[nodiscard]] QString GetPreviewPackagePath() const;
    [[nodiscard]] QString GetStatusText() const;
    /// Current source, activation, build or extraction diagnostic, even while another graph keeps rendering.
    [[nodiscard]] QString GetRenderProblem() const;
    /// Last current activation failure, including the runtime status and candidate generation.
    /// Cleared by a replacement candidate or document change, independently of compiler results.
    [[nodiscard]] LUDUS_INLINE const QString& GetActivationProblem() const noexcept
    {
        return mActivationProblem;
    }
    /// Failure that prevents scene preparation. Source/build errors are nonfatal while the
    /// prebuilt default prepares or a previously accepted graph remains active.
    [[nodiscard]] QString GetScenePreparationProblem() const;
    /// True while the viewport requests the engine default, including its pending first frame.
    [[nodiscard]] LUDUS_INLINE bool IsUsingDefaultRenderer() const noexcept
    {
        return mIsDefaultPreview;
    }

    /// Node canvas over the selected configuration (RG3b).
    [[nodiscard]] RenderGraphCanvas& GetCanvas() noexcept
    {
        return *mCanvas;
    }
    /// Move the selected node's inspector (fields, layout position, node creation and
    /// connections) into a shell tool window. Call once; the panel then lives under
    /// newParent but stays driven by this document's selection and edits.
    [[nodiscard]] QWidget* TakeInspectorPanel(QWidget* newParent);
    /// Move the runtime preview panel (Preview Runtime, status and image) into a shell tool
    /// window. Call once; the panel then lives under newParent.
    [[nodiscard]] QWidget* TakePreviewPanel(QWidget* newParent);
    /// Borrow the World viewport whose device and extraction feed the preview; null detaches.
    /// The source must outlive this workspace or be cleared first.
    void SetPreviewSource(RenderPreview* sourceOrNull);
    /// The borrowed World viewport renderer feeding the preview, or null.
    [[nodiscard]] LUDUS_INLINE RenderPreview* GetPreviewSourceOrNull() const noexcept
    {
        return mPreviewSourceOrNull;
    }
    /// Explicitly preview the last cooked runtime generation. False when the editor was built
    /// without the native GPU preview or the generation cannot be staged. Saved source
    /// requests keep using the built-in generation while its draft has unsaved edits.
    [[nodiscard]] bool PreviewRuntime(RenderGraphSource source = RenderGraphSource::Draft);
    /// Trusted preview, or null in builds without the native GPU preview.
    [[nodiscard]] RenderGraphPreview* GetPreviewOrNull() noexcept
    {
        return mPreviewOrNull;
    }
    /// Renderer tab texture viewer, or null in builds without the native GPU preview.
    [[nodiscard]] TextureViewer* GetTextureViewerOrNull() noexcept
    {
        return mTextureViewerOrNull;
    }
    /// Advance the preview from its source once (the timer calls this every display refresh).
    void ServicePreview();

Q_SIGNALS:
    void DocumentChanged();
    /// Node fields changed; the originating surface may retain its active controls.
    /// editOriginOrNull is borrowed during synchronous notification, or null for a full refresh.
    void NodeFieldsChanged(QWidget* editOriginOrNull);
    void JobFinished();
    /// What CanBuild, CanCreateGraph, IsJobActive or IsDirty report may have changed.
    void CommandsChanged();
    /// Live Preview was turned on or off (SetLivePreview, CancelJob or a project change).
    void LivePreviewChanged(bool isEnabled);
    /// A source/setup/extraction/activation diagnostic changed independently of a compiler job.
    void RenderProblemChanged();

private:
    [[nodiscard]] bool startJob(const QString& action,
                                const QStringList& arguments,
                                bool shouldCaptureSnapshot,
                                bool shouldPreview = false,
                                RenderGraphSource source = RenderGraphSource::Draft);
    [[nodiscard]] bool startBuild(bool shouldPreview, RenderGraphSource source = RenderGraphSource::Draft);
    [[nodiscard]] bool buildAndPreview(RenderGraphSource source);
    [[nodiscard]] bool isUneditedBuiltIn() const;
    [[nodiscard]] bool writeDocument();
    void openProjectGraph();
    void showDocument(const RenderGraphDocument& document);
    [[nodiscard]] QByteArray getBuildIdentity(RenderGraphSource source = RenderGraphSource::Draft) const;
    void scheduleLiveBuild(bool hasDebouncedSource = false);
    void refreshWatchedSources();
    void enableSceneMaterials();
    void expandSceneBundle(bool isExpanded);
    void applySourceEdit();
    void setProblem(QString& inoutProblem, const QString& text);
    void prepareDefaultPreview();
    void renderTree(bool shouldRefreshInspector = true, QWidget* editOriginOrNull = nullptr);
    void renderInspector();
    void populateNodeFields(QFormLayout& outFields, QWidget& inoutParent);
    void populateNodeNameFields(QFormLayout& outFields, QWidget& inoutParent, QDialog* closeOnRenameOrNull = nullptr);
    [[nodiscard]] bool renameSelectedNode(const QString& name, QString& outError);
    void promptRenameNode();
    void editGraphSettingsOnCanvas();
    void populateGraphSettings(QFormLayout& outFields, QWidget& inoutParent);
    void setGraphSetting(const QString& identity, const QJsonValue& value, QWidget* editOrigin);
    void editNodeOnCanvas(const QString& diagnosticField = {});
    void renderEdges();
    void renderSource();
    void restoreSourceHistory();
    void renderStatus();
    void updateActions();
    void documentEdited(RenderGraphDocumentStatus status,
                        bool shouldRefreshInspector = true,
                        QWidget* editOriginOrNull = nullptr);
    void replaceSelectedField(const QString& field, const QJsonValue& value);
    void setSelectedNodeEnabled(bool isEnabled);
    void replaceSelectedNode(const QJsonObject& node);
    void editConfigurationInterface();
    void addNode();
    void showNodeContextMenu(const QPoint& globalPosition);
    void setupGraphSelection();
    void syncTreeSelection();
    void syncCanvasSelection(const QStringList& nodes);
    void promptCreateConfiguration();
    void promptConfigurationTemplate(const QString& templateIdentity = {});
    /// Add one catalog node to the canvas configuration, optionally at a layout position.
    void addNodeTemplate(const RenderGraphTemplate& entry, const QPoint* positionOrNull);
    void insertQuickAddTemplate(const RenderGraphTemplate& entry);
    void activateSelectedNode();
    void openSourceLine(const QString& file, foundation::int32 line, foundation::int32 column = 0);
    /// Mark the shown source's messages from the last result. A new result always replaces
    /// them; otherwise an unchanged result/project/file keeps the editor's anchored markers.
    void updateSourceDiagnostics(bool isNewResult);
    [[nodiscard]] QJsonObject getDiagnosticLocation(foundation::int32 row) const;
    [[nodiscard]] QJsonObject selectedConfiguration() const;
    [[nodiscard]] QJsonObject selectedNode() const;
    [[nodiscard]] QString buildDirectory() const;
    [[nodiscard]] QString manifestFile() const;
    void renderCanvas();
    void renderPreview();
    void openSubgraph(const QString& asset, const QString& configuration);

    static constexpr foundation::int32 FILE_ROLE = Qt::UserRole;
    static constexpr foundation::int32 CONFIGURATION_ROLE = Qt::UserRole + 1;
    static constexpr foundation::int32 NODE_ROLE = Qt::UserRole + 2;
    ToolingPaths mTooling;
    QString mProjectRoot;
    foundation::uint64 mEpoch = 0;
    foundation::uint64 mNextRequest = 0;
    foundation::uint64 mActiveRequest = 0;
    foundation::uint64 mValidatedRevision = 0;
    bool mIsValidated = false;
    bool mIsRendering = false;
    bool mHasResult = false;
    bool mIsLivePreviewEnabled = false;
    RenderGraphDocument mDocument;
    RenderGraphCodeDocument mSourceDocument;
    ExternalCodeEditor mCodeEditor;
    RenderGraphJob mJob;
    RenderGraphCreation mCreation;
    RenderGraphJobResult mLastResult;
    QString mSelectedFile;
    QString mSelectedConfiguration;
    QString mSelectedNode;
    QStringList mSelectedNodes;
    QLabel* mPath;
    QStackedWidget* mContent;
    QLabel* mEmptyMessage;
    QPushButton* mCreateFirst;
    QComboBox* mConfiguration;
    QTreeWidget* mTree;
    QTabWidget* mViews;
    QScrollArea* mInspectorPanel;
    QWidget* mInspector;
    QFormLayout* mFields;
    QSpinBox* mLayoutX;
    QSpinBox* mLayoutY;
    QComboBox* mNewKind;
    QLabel* mNodeCreationMessage;
    QTableWidget* mEdges;
    QLineEdit* mEdgeFrom;
    QLineEdit* mEdgeTo;
    QComboBox* mSourceFile;
    CodeEditor* mSource;
    CodeDocumentController* mSourceController;
    CodeFormatter* mSourceFormatter;
    QPlainTextEdit* mSourceInterface;
    QPlainTextEdit* mPlan;
    RenderGraphBuildSettings mBuildSettings;
    RenderGraphBuildTools mResolvedTools;
    QString mResolvedSdk;
    QAction* mCustomize;
    RenderGraphLiveBuild mLiveBuild;
    RenderGraphSourceWatch mSourceWatch;
    RenderGraphWorker* mWorkerOrNull = nullptr;
    bool mHasPendingWorkerBuild = false;
    bool mShouldPreviewAfterWorker = false;
    RenderGraphSource mSourceAfterWorker = RenderGraphSource::Draft;
    bool mIsWorldRenderingEnabled = false;
    bool mIsDefaultPreview = false;
    bool mHasPendingSourceChange = false;
    QString mBuildProblem;
    QString mSourceProblem;
    QString mExtractionProblem;
    QString mActivationProblem;
    QLabel* mStatus;
    RenderGraphCanvas* mCanvas;
    RenderGraphQuickAdd* mQuickAdd;
    /// Canvas scene position where Quick Add last opened; chosen nodes are placed there.
    QPointF mQuickAddPosition;
    QWidget* mPreviewPage;
    QWidget* mPreviewImage;
    QLabel* mPreviewStatus;
    QPushButton* mPreviewButton;
    RenderPreview* mPreviewSourceOrNull = nullptr;
    RenderGraphPreview* mPreviewOrNull = nullptr;
    /// Renderer tab texture viewer; null in builds without the native GPU preview.
    TextureViewer* mTextureViewerOrNull = nullptr;
    QTimer mSourceEditDebounce;
    /// Result, project and file whose diagnostics the Source view marks.
    foundation::uint64 mSourceDiagnosticsRequest = 0;
    foundation::uint64 mSourceDiagnosticsEpoch = 0;
    QString mSourceDiagnosticsFile;
    ViewportFramePump mPreviewPump;
    foundation::uint64 mPreviewDeviceSerial = 0;
    foundation::uint64 mPreviewRevision = 0;
    bool mArePreviewInputsDirty = true;
    QSize mPreviewTargetSize;
    QByteArray mPreviewSceneIdentity;
    /// Semantic graph inputs of the runtime request; older activation errors cannot fail a new draft.
    QByteArray mPreviewInputIdentity;
    RenderGraphSource mPreviewSourceVersion = RenderGraphSource::Draft;
    foundation::math::Matrix4 mPreviewWorldToClip = foundation::math::Matrix4::Identity();
};
} // namespace ludus::editor
