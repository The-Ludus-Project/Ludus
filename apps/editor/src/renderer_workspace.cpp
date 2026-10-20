#include "internal/render_graph_configuration_editor.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_graph_worker.h"
#endif
#include "internal/item_selection.h"
#include "internal/render_graph_pipeline.h"
#include "internal/render_graph_shader_interface.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

#include "internal/render_preview.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "editor_default_graph.h"
#    include "editor_renderer_static.h"
#    include "internal/render_graph_preview.h"
#    include "internal/texture_viewer.h"
#    include "renderer_composite.h"
#endif

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
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
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextDocument>
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
// The source tree stays wide enough for nested node names.
constexpr int32 MINIMUM_SOURCE_TREE_WIDTH = 200;
// Initial share of the document's width: source tree, then the Canvas/Source/Plan/Textures views.
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
    TEXTURES_VIEW,
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

/// Choices for a Subgraph reference: every other draft graph for `asset`, and the
/// referenced graph's configurations for `configuration`. An authored value that is not a
/// current choice (a missing file or configuration) stays listed, so the inspector shows
/// it unchanged; the shared validator still owns legality, including cycles.
// File and field are distinct authored identities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QStringList GetSubgraphChoices(const RenderGraphDocument& document,
                               const QString& file,
                               const QJsonObject& node,
                               const QString& field)
{
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph"))
    {
        return {};
    }
    QStringList choices;
    if (field == QStringLiteral("asset"))
    {
        for (const QString& candidate : document.GetFiles())
        {
            QJsonObject graph;
            if (candidate != file && document.GetGraph(candidate, graph) == true)
            {
                choices.append(candidate);
            }
        }
    }
    else if (field == QStringLiteral("configuration"))
    {
        QJsonObject graph;
        if (document.GetGraph(node.value(QStringLiteral("asset")).toString(), graph) == true)
        {
            const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
            for (const QJsonValueConstRef configuration : configurations)
            {
                choices.append(configuration.toObject().value(QStringLiteral("id")).toString());
            }
        }
    }
    else
    {
        return {};
    }
    const QString current = node.value(field).toString();
    if (choices.contains(current) == false)
    {
        choices.prepend(current);
    }
    return choices;
}

/// Default authored record for a new node; the shared validator still owns legality.
QJsonObject MakeNode(const QString& kind, const QString& identity)
{
    if (IsPipelineNode(kind) == true)
    {
        return MakePipelineNode(kind, identity);
    }
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
      mNodeCreationMessage(new QLabel(mInspector)), mEdges(new QTableWidget(mInspector)),
      mEdgeFrom(new QLineEdit(mInspector)), mEdgeTo(new QLineEdit(mInspector)), mSourceFile(new QComboBox(this)),
      mSource(new QPlainTextEdit(this)), mSourceInterface(new QPlainTextEdit(this)), mPlan(new QPlainTextEdit(this)),
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
    QPushButton* editInterface = new QPushButton(QStringLiteral("Edit Inputs / Outputs..."), this);
    editInterface->setObjectName(QStringLiteral("renderGraphEditInterface"));
    header->addWidget(editInterface);
    connect(editInterface, &QPushButton::clicked, this, &RendererWorkspace::editConfigurationInterface);
    connect(this, &RendererWorkspace::CommandsChanged, editInterface, [this, editInterface]() {
        editInterface->setEnabled(CanCreateSubgraph());
    });
    QPushButton* createSubgraph = new QPushButton(QStringLiteral("Create Subgraph..."), this);
    createSubgraph->setObjectName(QStringLiteral("renderGraphCreateSubgraph"));
    header->addWidget(createSubgraph);
    connect(createSubgraph, &QPushButton::clicked, this, [this]() { static_cast<void>(PromptCreateSubgraph()); });
    connect(this, &RendererWorkspace::CommandsChanged, createSubgraph, [this, createSubgraph]() {
        createSubgraph->setEnabled(CanCreateSubgraph());
    });

    mTree->setObjectName(QStringLiteral("renderGraphTree"));
    ConfigureItemSelection(*mTree);
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
                        QStringLiteral("GraphicsPipeline"),
                        QStringLiteral("ComputePipeline"),
                        QStringLiteral("RayTracingPipeline"),
                        QStringLiteral("Subgraph"),
                        QStringLiteral("Export"),
                        QStringLiteral("Present")});
    mNewKind->setAccessibleName(QStringLiteral("New node type"));
    QPushButton* add = new QPushButton(QStringLiteral("Add Node"), mInspector);
    add->setObjectName(QStringLiteral("renderGraphAddNode"));
    QHBoxLayout* addRow = new QHBoxLayout;
    addRow->addWidget(mNewKind);
    addRow->addStretch();
    addRow->addWidget(add);
    inspectorLayout->addLayout(addRow);
    mNodeCreationMessage->setObjectName(QStringLiteral("renderGraphNodeCreationMessage"));
    mNodeCreationMessage->setTextFormat(Qt::PlainText);
    mNodeCreationMessage->setWordWrap(true);
    mNodeCreationMessage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mNodeCreationMessage->hide();
    inspectorLayout->addWidget(mNodeCreationMessage);
    add->setToolTip(QStringLiteral("Add this type to the configuration shown on the canvas, with a unique ID."));
    mEdges->setObjectName(QStringLiteral("renderGraphEdges"));
    ConfigureItemSelection(*mEdges);
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
    mSource->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mSource->setLineWrapMode(QPlainTextEdit::NoWrap);
    QPushButton* applyText = new QPushButton(QStringLiteral("Apply Text to Draft"), sourcePage);
    QPushButton* revertText = new QPushButton(QStringLiteral("Revert Text"), sourcePage);
    QHBoxLayout* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(mSourceFile, 1);
    sourceRow->addWidget(applyText);
    sourceRow->addWidget(revertText);
    sourceLayout->addLayout(sourceRow);
    QPushButton* openCodeEditor = new QPushButton(QStringLiteral("Open in Code Editor"), sourcePage);
    QPushButton* chooseCodeEditor = new QPushButton(QStringLiteral("Choose Code Editor..."), sourcePage);
    QPushButton* reloadSources = new QPushButton(QStringLiteral("Reload Sources"), sourcePage);
    openCodeEditor->setObjectName(QStringLiteral("renderGraphOpenCodeEditor"));
    chooseCodeEditor->setObjectName(QStringLiteral("renderGraphChooseCodeEditor"));
    reloadSources->setObjectName(QStringLiteral("renderGraphReloadSources"));
    QHBoxLayout* editorRow = new QHBoxLayout;
    editorRow->addWidget(openCodeEditor);
    editorRow->addWidget(chooseCodeEditor);
    editorRow->addWidget(reloadSources);
    sourceLayout->addLayout(editorRow);
    QLabel* sourceHint = new QLabel(QStringLiteral("Shader edits update the World viewport automatically. "
                                                   "Save external edits in your code editor to reload them."),
                                    sourcePage);
    sourceHint->setWordWrap(true);
    sourceLayout->addWidget(sourceHint);
#if defined(Q_OS_WASM)
    openCodeEditor->setEnabled(false);
    chooseCodeEditor->setEnabled(false);
    sourceHint->setText(QStringLiteral("External code editors are available in the native Ludus editor."));
#endif
    mSourceInterface->setObjectName(QStringLiteral("renderGraphSourceInterface"));
    mSourceInterface->setReadOnly(true);
    mSourceInterface->setToolTip(QStringLiteral("Shader fields and uniforms declared by the draft build manifest. "
                                                "Canvas ports describe pass resources, not shader parameters."));
    mSourceInterface->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mSourceInterface->setAccessibleName(QStringLiteral("Shader inputs and outputs"));
    QWidget* interfacePage = new QWidget(sourcePage);
    QVBoxLayout* interfaceLayout = new QVBoxLayout(interfacePage);
    interfaceLayout->setContentsMargins(0, 0, 0, 0);
    interfaceLayout->addWidget(new QLabel(QStringLiteral("Inputs / Outputs — draft manifest"), interfacePage));
    interfaceLayout->addWidget(mSourceInterface, 1);
    QSplitter* sourceSplit = new QSplitter(Qt::Horizontal, sourcePage);
    sourceSplit->addWidget(mSource);
    sourceSplit->addWidget(interfacePage);
    sourceSplit->setStretchFactor(0, 2);
    sourceSplit->setStretchFactor(1, 1);
    sourceLayout->addWidget(sourceSplit, 1);

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
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // Textures: any target or attachment of the previewed generation (native GPU builds).
    mTextureViewerOrNull = new TextureViewer(this);
    mViews->insertTab(TEXTURES_VIEW, mTextureViewerOrNull, QStringLiteral("Textures"));
#endif

    mStatus->setObjectName(QStringLiteral("renderGraphStatus"));
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QSplitter* horizontal = new QSplitter(Qt::Horizontal, this);
    horizontal->setObjectName(QStringLiteral("renderGraphSplitter"));
    horizontal->addWidget(mTree);
    horizontal->addWidget(mViews);
    mTree->setMinimumWidth(MINIMUM_SOURCE_TREE_WIDTH);
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
    connect(mConfiguration, &QComboBox::currentTextChanged, this, [this]() {
        scheduleLiveBuild();
        Q_EMIT DocumentChanged();
    });
    connect(mCustomize, &QAction::triggered, this, [this]() { static_cast<void>(CustomizeBuiltIn()); });
    connect(mTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* itemOrNull = mTree->itemAt(position);
        if (itemOrNull != nullptr)
        {
            mTree->setCurrentItem(itemOrNull);
        }
        else
        {
            mSelectedNode.clear();
        }
        showNodeContextMenu(mTree->viewport()->mapToGlobal(position));
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
                mStatus->setText(
                    (mIsWorldRenderingEnabled == true
                         ? QStringLiteral("Created %1. The World viewport now uses this graph.")
                         : QStringLiteral("Created %1. Open a World viewport, then choose Build & Preview."))
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
        updateActions();
    });
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this]() { activateSelectedNode(); });
    connect(mCanvas, &RenderGraphCanvas::InterfaceEditRequested, this, &RendererWorkspace::editConfigurationInterface);
    connect(mCanvas, &RenderGraphCanvas::ShaderSourcesRequested, this, [this](const QString& node) {
        static_cast<void>(SelectNode(mCanvas->GetFile(), mCanvas->GetConfiguration(), node));
        activateSelectedNode();
    });
    connect(mCanvas, &RenderGraphCanvas::NodeSelected, this, [this](const QString& node) {
        mSelectedFile = mCanvas->GetFile();
        mSelectedConfiguration = mCanvas->GetConfiguration();
        mSelectedNode = node;
        renderTree();
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        // A node that writes or publishes textures shows them in the texture viewer.
        QJsonObject graph;
        if (mTextureViewerOrNull != nullptr && mDocument.GetGraph(mSelectedFile, graph) == true)
        {
            static_cast<void>(
                mTextureViewerOrNull->SelectCanvasNode(graph.value(QStringLiteral("id")).toString(), node));
        }
#endif
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
    connect(mCanvas,
            &RenderGraphCanvas::ContextMenuRequested,
            this,
            [this](const QString& node, const QPoint& globalPosition) {
                mSelectedFile = mCanvas->GetFile();
                mSelectedConfiguration = mCanvas->GetConfiguration();
                mSelectedNode = node;
                renderTree();
                showNodeContextMenu(globalPosition);
            });
    connect(mPreviewButton, &QPushButton::clicked, this, [this]() { static_cast<void>(PreviewRuntime()); });
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mPreviewOrNull = new RenderGraphPreview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment(),
        this);
    connect(mPreviewOrNull, &RenderGraphPreview::Changed, this, &RendererWorkspace::renderPreview);
    connect(mPreviewOrNull, &RenderGraphPreview::FramePresented, this, &RendererWorkspace::renderPreview);
    mTextureViewerOrNull->SetPreview(mPreviewOrNull);
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
    connect(mSourceFile, &QComboBox::currentTextChanged, this, [this]() {
        if (mSourceEditDebounce.isActive() == true)
        {
            applySourceEdit();
        }
        renderSource();
    });
    connect(applyText, &QPushButton::clicked, this, [this]() {
        documentEdited(mDocument.SetText(mSourceFile->currentText(), mSource->toPlainText().toUtf8()));
    });
    connect(revertText, &QPushButton::clicked, this, [this]() { renderSource(); });
    connect(openCodeEditor, &QPushButton::clicked, this, [this]() { static_cast<void>(OpenSourceInCodeEditor()); });
    connect(chooseCodeEditor, &QPushButton::clicked, this, [this]() {
        static_cast<void>(mCodeEditor.Configure(this));
    });
    connect(reloadSources, &QPushButton::clicked, this, [this]() { static_cast<void>(ReloadSourcesFromDisk()); });
    connect(&mJob, &RenderGraphJob::Finished, this, &RendererWorkspace::AcceptResult);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr)
    {
        connect(mPreviewOrNull, &RenderGraphPreview::FramePresented, this, [this]() {
            if (mPreviewSourceOrNull != nullptr && mPreviewOrNull->GetPresentedRevision() == mPreviewRevision &&
                mPreviewSourceOrNull->Scene().Identity == mPreviewSceneIdentity &&
                mPreviewSourceOrNull->GetDeviceSerial() == mPreviewDeviceSerial)
            {
                mPreviewSourceOrNull->NotifyProjectFrame();
            }
        });
    }
#endif
    constexpr int32 SOURCE_EDIT_DEBOUNCE_MILLISECONDS = 250;
    mSourceEditDebounce.setSingleShot(true);
    mSourceEditDebounce.setInterval(SOURCE_EDIT_DEBOUNCE_MILLISECONDS);
    connect(mSource, &QPlainTextEdit::textChanged, this, [this]() {
        if (mIsWorldRenderingEnabled == true && mSourceFile->currentText().endsWith(QStringLiteral(".slang")) == true)
        {
            mEditingSource = mSourceFile->currentText();
            mEditingText = mSource->toPlainText().toUtf8();
            mSourceEditDebounce.start();
        }
    });
    connect(&mSourceEditDebounce, &QTimer::timeout, this, &RendererWorkspace::applySourceEdit);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mWorkerOrNull = new RenderGraphWorker(this);
    connect(mWorkerOrNull, &RenderGraphWorker::Ready, this, [this]() {
        if (mHasPendingWorkerBuild == true)
        {
            mHasPendingWorkerBuild = false;
            if (mShouldPreviewAfterWorker == true)
            {
                static_cast<void>(BuildAndPreview());
            }
            else
            {
                static_cast<void>(Build());
            }
        }
    });
    connect(mWorkerOrNull, &RenderGraphWorker::Failed, this, [this]() {
        mHasPendingWorkerBuild = false;
        mStatus->setText(mWorkerOrNull->GetError());
        setProblem(mBuildProblem, mStatus->text());
        Q_EMIT JobFinished();
    });
#endif
    connect(&mSourceWatch, &RenderGraphSourceWatch::SourcesChanged, this, [this]() {
        mHasPendingSourceChange = true;
        refreshWatchedSources();
    });
    connect(this, &RendererWorkspace::JobFinished, this, [this]() { refreshWatchedSources(); });
    connect(this, &RendererWorkspace::DocumentChanged, this, [this]() {
        mSourceWatch.SetDocument(mDocument, mProjectRoot);
        if (mIsWorldRenderingEnabled == true)
        {
            scheduleLiveBuild();
        }
    });
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
    const bool isSameRoot = canonicalRoot == mProjectRoot;
    const bool hasNewEpoch = epoch != mEpoch;
    mEpoch = epoch;
    if (isSameRoot == true)
    {
        if (hasNewEpoch == true)
        {
            CancelJob();
            mLiveBuild.Reset();
            if (mIsWorldRenderingEnabled == true)
            {
                SetLivePreview(true);
            }
        }
        return;
    }
    mSourceEditDebounce.stop();
    mLiveBuild.Reset();
    CancelJob();
    mProjectRoot = canonicalRoot;
    // Tool overrides and resolved tools belong to one project's SDK; the worker socket is an
    // editor preference and stays.
    mBuildSettings.ShouldOverrideTools = false;
    mBuildSettings.Tools = {};
    mResolvedTools = {};
    mResolvedSdk.clear();
    mDocument.Reset();
    mBuildProblem.clear();
    mSourceProblem.clear();
    mExtractionProblem.clear();
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
    const QString projectGraph = ResolveProjectRenderGraph(mProjectRoot);
    if (projectGraph.isEmpty() == false)
    {
        // A broken authored graph is an error, never a request to silently draw the default.
        static_cast<void>(OpenGraph(projectGraph));
        return;
    }
    RenderGraphDocument candidate;
    if (candidate.LoadBuiltIn(GetDefaultRenderGraphSource(), DEFAULT_RENDER_GRAPH_ROOT, package) !=
        RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(QStringLiteral("The engine default render graph is unavailable in this editor build."));
        return;
    }
    showDocument(candidate);
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
    const QString selectedFile = candidate.GetRootGraph();
    QJsonObject selectedGraph;
    if (selectedFile != DEFAULT_RENDER_GRAPH_ROOT && candidate.GetGraph(selectedFile, selectedGraph) == true &&
        QFileInfo::exists(QDir(candidate.GetPackageRoot()).filePath(DEFAULT_RENDER_GRAPH_ROOT)) == true)
    {
        const QJsonArray configurations = selectedGraph.value(QStringLiteral("configurations")).toArray();
        bool hasRoot = false;
        for (const QJsonValueConstRef configuration : configurations)
        {
            hasRoot = hasRoot == true || configuration.toObject().value(QStringLiteral("inputs")).toArray().isEmpty();
        }
        if (hasRoot == false &&
            candidate.Load(candidate.GetPackageRoot(), DEFAULT_RENDER_GRAPH_ROOT) != RenderGraphDocumentStatus::Ok)
        {
            return false;
        }
    }
    showDocument(candidate);
    mSelectedFile = selectedFile;
    renderTree();
    Q_EMIT DocumentChanged();
    return true;
}

void RendererWorkspace::showDocument(const RenderGraphDocument& document)
{
    CancelJob();
    mSourceEditDebounce.stop();
    mDocument = document;
    mBuildProblem.clear();
    mSourceProblem.clear();
    mExtractionProblem.clear();
    mPreviewSceneIdentity.clear();
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
    if (mIsWorldRenderingEnabled == true)
    {
        SetLivePreview(true);
    }
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
    if (mSourceEditDebounce.isActive() == true)
    {
        applySourceEdit();
    }
    if (isUneditedBuiltIn() == true)
    {
        // The unedited engine default is not project content; Save never copies it.
        return true;
    }
    return writeDocument();
}

bool RendererWorkspace::hasPendingSourceText() const
{
    QString draftText = QString::fromUtf8(mDocument.GetDraft(mSourceFile->currentText()));
    draftText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    draftText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return mSourceFile->currentText().isEmpty() == false && mSource->document()->isModified() == true &&
           mSource->toPlainText() != draftText;
}

bool RendererWorkspace::OpenSourceInCodeEditor()
{
    if (mDocument.IsLoaded() == false || IsJobActive() == true)
    {
        return false;
    }
    const QString file = mSourceFile->currentText();
    const bool hasPendingText = hasPendingSourceText();
    if (mDocument.IsBuiltIn() == true || mDocument.IsDirty() == true || hasPendingText == true)
    {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this,
            QStringLiteral("Open in Code Editor"),
            mDocument.IsBuiltIn() == true
                ? QStringLiteral("Create an editable project copy before opening this source in your code editor?")
                : QStringLiteral("Save the graph and source changes before opening this file in your code editor?"),
            QMessageBox::Save | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer != QMessageBox::Save)
        {
            return false;
        }
        if (hasPendingText == true)
        {
            const RenderGraphDocumentStatus status = mDocument.SetText(file, mSource->toPlainText().toUtf8());
            documentEdited(status);
            if (status != RenderGraphDocumentStatus::Ok)
            {
                return false;
            }
        }
        if ((mDocument.IsBuiltIn() == true ? CustomizeBuiltIn() : Save()) == false)
        {
            return false;
        }
    }
    const QString path = QFileInfo(QDir(mDocument.GetPackageRoot()).filePath(file)).canonicalFilePath();
    if (path.isEmpty() == true || path.startsWith(mProjectRoot + QLatin1Char('/')) == false)
    {
        mStatus->setText(QStringLiteral("Code editing requires a saved source inside this project."));
        return false;
    }
    QString error;
    if (mCodeEditor.Open(this, path, error) == false)
    {
        mStatus->setText(error);
        return false;
    }
    mStatus->setText(QStringLiteral("Opening %1. Save in your code editor, then Build & Preview.").arg(file));
    return true;
}

bool RendererWorkspace::ReloadSourcesFromDisk()
{
    if (IsJobActive() == true || hasPendingSourceText() == true)
    {
        mStatus->setText(
            QStringLiteral("Finish the current job and apply or revert pending text before reloading sources."));
        return false;
    }
    bool hasChanged = false;
    const RenderGraphDocumentStatus status = mDocument.RefreshFromDisk(hasChanged);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(
            status == RenderGraphDocumentStatus::Conflict
                ? QStringLiteral("External sources changed while local edits exist. Undo local edits or reopen "
                                 "the graph to resolve them; the current draft is retained.")
                : DescribeStatus(status));
        setProblem(mSourceProblem, mStatus->text());
        return false;
    }
    setProblem(mSourceProblem, {});
    if (hasChanged == true)
    {
        documentEdited(RenderGraphDocumentStatus::Ok);
    }
    return true;
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
    if (shouldCaptureSnapshot == true && ReloadSourcesFromDisk() == false)
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
    setProblem(mBuildProblem, {});
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
    if (mSourceEditDebounce.isActive() == true)
    {
        applySourceEdit();
    }
    // The unedited default already has a generation built with the editor: no tools needed.
    if (isUneditedBuiltIn() == true && GetDefaultRenderGraphRuntime().isEmpty() == false && mPreviewOrNull != nullptr)
    {
        mLiveBuild.MarkRequested(getBuildIdentity());
        return PreviewRuntime();
    }
    if (startBuild(true) == false)
    {
        if (mHasPendingWorkerBuild == false)
        {
            setProblem(mBuildProblem, mSourceProblem.isEmpty() == true ? mStatus->text() : QString());
        }
        return false;
    }
    mLiveBuild.MarkRequested(getBuildIdentity());
    return true;
}

bool RendererWorkspace::CanBuild() const
{
    return mDocument.IsLoaded() == true && IsJobActive() == false;
}

QString RendererWorkspace::GetRenderProblem() const
{
    if (mSourceProblem.isEmpty() == false)
    {
        return mSourceProblem;
    }
    return mExtractionProblem.isEmpty() == false ? mExtractionProblem : mBuildProblem;
}
void RendererWorkspace::setProblem(QString& inoutProblem, const QString& text)
{
    if (inoutProblem != text)
    {
        inoutProblem = text;
        Q_EMIT RenderProblemChanged();
    }
}
void RendererWorkspace::applySourceEdit()
{
    mSourceEditDebounce.stop();
    const RenderGraphDocumentStatus status = mDocument.SetText(mEditingSource, mEditingText);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        renderStatus();
        Q_EMIT DocumentChanged();
        scheduleLiveBuild();
    }
    else
    {
        mStatus->setText(DescribeStatus(status));
    }
}
void RendererWorkspace::EnableWorldRendering()
{
    mIsWorldRenderingEnabled = true;
    mSourceWatch.SetDocument(mDocument, mProjectRoot);
    SetLivePreview(true);
}
void RendererWorkspace::refreshWatchedSources()
{
    if (mHasPendingSourceChange == false || IsJobActive() == true)
    {
        return;
    }
    mHasPendingSourceChange = false;
    if (mDocument.IsLoaded() == false || mDocument.IsBuiltIn() == true)
    {
        if (ResolveProjectRenderGraph(mProjectRoot).isEmpty() == false)
        {
            openProjectGraph();
        }
    }
    else
    {
        if (ReloadSourcesFromDisk() == true)
        {
            setProblem(mSourceProblem, {});
            scheduleLiveBuild();
        }
        else
        {
            setProblem(mSourceProblem, mStatus->text());
        }
    }
    mSourceWatch.SetDocument(mDocument, mProjectRoot);
}
QStringList RendererWorkspace::GetConfigurations() const
{
    QStringList configurations;
    for (int32 i = 0; i < mConfiguration->count(); ++i)
    {
        const QString configuration = mConfiguration->itemText(i);
        if (configuration != DEFAULT_CONFIGURATION)
        {
            configurations.append(configuration);
        }
    }
    return configurations;
}
QString RendererWorkspace::GetConfiguration() const
{
    if (mConfiguration->currentText() == DEFAULT_CONFIGURATION)
    {
        QJsonObject graph;
        if (mDocument.GetGraph(mDocument.GetRootGraph(), graph) == true)
        {
            return graph.value(QStringLiteral("default")).toString();
        }
    }
    return mConfiguration->currentText();
}
void RendererWorkspace::SetConfiguration(const QString& configuration)
{
    if (mConfiguration->findText(configuration) >= 0)
    {
        mConfiguration->setCurrentText(configuration);
    }
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
    QString endpoint = mBuildSettings.WorkerSocket;
    if (endpoint.isEmpty() == true)
    {
        endpoint = qEnvironmentVariable("LUDUS_SHADER_WORKER_SOCKET");
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsWorldRenderingEnabled == true && shouldOverride == false && endpoint.isEmpty() == true)
    {
        endpoint = mWorkerOrNull->Prepare();
        if (endpoint.isEmpty() == true)
        {
            const QString error = mWorkerOrNull->GetError();
            mHasPendingWorkerBuild = mWorkerOrNull->GetState() == RenderGraphWorkerState::Starting;
            mShouldPreviewAfterWorker = shouldPreview;
            mStatus->setText(error.isEmpty() == true ? QStringLiteral("Starting shader compiler…") : error);
            if (error.isEmpty() == false)
            {
                Q_EMIT JobFinished();
            }
            return false;
        }
    }
#endif
    if (endpoint.isEmpty() == false)
    {
        arguments += {QStringLiteral("--worker-socket"), endpoint};
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
    mHasPendingWorkerBuild = false;
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
    if (result.Request.ShouldPreview == true)
    {
        // Files can change while the compiler runs, before the watch debounce expires.
        mHasPendingSourceChange = true;
    }
    refreshWatchedSources();
    if (mSourceProblem.isEmpty() == false)
    {
        mLiveBuild.Reset();
    }
    if (result.IsSuccessful == true && result.Request.ShouldPreview == true && mSourceProblem.isEmpty() == true &&
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
        if (result.Request.ShouldPreview == true && result.Request.InputIdentity == getBuildIdentity())
        {
            setProblem(mBuildProblem, mStatus->text());
        }
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
    if (mDocument.GetFiles().contains(mSelectedFile) == false)
    {
        mSelectedFile = mDocument.GetRootGraph();
        QJsonObject graph;
        static_cast<void>(mDocument.GetGraph(mSelectedFile, graph));
        mSelectedConfiguration = graph.value(QStringLiteral("default")).toString();
        mSelectedNode.clear();
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
    // A field's own commit (Return, focus loss, activation, Apply) re-renders the inspector
    // from inside that widget's event handler. Deleting the widget there is a use-after-free,
    // so detach and hide the old rows now and let the event loop delete them.
    while (mFields->rowCount() > 0)
    {
        const QFormLayout::TakeRowResult row = mFields->takeRow(0);
        for (QLayoutItem* itemOrNull : {row.labelItem, row.fieldItem})
        {
            if (itemOrNull == nullptr)
            {
                continue;
            }
            QWidget* widgetOrNull = itemOrNull->widget();
            if (widgetOrNull != nullptr)
            {
                widgetOrNull->hide();
                widgetOrNull->deleteLater();
            }
            delete itemOrNull;
        }
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
    if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
    {
        RenderGraphPipelineEditor* editor = new RenderGraphPipelineEditor(node, mInspector);
        connect(editor, &RenderGraphPipelineEditor::Applied, this, [this](const QJsonObject& candidate) {
            const RenderGraphDocumentStatus status =
                mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, candidate);
            if (status != RenderGraphDocumentStatus::Ok)
            {
                mStatus->setText(QStringLiteral("Disconnect affected ports before removing or changing their type."));
                return;
            }
            documentEdited(status);
        });
        mFields->addRow(editor);
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Raster"))
    {
        QPushButton* migrate = new QPushButton(QStringLiteral("Convert to Graphics Pipeline"), mInspector);
        migrate->setObjectName(QStringLiteral("renderGraphMigratePipeline"));
        connect(migrate, &QPushButton::clicked, this, [this]() {
            QJsonObject candidate;
            const QJsonObject manifest = QJsonDocument::fromJson(mDocument.GetDraft(manifestFile())).object();
            if (MigrateRasterNode(selectedNode(), manifest, candidate) == false)
            {
                mStatus->setText(QStringLiteral("Conversion requires one explicit technique and one complete recipe. "
                                                "The draft was preserved."));
                return;
            }
            documentEdited(mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, candidate));
        });
        mFields->addRow(migrate);
    }
    for (QJsonObject::const_iterator field = node.constBegin(); field != node.constEnd(); ++field)
    {
        const QString name = field.key();
        if (name == QStringLiteral("id") || name == QStringLiteral("kind") ||
            IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
        {
            continue;
        }
        QStringList choices = GetChoices(name);
        if (choices.isEmpty() == true)
        {
            choices = GetSubgraphChoices(mDocument, mSelectedFile, node, name);
        }
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
        const QSignalBlocker blocker(mSource);
        mSource->setPlainText(text);
    }
    mSource->setEnabled(file.isEmpty() == false);
    mSourceInterface->parentWidget()->setVisible(file.endsWith(QStringLiteral(".slang")));
    mSourceInterface->setPlainText(DescribeShaderInterface(mDocument.GetDraft(manifestFile()), file));
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
    // A new Subgraph asset keeps its configuration only when that asset defines it;
    // otherwise it follows the asset's authored default, or its first configuration.
    QJsonObject graph;
    if (field == QStringLiteral("asset") &&
        node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph") &&
        mDocument.GetGraph(value.toString(), graph) == true)
    {
        QStringList configurations;
        const QJsonArray records = graph.value(QStringLiteral("configurations")).toArray();
        for (const QJsonValueConstRef record : records)
        {
            configurations.append(record.toObject().value(QStringLiteral("id")).toString());
        }
        const QString authoredDefault = graph.value(QStringLiteral("default")).toString();
        if (configurations.contains(node.value(QStringLiteral("configuration")).toString()) == false &&
            configurations.isEmpty() == false)
        {
            node.insert(QStringLiteral("configuration"),
                        configurations.contains(authoredDefault) == true ? authoredDefault : configurations.first());
        }
    }
    documentEdited(mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, node));
}

bool RendererWorkspace::CanCreateSubgraph() const
{
    return mDocument.IsLoaded() == true && mCanvas->GetConfiguration().isEmpty() == false && IsJobActive() == false &&
           hasPendingSourceText() == false && mSourceProblem.isEmpty() == true;
}

bool RendererWorkspace::PromptCreateSubgraph()
{
    if (CanCreateSubgraph() == false)
    {
        return false;
    }
    bool isAccepted = false;
    const QString suggested = selectedNode().value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph")
                                  ? mSelectedNode
                                  : QString();
    const QString prompt = suggested.isEmpty() == true ? QStringLiteral("Name (creates <name>.lurender.json):")
                                                       : QStringLiteral("Create a new source for Subgraph '%1'.\n"
                                                                        "Name (creates <name>.lurender.json):")
                                                             .arg(suggested);
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Create Subgraph"),
                                               prompt,
                                               QLineEdit::Normal,
                                               suggested,
                                               &isAccepted)
                             .trimmed();
    return isAccepted == true && CreateSubgraph(name) == true;
}

bool RendererWorkspace::CreateSubgraph(const QString& name)
{
    if (CanCreateSubgraph() == false)
    {
        return false;
    }
    const QString node =
        selectedNode().value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph") ? mSelectedNode : name;
    const RenderGraphDocumentStatus status =
        mDocument.CreateSubgraph(mCanvas->GetFile(), mCanvas->GetConfiguration(), node, name);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(QStringLiteral("Cannot create subgraph. Use a unique name beginning with an ASCII letter, "
                                        "followed by letters, digits, '_' or '-' (up to 63 characters). "
                                        "Existing files and non-Subgraph nodes are never replaced. %1")
                             .arg(DescribeStatus(status)));
        return false;
    }
    const QString asset = name + QStringLiteral(".lurender.json");
    mSelectedFile = asset;
    mSelectedConfiguration = QStringLiteral("Default");
    mSelectedNode.clear();
    documentEdited(status);
    mSourceFile->setCurrentText(asset);
    mViews->setCurrentIndex(CANVAS_VIEW);
    mStatus->setText(
        QStringLiteral("Created %1 as an empty draft. Add nodes on the canvas; choose Edit Inputs / Outputs "
                       "to define its interface. Save publishes the file.")
            .arg(asset));
    return true;
}

QStringList RendererWorkspace::GetSelectedShaderSources() const
{
    const QJsonObject node = selectedNode();
    if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
    {
        QStringList sources;
        const QJsonObject manifest = QJsonDocument::fromJson(mDocument.GetDraft(manifestFile())).object();
        const QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
        const QJsonArray shaders = node.value(QStringLiteral("shaders")).toArray();
        for (const QJsonValueConstRef shader : shaders)
        {
            for (const QJsonValueConstRef program : programs)
            {
                const QJsonObject record = program.toObject();
                const QString source = record.value(QStringLiteral("source")).toString();
                if (record.value(QStringLiteral("id")) == shader.toObject().value(QStringLiteral("program")) &&
                    mDocument.GetFiles().contains(source) == true && sources.contains(source) == false)
                {
                    sources.append(source);
                }
            }
        }
        return sources;
    }
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("Raster"))
    {
        return {};
    }
    const QString material = node.value(QStringLiteral("material")).toString();
    const QString mode = node.value(QStringLiteral("mode")).toString();
    if ((material != QStringLiteral("DefaultOpaque") && material != QStringLiteral("DefaultMasked")) ||
        (mode != QStringLiteral("ColorAndDepth") && mode != QStringLiteral("DepthOnly")))
    {
        return {};
    }
    const QString techniqueId = (material == QStringLiteral("DefaultMasked") ? QStringLiteral("DefaultStaticMasked.")
                                                                             : QStringLiteral("DefaultStaticOpaque.")) +
                                node.value(QStringLiteral("mode")).toString();
    const QJsonObject manifest = QJsonDocument::fromJson(mDocument.GetDraft(manifestFile())).object();
    QStringList programIds;
    const QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    for (const QJsonValueConstRef value : techniques)
    {
        const QJsonObject technique = value.toObject();
        if (technique.value(QStringLiteral("id")).toString() == techniqueId)
        {
            programIds = {technique.value(QStringLiteral("vertex")).toString(),
                          technique.value(QStringLiteral("fragment")).toString()};
        }
    }
    QStringList sources;
    const QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    for (const QJsonValueConstRef value : programs)
    {
        const QJsonObject program = value.toObject();
        const QString source = program.value(QStringLiteral("source")).toString();
        if (programIds.contains(program.value(QStringLiteral("id")).toString()) == true &&
            mDocument.GetFiles().contains(source) == true && sources.contains(source) == false)
        {
            sources.append(source);
        }
    }
    return sources;
}

void RendererWorkspace::activateSelectedNode()
{
    if (hasPendingSourceText() == true)
    {
        mStatus->setText(QStringLiteral("Apply or revert pending Source text before navigating."));
        return;
    }
    const QJsonObject node = selectedNode();
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
    {
        openSubgraph(node.value(QStringLiteral("asset")).toString(),
                     node.value(QStringLiteral("configuration")).toString());
        return;
    }
    const QStringList sources = GetSelectedShaderSources();
    if (sources.isEmpty() == false)
    {
        openSourceLine(sources.first(), 1);
    }
}

void RendererWorkspace::showNodeContextMenu(const QPoint& globalPosition)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("renderGraphNodeMenu"));
    const QJsonObject node = selectedNode();
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
    {
        const QString asset = node.value(QStringLiteral("asset")).toString();
        const QString configuration = node.value(QStringLiteral("configuration")).toString();
        QAction* open = menu.addAction(QStringLiteral("Open Subgraph"));
        open->setObjectName(QStringLiteral("renderGraphOpenSubgraph"));
        RenderGraphNodePorts ports;
        open->setEnabled(GetRenderGraphNodePorts(mDocument, node, ports) == true && hasPendingSourceText() == false);
        connect(open, &QAction::triggered, this, [this, asset, configuration]() {
            openSubgraph(asset, configuration);
        });
    }
    else if (node.isEmpty() == false)
    {
        QMenu* shaders = menu.addMenu(QStringLiteral("View Shaders"));
        shaders->setObjectName(QStringLiteral("renderGraphViewShaders"));
        const QStringList sources = GetSelectedShaderSources();
        shaders->setEnabled(sources.isEmpty() == false && hasPendingSourceText() == false);
        shaders->setToolTip(QStringLiteral("Shader sources used by this pass's authored technique; scene and resource "
                                           "nodes have no shaders."));
        for (const QString& source : sources)
        {
            QAction* open = shaders->addAction(source);
            connect(open, &QAction::triggered, this, [this, source]() { openSourceLine(source, 1); });
        }
    }
    QAction* editInterface = menu.addAction(QStringLiteral("Edit Inputs / Outputs..."));
    editInterface->setObjectName(QStringLiteral("renderGraphEditInterfaceAction"));
    editInterface->setEnabled(CanCreateSubgraph());
    connect(editInterface, &QAction::triggered, this, &RendererWorkspace::editConfigurationInterface);
    QMenu* createNodes = menu.addMenu(QStringLiteral("Create Node"));
    createNodes->setObjectName(QStringLiteral("renderGraphCreateNodeMenu"));
    createNodes->setEnabled(CanCreateSubgraph());
    for (int32 index = 0; index < mNewKind->count(); ++index)
    {
        const QString kind = mNewKind->itemText(index);
        QAction* createNode =
            createNodes->addAction(kind == QStringLiteral("Subgraph") ? QStringLiteral("Subgraph Reference") : kind);
        createNode->setData(kind);
        connect(createNode, &QAction::triggered, this, [this, index]() {
            mNewKind->setCurrentIndex(index);
            addNode();
        });
    }
    QAction* create = menu.addAction(QStringLiteral("Create Subgraph..."));
    create->setObjectName(QStringLiteral("renderGraphCreateSubgraphAction"));
    create->setEnabled(CanCreateSubgraph());
    connect(create, &QAction::triggered, this, [this]() { static_cast<void>(PromptCreateSubgraph()); });
    if (node.isEmpty() == false)
    {
        menu.addSeparator();
        QAction* remove = menu.addAction(QStringLiteral("Remove Node"));
        remove->setObjectName(QStringLiteral("renderGraphRemoveNodeAction"));
        remove->setEnabled(hasPendingSourceText() == false);
        connect(remove, &QAction::triggered, this, [this]() {
            const QString identity = mSelectedNode;
            const RenderGraphDocumentStatus status =
                mDocument.RemoveNode(mSelectedFile, mSelectedConfiguration, identity);
            if (status == RenderGraphDocumentStatus::Ok)
            {
                mSelectedNode.clear();
            }
            documentEdited(status);
        });
    }
    if (mCustomize->isVisible() == true)
    {
        menu.addAction(mCustomize);
    }
    menu.exec(globalPosition);
}

void RendererWorkspace::editConfigurationInterface()
{
    if (CanCreateSubgraph() == false)
    {
        mStatus->setText(QStringLiteral("Apply or revert pending Source text before editing configuration ports."));
        return;
    }
    const QString file = mCanvas->GetFile();
    const QString configuration = mCanvas->GetConfiguration();
    QJsonObject graph;
    if (mDocument.GetGraph(file, graph) == false)
    {
        return;
    }
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef value : configurations)
    {
        const QJsonObject record = value.toObject();
        if (record.value(QStringLiteral("id")).toString() != configuration)
        {
            continue;
        }
        const uint64 revision = mDocument.GetRevision();
        const uint64 epoch = mEpoch;
        const QString packageRoot = mDocument.GetPackageRoot();
        RenderGraphConfigurationEditor editor(mDocument, file, record, this);
        connect(
            &editor,
            &RenderGraphConfigurationEditor::ApplyRequested,
            this,
            [this, &editor, file, configuration, revision, epoch, packageRoot]() {
                if (mDocument.GetRevision() != revision || mEpoch != epoch || mDocument.GetPackageRoot() != packageRoot)
                {
                    editor.ShowError(QStringLiteral("The graph changed while this dialog was open. Cancel and "
                                                    "reopen it to edit the current interface."));
                    return;
                }
                QString error;
                const RenderGraphDocumentStatus status = mDocument.ReplaceConfigurationInterface(file,
                                                                                                 configuration,
                                                                                                 editor.GetInputs(),
                                                                                                 editor.GetOutputs(),
                                                                                                 error);
                if (status != RenderGraphDocumentStatus::Ok)
                {
                    editor.ShowError(error.isEmpty() == true ? DescribeStatus(status) : error);
                    return;
                }
                editor.accept();
            });
        if (editor.exec() == QDialog::Accepted && mDocument.GetRevision() != revision)
        {
            documentEdited(RenderGraphDocumentStatus::Ok);
        }
        return;
    }
}

void RendererWorkspace::addNode()
{
    const auto report = [this](const QString& message) {
        mNodeCreationMessage->setText(message);
        mNodeCreationMessage->show();
        mStatus->setText(message);
    };
    if (mDocument.IsLoaded() == false)
    {
        report(QStringLiteral("Cannot add a node: open or create a render graph first."));
        return;
    }
    // The displayed canvas owns the authoring context, including a selected file's default.
    const QString file = mCanvas->GetFile();
    const QString configuration = mCanvas->GetConfiguration();
    QJsonObject graph;
    if (mDocument.GetGraph(file, graph) == false)
    {
        report(QStringLiteral("Cannot add a node: %1 is not a valid graph JSON object. Fix it in Source.").arg(file));
        return;
    }
    QJsonObject record;
    const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (const QJsonValueConstRef value : configurations)
    {
        if (configuration.isEmpty() == false &&
            value.toObject().value(QStringLiteral("id")).toString() == configuration)
        {
            record = value.toObject();
            break;
        }
    }
    if (record.isEmpty() == true)
    {
        report(QStringLiteral("Cannot add a node: %1 has no valid configuration to display. "
                              "Fix its default or configurations in Source.")
                   .arg(file));
        return;
    }
    if (record.value(QStringLiteral("nodes")).isArray() == false)
    {
        report(QStringLiteral("Cannot add a node: %1#%2 must contain a nodes array. Fix it in Source.")
                   .arg(file, configuration));
        return;
    }
    QStringList identities;
    const QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef node : nodes)
    {
        identities.append(node.toObject().value(QStringLiteral("id")).toString());
    }
    const QString kind = mNewKind->currentText();
    const QString stem = kind.toLower();
    QString identity = stem;
    for (qsizetype suffix = 2; identities.contains(identity) == true; ++suffix)
    {
        identity = stem + QString::number(suffix);
    }
    const RenderGraphDocumentStatus status = mDocument.AddNode(file, configuration, MakeNode(kind, identity));
    if (status != RenderGraphDocumentStatus::Ok)
    {
        report(QStringLiteral("Cannot add %1 to %2#%3: %4").arg(kind, file, configuration, DescribeStatus(status)));
        return;
    }
    mSelectedFile = file;
    mSelectedConfiguration = configuration;
    mSelectedNode = identity;
    documentEdited(status);
    mViews->setCurrentIndex(CANVAS_VIEW);
    mCanvas->RevealNode(identity);
    report(QStringLiteral("Added %1 to %2#%3.").arg(identity, file, configuration));
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
    mNodeCreationMessage->clear();
    mNodeCreationMessage->hide();
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
    if (hasPendingSourceText() == true)
    {
        mStatus->setText(QStringLiteral("Apply or revert pending Source text before navigating."));
        return;
    }
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
    mViews->setCurrentIndex(CANVAS_VIEW);
    mStatus->setText(QStringLiteral("Showing subgraph %1#%2.").arg(asset, configuration));
}

void RendererWorkspace::SetPreviewSource(RenderPreview* sourceOrNull)
{
    if (sourceOrNull == mPreviewSourceOrNull)
    {
        return;
    }
    mPreviewTimer.stop();
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
            mPreviewTimer.stop();
            mPreviewOrNull->Detach();
            mPreviewDeviceSerial = 0;
        });
    }
#endif
    if (mPreviewSourceOrNull != nullptr)
    {
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        connect(mPreviewSourceOrNull, &RenderPreview::Changed, this, [this]() {
            if (mPreviewSourceOrNull->HasDevice() == true && mPreviewTimer.isActive() == false)
            {
                mPreviewTimer.start();
            }
        });
        if (mPreviewSourceOrNull->HasDevice() == true)
        {
            mPreviewTimer.start();
        }
#endif
        connect(mPreviewSourceOrNull, &QObject::destroyed, this, [this]() {
            mPreviewTimer.stop();
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
    if (loaded == RenderGraphPreviewLoad::Staged || loaded == RenderGraphPreviewLoad::Waiting)
    {
        mPreviewInputIdentity = getBuildIdentity();
        setProblem(mBuildProblem, {});
    }
    else
    {
        setProblem(mBuildProblem, mPreviewOrNull->GetDisplayStatus());
    }
    if (mPreviewSourceOrNull != nullptr && mPreviewSourceOrNull->HasDevice() == true)
    {
        mPreviewTimer.start();
    }
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
        if (mIsWorldRenderingEnabled == true)
        {
            mPreviewSourceOrNull->Resize(target.width(), target.height());
        }
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
            setProblem(mExtractionProblem, error);
            mPreviewSceneIdentity = scene.Identity;
        }
        if (hasView == true && (scene.Identity != mPreviewSceneIdentity ||
                                AreMatricesEqual(view.WorldToClip, mPreviewWorldToClip) == false))
        {
            RenderGraphExtraction extraction;
            if (MakeRenderGraphExtraction(scene, view, ++mPreviewRevision, extraction, error) == true)
            {
                setProblem(mExtractionProblem, {});
                preview.SetExtraction(extraction);
            }
            else
            {
                setProblem(mExtractionProblem, error);
                mPreviewStatus->setText(error);
            }
            mPreviewSceneIdentity = scene.Identity;
            mPreviewWorldToClip = view.WorldToClip;
        }
    }
    preview.Tick();
    if (mPreviewSourceOrNull == nullptr || mPreviewSourceOrNull->HasDevice() == false ||
        preview.GetTargetSize().isEmpty() == true)
    {
        mPreviewTimer.stop();
    }
#endif
}

void RendererWorkspace::renderPreview()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr)
    {
        const graphics::renderer::RenderGraphCandidateState candidate = mPreviewOrNull->GetReport().Candidate;
        if ((candidate == graphics::renderer::RenderGraphCandidateState::Failed ||
             candidate == graphics::renderer::RenderGraphCandidateState::RestartRequired) &&
            IsJobActive() == false && mHasPendingWorkerBuild == false && mBuildProblem.isEmpty() == true &&
            mPreviewInputIdentity == getBuildIdentity())
        {
            setProblem(mBuildProblem, mPreviewOrNull->GetDisplayStatus());
        }
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
