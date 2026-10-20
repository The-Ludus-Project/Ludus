#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>

#include "internal/controller.h"
#include "internal/external_code_editor.h"
#include "internal/render_graph_build_settings.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_creation.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_source_watch.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
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
    /// Create and open an offscreen Renderer graph in the selected project's
    /// graphs/name directory. Completion is asynchronous through JobFinished.
    /// Existing files and the current draft are retained if creation fails.
    [[nodiscard]] bool CreateGraph(const QString& name);
    /// True when a project is bound and no job or creation is running, so New Graph and
    /// Open Graph can start.
    [[nodiscard]] bool CanCreateGraph() const;
    /// Ask to save a dirty draft, then ask for a graph name and start CreateGraph. False
    /// when CanCreateGraph() is false, a prompt is cancelled or creation cannot start.
    [[nodiscard]] bool PromptNewGraph();
    /// Ask to save a dirty draft, then choose a root .lurender.json in the project and open
    /// it. False when CanCreateGraph() is false, a prompt is cancelled or the graph cannot open.
    [[nodiscard]] bool PromptOpenGraph();
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool IsDirty() const;
    [[nodiscard]] bool CanSave() const;
    /// Save the draft. An unedited engine default is not project content: nothing is written
    /// and the result is true. An edited default creates its package under graphs/Main/.
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
    /// Turn background rebuilds of changed drafts on or off. Turning it on schedules a build
    /// of the current draft; later edits rebuild after a short delay and preview only
    /// matching successful results. CancelJob and a project change turn it off. Emits
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
    /// Rows of the last published result: one summary row, then one row per tool
    /// diagnostic of a failed result. Zero before any result. The shell's Error List shows
    /// the same rows in the same order (MakeRenderGraphErrorEntries).
    [[nodiscard]] foundation::int32 GetDiagnosticCount() const;
    /// Navigate to a diagnostic row's authored node (selecting it on the Canvas) or source
    /// line (showing it in the Source view). False for a row without an authored location.
    [[nodiscard]] bool ActivateDiagnostic(foundation::int32 row);
    [[nodiscard]] QString GetSourceFile() const;
    [[nodiscard]] foundation::int32 GetSourceLine() const;
    /// Canonical `<project>/out/render-graph/<package>/<graph>.lurgrt` path that Build and
    /// Cook Runtime write. Empty when nothing is open or the package lies outside the project.
    [[nodiscard]] QString GetRuntimePackagePath() const;
    /// Generation Preview Runtime loads: the editor's prebuilt engine default while the
    /// unedited default is shown (when this build has one), otherwise GetRuntimePackagePath().
    [[nodiscard]] QString GetPreviewPackagePath() const;
    [[nodiscard]] QString GetStatusText() const;
    /// Source refresh, preflight or extraction failure preventing a new World frame; empty after recovery.
    [[nodiscard]] QString GetRenderProblem() const;

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
    /// without the native GPU preview or the generation cannot be staged.
    [[nodiscard]] bool PreviewRuntime();
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
    void JobFinished();
    /// What CanBuild, CanCreateGraph, IsJobActive or IsDirty report may have changed.
    void CommandsChanged();
    /// Live Preview was turned on or off (SetLivePreview, CancelJob or a project change).
    void LivePreviewChanged(bool isEnabled);
    /// A source/setup/extraction diagnostic changed independently of a compiler job.
    void RenderProblemChanged();

private:
    [[nodiscard]] bool startJob(const QString& action,
                                const QStringList& arguments,
                                bool shouldCaptureSnapshot,
                                bool shouldPreview = false);
    [[nodiscard]] bool startBuild(bool shouldPreview);
    [[nodiscard]] bool isUneditedBuiltIn() const;
    [[nodiscard]] bool writeDocument();
    [[nodiscard]] bool hasPendingSourceText() const;
    void openProjectGraph();
    void showDocument(const RenderGraphDocument& document);
    [[nodiscard]] QByteArray getBuildIdentity() const;
    void scheduleLiveBuild();
    void refreshWatchedSources();
    void applySourceEdit();
    void setProblem(QString& inoutProblem, const QString& text);
    void renderTree();
    void renderInspector();
    void renderEdges();
    void renderSource();
    void renderStatus();
    void updateActions();
    void documentEdited(RenderGraphDocumentStatus status);
    void replaceSelectedField(const QString& field, const QJsonValue& value);
    void addNode();
    void openSourceLine(const QString& file, foundation::int32 line);
    [[nodiscard]] QJsonObject getDiagnosticLocation(foundation::int32 row) const;
    [[nodiscard]] QJsonObject selectedConfiguration() const;
    [[nodiscard]] QJsonObject selectedNode() const;
    [[nodiscard]] QString buildDirectory() const;
    [[nodiscard]] QString manifestFile() const;
    void renderCanvas();
    void renderPreview();
    void openSubgraph(const QString& asset, const QString& configuration);

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
    ExternalCodeEditor mCodeEditor;
    RenderGraphJob mJob;
    RenderGraphCreation mCreation;
    RenderGraphJobResult mLastResult;
    QString mSelectedFile;
    QString mSelectedConfiguration;
    QString mSelectedNode;
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
    QLineEdit* mNewIdentity;
    QTableWidget* mEdges;
    QLineEdit* mEdgeFrom;
    QLineEdit* mEdgeTo;
    QComboBox* mSourceFile;
    QPlainTextEdit* mSource;
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
    bool mIsWorldRenderingEnabled = false;
    bool mHasPendingSourceChange = false;
    QString mBuildProblem;
    QString mSourceProblem;
    QString mExtractionProblem;
    QLabel* mStatus;
    RenderGraphCanvas* mCanvas;
    QWidget* mPreviewPage;
    QWidget* mPreviewImage;
    QLabel* mPreviewStatus;
    QPushButton* mPreviewButton;
    RenderPreview* mPreviewSourceOrNull = nullptr;
    RenderGraphPreview* mPreviewOrNull = nullptr;
    /// Renderer tab texture viewer; null in builds without the native GPU preview.
    TextureViewer* mTextureViewerOrNull = nullptr;
    QTimer mSourceEditDebounce;
    QString mEditingSource;
    QByteArray mEditingText;
    QTimer mPreviewTimer;
    foundation::uint64 mPreviewDeviceSerial = 0;
    foundation::uint64 mPreviewRevision = 0;
    QByteArray mPreviewSceneIdentity;
    foundation::math::Matrix4 mPreviewWorldToClip = foundation::math::Matrix4::Identity();
};
} // namespace ludus::editor
