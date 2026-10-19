#include "internal/renderer_workspace.h"

#include "internal/render_preview.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "editor_default_graph.h"
#    include "editor_renderer_static.h"
#    include "internal/render_graph_preview.h"
#    include "renderer_composite.h"
#endif

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int FILE_ROLE = Qt::UserRole;
constexpr int CONFIGURATION_ROLE = Qt::UserRole + 1;
constexpr int NODE_ROLE = Qt::UserRole + 2;
constexpr int LOCATION_ROLE = Qt::UserRole + 3;
constexpr int32 MAX_EXTENT = 16384;
constexpr int32 MAX_COORDINATE = 32768;
const QString DEFAULT_CONFIGURATION = QStringLiteral("(asset default)");
const QString HELPER_KEY = QStringLiteral("renderGraph/helper");
const QString COMPILER_KEY = QStringLiteral("renderGraph/compiler");
const QString VALIDATOR_KEY = QStringLiteral("renderGraph/validator");
const QString WORKER_SOCKET_KEY = QStringLiteral("renderGraph/workerSocket");
const QString SPIRV_CROSS_KEY = QStringLiteral("renderGraph/spirvCross");
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
/// Preview service period, about one 60 Hz display refresh.
constexpr int32 PREVIEW_INTERVAL_MILLISECONDS = 16;
#endif

/// Paints the last presented preview frame, letterboxed to its aspect ratio.
class PreviewImage final : public QWidget
{
public:
    explicit PreviewImage(QWidget* parentOrNull) : QWidget(parentOrNull)
    {
        setObjectName(QStringLiteral("renderGraphPreviewImage"));
        setMinimumSize(160, 120);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void SetImage(const QImage& image)
    {
        mImage = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(9, 12, 15));
        if (mImage.isNull() == true)
        {
            return;
        }
        const QSize size = mImage.size().scaled(this->size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, mImage);
    }

private:
    QImage mImage;
};

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
namespace default_graph = render_graph_packages::editor_default_graph;
namespace host_fixture = render_graph_packages::editor_renderer_static;

constexpr bool AreHostInterfacesEqual()
{
    for (usize i = 0; i < default_graph::INTERFACE_IDENTITY.GetSize(); ++i)
    {
        if (default_graph::INTERFACE_IDENTITY[i] != host_fixture::INTERFACE_IDENTITY[i])
        {
            return false;
        }
    }
    return true;
}
// The engine default is the editor's host interface. The frozen RG2 fixture that the
// preview parity tests cook must keep the same CPU interface, so either can be previewed.
static_assert(AreHostInterfacesEqual() == true,
              "The default render graph and the editor host fixture must share one RendererStatic interface");

bool AreMatricesEqual(const math::Matrix4& left, const math::Matrix4& right)
{
    for (usize i = 0; i < 4; ++i)
    {
        const math::Vector4& a = left.Columns[i];
        const math::Vector4& b = right.Columns[i];
        if (a.X != b.X || a.Y != b.Y || a.Z != b.Z || a.W != b.W)
        {
            return false;
        }
    }
    return true;
}
#endif

QStringList GetChoices(const QString& field)
{
    if (field == QStringLiteral("mode"))
    {
        return {QStringLiteral("ColorAndDepth"), QStringLiteral("DepthOnly")};
    }
    if (field == QStringLiteral("role"))
    {
        return {QStringLiteral("Presentation"), QStringLiteral("Offscreen")};
    }
    if (field == QStringLiteral("target_type") || field == QStringLiteral("type"))
    {
        return {QStringLiteral("ColorTexture"), QStringLiteral("DepthTexture")};
    }
    if (field == QStringLiteral("format"))
    {
        return {QStringLiteral("RGBA8Unorm"), QStringLiteral("Depth32Float")};
    }
    if (field == QStringLiteral("vertex_layout"))
    {
        return {QStringLiteral("RendererStatic")};
    }
    if (field == QStringLiteral("material"))
    {
        return {QStringLiteral("DefaultOpaque")};
    }
    if (field.endsWith(QStringLiteral("_load")))
    {
        return {QStringLiteral("Clear"), QStringLiteral("Load"), QStringLiteral("Discard")};
    }
    if (field.endsWith(QStringLiteral("_store")))
    {
        return {QStringLiteral("Store"), QStringLiteral("Discard")};
    }
    return {};
}

/// Default authored record for a new node; the shared validator still owns legality.
QJsonObject MakeNode(const QString& kind, const QString& identity)
{
    QJsonObject node{{QStringLiteral("id"), identity}, {QStringLiteral("kind"), kind}};
    if (kind == QStringLiteral("Camera"))
    {
        node.insert(QStringLiteral("active"), true);
        node.insert(QStringLiteral("role"), QStringLiteral("Offscreen"));
        node.insert(QStringLiteral("target_type"), QStringLiteral("ColorTexture"));
    }
    else if (kind == QStringLiteral("Target"))
    {
        node.insert(QStringLiteral("type"), QStringLiteral("ColorTexture"));
        node.insert(QStringLiteral("format"), QStringLiteral("RGBA8Unorm"));
        node.insert(QStringLiteral("extent"), QJsonArray{640, 480});
        node.insert(QStringLiteral("samples"), 1);
        node.insert(QStringLiteral("imported"), false);
        node.insert(QStringLiteral("initialized"), false);
        node.insert(QStringLiteral("presentation"), false);
    }
    else if (kind == QStringLiteral("Raster"))
    {
        node.insert(QStringLiteral("mode"), QStringLiteral("ColorAndDepth"));
        node.insert(QStringLiteral("vertex_layout"), QStringLiteral("RendererStatic"));
        node.insert(QStringLiteral("material"), QStringLiteral("DefaultOpaque"));
        node.insert(QStringLiteral("depth_load"), QStringLiteral("Clear"));
        node.insert(QStringLiteral("depth_store"), QStringLiteral("Store"));
        node.insert(QStringLiteral("color_load"), QStringLiteral("Clear"));
        node.insert(QStringLiteral("color_store"), QStringLiteral("Store"));
    }
    else if (kind == QStringLiteral("Subgraph"))
    {
        node.insert(QStringLiteral("asset"), QStringLiteral("rasterize.lurender.json"));
        node.insert(QStringLiteral("configuration"), QStringLiteral("ColorAndDepth"));
    }
    else if (kind == QStringLiteral("Export"))
    {
        node.insert(QStringLiteral("type"), QStringLiteral("ColorTexture"));
    }
    return node;
}

QString DescribeStatus(RenderGraphDocumentStatus status)
{
    switch (status)
    {
        case RenderGraphDocumentStatus::Ok:
            return {};
        case RenderGraphDocumentStatus::NotFound:
            return QStringLiteral("The source is not part of this render-graph package.");
        case RenderGraphDocumentStatus::Capacity:
            return QStringLiteral("The package exceeds the bounded source limits (64 files, 128 KiB each, 4 MiB).");
        case RenderGraphDocumentStatus::Malformed:
            return QStringLiteral("The draft graph is not a JSON object; fix it in the Source tab first.");
        case RenderGraphDocumentStatus::InvalidEdit:
            return QStringLiteral("That edit does not apply to the current draft (unknown or duplicate identity).");
        case RenderGraphDocumentStatus::Conflict:
            return QStringLiteral("A saved source changed on disk since it was loaded. Nothing was written; "
                                  "reopen the graph to review the external change.");
        case RenderGraphDocumentStatus::Locked:
            return QStringLiteral("Another editor is saving this package. Nothing was written.");
        case RenderGraphDocumentStatus::WriteFailed:
            return QStringLiteral("A source could not be replaced; unsaved files remain marked as changed.");
    }
    return {};
}

QString DescribeLocation(const QJsonObject& location)
{
    QStringList parts;
    if (location.contains(QStringLiteral("source")) == true)
    {
        QString source = location.value(QStringLiteral("source")).toString();
        if (location.contains(QStringLiteral("line")) == true)
        {
            source += QLatin1Char(':') + QString::number(location.value(QStringLiteral("line")).toInt());
        }
        parts.append(source);
    }
    if (location.contains(QStringLiteral("generated")) == true)
    {
        parts.append(QStringLiteral("generated wrapper of program ") +
                     location.value(QStringLiteral("program")).toString());
    }
    if (location.contains(QStringLiteral("configuration")) == true)
    {
        parts.append(QLatin1Char('#') + location.value(QStringLiteral("configuration")).toString());
    }
    if (location.contains(QStringLiteral("node")) == true)
    {
        parts.append(QStringLiteral("node ") + location.value(QStringLiteral("node")).toString());
    }
    return parts.join(QLatin1Char(' '));
}
} // namespace

RendererWorkspace::RendererWorkspace(const ToolingPaths& tooling, QWidget* parentOrNull)
    : QWidget(parentOrNull), mTooling(tooling), mPath(new QLabel(QStringLiteral("No render graph is open."), this)),
      mConfiguration(new QComboBox(this)), mValidate(new QPushButton(QStringLiteral("&Validate Draft"), this)),
      mBuild(new QPushButton(QStringLiteral("&Build Draft"), this)),
      mCook(new QPushButton(QStringLiteral("Cook &Runtime"), this)),
      mCancel(new QPushButton(QStringLiteral("Cancel"), this)), mTree(new QTreeWidget(this)),
      mTabs(new QTabWidget(this)), mInspector(new QWidget(this)), mFields(new QFormLayout),
      mLayoutX(new QSpinBox(this)), mLayoutY(new QSpinBox(this)), mNewKind(new QComboBox(this)),
      mNewIdentity(new QLineEdit(this)), mEdges(new QTableWidget(this)), mEdgeFrom(new QLineEdit(this)),
      mEdgeTo(new QLineEdit(this)), mSourceFile(new QComboBox(this)), mSource(new QPlainTextEdit(this)),
      mPlan(new QPlainTextEdit(this)), mHelper(new QLineEdit(this)), mCompiler(new QLineEdit(this)),
      mValidator(new QLineEdit(this)), mSpirvCross(new QLineEdit(this)), mWorkerSocket(new QLineEdit(this)),
      mToolSummary(new QLabel(this)), mDefaultTools(GetEditorBuildTools()),
      mCustomize(new QAction(QStringLiteral("&Customize in Project"), this)),
      mLivePreview(new QCheckBox(QStringLiteral("Live Preview"), this)), mDiagnostics(new QListWidget(this)),
      mStatus(new QLabel(this)), mCanvas(new RenderGraphCanvas(mDocument, this)), mPreviewPage(new QWidget(this)),
      mPreviewImage(new PreviewImage(this)), mPreviewStatus(new QLabel(this)),
      mPreviewButton(new QPushButton(QStringLiteral("&Preview Runtime"), this))
{
    setObjectName(QStringLiteral("rendererWorkspace"));
    // Open keeps its one-click action; the rarely needed explicit copy of the engine default
    // sits in its drop-down and in the source tree's context menu, never as a prompt.
    QToolButton* open = new QToolButton(this);
    open->setObjectName(QStringLiteral("renderGraphOpen"));
    open->setText(QStringLiteral("&Open Graph..."));
    open->setPopupMode(QToolButton::MenuButtonPopup);
    QMenu* openMenu = new QMenu(open);
    openMenu->addAction(mCustomize);
    open->setMenu(openMenu);
    mCustomize->setObjectName(QStringLiteral("renderGraphCustomize"));
    mCustomize->setToolTip(QStringLiteral("Copy the engine default renderer into renderer/ in this project and "
                                          "edit it there. Editing the default does the same on Save."));
    mPath->setObjectName(QStringLiteral("renderGraphPath"));
    mConfiguration->setObjectName(QStringLiteral("renderGraphConfiguration"));
    mConfiguration->setToolTip(QStringLiteral("Root configuration to validate and cook; choices are authored."));
    mValidate->setObjectName(QStringLiteral("renderGraphValidate"));
    mBuild->setObjectName(QStringLiteral("renderGraphBuild"));
    mCook->setObjectName(QStringLiteral("renderGraphCook"));
    mCancel->setEnabled(false);
    QHBoxLayout* actions = new QHBoxLayout;
    actions->addWidget(open);
    actions->addWidget(mPath, 1);
    actions->addWidget(new QLabel(QStringLiteral("Configuration"), this));
    actions->addWidget(mConfiguration);
    actions->addWidget(mValidate);
    actions->addWidget(mBuild);
    QPushButton* buildPreview = new QPushButton(QStringLiteral("Build && Preview"), this);
    buildPreview->setObjectName(QStringLiteral("renderGraphBuildPreview"));
    mLivePreview->setObjectName(QStringLiteral("renderGraphLivePreview"));
    mLivePreview->setToolTip(QStringLiteral("Rebuild changed drafts in the background and preview only matching "
                                            "successful results; saved files are unchanged."));
    actions->addWidget(buildPreview);
    actions->addWidget(mLivePreview);
    actions->addWidget(mCook);
    actions->addWidget(mCancel);

    mTree->setObjectName(QStringLiteral("renderGraphTree"));
    mTree->setHeaderLabel(QStringLiteral("Graph sources"));
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);

    // Inspector: fields of the selected node, its layout position and node creation.
    QVBoxLayout* inspectorLayout = new QVBoxLayout(mInspector);
    inspectorLayout->addLayout(mFields);
    mLayoutX->setRange(-MAX_COORDINATE, MAX_COORDINATE);
    mLayoutY->setRange(-MAX_COORDINATE, MAX_COORDINATE);
    QHBoxLayout* layoutRow = new QHBoxLayout;
    layoutRow->addWidget(new QLabel(QStringLiteral("Layout x/y (presentation only)"), this));
    layoutRow->addWidget(mLayoutX);
    layoutRow->addWidget(mLayoutY);
    inspectorLayout->addLayout(layoutRow);
    QPushButton* remove = new QPushButton(QStringLiteral("Remove Node"), this);
    inspectorLayout->addWidget(remove);
    mNewKind->addItems({QStringLiteral("Scene"),
                        QStringLiteral("Camera"),
                        QStringLiteral("Target"),
                        QStringLiteral("Raster"),
                        QStringLiteral("Subgraph"),
                        QStringLiteral("Export"),
                        QStringLiteral("Present")});
    mNewIdentity->setPlaceholderText(QStringLiteral("new node ID"));
    QPushButton* add = new QPushButton(QStringLiteral("Add Node"), this);
    QHBoxLayout* addRow = new QHBoxLayout;
    addRow->addWidget(mNewKind);
    addRow->addWidget(mNewIdentity, 1);
    addRow->addWidget(add);
    inspectorLayout->addLayout(addRow);
    mEdges->setObjectName(QStringLiteral("renderGraphEdges"));
    mEdges->setColumnCount(2);
    mEdges->setHorizontalHeaderLabels({QStringLiteral("From"), QStringLiteral("To")});
    mEdges->horizontalHeader()->setStretchLastSection(true);
    mEdges->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mEdges->setSelectionBehavior(QAbstractItemView::SelectRows);
    inspectorLayout->addWidget(new QLabel(QStringLiteral("Connections in this configuration"), this));
    inspectorLayout->addWidget(mEdges, 1);
    mEdgeFrom->setPlaceholderText(QStringLiteral("node.port"));
    mEdgeTo->setPlaceholderText(QStringLiteral("node.port"));
    QPushButton* connectButton = new QPushButton(QStringLiteral("Connect"), this);
    QPushButton* disconnectButton = new QPushButton(QStringLiteral("Disconnect Selected"), this);
    QHBoxLayout* edgeRow = new QHBoxLayout;
    edgeRow->addWidget(mEdgeFrom);
    edgeRow->addWidget(mEdgeTo);
    edgeRow->addWidget(connectButton);
    edgeRow->addWidget(disconnectButton);
    inspectorLayout->addLayout(edgeRow);

    // Source: graph, build-manifest/schema and shader text share the same draft/undo/save.
    QWidget* sourcePage = new QWidget(this);
    QVBoxLayout* sourceLayout = new QVBoxLayout(sourcePage);
    mSourceFile->setObjectName(QStringLiteral("renderGraphSourceFile"));
    mSource->setObjectName(QStringLiteral("renderGraphSource"));
    QPushButton* applyText = new QPushButton(QStringLiteral("Apply Text to Draft"), this);
    QPushButton* revertText = new QPushButton(QStringLiteral("Revert Text"), this);
    QHBoxLayout* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(mSourceFile, 1);
    sourceRow->addWidget(applyText);
    sourceRow->addWidget(revertText);
    sourceLayout->addLayout(sourceRow);
    sourceLayout->addWidget(mSource, 1);

    mPlan->setObjectName(QStringLiteral("renderGraphPlan"));
    mPlan->setReadOnly(true);
    mPlan->setPlaceholderText(QStringLiteral("Validate the draft to inspect its deterministic serial plan."));

    // Build Settings: the editor build's own tools by default; manual paths are advanced overrides.
    QWidget* buildPage = new QWidget(this);
    QVBoxLayout* buildPageLayout = new QVBoxLayout(buildPage);
    mToolSummary->setObjectName(QStringLiteral("renderGraphToolSummary"));
    mToolSummary->setWordWrap(true);
    buildPageLayout->addWidget(mToolSummary);
    QGroupBox* overrides = new QGroupBox(QStringLiteral("Tool overrides (advanced)"), buildPage);
    overrides->setObjectName(QStringLiteral("renderGraphToolOverrides"));
    QFormLayout* buildLayout = new QFormLayout(overrides);
    QSettings settings;
    mHelper->setText(settings.value(HELPER_KEY).toString());
    mCompiler->setText(settings.value(COMPILER_KEY).toString());
    mValidator->setText(settings.value(VALIDATOR_KEY).toString());
    mSpirvCross->setText(settings.value(SPIRV_CROSS_KEY).toString());
    mWorkerSocket->setText(
        settings.value(WORKER_SOCKET_KEY, qEnvironmentVariable("LUDUS_SHADER_WORKER_SOCKET")).toString());
    mHelper->setObjectName(QStringLiteral("renderGraphHelper"));
    mCompiler->setObjectName(QStringLiteral("renderGraphCompiler"));
    mWorkerSocket->setObjectName(QStringLiteral("renderGraphWorkerSocket"));
    mWorkerSocket->setPlaceholderText(QStringLiteral("existing ludus-shader-worker socket"));
    buildLayout->addRow(QStringLiteral("SDK shader helper"), mHelper);
    buildLayout->addRow(QStringLiteral("Slang compiler"), mCompiler);
    buildLayout->addRow(QStringLiteral("SPIR-V validator"), mValidator);
    buildLayout->addRow(QStringLiteral("SPIRV-Cross"), mSpirvCross);
    buildLayout->addRow(QStringLiteral("Native shader worker"), mWorkerSocket);
    buildPageLayout->addWidget(overrides);
    QLabel* buildOutput = new QLabel(QStringLiteral("Builds and runtime generations go to out/render-graph in the "
                                                    "project; saved sources are never modified by a build."),
                                     buildPage);
    buildOutput->setWordWrap(true);
    buildPageLayout->addWidget(buildOutput);
    buildPageLayout->addStretch(1);

    // Preview: the cooked runtime generation on the World viewport device (RG3b).
    QVBoxLayout* previewLayout = new QVBoxLayout(mPreviewPage);
    mPreviewButton->setObjectName(QStringLiteral("renderGraphPreview"));
    mPreviewButton->setToolTip(QStringLiteral("Stage the last cooked .lurgrt beside the drawing generation; it is "
                                              "shown only after it activates and renders the World extraction."));
    mPreviewStatus->setObjectName(QStringLiteral("renderGraphPreviewStatus"));
    mPreviewStatus->setWordWrap(true);
    previewLayout->addWidget(mPreviewButton);
    previewLayout->addWidget(mPreviewStatus);
    previewLayout->addWidget(mPreviewImage, 1);

    mTabs->addTab(mInspector, QStringLiteral("Inspector"));
    mTabs->addTab(sourcePage, QStringLiteral("Source"));
    mTabs->addTab(mPlan, QStringLiteral("Plan"));
    mTabs->addTab(buildPage, QStringLiteral("Build Settings"));
    mTabs->addTab(mPreviewPage, QStringLiteral("Preview"));

    mDiagnostics->setObjectName(QStringLiteral("renderGraphDiagnostics"));
    mStatus->setObjectName(QStringLiteral("renderGraphStatus"));
    mStatus->setWordWrap(true);
    QSplitter* horizontal = new QSplitter(Qt::Horizontal, this);
    horizontal->addWidget(mTree);
    horizontal->addWidget(mCanvas);
    horizontal->addWidget(mTabs);
    horizontal->setStretchFactor(1, 2);
    horizontal->setStretchFactor(2, 1);
    QSplitter* vertical = new QSplitter(Qt::Vertical, this);
    vertical->addWidget(horizontal);
    vertical->addWidget(mDiagnostics);
    vertical->setStretchFactor(0, 1);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(actions);
    layout->addWidget(vertical, 1);
    layout->addWidget(mStatus);

    connect(open, &QToolButton::clicked, this, [this]() {
        if (mProjectRoot.isEmpty() == true || ConfirmDiscard() == false)
        {
            return;
        }
        const uint64 epoch = mEpoch;
        const QString path = QFileDialog::getOpenFileName(this,
                                                          QStringLiteral("Open Render Graph"),
                                                          mProjectRoot,
                                                          QStringLiteral("Render graphs (*.lurender.json)"));
        if (path.isEmpty() == false && epoch == mEpoch && OpenGraph(path) == false)
        {
            mStatus->setText(QStringLiteral("Open a root .lurender.json inside the project folder."));
        }
    });
    connect(mConfiguration, &QComboBox::currentTextChanged, this, [this]() { scheduleLiveBuild(); });
    connect(mValidate, &QPushButton::clicked, this, [this]() { static_cast<void>(Validate()); });
    connect(mBuild, &QPushButton::clicked, this, [this]() { static_cast<void>(Build()); });
    connect(buildPreview, &QPushButton::clicked, this, [this]() {
        // The explicit gesture shows its result; Live Preview rebuilds never switch tabs.
        showPreviewPage();
        static_cast<void>(BuildAndPreview());
    });
    connect(mCustomize, &QAction::triggered, this, [this]() { static_cast<void>(CustomizeBuiltIn()); });
    connect(mTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        if (mCustomize->isVisible() == false)
        {
            return;
        }
        QMenu menu(this);
        menu.addAction(mCustomize);
        menu.exec(mTree->viewport()->mapToGlobal(position));
    });
    connect(&mLiveBuild, &RenderGraphLiveBuild::BuildRequested, this, [this]() {
        static_cast<void>(BuildAndPreview());
    });
    connect(mLivePreview, &QCheckBox::toggled, this, [this](bool isEnabled) {
        mLiveBuild.SetEnabled(isEnabled);
        if (isEnabled == true)
        {
            scheduleLiveBuild();
        }
    });
    connect(mCook, &QPushButton::clicked, this, [this]() { static_cast<void>(CookRuntime()); });
    connect(mCancel, &QPushButton::clicked, this, &RendererWorkspace::CancelJob);
    connect(mTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (mIsRendering == true || current == nullptr)
        {
            return;
        }
        mSelectedFile = current->data(0, FILE_ROLE).toString();
        mSelectedConfiguration = current->data(0, CONFIGURATION_ROLE).toString();
        mSelectedNode = current->data(0, NODE_ROLE).toString();
        renderInspector();
        renderEdges();
        renderCanvas();
    });
    connect(mCanvas, &RenderGraphCanvas::NodeSelected, this, [this](const QString& node) {
        mSelectedFile = mCanvas->GetFile();
        mSelectedConfiguration = mCanvas->GetConfiguration();
        mSelectedNode = node;
        renderTree();
    });
    connect(mCanvas,
            &RenderGraphCanvas::MoveRequested,
            this,
            [this](const QVector<RenderGraphNodePosition>& positions) {
                documentEdited(mDocument.MoveNodes(mCanvas->GetFile(), mCanvas->GetConfiguration(), positions));
            });
    connect(mCanvas, &RenderGraphCanvas::ConnectRequested, this, [this](const QString& from, const QString& to) {
        documentEdited(mDocument.Connect(mCanvas->GetFile(), mCanvas->GetConfiguration(), from, to));
    });
    connect(mCanvas, &RenderGraphCanvas::DisconnectRequested, this, [this](const QString& from, const QString& to) {
        documentEdited(mDocument.Disconnect(mCanvas->GetFile(), mCanvas->GetConfiguration(), from, to));
    });
    connect(mCanvas, &RenderGraphCanvas::RemoveRequested, this, [this](const QString& node) {
        if (node == mSelectedNode)
        {
            mSelectedNode.clear();
        }
        documentEdited(mDocument.RemoveNode(mCanvas->GetFile(), mCanvas->GetConfiguration(), node));
    });
    connect(mCanvas, &RenderGraphCanvas::LinkRejected, this, [this](const QString& reason) {
        mStatus->setText(QStringLiteral("Link rejected - %1").arg(reason));
    });
    connect(mCanvas, &RenderGraphCanvas::SubgraphActivated, this, &RendererWorkspace::openSubgraph);
    connect(mPreviewButton, &QPushButton::clicked, this, [this]() { static_cast<void>(PreviewRuntime()); });
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mPreviewOrNull = new RenderGraphPreview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment(),
        this);
    connect(mPreviewOrNull, &RenderGraphPreview::Changed, this, &RendererWorkspace::renderPreview);
    connect(mPreviewOrNull, &RenderGraphPreview::FramePresented, this, &RendererWorkspace::renderPreview);
    mPreviewTimer.setInterval(PREVIEW_INTERVAL_MILLISECONDS);
    connect(&mPreviewTimer, &QTimer::timeout, this, &RendererWorkspace::ServicePreview);
#endif
    connect(mLayoutX, &QSpinBox::editingFinished, this, [this]() {
        if (mSelectedNode.isEmpty() == false)
        {
            documentEdited(mDocument.MoveNode(mSelectedFile,
                                              mSelectedConfiguration,
                                              mSelectedNode,
                                              mLayoutX->value(),
                                              mLayoutY->value()));
        }
    });
    connect(mLayoutY, &QSpinBox::editingFinished, mLayoutX, &QSpinBox::editingFinished);
    connect(remove, &QPushButton::clicked, this, [this]() {
        if (mSelectedNode.isEmpty() == false)
        {
            const QString node = mSelectedNode;
            mSelectedNode.clear();
            documentEdited(mDocument.RemoveNode(mSelectedFile, mSelectedConfiguration, node));
        }
    });
    connect(add, &QPushButton::clicked, this, &RendererWorkspace::addNode);
    connect(connectButton, &QPushButton::clicked, this, [this]() {
        documentEdited(mDocument.Connect(mSelectedFile,
                                         mSelectedConfiguration,
                                         mEdgeFrom->text().trimmed(),
                                         mEdgeTo->text().trimmed()));
    });
    connect(disconnectButton, &QPushButton::clicked, this, [this]() {
        const int32 row = mEdges->currentRow();
        if (row >= 0)
        {
            documentEdited(mDocument.Disconnect(mSelectedFile,
                                                mSelectedConfiguration,
                                                mEdges->item(row, 0)->text(),
                                                mEdges->item(row, 1)->text()));
        }
    });
    connect(mSourceFile, &QComboBox::currentTextChanged, this, [this]() { renderSource(); });
    connect(applyText, &QPushButton::clicked, this, [this]() {
        documentEdited(mDocument.SetText(mSourceFile->currentText(), mSource->toPlainText().toUtf8()));
    });
    connect(revertText, &QPushButton::clicked, this, [this]() { renderSource(); });
    connect(mDiagnostics, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        static_cast<void>(ActivateDiagnostic(mDiagnostics->row(item)));
    });
    for (QLineEdit* tool : {mHelper, mCompiler, mValidator, mSpirvCross, mWorkerSocket})
    {
        connect(tool, &QLineEdit::editingFinished, this, [this]() {
            QSettings preferences;
            preferences.setValue(HELPER_KEY, mHelper->text());
            preferences.setValue(COMPILER_KEY, mCompiler->text());
            preferences.setValue(VALIDATOR_KEY, mValidator->text());
            preferences.setValue(SPIRV_CROSS_KEY, mSpirvCross->text());
            preferences.setValue(WORKER_SOCKET_KEY, mWorkerSocket->text());
        });
    }
    connect(&mJob, &RenderGraphJob::Finished, this, &RendererWorkspace::AcceptResult);
    renderBuildTools();
    renderStatus();
    renderPreview();
}

RendererWorkspace::~RendererWorkspace() = default;

void RendererWorkspace::SetProject(const QString& root, uint64 epoch)
{
    mLiveBuild.Reset();
    const bool isSameRoot = root == mProjectRoot;
    mEpoch = epoch;
    if (isSameRoot == true)
    {
        return;
    }
    CancelJob();
    mProjectRoot = root;
    mDocument.Reset();
    mSelectedFile.clear();
    mSelectedConfiguration.clear();
    mSelectedNode.clear();
    mLastResult = {};
    mIsValidated = false;
    mDiagnostics->clear();
    mPlan->clear();
    mPath->setText(QStringLiteral("No render graph is open."));
    renderTree();
    renderSource();
    renderStatus();
    openProjectGraph();
    Q_EMIT DocumentChanged();
}

void RendererWorkspace::openProjectGraph()
{
    if (mProjectRoot.isEmpty() == true)
    {
        return;
    }
    // The project's own graph wins; otherwise the engine default stands in, so rendering
    // never asks for a graph. Neither path writes a file.
    const QString package = QDir(mProjectRoot).filePath(PROJECT_RENDER_GRAPH_DIRECTORY);
    const QString projectGraph = QDir(package).filePath(DEFAULT_RENDER_GRAPH_ROOT);
    QString problem;
    if (QFileInfo::exists(projectGraph) == true)
    {
        if (OpenGraph(projectGraph) == true)
        {
            return;
        }
        problem = mStatus->text();
    }
    RenderGraphDocument candidate;
    if (candidate.LoadBuiltIn(GetDefaultRenderGraphSource(), DEFAULT_RENDER_GRAPH_ROOT, package) !=
        RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(QStringLiteral("The engine default render graph is unavailable in this editor build."));
        return;
    }
    showDocument(candidate);
    if (problem.isEmpty() == false)
    {
        mStatus->setText(QStringLiteral("%1/%2 could not be opened (%3) Showing the engine default; Save will not "
                                        "replace the existing files.")
                             .arg(PROJECT_RENDER_GRAPH_DIRECTORY, DEFAULT_RENDER_GRAPH_ROOT, problem));
    }
}

bool RendererWorkspace::OpenGraph(const QString& path)
{
    const QFileInfo information(path);
    const QString canonical = information.canonicalFilePath();
    const QString project = QFileInfo(mProjectRoot).canonicalFilePath();
    if (canonical.isEmpty() == true || project.isEmpty() == true ||
        canonical.startsWith(project + QLatin1Char('/')) == false)
    {
        return false;
    }
    RenderGraphDocument candidate;
    const RenderGraphDocumentStatus status =
        candidate.Load(QFileInfo(canonical).absolutePath(), QFileInfo(canonical).fileName());
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(DescribeStatus(status));
        return false;
    }
    showDocument(candidate);
    Q_EMIT DocumentChanged();
    return true;
}

void RendererWorkspace::showDocument(const RenderGraphDocument& document)
{
    CancelJob();
    mDocument = document;
    mSelectedFile = mDocument.GetRootGraph();
    mSelectedConfiguration.clear();
    mSelectedNode.clear();
    mLastResult = {};
    mIsValidated = false;
    mDiagnostics->clear();
    mPlan->clear();
    renderTree();
    renderSource();
    renderStatus();
}

bool RendererWorkspace::ConfirmDiscard()
{
    if (IsDirty() == false)
    {
        return true;
    }
    const QMessageBox::StandardButton answer =
        QMessageBox::warning(this,
                             QStringLiteral("Unsaved render graph"),
                             QStringLiteral("Save the render-graph draft before continuing?"),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Save);
    if (answer == QMessageBox::Save)
    {
        return Save();
    }
    return answer == QMessageBox::Discard;
}

bool RendererWorkspace::IsDirty() const
{
    return mDocument.IsDirty();
}

bool RendererWorkspace::CanSave() const
{
    return mDocument.IsLoaded() == true && mDocument.IsDirty() == true;
}

bool RendererWorkspace::Save()
{
    if (isUneditedBuiltIn() == true)
    {
        // The unedited engine default is not project content; Save never copies it.
        return true;
    }
    return writeDocument();
}

bool RendererWorkspace::IsBuiltIn() const
{
    return mDocument.IsBuiltIn();
}

bool RendererWorkspace::CustomizeBuiltIn()
{
    if (mDocument.IsBuiltIn() == false || mProjectRoot.isEmpty() == true)
    {
        return false;
    }
    return writeDocument();
}

bool RendererWorkspace::writeDocument()
{
    const bool wasBuiltIn = mDocument.IsBuiltIn();
    QStringList saved;
    const RenderGraphDocumentStatus status = mDocument.Save(saved);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(wasBuiltIn == true && status == RenderGraphDocumentStatus::Conflict
                             ? QStringLiteral("%1/ already holds render-graph files; nothing was written. Open "
                                              "its graph with Open Graph... instead.")
                                   .arg(PROJECT_RENDER_GRAPH_DIRECTORY)
                             : DescribeStatus(status));
        Q_EMIT DocumentChanged();
        return false;
    }
    renderTree();
    renderStatus();
    if (wasBuiltIn == true)
    {
        // Say where the copy went; from now on the project's graph replaces the default.
        mStatus->setText(QStringLiteral("Created %1 in the project from the engine default (%2 files). This project "
                                        "now uses it; edits save there.")
                             .arg(QDir(mProjectRoot).relativeFilePath(mDocument.GetPackageRoot()))
                             .arg(saved.size()));
    }
    Q_EMIT DocumentChanged();
    return true;
}

bool RendererWorkspace::CanUndo() const
{
    return mDocument.CanUndo();
}

bool RendererWorkspace::CanRedo() const
{
    return mDocument.CanRedo();
}

void RendererWorkspace::Undo()
{
    if (mDocument.Undo() == true)
    {
        documentEdited(RenderGraphDocumentStatus::Ok);
    }
}

void RendererWorkspace::Redo()
{
    if (mDocument.Redo() == true)
    {
        documentEdited(RenderGraphDocumentStatus::Ok);
    }
}

bool RendererWorkspace::startJob(const QString& action,
                                 const QStringList& arguments,
                                 bool shouldCaptureSnapshot,
                                 bool shouldPreview)
{
    if (mDocument.IsLoaded() == false || mProjectRoot.isEmpty() == true || mJob.IsActive() == true)
    {
        return false;
    }
    RenderGraphJobRequest request;
    request.Request = ++mNextRequest;
    request.ProjectEpoch = mEpoch;
    request.Revision = mDocument.GetRevision();
    request.Action = action;
    request.ShouldPreview = shouldPreview;
    request.InputIdentity = getBuildIdentity();
    if (shouldCaptureSnapshot == true)
    {
        // Jobs read an immutable copy of the draft, never the saved package or live widgets.
        request.SnapshotDirectory =
            QDir(mProjectRoot)
                .filePath(QStringLiteral("out/render-graph/drafts/%1-%2").arg(mEpoch).arg(request.Request));
        QDir(request.SnapshotDirectory).removeRecursively();
        if (QDir().mkpath(request.SnapshotDirectory) == false ||
            mDocument.WriteSnapshot(request.SnapshotDirectory) != RenderGraphDocumentStatus::Ok)
        {
            QDir(request.SnapshotDirectory).removeRecursively();
            mStatus->setText(QStringLiteral("The draft snapshot could not be written under out/render-graph."));
            return false;
        }
    }
    for (const QString& argument : arguments)
    {
        request.Arguments.append(QString(argument).replace(QStringLiteral("{snapshot}"), request.SnapshotDirectory));
    }
    if (mJob.Start(mTooling, request) == false)
    {
        QDir(request.SnapshotDirectory).removeRecursively();
        mStatus->setText(QStringLiteral("The render-graph tool is unavailable from this editor's tooling root."));
        return false;
    }
    mLiveBuild.SetBusy(true);
    mActiveRequest = request.Request;
    mCancel->setEnabled(true);
    mStatus->setText(
        QStringLiteral("Running render-graph %1 on draft revision %2...").arg(action).arg(request.Revision));
    return true;
}

bool RendererWorkspace::Validate()
{
    QStringList arguments{QStringLiteral("{snapshot}/") + mDocument.GetRootGraph(),
                          QStringLiteral("--root"),
                          QStringLiteral("{snapshot}")};
    if (mConfiguration->currentText() != DEFAULT_CONFIGURATION && mConfiguration->currentText().isEmpty() == false)
    {
        arguments += {QStringLiteral("--configuration"), mConfiguration->currentText()};
    }
    return startJob(QStringLiteral("dump"), arguments, true);
}

bool RendererWorkspace::Build()
{
    return startBuild(false);
}

bool RendererWorkspace::BuildAndPreview()
{
    // The unedited default already has a generation built with the editor: no tools needed.
    if (isUneditedBuiltIn() == true && GetDefaultRenderGraphRuntime().isEmpty() == false && mPreviewOrNull != nullptr)
    {
        mLiveBuild.MarkRequested(getBuildIdentity());
        return PreviewRuntime();
    }
    if (startBuild(true) == false)
    {
        return false;
    }
    mLiveBuild.MarkRequested(getBuildIdentity());
    return true;
}

void RendererWorkspace::SetWorkerSocket(const QString& endpoint)
{
    mWorkerSocket->setText(endpoint);
}

bool RendererWorkspace::startBuild(bool shouldPreview)
{
    if (mDocument.GetFiles().contains(manifestFile()) == false)
    {
        mStatus->setText(
            QStringLiteral("Create %1 with `ludus render-graph new-build` before building.").arg(manifestFile()));
        return false;
    }
    const RenderGraphBuildTools tools = GetBuildTools();
    if (tools.Helper.isEmpty() == true || tools.Compiler.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("This editor build has no shader tools of its own. Set the SDK shader helper "
                                        "and Slang compiler under Build Settings > Tool overrides."));
        return false;
    }
    QStringList arguments{QStringLiteral("{snapshot}/") + manifestFile(),
                          buildDirectory(),
                          QStringLiteral("--root"),
                          QStringLiteral("{snapshot}"),
                          QStringLiteral("--helper"),
                          tools.Helper,
                          QStringLiteral("--compiler"),
                          tools.Compiler};
    if (tools.Validator.isEmpty() == false)
    {
        arguments += {QStringLiteral("--validator"), tools.Validator};
    }
    if (tools.SpirvCross.isEmpty() == false)
    {
        arguments += {QStringLiteral("--spirv-cross"), tools.SpirvCross};
    }
    if (mWorkerSocket->text().isEmpty() == false)
    {
        arguments += {QStringLiteral("--worker-socket"), mWorkerSocket->text()};
    }
    if (shouldPreview == true)
    {
        const QString configuration = mConfiguration->currentText() == DEFAULT_CONFIGURATION
                                          ? QStringLiteral("Default")
                                          : mConfiguration->currentText();
        arguments += {QStringLiteral("--runtime-package"),
                      GetRuntimePackagePath(),
                      QStringLiteral("--configuration"),
                      configuration};
    }
    return startJob(QStringLiteral("build"), arguments, true, shouldPreview);
}

bool RendererWorkspace::CookRuntime()
{
    const QString configuration = mConfiguration->currentText() == DEFAULT_CONFIGURATION
                                      ? QStringLiteral("Default")
                                      : mConfiguration->currentText();
    return startJob(QStringLiteral("cook-runtime"),
                    {buildDirectory(), GetRuntimePackagePath(), QStringLiteral("--configuration"), configuration},
                    false);
}

void RendererWorkspace::CancelJob()
{
    mLivePreview->setChecked(false);
    mLiveBuild.SetEnabled(false);
    mJob.Cancel();
}

bool RendererWorkspace::IsJobActive() const
{
    return mJob.IsActive();
}

void RendererWorkspace::SetBuildTools(const RenderGraphBuildTools& overrides)
{
    mHelper->setText(overrides.Helper);
    mCompiler->setText(overrides.Compiler);
    mValidator->setText(overrides.Validator);
    mSpirvCross->setText(overrides.SpirvCross);
}

void RendererWorkspace::SetDefaultBuildTools(const RenderGraphBuildTools& defaults)
{
    mDefaultTools = defaults;
    renderBuildTools();
}

RenderGraphBuildTools RendererWorkspace::GetBuildTools() const
{
    const RenderGraphBuildTools overrides
    {
        .Helper = mHelper->text(),
        .Compiler = mCompiler->text(),
        .Validator = mValidator->text(),
        .SpirvCross = mSpirvCross->text(),
    };
    return ResolveBuildTools(overrides, mDefaultTools);
}

void RendererWorkspace::renderBuildTools()
{
    // Placeholders show the default each empty field uses, so overriding is visibly optional.
    const auto describe = [](QLineEdit* field, const QString& fallback, const QString& hint) {
        field->setPlaceholderText(fallback.isEmpty() == false ? QStringLiteral("default: ") + fallback : hint);
        field->setToolTip(fallback.isEmpty() == false ? fallback : hint);
    };
    describe(mHelper, mDefaultTools.Helper, QStringLiteral("<sdk>/lib/cmake/Ludus/shaders"));
    describe(mCompiler, mDefaultTools.Compiler, QStringLiteral("pinned slangc"));
    describe(mValidator, mDefaultTools.Validator, QStringLiteral("pinned spirv-val (SPIR-V/GLSL profiles)"));
    describe(mSpirvCross, mDefaultTools.SpirvCross, QStringLiteral("verified spirv-cross (GLSL profile)"));
    mToolSummary->setText(mDefaultTools.Helper.isEmpty() == false && mDefaultTools.Compiler.isEmpty() == false
                              ? QStringLiteral("Builds use the shader tools this editor was built with. Fill a field "
                                               "below only to use a different tool.")
                              : QStringLiteral("This editor build has no shader tools of its own (it was built "
                                               "without the native preview). Set the SDK shader helper and Slang "
                                               "compiler below to build drafts."));
}

void RendererWorkspace::AcceptResult(const RenderGraphJobResult& result)
{
    if (result.Request.SnapshotDirectory.isEmpty() == false)
    {
        QDir(result.Request.SnapshotDirectory).removeRecursively();
    }
    mCancel->setEnabled(false);
    mLiveBuild.SetBusy(false);
    // A result for another project, request or closed document never publishes.
    if (result.Request.Request != mActiveRequest || result.Request.ProjectEpoch != mEpoch ||
        mDocument.IsLoaded() == false)
    {
        Q_EMIT JobFinished();
        return;
    }
    mLastResult = result;
    mDiagnostics->clear();
    if (result.IsSuccessful == true)
    {
        const QJsonObject plan = result.Payload.value(QStringLiteral("plan")).toObject();
        if (result.Request.Action == QStringLiteral("dump"))
        {
            mIsValidated = true;
            mValidatedRevision = result.Request.Revision;
            mPlan->setPlainText(QString::fromUtf8(QJsonDocument(plan).toJson(QJsonDocument::Indented)));
        }
        else
        {
            mPlan->setPlainText(QString::fromUtf8(QJsonDocument(result.Payload).toJson(QJsonDocument::Indented)));
        }
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("%1 succeeded for draft revision %2.")
                                                        .arg(result.Request.Action)
                                                        .arg(result.Request.Revision),
                                                    mDiagnostics);
        item->setData(LOCATION_ROLE, QJsonObject());
    }
    else
    {
        if (result.Request.Action == QStringLiteral("dump"))
        {
            mIsValidated = false;
        }
        QListWidgetItem* item = new QListWidgetItem(
            QStringLiteral("%1: %2 [%3]").arg(result.Code, result.Message, DescribeLocation(result.Location)),
            mDiagnostics);
        item->setData(LOCATION_ROLE, result.Location);
        const QJsonArray diagnostics = result.Diagnostics;
        for (const QJsonValueConstRef value : diagnostics)
        {
            const QJsonObject diagnostic = value.toObject();
            const QJsonObject location = diagnostic.value(QStringLiteral("location")).toObject();
            QListWidgetItem* entry =
                new QListWidgetItem(QStringLiteral("%1 %2 (%3): %4 [%5]")
                                        .arg(diagnostic.value(QStringLiteral("severity")).toString(),
                                             diagnostic.value(QStringLiteral("program")).toString(),
                                             diagnostic.value(QStringLiteral("target")).toString(),
                                             diagnostic.value(QStringLiteral("message")).toString(),
                                             DescribeLocation(location)),
                                    mDiagnostics);
            entry->setData(LOCATION_ROLE, location);
        }
    }
    if (result.IsSuccessful == true && result.Request.ShouldPreview == true &&
        result.Request.InputIdentity == getBuildIdentity())
    {
        static_cast<void>(PreviewRuntime());
    }
    renderStatus();
    Q_EMIT JobFinished();
}

QByteArray RendererWorkspace::getBuildIdentity() const
{
    return ComputeRenderGraphBuildIdentity(mDocument, mConfiguration->currentText());
}

void RendererWorkspace::scheduleLiveBuild()
{
    if (mIsRendering == false && mDocument.IsLoaded() == true)
    {
        mLiveBuild.Schedule(getBuildIdentity());
    }
}

void RendererWorkspace::documentEdited(RenderGraphDocumentStatus status)
{
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(DescribeStatus(status));
        return;
    }
    renderTree();
    renderSource();
    renderStatus();
    Q_EMIT DocumentChanged();
    scheduleLiveBuild();
}

QJsonObject RendererWorkspace::selectedConfiguration() const
{
    QJsonObject graph;
    if (mDocument.GetGraph(mSelectedFile, graph) == false)
    {
        return {};
    }
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef value : configurations)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == mSelectedConfiguration)
        {
            return value.toObject();
        }
    }
    return {};
}

QJsonObject RendererWorkspace::selectedNode() const
{
    const QJsonArray nodes = selectedConfiguration().value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef value : nodes)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == mSelectedNode)
        {
            return value.toObject();
        }
    }
    return {};
}

void RendererWorkspace::renderTree()
{
    mIsRendering = true;
    mTree->clear();
    QTreeWidgetItem* currentOrNull = nullptr;
    const QString selectedConfiguration = mConfiguration->currentText();
    mConfiguration->clear();
    mConfiguration->addItem(DEFAULT_CONFIGURATION);
    for (const QString& file : mDocument.GetFiles())
    {
        QJsonObject graph;
        if (mDocument.GetGraph(file, graph) == false)
        {
            continue;
        }
        QTreeWidgetItem* fileItem =
            new QTreeWidgetItem(mTree,
                                {file + (mDocument.IsFileDirty(file) == true ? QStringLiteral(" *") : QString())});
        fileItem->setData(0, FILE_ROLE, file);
        const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
        for (const QJsonValueConstRef configurationValue : configurations)
        {
            const QJsonObject configuration = configurationValue.toObject();
            const QString configurationId = configuration.value(QStringLiteral("id")).toString();
            if (file == mDocument.GetRootGraph())
            {
                mConfiguration->addItem(configurationId);
            }
            QTreeWidgetItem* configurationItem = new QTreeWidgetItem(fileItem, {QLatin1Char('#') + configurationId});
            configurationItem->setData(0, FILE_ROLE, file);
            configurationItem->setData(0, CONFIGURATION_ROLE, configurationId);
            if (file == mSelectedFile && configurationId == mSelectedConfiguration && mSelectedNode.isEmpty() == true)
            {
                currentOrNull = configurationItem;
            }
            const QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
            for (const QJsonValueConstRef nodeValue : nodes)
            {
                const QJsonObject node = nodeValue.toObject();
                const QString nodeId = node.value(QStringLiteral("id")).toString();
                QTreeWidgetItem* nodeItem = new QTreeWidgetItem(
                    configurationItem,
                    {nodeId + QStringLiteral(" : ") + node.value(QStringLiteral("kind")).toString()});
                nodeItem->setData(0, FILE_ROLE, file);
                nodeItem->setData(0, CONFIGURATION_ROLE, configurationId);
                nodeItem->setData(0, NODE_ROLE, nodeId);
                if (file == mSelectedFile && configurationId == mSelectedConfiguration && nodeId == mSelectedNode)
                {
                    currentOrNull = nodeItem;
                }
            }
        }
    }
    mTree->expandAll();
    const int32 configurationIndex = mConfiguration->findText(selectedConfiguration);
    mConfiguration->setCurrentIndex(configurationIndex >= 0 ? configurationIndex : 0);
    if (currentOrNull != nullptr)
    {
        mTree->setCurrentItem(currentOrNull);
    }
    else
    {
        mSelectedNode.clear();
    }
    mIsRendering = false;
    const QString selectedSource = mSourceFile->currentText();
    mSourceFile->blockSignals(true);
    mSourceFile->clear();
    mSourceFile->addItems(mDocument.GetFiles());
    const int32 sourceIndex = mSourceFile->findText(selectedSource);
    mSourceFile->setCurrentIndex(sourceIndex >= 0 ? sourceIndex : mSourceFile->findText(mDocument.GetRootGraph()));
    mSourceFile->blockSignals(false);
    renderInspector();
    renderEdges();
    renderCanvas();
}

void RendererWorkspace::renderInspector()
{
    while (mFields->rowCount() > 0)
    {
        mFields->removeRow(0);
    }
    const QJsonObject node = selectedNode();
    mLayoutX->setEnabled(node.isEmpty() == false);
    mLayoutY->setEnabled(node.isEmpty() == false);
    if (node.isEmpty() == true)
    {
        mFields->addRow(new QLabel(QStringLiteral("Select a node to inspect its authored fields."), mInspector));
        return;
    }
    mFields->addRow(QStringLiteral("Node"),
                    new QLabel(mSelectedNode + QStringLiteral(" (") + node.value(QStringLiteral("kind")).toString() +
                                   QLatin1Char(')'),
                               mInspector));
    for (QJsonObject::const_iterator field = node.constBegin(); field != node.constEnd(); ++field)
    {
        const QString name = field.key();
        if (name == QStringLiteral("id") || name == QStringLiteral("kind"))
        {
            continue;
        }
        const QStringList choices = GetChoices(name);
        if (choices.isEmpty() == false)
        {
            QComboBox* combo = new QComboBox(mInspector);
            combo->setObjectName(QStringLiteral("renderGraphField_") + name);
            combo->addItems(choices);
            combo->setCurrentText(field.value().toString());
            connect(combo, &QComboBox::activated, this, [this, name, combo]() {
                replaceSelectedField(name, combo->currentText());
            });
            mFields->addRow(name, combo);
        }
        else if (field.value().isBool() == true)
        {
            QCheckBox* check = new QCheckBox(mInspector);
            check->setObjectName(QStringLiteral("renderGraphField_") + name);
            check->setChecked(field.value().toBool());
            connect(check, &QCheckBox::toggled, this, [this, name](bool isChecked) {
                replaceSelectedField(name, isChecked);
            });
            mFields->addRow(name, check);
        }
        else if (name == QStringLiteral("extent"))
        {
            QWidget* row = new QWidget(mInspector);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            QSpinBox* width = new QSpinBox(row);
            QSpinBox* height = new QSpinBox(row);
            for (QSpinBox* spin : {width, height})
            {
                spin->setRange(1, MAX_EXTENT);
                rowLayout->addWidget(spin);
            }
            width->setValue(field.value().toArray().at(0).toInt());
            height->setValue(field.value().toArray().at(1).toInt());
            const auto commitExtent = [this, width, height]() {
                replaceSelectedField(QStringLiteral("extent"), QJsonArray{width->value(), height->value()});
            };
            connect(width, &QSpinBox::editingFinished, this, commitExtent);
            connect(height, &QSpinBox::editingFinished, this, commitExtent);
            mFields->addRow(name, row);
        }
        else if (field.value().isDouble() == true)
        {
            QSpinBox* spin = new QSpinBox(mInspector);
            spin->setRange(1, MAX_EXTENT);
            spin->setValue(field.value().toInt());
            connect(spin, &QSpinBox::editingFinished, this, [this, name, spin]() {
                replaceSelectedField(name, spin->value());
            });
            mFields->addRow(name, spin);
        }
        else
        {
            QLineEdit* line = new QLineEdit(field.value().toString(), mInspector);
            line->setObjectName(QStringLiteral("renderGraphField_") + name);
            connect(line, &QLineEdit::editingFinished, this, [this, name, line]() {
                replaceSelectedField(name, line->text());
            });
            mFields->addRow(name, line);
        }
    }
    int32 x = 0;
    int32 y = 0;
    const QJsonObject layout = selectedConfiguration().value(QStringLiteral("layout")).toObject();
    const QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
    for (const QJsonValueConstRef position : positions)
    {
        if (position.toObject().value(QStringLiteral("node")).toString() == mSelectedNode)
        {
            x = position.toObject().value(QStringLiteral("x")).toInt();
            y = position.toObject().value(QStringLiteral("y")).toInt();
        }
    }
    mLayoutX->blockSignals(true);
    mLayoutY->blockSignals(true);
    mLayoutX->setValue(x);
    mLayoutY->setValue(y);
    mLayoutX->blockSignals(false);
    mLayoutY->blockSignals(false);
}

void RendererWorkspace::renderEdges()
{
    const QJsonArray edges = selectedConfiguration().value(QStringLiteral("edges")).toArray();
    mEdges->setRowCount(static_cast<int32>(edges.size()));
    for (qsizetype i = 0; i < edges.size(); ++i)
    {
        const QJsonObject edge = edges.at(i).toObject();
        mEdges->setItem(static_cast<int32>(i), 0, new QTableWidgetItem(edge.value(QStringLiteral("from")).toString()));
        mEdges->setItem(static_cast<int32>(i), 1, new QTableWidgetItem(edge.value(QStringLiteral("to")).toString()));
    }
}

void RendererWorkspace::renderSource()
{
    const QString file = mSourceFile->currentText();
    const QString text = QString::fromUtf8(mDocument.GetDraft(file));
    if (mSource->toPlainText() != text)
    {
        mSource->setPlainText(text);
    }
    mSource->setEnabled(file.isEmpty() == false);
}

void RendererWorkspace::renderStatus()
{
    const QString destination =
        mDocument.IsLoaded() == true ? QDir(mProjectRoot).relativeFilePath(mDocument.GetPackageRoot()) : QString();
    if (mDocument.IsLoaded() == false)
    {
        mPath->setText(QStringLiteral("No render graph is open."));
        mPath->setToolTip({});
        mStatus->setText(QStringLiteral("Open a root .lurender.json to edit its graph, schema and shader sources."));
    }
    else if (isUneditedBuiltIn() == true)
    {
        mPath->setText(QStringLiteral("Engine default renderer"));
        mPath->setToolTip(QStringLiteral("Read-only and versioned with the engine. Editing it starts a project copy "
                                         "that Save creates in %1/.")
                              .arg(destination));
        mStatus->setText(QStringLiteral("Engine default renderer (read-only). This project has no render graph of "
                                        "its own; edits start one in %1/.")
                             .arg(destination));
    }
    else if (mDocument.IsBuiltIn() == true)
    {
        // Customized but unsaved: the draft belongs to the project, the files do not exist yet.
        mPath->setText(QStringLiteral("%1/%2 (new, unsaved)").arg(destination, mDocument.GetRootGraph()));
        mPath->setToolTip(QStringLiteral("Customized from the engine default. Save creates the package in %1/; "
                                         "undoing every edit returns to the default.")
                              .arg(destination));
        mStatus->setText(QStringLiteral("Customizing the engine default - draft revision %1. Save creates %2/ in the "
                                        "project; Undo returns to the default.")
                             .arg(mDocument.GetRevision())
                             .arg(destination));
    }
    else
    {
        mPath->setText(
            QDir(mProjectRoot).relativeFilePath(QDir(mDocument.GetPackageRoot()).filePath(mDocument.GetRootGraph())));
        mPath->setToolTip({});
        QString text = QStringLiteral("%1 - draft revision %2, %3.")
                           .arg(mDocument.GetRootGraph())
                           .arg(mDocument.GetRevision())
                           .arg(mDocument.IsDirty() == true ? QStringLiteral("unsaved changes")
                                                            : QStringLiteral("matches saved sources"));
        if (mIsValidated == true)
        {
            text += mValidatedRevision == mDocument.GetRevision()
                        ? QStringLiteral(" Validated.")
                        : QStringLiteral(" Last validation describes revision %1.").arg(mValidatedRevision);
        }
        mStatus->setText(text);
    }
    const bool isOpen = mDocument.IsLoaded();
    mValidate->setEnabled(isOpen);
    mBuild->setEnabled(isOpen);
    mCook->setEnabled(isOpen);
    mPreviewButton->setEnabled(isOpen == true && mPreviewOrNull != nullptr);
    mCustomize->setVisible(mDocument.IsBuiltIn() == true);
    mCustomize->setEnabled(mDocument.IsBuiltIn() == true && mProjectRoot.isEmpty() == false);
}

bool RendererWorkspace::isUneditedBuiltIn() const
{
    return mDocument.IsBuiltIn() == true && mDocument.IsDirty() == false;
}

void RendererWorkspace::showPreviewPage()
{
    mTabs->setCurrentWidget(mPreviewPage);
}

void RendererWorkspace::replaceSelectedField(const QString& field, const QJsonValue& value)
{
    QJsonObject node = selectedNode();
    if (node.isEmpty() == true || node.value(field) == value)
    {
        return;
    }
    node.insert(field, value);
    // Authored mode and texture-type choices carry their dependent fields explicitly.
    if (field == QStringLiteral("mode"))
    {
        if (value.toString() == QStringLiteral("DepthOnly"))
        {
            node.remove(QStringLiteral("color_load"));
            node.remove(QStringLiteral("color_store"));
        }
        else
        {
            node.insert(QStringLiteral("color_load"), QStringLiteral("Clear"));
            node.insert(QStringLiteral("color_store"), QStringLiteral("Store"));
        }
    }
    if (field == QStringLiteral("type") && node.contains(QStringLiteral("format")) == true)
    {
        node.insert(QStringLiteral("format"),
                    value.toString() == QStringLiteral("DepthTexture") ? QStringLiteral("Depth32Float")
                                                                       : QStringLiteral("RGBA8Unorm"));
    }
    documentEdited(mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, node));
}

void RendererWorkspace::addNode()
{
    const QString identity = mNewIdentity->text().trimmed();
    if (mSelectedConfiguration.isEmpty() == true || identity.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("Select a configuration and enter a new node ID."));
        return;
    }
    const RenderGraphDocumentStatus status =
        mDocument.AddNode(mSelectedFile, mSelectedConfiguration, MakeNode(mNewKind->currentText(), identity));
    if (status == RenderGraphDocumentStatus::Ok)
    {
        mSelectedNode = identity;
        mNewIdentity->clear();
    }
    documentEdited(status);
}

bool RendererWorkspace::SelectNode(const QString& file, const QString& configuration, const QString& node)
{
    mSelectedFile = file;
    mSelectedConfiguration = configuration;
    mSelectedNode = node;
    renderTree();
    return mSelectedNode == node && selectedConfiguration().isEmpty() == false;
}

QString RendererWorkspace::GetSelection() const
{
    if (mSelectedConfiguration.isEmpty() == true)
    {
        return {};
    }
    return mSelectedFile + QLatin1Char('#') + mSelectedConfiguration +
           (mSelectedNode.isEmpty() == true ? QString() : QLatin1Char('/') + mSelectedNode);
}

int32 RendererWorkspace::GetDiagnosticCount() const
{
    return mDiagnostics->count();
}

void RendererWorkspace::openSourceLine(const QString& file, int32 line)
{
    mTabs->setCurrentIndex(1);
    mSourceFile->setCurrentText(file);
    renderSource();
    const QTextBlock block = mSource->document()->findBlockByNumber(line > 0 ? line - 1 : 0);
    QTextCursor cursor(block);
    mSource->setTextCursor(cursor);
    mSource->setFocus();
}

bool RendererWorkspace::ActivateDiagnostic(int32 row)
{
    QListWidgetItem* item = mDiagnostics->item(row);
    if (item == nullptr)
    {
        return false;
    }
    const QJsonObject location = item->data(LOCATION_ROLE).toJsonObject();
    const QString source = location.value(QStringLiteral("source")).toString();
    if (location.contains(QStringLiteral("generated")) == true)
    {
        // Generated wrappers are read-only products; their owner is the program's manifest record.
        const QString manifest = manifestFile();
        const QStringList lines = QString::fromUtf8(mDocument.GetDraft(manifest)).split(QLatin1Char('\n'));
        const QString needle =
            QStringLiteral("\"id\": \"%1\"").arg(location.value(QStringLiteral("program")).toString());
        for (qsizetype i = 0; i < lines.size(); ++i)
        {
            if (lines.at(i).contains(needle) == true)
            {
                openSourceLine(manifest, static_cast<int32>(i + 1));
                return true;
            }
        }
        openSourceLine(manifest, 1);
        return true;
    }
    if (source.isEmpty() == true || mDocument.GetFiles().contains(source) == false)
    {
        return false;
    }
    if (location.contains(QStringLiteral("line")) == true)
    {
        openSourceLine(source, location.value(QStringLiteral("line")).toInt());
        return true;
    }
    mTabs->setCurrentIndex(0);
    return SelectNode(source,
                      location.value(QStringLiteral("configuration")).toString(),
                      location.value(QStringLiteral("node")).toString());
}

QString RendererWorkspace::GetSourceFile() const
{
    return mSourceFile->currentText();
}

int32 RendererWorkspace::GetSourceLine() const
{
    return mSource->textCursor().blockNumber() + 1;
}

QString RendererWorkspace::manifestFile() const
{
    QString file = mDocument.GetRootGraph();
    file.chop(static_cast<qsizetype>(QStringLiteral(".lurender.json").size()));
    return file + QStringLiteral(".lushader.json");
}

QString RendererWorkspace::buildDirectory() const
{
    QString name = mDocument.GetRootGraph();
    name.chop(static_cast<qsizetype>(QStringLiteral(".lurender.json").size()));
    const QString package = QDir(mProjectRoot).relativeFilePath(mDocument.GetPackageRoot());
    const QString owner = package == QStringLiteral(".") ? name : package + QLatin1Char('/') + name;
    return QDir(mProjectRoot).filePath(QStringLiteral("out/render-graph/") + owner + QStringLiteral("/build"));
}

QString RendererWorkspace::GetRuntimePackagePath() const
{
    QString name = mDocument.GetRootGraph();
    name.chop(static_cast<qsizetype>(QStringLiteral(".lurender.json").size()));
    return QDir::cleanPath(QDir(buildDirectory()).filePath(QStringLiteral("../") + name + QStringLiteral(".lurgrt")));
}

QString RendererWorkspace::GetPreviewPackagePath() const
{
    const QString prebuilt = GetDefaultRenderGraphRuntime();
    return isUneditedBuiltIn() == true && prebuilt.isEmpty() == false ? prebuilt : GetRuntimePackagePath();
}

QString RendererWorkspace::GetStatusText() const
{
    return mStatus->text();
}

void RendererWorkspace::renderCanvas()
{
    QString configuration = mSelectedConfiguration;
    QJsonObject graph;
    if (configuration.isEmpty() == true && mDocument.GetGraph(mSelectedFile, graph) == true)
    {
        // A selected file shows its authored default configuration.
        configuration = graph.value(QStringLiteral("default")).toString();
    }
    mCanvas->ShowConfiguration(mDocument.IsLoaded() == true ? mSelectedFile : QString(), configuration);
    mCanvas->SelectNode(mSelectedNode);
}

void RendererWorkspace::openSubgraph(const QString& asset, const QString& configuration)
{
    if (mDocument.GetFiles().contains(asset) == false)
    {
        mStatus->setText(QStringLiteral("Subgraph source %1 is not part of this package.").arg(asset));
        return;
    }
    // Navigate canvas, tree and Source tab to the referenced asset's configuration.
    mSelectedFile = asset;
    mSelectedConfiguration = configuration;
    mSelectedNode.clear();
    mSourceFile->setCurrentText(asset);
    renderTree();
    renderSource();
    mStatus->setText(QStringLiteral("Showing subgraph %1#%2.").arg(asset, configuration));
}

void RendererWorkspace::SetPreviewSource(RenderPreview* sourceOrNull)
{
    if (mPreviewSourceOrNull != nullptr)
    {
        disconnect(mPreviewSourceOrNull, nullptr, this, nullptr);
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr && sourceOrNull != mPreviewSourceOrNull)
    {
        mPreviewOrNull->Detach();
    }
#endif
    mPreviewSourceOrNull = sourceOrNull;
    mPreviewDeviceSerial = 0;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewSourceOrNull != nullptr && mPreviewOrNull != nullptr)
    {
        // Release generations before RenderPreview destroys the shared device.
        connect(mPreviewSourceOrNull, &RenderPreview::DeviceStopping, this, [this]() {
            mPreviewOrNull->Detach();
            mPreviewDeviceSerial = 0;
        });
    }
#endif
    if (mPreviewSourceOrNull != nullptr)
    {
        connect(mPreviewSourceOrNull, &QObject::destroyed, this, [this]() {
            mPreviewSourceOrNull = nullptr;
            mPreviewDeviceSerial = 0;
        });
    }
    renderPreview();
}

bool RendererWorkspace::PreviewRuntime()
{
    if (mPreviewOrNull == nullptr)
    {
        mStatus->setText(QStringLiteral("The render-graph preview needs the native editor built with "
                                        "LUDUS_EDITOR_RENDER_PREVIEW."));
        renderPreview();
        return false;
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mDocument.IsLoaded() == false)
    {
        return false;
    }
    const RenderGraphPreviewLoad loaded = mPreviewOrNull->Load(GetPreviewPackagePath());
    mPreviewTimer.start();
    ServicePreview();
    return loaded == RenderGraphPreviewLoad::Staged || loaded == RenderGraphPreviewLoad::Waiting;
#else
    return false;
#endif
}

void RendererWorkspace::ServicePreview()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull == nullptr)
    {
        return;
    }
    RenderGraphPreview& preview = *mPreviewOrNull;
    if (mPreviewSourceOrNull != nullptr && mPreviewSourceOrNull->HasDevice() == true &&
        (preview.IsAttached() == false || mPreviewSourceOrNull->GetDeviceSerial() != mPreviewDeviceSerial))
    {
        preview.Detach();
        if (preview.Attach(mPreviewSourceOrNull->GetDevice(), mPreviewSourceOrNull->GetSurface()) == true)
        {
            mPreviewDeviceSerial = mPreviewSourceOrNull->GetDeviceSerial();
            mPreviewSceneIdentity.clear();
        }
    }
    const QSize target = preview.GetTargetSize();
    if (mPreviewSourceOrNull != nullptr && preview.IsAttached() == true && target.isEmpty() == false)
    {
        // The World viewport's current camera or orbit, at the graph's exported extent.
        graphics::renderer::ViewDescription view;
        QString error;
        const RenderScene& scene = mPreviewSourceOrNull->Scene();
        if (mPreviewSourceOrNull->DescribeView(static_cast<uint32>(target.width()),
                                               static_cast<uint32>(target.height()),
                                               view,
                                               error) == true &&
            (scene.Identity != mPreviewSceneIdentity ||
             AreMatricesEqual(view.WorldToClip, mPreviewWorldToClip) == false))
        {
            RenderGraphExtraction extraction;
            if (MakeRenderGraphExtraction(scene, view, ++mPreviewRevision, extraction, error) == true)
            {
                preview.SetExtraction(extraction);
            }
            else
            {
                mPreviewStatus->setText(error);
            }
            mPreviewSceneIdentity = scene.Identity;
            mPreviewWorldToClip = view.WorldToClip;
        }
    }
    preview.Tick();
#endif
}

void RendererWorkspace::renderPreview()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr)
    {
        static_cast<PreviewImage*>(mPreviewImage)->SetImage(mPreviewOrNull->GetImage());
        mPreviewStatus->setText(mPreviewOrNull->GetStatus());
        mPreviewButton->setEnabled(mDocument.IsLoaded());
        return;
    }
#endif
    mPreviewButton->setEnabled(false);
    mPreviewStatus->setText(QStringLiteral("The render-graph preview needs the native editor built with "
                                           "LUDUS_EDITOR_RENDER_PREVIEW and pinned shader tools."));
}
} // namespace ludus::editor
