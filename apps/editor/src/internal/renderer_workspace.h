#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>

#include "internal/controller.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_creation.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_job.h"
#include "internal/render_graph_live_build.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
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
// See docs/development/render-graph-authoring.md#edit-and-reload-graphs-rg3 and
// docs/development/render-graph-authoring.md#node-canvas-and-preview-rg3b.
class RendererWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit RendererWorkspace(const ToolingPaths& tooling, QWidget* parentOrNull = nullptr);
    ~RendererWorkspace() override;
    RendererWorkspace(const RendererWorkspace&) = delete;
    RendererWorkspace& operator=(const RendererWorkspace&) = delete;

    /// Bind a project root and epoch; a different root closes the document and cancels jobs.
    void SetProject(const QString& root, foundation::uint64 epoch);
    /// Open a root .lurender.json inside the project; its directory is the package root.
    [[nodiscard]] bool OpenGraph(const QString& path);
    /// Create and open an offscreen Renderer graph in the selected project's
    /// graphs/name directory. Completion is asynchronous through JobFinished.
    /// Existing files and the current draft are retained if creation fails.
    [[nodiscard]] bool CreateGraph(const QString& name);
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool IsDirty() const;
    [[nodiscard]] bool CanSave() const;
    [[nodiscard]] bool Save();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Undo();
    void Redo();

    /// Validate and dump the current draft snapshot. False when a job is active or nothing is open.
    [[nodiscard]] bool Validate();
    /// Build the draft snapshot's sibling .lushader.json into the project's render-graph build tree.
    [[nodiscard]] bool Build();
    /// Recompile/cook the immutable draft and preview only a current successful result.
    [[nodiscard]] bool BuildAndPreview();
    /// Use an existing shared native worker endpoint; empty retains the offline compiler path.
    void SetWorkerSocket(const QString& endpoint);
    /// Cook the last published build into a runtime generation for atomic activation.
    [[nodiscard]] bool CookRuntime();
    void CancelJob();
    [[nodiscard]] bool IsJobActive() const;
    /// Explicitly opt into advanced shader path overrides; two empty paths restore project detection.
    void SetBuildTools(const QString& helper, const QString& compiler);
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
    [[nodiscard]] QString GetStatusText() const;

    /// Node canvas over the selected configuration (RG3b).
    [[nodiscard]] RenderGraphCanvas& GetCanvas() noexcept
    {
        return *mCanvas;
    }
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
    [[nodiscard]] QByteArray getBuildIdentity() const;
    void scheduleLiveBuild();
    void renderTree();
    void renderInspector();
    void renderEdges();
    void renderSource();
    void renderStatus();
    void updateActions();
    void documentEdited(RenderGraphDocumentStatus status);
    void replaceSelectedField(const QString& field, const QJsonValue& value);
    void addNode();
    void promptNewGraph();
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
    RenderGraphCreation mCreation;
    RenderGraphJobResult mLastResult;
    QString mSelectedFile;
    QString mSelectedConfiguration;
    QString mSelectedNode;
    QLabel* mPath;
    QPushButton* mNewGraph;
    QPushButton* mOpenGraph;
    QPushButton* mBuildPreview;
    QStackedWidget* mContent;
    QLabel* mEmptyMessage;
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
    QCheckBox* mOverrideBuildTools;
    QLabel* mBuildToolsStatus;
    QCheckBox* mLivePreview;
    RenderGraphLiveBuild mLiveBuild;
    QListWidget* mDiagnostics;
    QLabel* mStatus;
    RenderGraphCanvas* mCanvas;
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
