#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

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
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
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
constexpr int32 MAX_EXTENT = 16384;
constexpr int32 MAX_COORDINATE = 32768;
constexpr int32 MINIMUM_CANVAS_WIDTH = 360;
// Initial share of the document's width: source tree, then the Canvas/Source/Plan views.
constexpr int32 SOURCE_TREE_SHARE = 1;
constexpr int32 VIEWS_SHARE = 4;
constexpr int32 SOURCE_TREE_COLUMN = 0;
constexpr int32 VIEWS_COLUMN = 1;
/// Tabs of the document's view switcher, in order.
enum DocumentView : int32
{
    CANVAS_VIEW,
    SOURCE_VIEW,
    PLAN_VIEW,
};
const QString DEFAULT_CONFIGURATION = QStringLiteral("(asset default)");
const QString WORKER_SOCKET_KEY = QStringLiteral("renderGraph/workerSocket");
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

QString DescribePackageOutsideProject()
{
    return QStringLiteral("The render graph's package lies outside the project folder, so it has no build tree there.");
}
} // namespace

RendererWorkspace::RendererWorkspace(const ToolingPaths& tooling, QWidget* parentOrNull)
    : QWidget(parentOrNull), mTooling(tooling), mPath(new QLabel(QStringLiteral("No render graph is open."), this)),
      mContent(new QStackedWidget(this)), mEmptyMessage(new QLabel(this)),
      mCreateFirst(new QPushButton(QStringLiteral("Create Render Graph..."), this)),
      mConfiguration(new QComboBox(this)), mTree(new QTreeWidget(this)), mViews(new QTabWidget(this)),
      mInspectorPanel(new QScrollArea(this)), mInspector(new QWidget(mInspectorPanel)), mFields(new QFormLayout),
      mLayoutX(new QSpinBox(mInspector)), mLayoutY(new QSpinBox(mInspector)), mNewKind(new QComboBox(mInspector)),
      mNewIdentity(new QLineEdit(mInspector)), mEdges(new QTableWidget(mInspector)),
      mEdgeFrom(new QLineEdit(mInspector)), mEdgeTo(new QLineEdit(mInspector)), mSourceFile(new QComboBox(this)),
      mSource(new QPlainTextEdit(this)), mPlan(new QPlainTextEdit(this)),
      mCustomize(new QAction(QStringLiteral("&Customize in Project"), this)), mStatus(new QLabel(this)),
      mCanvas(new RenderGraphCanvas(mDocument, this)), mPreviewPage(new QWidget(this)),
      mPreviewImage(new PreviewImage(mPreviewPage)), mPreviewStatus(new QLabel(mPreviewPage)),
      mPreviewButton(new QPushButton(QStringLiteral("&Preview Runtime"), mPreviewPage))
{
    setObjectName(QStringLiteral("rendererWorkspace"));
    // The rarely needed explicit copy of the engine default sits in the source tree's context
    // menu; editing the default does the same on Save. Neither is ever a prompt.
    mCustomize->setObjectName(QStringLiteral("renderGraphCustomize"));
    mCustomize->setToolTip(QStringLiteral("Copy the engine default renderer into graphs/Main/ in this project and "
                                          "edit it there. Editing the default does the same on Save."));
    // Like an editor's navigation bar: what is open and which configuration builds. The
    // commands that act on it are the shell's renderGraph.* menu and toolbar commands.
    mPath->setObjectName(QStringLiteral("renderGraphPath"));
    mPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mConfiguration->setObjectName(QStringLiteral("renderGraphConfiguration"));
    mConfiguration->setToolTip(QStringLiteral("Root configuration to validate and cook; choices are authored."));
    mConfiguration->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    QHBoxLayout* header = new QHBoxLayout;
    header->addWidget(mPath, 1);
    header->addWidget(new QLabel(QStringLiteral("Configuration"), this));
    header->addWidget(mConfiguration);

    mTree->setObjectName(QStringLiteral("renderGraphTree"));
    mTree->setHeaderLabel(QStringLiteral("Graph sources"));
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);

    // Inspector: fields of the selected node, its layout position, node creation and the
    // configuration's connections. The shell hosts it in the narrow Properties tool window,
    // so every row stays readable at a tool window's default width.
    mInspectorPanel->setObjectName(QStringLiteral("renderGraphProperties"));
    mInspectorPanel->setWidgetResizable(true);
    mInspectorPanel->setFrameShape(QFrame::NoFrame);
    mInspectorPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    QVBoxLayout* inspectorLayout = new QVBoxLayout(mInspector);
    inspectorLayout->addLayout(mFields);
    mLayoutX->setObjectName(QStringLiteral("renderGraphLayoutX"));
    mLayoutY->setObjectName(QStringLiteral("renderGraphLayoutY"));
    mLayoutX->setRange(-MAX_COORDINATE, MAX_COORDINATE);
    mLayoutY->setRange(-MAX_COORDINATE, MAX_COORDINATE);
    QHBoxLayout* layoutRow = new QHBoxLayout;
    layoutRow->addWidget(mLayoutX);
    layoutRow->addWidget(mLayoutY);
    QFormLayout* positionForm = new QFormLayout;
    QLabel* positionLabel = new QLabel(QStringLiteral("Layout x/y"), mInspector);
    positionLabel->setToolTip(QStringLiteral("Canvas position; presentation only, never part of the build."));
    positionForm->addRow(positionLabel, layoutRow);
    inspectorLayout->addLayout(positionForm);
    QPushButton* remove = new QPushButton(QStringLiteral("Remove Node"), mInspector);
    remove->setObjectName(QStringLiteral("renderGraphRemoveNode"));
    inspectorLayout->addWidget(remove, 0, Qt::AlignLeft);
    mNewKind->setObjectName(QStringLiteral("renderGraphNewKind"));
    mNewKind->addItems({QStringLiteral("Scene"),
                        QStringLiteral("Camera"),
                        QStringLiteral("Target"),
                        QStringLiteral("Raster"),
                        QStringLiteral("Subgraph"),
                        QStringLiteral("Export"),
                        QStringLiteral("Present")});
    mNewIdentity->setObjectName(QStringLiteral("renderGraphNewIdentity"));
    mNewIdentity->setPlaceholderText(QStringLiteral("new node ID"));
    QPushButton* add = new QPushButton(QStringLiteral("Add Node"), mInspector);
    add->setObjectName(QStringLiteral("renderGraphAddNode"));
    QHBoxLayout* addRow = new QHBoxLayout;
    addRow->addWidget(mNewKind);
    addRow->addWidget(mNewIdentity, 1);
    addRow->addWidget(add);
    inspectorLayout->addLayout(addRow);
    mEdges->setObjectName(QStringLiteral("renderGraphEdges"));
    mEdges->setColumnCount(2);
    mEdges->setHorizontalHeaderLabels({QStringLiteral("From"), QStringLiteral("To")});
    mEdges->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    mEdges->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mEdges->setSelectionBehavior(QAbstractItemView::SelectRows);
    inspectorLayout->addWidget(new QLabel(QStringLiteral("Connections in this configuration"), mInspector));
    inspectorLayout->addWidget(mEdges, 1);
    mEdgeFrom->setPlaceholderText(QStringLiteral("node.port"));
    mEdgeTo->setPlaceholderText(QStringLiteral("node.port"));
    QHBoxLayout* edgeRow = new QHBoxLayout;
    edgeRow->addWidget(mEdgeFrom);
    edgeRow->addWidget(mEdgeTo);
    inspectorLayout->addLayout(edgeRow);
    QPushButton* connectButton = new QPushButton(QStringLiteral("Connect"), mInspector);
    QPushButton* disconnectButton = new QPushButton(QStringLiteral("Disconnect Selected"), mInspector);
    QHBoxLayout* edgeButtons = new QHBoxLayout;
    edgeButtons->addWidget(connectButton);
    edgeButtons->addWidget(disconnectButton);
    edgeButtons->addStretch();
    inspectorLayout->addLayout(edgeButtons);
    mInspectorPanel->setWidget(mInspector);
    // Hidden until the shell takes it; a standalone workspace has no Properties window.
    mInspectorPanel->hide();

    // Source: graph, build-manifest/schema and shader text share the same draft/undo/save.
    QWidget* sourcePage = new QWidget(this);
    QVBoxLayout* sourceLayout = new QVBoxLayout(sourcePage);
    mSourceFile->setObjectName(QStringLiteral("renderGraphSourceFile"));
    mSource->setObjectName(QStringLiteral("renderGraphSource"));
    QPushButton* applyText = new QPushButton(QStringLiteral("Apply Text to Draft"), sourcePage);
    QPushButton* revertText = new QPushButton(QStringLiteral("Revert Text"), sourcePage);
    QHBoxLayout* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(mSourceFile, 1);
    sourceRow->addWidget(applyText);
    sourceRow->addWidget(revertText);
    sourceLayout->addLayout(sourceRow);
    sourceLayout->addWidget(mSource, 1);

    mPlan->setObjectName(QStringLiteral("renderGraphPlan"));
    mPlan->setReadOnly(true);
    mPlan->setPlaceholderText(QStringLiteral("Validate the draft to inspect its deterministic serial plan."));

    QSettings settings;
    mBuildSettings.WorkerSocket =
        settings.value(WORKER_SOCKET_KEY, qEnvironmentVariable("LUDUS_SHADER_WORKER_SOCKET")).toString();

    // Preview: the cooked runtime generation on the World viewport device (RG3b). The shell
    // hosts it in the Render Preview tool window.
    mPreviewPage->setObjectName(QStringLiteral("renderGraphPreviewPanel"));
    QVBoxLayout* previewLayout = new QVBoxLayout(mPreviewPage);
    mPreviewButton->setObjectName(QStringLiteral("renderGraphPreview"));
    mPreviewButton->setToolTip(QStringLiteral("Stage the last cooked .lurgrt beside the drawing generation; it is "
                                              "shown only after it activates and renders the World extraction."));
    mPreviewStatus->setObjectName(QStringLiteral("renderGraphPreviewStatus"));
    mPreviewStatus->setWordWrap(true);
    previewLayout->addWidget(mPreviewButton, 0, Qt::AlignLeft);
    previewLayout->addWidget(mPreviewStatus);
    previewLayout->addWidget(mPreviewImage, 1);
    mPreviewPage->hide();

    // Canvas, Source and Plan are views of the one document, switched below the view like a
    // designer's Design and Source tabs.
    mViews->setObjectName(QStringLiteral("renderGraphViews"));
    mViews->setTabPosition(QTabWidget::South);
    mViews->setDocumentMode(true);
    mViews->insertTab(CANVAS_VIEW, mCanvas, QStringLiteral("Canvas"));
    mViews->insertTab(SOURCE_VIEW, sourcePage, QStringLiteral("Source"));
    mViews->insertTab(PLAN_VIEW, mPlan, QStringLiteral("Plan"));

    mStatus->setObjectName(QStringLiteral("renderGraphStatus"));
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QSplitter* horizontal = new QSplitter(Qt::Horizontal, this);
    horizontal->setObjectName(QStringLiteral("renderGraphSplitter"));
    horizontal->addWidget(mTree);
    horizontal->addWidget(mViews);
    mCanvas->setMinimumWidth(MINIMUM_CANVAS_WIDTH);
    horizontal->setCollapsible(VIEWS_COLUMN, false);
    // The views get most of the width, now and as the document grows; the tree keeps a
    // readable share for nested node names.
    horizontal->setStretchFactor(SOURCE_TREE_COLUMN, SOURCE_TREE_SHARE);
    horizontal->setStretchFactor(VIEWS_COLUMN, VIEWS_SHARE);
    horizontal->setSizes({SOURCE_TREE_SHARE, VIEWS_SHARE});
    QWidget* emptyPage = new QWidget(this);
    QVBoxLayout* emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->addStretch();
    mEmptyMessage->setObjectName(QStringLiteral("renderGraphEmptyMessage"));
    mEmptyMessage->setAlignment(Qt::AlignCenter);
    mEmptyMessage->setWordWrap(true);
    emptyLayout->addWidget(mEmptyMessage);
    mCreateFirst->setObjectName(QStringLiteral("renderGraphCreateFirst"));
    mCreateFirst->setProperty("visualRole", QStringLiteral("primary"));
    emptyLayout->addWidget(mCreateFirst, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();
    mContent->addWidget(emptyPage);
    mContent->addWidget(horizontal);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(mContent, 1);
    layout->addWidget(mStatus);

    connect(mCreateFirst, &QPushButton::clicked, this, [this]() { static_cast<void>(PromptNewGraph()); });
    connect(mConfiguration, &QComboBox::currentTextChanged, this, [this]() { scheduleLiveBuild(); });
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
    connect(&mCreation, &RenderGraphCreation::Finished, this, [this]() {
        updateActions();
        Q_EMIT JobFinished();
    });
    connect(&mCreation, &RenderGraphCreation::Created, this, [this](const QString& rootGraph, uint64 epoch) {
        if (epoch == mEpoch)
        {
            if (OpenGraph(rootGraph) == true)
            {
                mStatus->setText(QStringLiteral("Created %1. Open a World viewport, then choose Build & Preview.")
                                     .arg(QDir(mProjectRoot).relativeFilePath(rootGraph)));
            }
            else
            {
                mStatus->setText(QStringLiteral("Graph sources were created at %1 but could not be opened. "
                                                "The previous draft is still open.")
                                     .arg(rootGraph));
            }
        }
    });
    connect(&mCreation, &RenderGraphCreation::Failed, this, [this](const QString& message) {
        renderStatus();
        mStatus->setText(message);
    });
    connect(&mLiveBuild, &RenderGraphLiveBuild::BuildRequested, this, [this]() {
        static_cast<void>(BuildAndPreview());
    });
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
    connect(&mJob, &RenderGraphJob::Finished, this, &RendererWorkspace::AcceptResult);
    renderStatus();
    renderPreview();
}

RendererWorkspace::~RendererWorkspace() = default;

void RendererWorkspace::SetProject(const QString& root, uint64 epoch)
{
    // Package and project roots share one canonical identity, including symlinked projects.
    const QString canonicalRoot = root.isEmpty() == true ? QString() : QFileInfo(root).canonicalFilePath();
    if (canonicalRoot != mProjectRoot || epoch != mEpoch)
    {
        mCreation.Cancel();
    }
    mLiveBuild.Reset();
    const bool isSameRoot = canonicalRoot == mProjectRoot;
    mEpoch = epoch;
    if (isSameRoot == true)
    {
        return;
    }
    CancelJob();
    mProjectRoot = canonicalRoot;
    // Tool overrides and resolved tools belong to one project's SDK; the worker socket is an
    // editor preference and stays.
    mBuildSettings.ShouldOverrideTools = false;
    mBuildSettings.Tools = {};
    mResolvedTools = {};
    mResolvedSdk.clear();
    mDocument.Reset();
    mSelectedFile.clear();
    mSelectedConfiguration.clear();
    mSelectedNode.clear();
    mLastResult = {};
    mHasResult = false;
    mIsValidated = false;
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
        // The reason is a full sentence; drop its period so the parenthesis closes the clause.
        if (problem.endsWith(QLatin1Char('.')) == true)
        {
            problem.chop(1);
        }
        mStatus->setText(QStringLiteral("%1/%2 could not be opened (%3). Showing the engine default; Save will not "
                                        "replace the existing files.")
                             .arg(PROJECT_RENDER_GRAPH_DIRECTORY, DEFAULT_RENDER_GRAPH_ROOT, problem));
    }
}

bool RendererWorkspace::CanCreateGraph() const
{
    return mProjectRoot.isEmpty() == false && IsJobActive() == false;
}

bool RendererWorkspace::PromptNewGraph()
{
    if (CanCreateGraph() == false || ConfirmDiscard() == false)
    {
        return false;
    }
    const uint64 epoch = mEpoch;
    QInputDialog dialog(this);
    dialog.setObjectName(QStringLiteral("newRenderGraphDialog"));
    dialog.setWindowTitle(QStringLiteral("New Render Graph"));
    dialog.setLabelText(QStringLiteral("Graph name\n\nCreates an editable graph and shaders in:\n%1/<name>/\n\n"
                                       "Use letters, numbers, - or _, starting with a letter.\n"
                                       "The graph will render your World into a color image for preview.")
                            .arg(QDir(mProjectRoot).filePath(QStringLiteral("graphs"))));
    dialog.setTextValue(QStringLiteral("Main"));
    dialog.setOkButtonText(QStringLiteral("Create"));
    return dialog.exec() == QDialog::Accepted && epoch == mEpoch && CreateGraph(dialog.textValue().trimmed()) == true;
}

bool RendererWorkspace::CreateGraph(const QString& name)
{
    if (IsJobActive() == true)
    {
        return false;
    }
    QString error;
    if (mCreation.Start(mTooling, mProjectRoot, name, mEpoch, error) == false)
    {
        mStatus->setText(error);
        return false;
    }
    renderStatus();
    return true;
}

bool RendererWorkspace::PromptOpenGraph()
{
    if (CanCreateGraph() == false || ConfirmDiscard() == false)
    {
        return false;
    }
    const uint64 epoch = mEpoch;
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Open Render Graph"),
                                                      mProjectRoot,
                                                      QStringLiteral("Render graphs (*.lurender.json)"));
    if (path.isEmpty() == true || epoch != mEpoch)
    {
        return false;
    }
    if (OpenGraph(path) == false)
    {
        mStatus->setText(QStringLiteral("Open a root .lurender.json inside the project folder."));
        return false;
    }
    return true;
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
    mHasResult = false;
    mIsValidated = false;
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
    // Like New Graph, never write project graphs through a linked graphs folder.
    const QFileInfo graphs(QDir(mProjectRoot).filePath(QStringLiteral("graphs")));
    if (wasBuiltIn == true && (graphs.isSymLink() == true || (graphs.exists() == true && graphs.isDir() == false)))
    {
        mStatus->setText(QStringLiteral("The project's graphs folder must be a local directory, not a link or file. "
                                        "Nothing was written."));
        Q_EMIT DocumentChanged();
        return false;
    }
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
    if (mDocument.IsLoaded() == false || mProjectRoot.isEmpty() == true || IsJobActive() == true)
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
    mStatus->setText(
        QStringLiteral("Running render-graph %1 on draft revision %2...").arg(action).arg(request.Revision));
    renderStatus();
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

bool RendererWorkspace::CanBuild() const
{
    return mDocument.IsLoaded() == true && IsJobActive() == false;
}

void RendererWorkspace::SetWorkerSocket(const QString& endpoint)
{
    mBuildSettings.WorkerSocket = endpoint;
}

bool RendererWorkspace::startBuild(bool shouldPreview)
{
    if (mDocument.GetFiles().contains(manifestFile()) == false)
    {
        mStatus->setText(
            QStringLiteral("Create %1 with `ludus render-graph new-build` before building.").arg(manifestFile()));
        return false;
    }
    const QString build = buildDirectory();
    if (build.isEmpty() == true)
    {
        mStatus->setText(DescribePackageOutsideProject());
        return false;
    }
    const bool shouldOverride = mBuildSettings.ShouldOverrideTools;
    const RenderGraphBuildTools& tools = mBuildSettings.Tools;
    if (shouldOverride == true &&
        (tools.Helper.trimmed().isEmpty() == true || tools.Compiler.trimmed().isEmpty() == true))
    {
        mStatus->setText(QStringLiteral("Supply both advanced overrides in Tools > Options > Render Graph, or turn "
                                        "off Override detected shader tools to use the project's SDK automatically."));
        return false;
    }
    QStringList arguments{QStringLiteral("{snapshot}/") + manifestFile(),
                          build,
                          QStringLiteral("--root"),
                          QStringLiteral("{snapshot}")};
    if (shouldOverride == true)
    {
        arguments += {QStringLiteral("--helper"), tools.Helper, QStringLiteral("--compiler"), tools.Compiler};
    }
    else
    {
        arguments +=
            {QStringLiteral("--project"), mProjectRoot, QStringLiteral("--tooling-root"), mTooling.ToolingRoot};
    }
    if (shouldOverride == true && tools.Validator.isEmpty() == false)
    {
        arguments += {QStringLiteral("--validator"), tools.Validator};
    }
    if (shouldOverride == true && tools.SpirvCross.isEmpty() == false)
    {
        arguments += {QStringLiteral("--spirv-cross"), tools.SpirvCross};
    }
    if (mBuildSettings.WorkerSocket.isEmpty() == false)
    {
        arguments += {QStringLiteral("--worker-socket"), mBuildSettings.WorkerSocket};
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
    const QString build = buildDirectory();
    if (build.isEmpty() == true)
    {
        mStatus->setText(DescribePackageOutsideProject());
        return false;
    }
    const QString configuration = mConfiguration->currentText() == DEFAULT_CONFIGURATION
                                      ? QStringLiteral("Default")
                                      : mConfiguration->currentText();
    return startJob(QStringLiteral("cook-runtime"),
                    {build, GetRuntimePackagePath(), QStringLiteral("--configuration"), configuration},
                    false);
}

void RendererWorkspace::CancelJob()
{
    const bool isCreating = mCreation.IsActive();
    SetLivePreview(false);
    mCreation.Cancel();
    mJob.Cancel();
    if (isCreating == true)
    {
        mStatus->setText(QStringLiteral("Graph creation cancelled. Choose New Graph to try again."));
    }
}

bool RendererWorkspace::IsJobActive() const
{
    return mJob.IsActive() == true || mCreation.IsActive() == true;
}

void RendererWorkspace::SetLivePreview(bool isEnabled)
{
    // Live builds need an open document; the shell's toggle reads the state back.
    if (isEnabled == mIsLivePreviewEnabled || (isEnabled == true && mDocument.IsLoaded() == false))
    {
        return;
    }
    mIsLivePreviewEnabled = isEnabled;
    mLiveBuild.SetEnabled(isEnabled);
    if (isEnabled == true)
    {
        scheduleLiveBuild();
    }
    Q_EMIT LivePreviewChanged(isEnabled);
}

bool RendererWorkspace::IsLivePreviewEnabled() const
{
    return mIsLivePreviewEnabled;
}

void RendererWorkspace::SetBuildTools(const QString& helper, const QString& compiler)
{
    RenderGraphBuildSettings settings = mBuildSettings;
    settings.ShouldOverrideTools = helper.isEmpty() == false || compiler.isEmpty() == false;
    settings.Tools.Helper = helper;
    settings.Tools.Compiler = compiler;
    SetBuildSettings(settings);
}

void RendererWorkspace::SetBuildSettings(const RenderGraphBuildSettings& settings)
{
    if (settings.WorkerSocket != mBuildSettings.WorkerSocket)
    {
        QSettings preferences;
        preferences.setValue(WORKER_SOCKET_KEY, settings.WorkerSocket);
    }
    mBuildSettings = settings;
}

void RendererWorkspace::AcceptResult(const RenderGraphJobResult& result)
{
    if (result.Request.SnapshotDirectory.isEmpty() == false)
    {
        QDir(result.Request.SnapshotDirectory).removeRecursively();
    }
    mLiveBuild.SetBusy(false);
    // A result for another project, request or closed document never publishes.
    if (result.Request.Request != mActiveRequest || result.Request.ProjectEpoch != mEpoch ||
        mDocument.IsLoaded() == false)
    {
        Q_EMIT JobFinished();
        return;
    }
    // The rows are the Error List's (MakeRenderGraphErrorEntries); ActivateDiagnostic maps
    // a row back to its authored location from this result.
    mLastResult = result;
    mHasResult = true;
    if (result.IsSuccessful == true)
    {
        const QJsonObject toolchain = result.Payload.value(QStringLiteral("toolchain")).toObject();
        if (toolchain.isEmpty() == false && mBuildSettings.ShouldOverrideTools == false)
        {
            mResolvedTools =
            {
                .Helper = toolchain.value(QStringLiteral("helper")).toString(),
                .Compiler = toolchain.value(QStringLiteral("compiler")).toString(),
                .Validator = toolchain.value(QStringLiteral("validator")).toString(),
                .SpirvCross = toolchain.value(QStringLiteral("spirv_cross")).toString(),
            };
            mResolvedSdk = toolchain.value(QStringLiteral("sdk")).toString();
        }
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
    }
    else if (result.Request.Action == QStringLiteral("dump"))
    {
        mIsValidated = false;
    }
    if (result.IsSuccessful == true && result.Request.ShouldPreview == true &&
        result.Request.InputIdentity == getBuildIdentity())
    {
        static_cast<void>(PreviewRuntime());
    }
    renderStatus();
    if (result.IsSuccessful == false)
    {
        // The Error List shows every row; the status line names the first.
        mStatus->setText(
            QStringLiteral("%1 failed - %2: %3 [%4]")
                .arg(result.Request.Action, result.Code, result.Message, DescribeLocation(result.Location)));
    }
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
    if (mCreation.IsActive() == true)
    {
        mStatus->setText(QStringLiteral("Creating the graph and shader sources in your project..."));
    }
    else if (mJob.IsActive() == true)
    {
        mStatus->setText(QStringLiteral("Working on the render graph..."));
    }
    else if (mDocument.IsLoaded() == false)
    {
        mPath->setText(QStringLiteral("No render graph is open."));
        mPath->setToolTip({});
        mStatus->setText(QStringLiteral("Choose New Graph to create a graph in this project, or Open Graph "
                                        "to edit an existing one."));
    }
    else if (isUneditedBuiltIn() == true)
    {
        const QString destination = QDir(mProjectRoot).relativeFilePath(mDocument.GetPackageRoot());
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
        const QString destination = QDir(mProjectRoot).relativeFilePath(mDocument.GetPackageRoot());
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
    updateActions();
}

bool RendererWorkspace::isUneditedBuiltIn() const
{
    return mDocument.IsBuiltIn() == true && mDocument.IsDirty() == false;
}

void RendererWorkspace::updateActions()
{
    const bool isOpen = mDocument.IsLoaded();
    const bool canCreate = CanCreateGraph();
    mCreateFirst->setEnabled(canCreate);
    mContent->setCurrentIndex(isOpen == true ? 1 : 0);
    mEmptyMessage->setText(
        mProjectRoot.isEmpty() == true
            ? QStringLiteral("Open a game project to create or edit its render graphs.")
            : QStringLiteral("Create your first render graph\n\n"
                             "Graphs and shader sources are saved in this project's graphs folder.\n"
                             "Create a graph, then use Build & Preview with an open World viewport."));
    mConfiguration->setEnabled(CanBuild());
    mPreviewButton->setEnabled(CanBuild() == true && mPreviewOrNull != nullptr);
    mCustomize->setVisible(mDocument.IsBuiltIn() == true);
    mCustomize->setEnabled(mDocument.IsBuiltIn() == true && canCreate == true);
    Q_EMIT CommandsChanged();
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
    if (mHasResult == false)
    {
        return 0;
    }
    return mLastResult.IsSuccessful == true ? 1 : 1 + static_cast<int32>(mLastResult.Diagnostics.size());
}

QJsonObject RendererWorkspace::getDiagnosticLocation(int32 row) const
{
    // Row 0 summarizes the result; later rows are the tool diagnostics in report order.
    if (row == 0)
    {
        return mLastResult.IsSuccessful == true ? QJsonObject() : mLastResult.Location;
    }
    return mLastResult.Diagnostics.at(row - 1).toObject().value(QStringLiteral("location")).toObject();
}

void RendererWorkspace::openSourceLine(const QString& file, int32 line)
{
    mViews->setCurrentIndex(SOURCE_VIEW);
    mSourceFile->setCurrentText(file);
    renderSource();
    const QTextBlock block = mSource->document()->findBlockByNumber(line > 0 ? line - 1 : 0);
    QTextCursor cursor(block);
    mSource->setTextCursor(cursor);
    mSource->setFocus();
}

bool RendererWorkspace::ActivateDiagnostic(int32 row)
{
    if (row < 0 || row >= GetDiagnosticCount())
    {
        return false;
    }
    const QJsonObject location = getDiagnosticLocation(row);
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
    mViews->setCurrentIndex(CANVAS_VIEW);
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
    if (mProjectRoot.isEmpty() == true || mDocument.IsLoaded() == false)
    {
        return {};
    }
    QString name = mDocument.GetRootGraph();
    name.chop(static_cast<qsizetype>(QStringLiteral(".lurender.json").size()));
    // Both roots are canonical, so a package outside the project shows as an escaping path.
    const QString package = QDir(mProjectRoot).relativeFilePath(mDocument.GetPackageRoot());
    if (package == QStringLiteral("..") || package.startsWith(QStringLiteral("../")) == true ||
        QDir::isAbsolutePath(package) == true)
    {
        return {};
    }
    const QString owner = package == QStringLiteral(".") ? name : package + QLatin1Char('/') + name;
    return QDir(mProjectRoot).filePath(QStringLiteral("out/render-graph/") + owner + QStringLiteral("/build"));
}

QString RendererWorkspace::GetRuntimePackagePath() const
{
    const QString build = buildDirectory();
    if (build.isEmpty() == true)
    {
        return {};
    }
    QString name = mDocument.GetRootGraph();
    name.chop(static_cast<qsizetype>(QStringLiteral(".lurender.json").size()));
    return QDir::cleanPath(QDir(build).filePath(QStringLiteral("../") + name + QStringLiteral(".lurgrt")));
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

QWidget* RendererWorkspace::TakeInspectorPanel(QWidget* newParent)
{
    LUDUS_REQUIRE(newParent != nullptr && mInspectorPanel->parentWidget() == this);
    mInspectorPanel->setParent(newParent);
    // Undo the standalone hide, so the host's layout or stack shows the panel.
    mInspectorPanel->show();
    return mInspectorPanel;
}

QWidget* RendererWorkspace::TakePreviewPanel(QWidget* newParent)
{
    LUDUS_REQUIRE(newParent != nullptr && mPreviewPage->parentWidget() == this);
    mPreviewPage->setParent(newParent);
    mPreviewPage->show();
    return mPreviewPage;
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
    if (sourceOrNull == mPreviewSourceOrNull)
    {
        return;
    }
    if (mPreviewSourceOrNull != nullptr)
    {
        disconnect(mPreviewSourceOrNull, nullptr, this, nullptr);
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr)
    {
        // Release generations on the previous viewport's device; the new source's device (once
        // it has one) is attached by ServicePreview, which also restages the requested generation.
        mPreviewOrNull->Detach();
    }
#endif
    mPreviewSourceOrNull = sourceOrNull;
    mPreviewDeviceSerial = 0;
    mPreviewSceneIdentity.clear();
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
    // The unedited engine default previews the editor's prebuilt generation; anything else
    // previews its canonical project build output, which is empty outside the project.
    const QString runtime = GetPreviewPackagePath();
    if (runtime.isEmpty() == true)
    {
        return false;
    }
    const RenderGraphPreviewLoad loaded = mPreviewOrNull->Load(runtime);
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
        // The World viewport's current camera or orbit, at the graph's exported extent. The
        // runtime profile admits only conventional depth, so request a finite conventional
        // projection of the same camera instead of the viewport's infinite reverse-Z one.
        graphics::renderer::ViewDescription view;
        QString error;
        const RenderScene& scene = mPreviewSourceOrNull->Scene();
        const bool hasView = mPreviewSourceOrNull->DescribeConventionalView(static_cast<uint32>(target.width()),
                                                                            static_cast<uint32>(target.height()),
                                                                            view,
                                                                            error);
        if (hasView == false && scene.Identity != mPreviewSceneIdentity)
        {
            mPreviewStatus->setText(error);
            mPreviewSceneIdentity = scene.Identity;
        }
        if (hasView == true && (scene.Identity != mPreviewSceneIdentity ||
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
