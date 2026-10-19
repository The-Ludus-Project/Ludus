#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>

#include "internal/controller.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_live_build.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphPreview;
class RenderPreview;
// RG3 Renderer workspace: edits one render-graph package through the typed
// RenderGraphDocument, and runs validation, builds and runtime cooking through
// the shared `ludus render-graph` backend on explicit immutable draft snapshots.
// Saved sources change only on Save. Diagnostics map to authored nodes and source
// lines. RG3b adds a node canvas over the same document operations and an explicit
// trusted preview of the cooked runtime generation on the World viewport device.
// A project without renderer/main.lurender.json shows the engine default read-only; the
// first edit turns it into a project draft that Save creates under renderer/, and the
// shader tools default to the ones this editor build used.
// See docs/development/render-graph-authoring.md#edit-and-reload-graphs-rg3,
// docs/development/render-graph-authoring.md#node-canvas-and-preview-rg3b and
// docs/development/render-graph-authoring.md#start-customizing-the-renderer.
class RendererWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit RendererWorkspace(const ToolingPaths& tooling, QWidget* parentOrNull = nullptr);
    ~RendererWorkspace() override;
    RendererWorkspace(const RendererWorkspace&) = delete;
    RendererWorkspace& operator=(const RendererWorkspace&) = delete;

    /// Bind a project root and epoch; a different root closes the document and cancels jobs,
    /// then shows the project's renderer/main.lurender.json, or the engine default when the
    /// project has none (or it cannot be loaded; the status line says why). Writes nothing.
    void SetProject(const QString& root, foundation::uint64 epoch);
    /// Open a root .lurender.json inside the project; its directory is the package root.
    [[nodiscard]] bool OpenGraph(const QString& path);
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool IsDirty() const;
    [[nodiscard]] bool CanSave() const;
    /// Save the draft. An unedited engine default is not project content: nothing is written
    /// and the result is true. An edited default creates its package under renderer/.
    [[nodiscard]] bool Save();
    /// True while the engine default is shown and Save has not created a project copy yet.
    [[nodiscard]] bool IsBuiltIn() const;
    /// Explicitly copy the shown engine default, with any draft edits, into renderer/ in the
    /// project and continue editing it there. False when the default is not shown or the copy
    /// fails; an existing file at the destination is never replaced.
    [[nodiscard]] bool CustomizeBuiltIn();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Undo();
    void Redo();

    /// Validate and dump the current draft snapshot. False when a job is active or nothing is open.
    [[nodiscard]] bool Validate();
    /// Build the draft snapshot's sibling .lushader.json into the project's render-graph build tree.
    [[nodiscard]] bool Build();
    /// Recompile/cook the immutable draft and preview only a current successful result. An
    /// unedited engine default previews the editor's prebuilt generation without a build.
    [[nodiscard]] bool BuildAndPreview();
    /// Use an existing shared native worker endpoint; empty retains the offline compiler path.
    void SetWorkerSocket(const QString& endpoint);
    /// Cook the last published build into a runtime generation for atomic activation.
    [[nodiscard]] bool CookRuntime();
    void CancelJob();
    [[nodiscard]] bool IsJobActive() const;
    /// Manual tool overrides (the Build Settings fields); empty entries use the defaults.
    void SetBuildTools(const RenderGraphBuildTools& overrides);
    /// Tools used when no override is set; the constructor takes GetEditorBuildTools().
    void SetDefaultBuildTools(const RenderGraphBuildTools& defaults);
    /// Tools the next build passes to the CLI: each override, or else its default.
    [[nodiscard]] RenderGraphBuildTools GetBuildTools() const;
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
    [[nodiscard]] foundation::int32 GetDiagnosticCount() const;
    /// Navigate to a diagnostic's authored node or source line.
    [[nodiscard]] bool ActivateDiagnostic(foundation::int32 row);
    [[nodiscard]] QString GetSourceFile() const;
    [[nodiscard]] foundation::int32 GetSourceLine() const;
    [[nodiscard]] QString GetRuntimePackagePath() const;
    /// Generation Preview Runtime loads: the editor's prebuilt engine default while the
    /// unedited default is shown (when this build has one), otherwise GetRuntimePackagePath().
    [[nodiscard]] QString GetPreviewPackagePath() const;
    [[nodiscard]] QString GetStatusText() const;

    /// Node canvas over the selected configuration (RG3b).
    [[nodiscard]] RenderGraphCanvas& GetCanvas() noexcept
    {
        return *mCanvas;
    }
    /// Borrow the World viewport whose device and extraction feed the preview; null detaches.
    /// The source must outlive this workspace or be cleared first.
    void SetPreviewSource(RenderPreview* sourceOrNull);
    /// Explicitly preview the last cooked runtime generation. False when the editor was built
    /// without the native GPU preview or the generation cannot be staged.
    [[nodiscard]] bool PreviewRuntime();
    /// Trusted preview, or null in builds without the native GPU preview.
    [[nodiscard]] RenderGraphPreview* GetPreviewOrNull() noexcept
    {
        return mPreviewOrNull;
    }
    /// Advance the preview from its source once (the timer calls this every display refresh).
    void ServicePreview();

Q_SIGNALS:
    void DocumentChanged();
    void JobFinished();

private:
    [[nodiscard]] bool startJob(const QString& action,
                                const QStringList& arguments,
                                bool shouldCaptureSnapshot,
                                bool shouldPreview = false);
    [[nodiscard]] bool startBuild(bool shouldPreview);
    [[nodiscard]] bool isUneditedBuiltIn() const;
    [[nodiscard]] bool writeDocument();
    void openProjectGraph();
    void showDocument(const RenderGraphDocument& document);
    void renderBuildTools();
    void showPreviewPage();
    [[nodiscard]] QByteArray getBuildIdentity() const;
    void scheduleLiveBuild();
    void renderTree();
    void renderInspector();
    void renderEdges();
    void renderSource();
    void renderStatus();
    void documentEdited(RenderGraphDocumentStatus status);
    void replaceSelectedField(const QString& field, const QJsonValue& value);
    void addNode();
    void openSourceLine(const QString& file, foundation::int32 line);
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
    RenderGraphDocument mDocument;
    RenderGraphJob mJob;
    RenderGraphJobResult mLastResult;
    QString mSelectedFile;
    QString mSelectedConfiguration;
    QString mSelectedNode;
    QLabel* mPath;
    QComboBox* mConfiguration;
    QPushButton* mValidate;
    QPushButton* mBuild;
    QPushButton* mCook;
    QPushButton* mCancel;
    QTreeWidget* mTree;
    QTabWidget* mTabs;
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
    QPlainTextEdit* mPlan;
    QLineEdit* mHelper;
    QLineEdit* mCompiler;
    QLineEdit* mValidator;
    QLineEdit* mSpirvCross;
    QLineEdit* mWorkerSocket;
    QLabel* mToolSummary;
    RenderGraphBuildTools mDefaultTools;
    QAction* mCustomize;
    QCheckBox* mLivePreview;
    RenderGraphLiveBuild mLiveBuild;
    QListWidget* mDiagnostics;
    QLabel* mStatus;
    RenderGraphCanvas* mCanvas;
    QWidget* mPreviewPage;
    QWidget* mPreviewImage;
    QLabel* mPreviewStatus;
    QPushButton* mPreviewButton;
    RenderPreview* mPreviewSourceOrNull = nullptr;
    RenderGraphPreview* mPreviewOrNull = nullptr;
    QTimer mPreviewTimer;
    foundation::uint64 mPreviewDeviceSerial = 0;
    foundation::uint64 mPreviewRevision = 0;
    QByteArray mPreviewSceneIdentity;
    foundation::math::Matrix4 mPreviewWorldToClip = foundation::math::Matrix4::Identity();
};
} // namespace ludus::editor
