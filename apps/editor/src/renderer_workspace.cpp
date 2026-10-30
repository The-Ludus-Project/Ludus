#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_scene_bundle.h"
#include "internal/render_graph_settings_editor.h"
#include <ludus/content/render_graph_format/plan.h>

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_graph_worker.h"
#endif
#include "internal/code_document_controller.h"
#include "internal/code_editor.h"
#include "internal/code_editor_panel.h"
#include "internal/code_formatter.h"
#include "internal/item_selection.h"
#include "internal/render_graph_comments.h"
#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_execution_plan.h"
#include "internal/render_graph_history.h"
#include "internal/render_graph_options.h"
#include "internal/render_graph_pipeline.h"
#include "internal/render_graph_quick_add.h"
#include "internal/render_graph_quick_add_graphs.h"
#include "internal/render_graph_reroutes.h"
#include "internal/render_graph_resource_inspection.h"
#include "internal/render_graph_resources.h"
#include "internal/render_graph_shader_assistance.h"
#include "internal/render_graph_shader_creation.h"
#include "internal/render_graph_shader_interface.h"
#include "internal/render_graph_template_registry.h"
#include "internal/render_graph_upscale_node.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

#include "internal/render_preview.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include <ludus/foundation/profiling/profiling.hpp>

#    include "editor_default_graph.h"
#    include "editor_renderer_static.h"
#    include "internal/ray_path_inspection.h"
#    include "internal/render_graph_preview.h"
#    include "internal/texture_viewer.h"
#    include "renderer_composite.h"
#endif

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
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
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_EXTENT = static_cast<int32>(content::render_graph_format::MAX_TARGET_EXTENT);
constexpr int32 MAX_COORDINATE = RenderGraphDocument::MAX_COORDINATE;
constexpr int32 MINIMUM_CANVAS_WIDTH = 360;
// The source tree stays wide enough for nested node names.
constexpr int32 MINIMUM_SOURCE_TREE_WIDTH = 200;
constexpr int32 STATUS_VISIBLE_LINES = 1;
constexpr int32 STATUS_PADDING_PIXELS = 4;
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
const QString SOURCES_VISIBLE_KEY = QStringLiteral("renderGraph/sourcesVisible");
const QString SPLITTER_STATE_KEY = QStringLiteral("renderGraph/splitterState");

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
    if (field == QStringLiteral("algorithm"))
    {
        return GetUpscaleAlgorithms();
    }
    if (field == QStringLiteral("color_processing"))
    {
        return {QStringLiteral("Linear"), QStringLiteral("Perceptual")};
    }
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

/// Why this editor cannot run a Denoise node, or empty when it can. OIDN is an optional
/// native build feature; browser editors never have it.
QString GetDenoiseUnavailableMessage()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (graphics::renderer::IsDenoisingAvailable() == true)
    {
        return {};
    }
#endif
    return QStringLiteral(
        "This editor was set up without the optional OIDN denoiser, so the preview skips this node and "
        "shows unfiltered color. Games built with denoising still apply it. See \"Denoise ray output with "
        "OIDN\" in the render-graph authoring guide to add the denoiser.");
}

/// Choices for a Subgraph reference: every other draft graph for `asset`, and the
/// referenced graph's configurations for `configuration`. An authored value that is not a
/// current choice (a missing file or configuration) stays listed, so the inspector shows
/// it unchanged; the shared validator still owns legality, including cycles.
// File and field are distinct authored identities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QStringList GetNodeFieldChoices(const RenderGraphDocument& document,
                                const QString& file,
                                const QJsonObject& node,
                                const QString& field)
{
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Denoise") &&
        field == QStringLiteral("quality"))
    {
        return {QStringLiteral("Fast"), QStringLiteral("Balanced"), QStringLiteral("High")};
    }
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
    else if (kind == QStringLiteral("Denoise"))
    {
        node.insert(QStringLiteral("quality"), QStringLiteral("Balanced"));
    }
    else if (kind == QStringLiteral("ToneMap"))
    {
        node.insert(QStringLiteral("exposure"), 1);
    }
    else if (kind == QStringLiteral("Accumulate"))
    {
        node.insert(QStringLiteral("samples"),
                    static_cast<int32>(content::render_graph_format::MAX_ACCUMULATION_SAMPLES));
    }
    else if (kind == QStringLiteral("Buffer"))
    {
        node.insert(QStringLiteral("bytes"), static_cast<int32>(content::render_graph_format::RAY_BUFFER_WORD_BYTES));
    }
    else if (kind == QStringLiteral("StructuredBuffer"))
    {
        constexpr int32 DEFAULT_STORAGE_STRIDE = 16;
        node.insert(QStringLiteral("stride"), DEFAULT_STORAGE_STRIDE);
        node.insert(QStringLiteral("count"), QStringLiteral("ColorTarget"));
    }
    else if (kind == QStringLiteral("Target"))
    {
        node.insert(QStringLiteral("type"), QStringLiteral("ColorTexture"));
        node.insert(QStringLiteral("format"), QStringLiteral("RGBA8Unorm"));
        node.insert(QStringLiteral("extent"), QJsonArray{DEFAULT_RENDER_WIDTH, DEFAULT_RENDER_HEIGHT});
        node.insert(QStringLiteral("samples"), 1);
        node.insert(QStringLiteral("imported"), false);
        node.insert(QStringLiteral("initialized"), false);
        node.insert(QStringLiteral("presentation"), false);
    }
    else if (kind == QStringLiteral("RenderSize"))
    {
        node.insert(QStringLiteral("extent"), QStringLiteral("OutputSize"));
        node.insert(QStringLiteral("scale"), QJsonArray{1, 1});
    }
    else if (kind == QStringLiteral("OutputSize"))
    {
        node.insert(QStringLiteral("extent"), QStringLiteral("Viewport"));
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

/// Build a node record for a catalog node template or an existing package graph reference.
QJsonObject MakeTemplateNode(const RenderGraphTemplate& entry, const QString& identity)
{
    if (entry.Kind == RenderGraphTemplateKind::Graph)
    {
        return {{QStringLiteral("id"), identity},
                {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                {QStringLiteral("asset"), entry.Identity},
                {QStringLiteral("configuration"), entry.DefaultConfiguration}};
    }
    return entry.Algorithm.isEmpty() == true ? MakeNode(entry.Identity, identity)
                                             : MakeUpscaleNode(entry.Algorithm, identity);
}

/// First free node identity for a template in a configuration record: the preset's algorithm or the
/// lowercase kind (subgraph for graph references), then the same with 2, 3, ... appended.
QString MakeNodeIdentity(const RenderGraphTemplate& entry, const QJsonObject& configuration)
{
    QStringList identities;
    const QJsonArray nodes = configuration.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef node : nodes)
    {
        identities.append(node.toObject().value(QStringLiteral("id")).toString());
    }
    const QString stem = entry.Kind == RenderGraphTemplateKind::Graph ? QStringLiteral("subgraph")
                         : entry.Algorithm.isEmpty() == false         ? entry.Algorithm
                                                                      : entry.Identity.toLower();
    QString identity = stem;
    for (qsizetype suffix = 2; identities.contains(identity) == true; ++suffix)
    {
        identity = stem + QString::number(suffix);
    }
    return identity;
}

bool IsNodeFieldControl(const QWidget& workspace, const QWidget& inspector, QWidget* controlOrNull)
{
    if (controlOrNull == nullptr)
    {
        return false;
    }
    if (inspector.isAncestorOf(controlOrNull) == true)
    {
        return true;
    }
    for (QWidget* ancestorOrNull = controlOrNull; ancestorOrNull != nullptr;
         ancestorOrNull = ancestorOrNull->parentWidget())
    {
        if (qobject_cast<RenderGraphPipelineEditor*>(ancestorOrNull) != nullptr)
        {
            // QWidget::isAncestorOf stops at a window boundary; the canvas dialog
            // is a separate window owned by this workspace.
            for (QWidget* ownerOrNull = ancestorOrNull; ownerOrNull != nullptr;
                 ownerOrNull = ownerOrNull->parentWidget())
            {
                if (ownerOrNull == &workspace)
                {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
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
            return DescribePackageCapacity();
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
            if (location.contains(QStringLiteral("column")) == true)
            {
                source += QLatin1Char(':') + QString::number(location.value(QStringLiteral("column")).toInt());
            }
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

/// What disabling a node of this kind does in the compiled graph; Export is a sink, the others pass through.
QString DescribeDisabledNode(const QString& kind)
{
    if (kind == QStringLiteral("Export"))
    {
        return QStringLiteral("When disabled, the compiler builds no root for this Export: its input may stay "
                              "unconnected and nodes used only by it are skipped. Its wire is kept for re-enabling.");
    }
    return QStringLiteral("When disabled, the compiler connects this node's color input directly to its consumers. "
                          "Settings and wires are kept for re-enabling.");
}

QString DescribeSceneComputeConversion()
{
    return QStringLiteral("Make this ComputePipeline a scene compute pass that traces the World with native ray "
                          "queries: adds Scene and View inputs, automatic state and Match color target launch size "
                          "in one undoable edit. Save publishes the change.");
}
} // namespace

RendererWorkspace::RendererWorkspace(const ToolingPaths& tooling, QWidget* parentOrNull)
    : QWidget(parentOrNull), mTooling(tooling), mSourceDocument(mDocument),
      mPath(new QLabel(QStringLiteral("No render graph is open."), this)), mContent(new QStackedWidget(this)),
      mEmptyMessage(new QLabel(this)), mCreateFirst(new QPushButton(QStringLiteral("Create Render Graph..."), this)),
      mConfiguration(new QComboBox(this)), mTree(new QTreeWidget(this)), mNavigation(new QWidget(this)),
      mStatusPanel(new QScrollArea(this)), mSourcesToggle(new QPushButton(QStringLiteral("Graph Sources"), this)),
      mViews(new QTabWidget(this)), mInspectorPanel(new QScrollArea(this)), mInspector(new QWidget(mInspectorPanel)),
      mFields(new QFormLayout), mLayoutX(new QSpinBox(mInspector)), mLayoutY(new QSpinBox(mInspector)),
      mNewKind(new QComboBox(mInspector)), mNodeCreationMessage(new QLabel(mInspector)),
      mEdges(new QTableWidget(mInspector)), mEdgeFrom(new QLineEdit(mInspector)), mEdgeTo(new QLineEdit(mInspector)),
      mSourceFile(new QComboBox(this)), mSource(new CodeEditor(this)),
      mSourceController(new CodeDocumentController(*mSource, this)),
      mSourceFormatter(new CodeFormatter(*mSourceController, this)), mSourceInterface(new QPlainTextEdit(this)),
      mCustomize(new QAction(QStringLiteral("&Customize in Project"), this)), mStatus(new QLabel(this)),
      mCanvas(new RenderGraphCanvas(mDocument, this)),
      mQuickAdd(new RenderGraphQuickAdd(RenderGraphTemplateRegistry::GetBuiltIn(), this)),
      mPreviewPage(new QWidget(this)), mPreviewImage(new PreviewImage(mPreviewPage)),
      mPreviewStatus(new QLabel(mPreviewPage)),
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
    QHBoxLayout* header = new QHBoxLayout(mNavigation);
    header->setContentsMargins(0, 0, 0, 0);
    mSourcesToggle->setObjectName(QStringLiteral("renderGraphToggleSources"));
    mSourcesToggle->setCheckable(true);
    mSourcesToggle->setChecked(QSettings().value(SOURCES_VISIBLE_KEY, true).toBool());
    mSourcesToggle->setToolTip(
        QStringLiteral("Show or hide graph sources. View > Focus Viewport maximizes the graph."));
    header->addWidget(mSourcesToggle);
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
    mFields->setRowWrapPolicy(QFormLayout::WrapLongRows);
    mFields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    inspectorLayout->addLayout(mFields);
    mLayoutX->setObjectName(QStringLiteral("renderGraphLayoutX"));
    mLayoutY->setObjectName(QStringLiteral("renderGraphLayoutY"));
    mLayoutX->setRange(-MAX_COORDINATE, MAX_COORDINATE);
    mLayoutY->setRange(-MAX_COORDINATE, MAX_COORDINATE);
    QHBoxLayout* layoutRow = new QHBoxLayout;
    layoutRow->addWidget(mLayoutX);
    layoutRow->addWidget(mLayoutY);
    QFormLayout* positionForm = new QFormLayout;
    positionForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    QLabel* positionLabel = new QLabel(QStringLiteral("Layout x/y"), mInspector);
    positionLabel->setToolTip(QStringLiteral("Canvas position; presentation only, never part of the build."));
    positionForm->addRow(positionLabel, layoutRow);
    inspectorLayout->addLayout(positionForm);
    QPushButton* remove = new QPushButton(QStringLiteral("Remove Node"), mInspector);
    remove->setObjectName(QStringLiteral("renderGraphRemoveNode"));
    inspectorLayout->addWidget(remove, 0, Qt::AlignLeft);
    mNewKind->setObjectName(QStringLiteral("renderGraphNewKind"));
    mNewKind->setMinimumContentsLength(1);
    mNewKind->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const RenderGraphTemplate& entry :
         RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Node))
    {
        // Item data distinguishes Upscale presets, which carry their algorithm, from plain node kinds.
        if (entry.Algorithm.isEmpty() == true)
        {
            mNewKind->addItem(entry.Label);
        }
        else
        {
            mNewKind->addItem(entry.Label, entry.Algorithm);
        }
        // Graphs stay portable to OIDN builds, so Denoise remains authorable; this build says why it cannot
        // preview it.
        if (entry.Requirements.contains(QStringLiteral("denoiser")) == true)
        {
            mNewKind->setItemData(mNewKind->count() - 1, GetDenoiseUnavailableMessage(), Qt::ToolTipRole);
        }
    }
    mNewKind->setAccessibleName(QStringLiteral("New node type"));
    QPushButton* add = new QPushButton(QStringLiteral("Add Node"), mInspector);
    add->setObjectName(QStringLiteral("renderGraphAddNode"));
    QHBoxLayout* addRow = new QHBoxLayout;
    addRow->addWidget(mNewKind, 1);
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
    // A short label keeps both buttons within the narrowest docked Properties width.
    QPushButton* disconnectButton = new QPushButton(QStringLiteral("Disconnect"), mInspector);
    disconnectButton->setToolTip(QStringLiteral("Remove the connection selected in the table above."));
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
    mSource->setAccessibleName(QStringLiteral("Shader and render graph source"));
    mSourceFocusToggle = new QPushButton(QStringLiteral("Focus Source"), sourcePage);
    mSourceFocusToggle->setObjectName(QStringLiteral("renderGraphFocusSource"));
    mSourceFocusToggle->setToolTip(QStringLiteral("Hide surrounding panels; Shift+Alt+Return restores the layout."));
    connect(mSourceFocusToggle, &QPushButton::clicked, this, &RendererWorkspace::FocusViewRequested);
    mSourceInterfaceToggle = new QPushButton(QStringLiteral("Shader API"), sourcePage);
    mSourceInterfaceToggle->setObjectName(QStringLiteral("renderGraphShaderApiToggle"));
    mSourceInterfaceToggle->setCheckable(true);
    mSourceInterfaceToggle->setChecked(
        QSettings().value(QStringLiteral("renderGraph/shaderApiVisible"), false).toBool());
    QHBoxLayout* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(mSourceFile, 1);
    sourceRow->addWidget(mSourceInterfaceToggle);
    sourceRow->addWidget(mSourceFocusToggle);
    sourceLayout->addLayout(sourceRow);
    QPushButton* openCodeEditor = new QPushButton(QStringLiteral("Open in Code Editor"), sourcePage);
    QPushButton* chooseCodeEditor = new QPushButton(QStringLiteral("Choose Code Editor..."), sourcePage);
    QPushButton* reloadSources = new QPushButton(QStringLiteral("Reload Sources"), sourcePage);
    openCodeEditor->setObjectName(QStringLiteral("renderGraphOpenCodeEditor"));
    chooseCodeEditor->setObjectName(QStringLiteral("renderGraphChooseCodeEditor"));
    reloadSources->setObjectName(QStringLiteral("renderGraphReloadSources"));
    QToolButton* sourceActions = new QToolButton(sourcePage);
    sourceActions->setText(QStringLiteral("Source"));
    sourceActions->setToolButtonStyle(Qt::ToolButtonTextOnly);
    sourceActions->setObjectName(QStringLiteral("renderGraphSourceActions"));
    sourceActions->setPopupMode(QToolButton::InstantPopup);
    QMenu* sourceMenu = new QMenu(sourceActions);
    for (QPushButton* button : {openCodeEditor, chooseCodeEditor, reloadSources})
    {
        button->hide();
        QAction* action = sourceMenu->addAction(button->text());
        connect(action, &QAction::triggered, button, &QPushButton::click);
#if defined(Q_OS_WASM)
        action->setEnabled(button == reloadSources);
#endif
    }
    sourceActions->setMenu(sourceMenu);
    sourceRow->addWidget(sourceActions);
    mSourceStatus = new QLabel(sourcePage);
    mSourceStatus->setObjectName(QStringLiteral("renderGraphSourceStatus"));
    mSourceStatus->setTextFormat(Qt::PlainText);
    mSourceStatus->setWordWrap(true);
    mSourceStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sourceLayout->addWidget(mSourceStatus);
#if defined(Q_OS_WASM)
    openCodeEditor->setEnabled(false);
    chooseCodeEditor->setEnabled(false);
    openCodeEditor->setToolTip(QStringLiteral("External code editors are available in the native Ludus editor."));
#endif
    mSourceAssistance = new RenderGraphShaderAssistance(mDocument, *mSource, *mSourceController, mTooling, sourcePage);
    sourceLayout->addWidget(mSourceAssistance);
    QWidget* interfacePage = CreateShaderInterfaceView(mSourceInterface, sourcePage);
    mSourceSplit = new QSplitter(Qt::Horizontal, sourcePage);
    mSourceSplit->setObjectName(QStringLiteral("renderGraphSourceSplitter"));
    mSourceSplit->addWidget(new CodeEditorPanel(mSource, mSourceSplit));
    mSourceSplit->addWidget(interfacePage);
    mSourceSplit->setCollapsible(0, false);
    mSourceSplit->setStretchFactor(0, 2);
    mSourceSplit->setStretchFactor(1, 1);
    static_cast<void>(
        mSourceSplit->restoreState(QSettings().value(QStringLiteral("renderGraph/sourceSplit")).toByteArray()));
    sourceLayout->addWidget(mSourceSplit, 1);
    connect(mSourceSplit, &QSplitter::splitterMoved, this, [this]() {
        if (mIsChromeVisible == true)
        {
            const bool isVisible = mSourceSplit->sizes().value(1) > 0;
            const QSignalBlocker blocker(mSourceInterfaceToggle);
            mSourceInterfaceToggle->setChecked(isVisible);
            QSettings().setValue(QStringLiteral("renderGraph/shaderApiVisible"), isVisible);
        }
        if (mSourceSplit->sizes().value(1) > 0)
        {
            QSettings().setValue(QStringLiteral("renderGraph/sourceSplit"), mSourceSplit->saveState());
        }
    });
    connect(mSourceInterfaceToggle, &QPushButton::toggled, this, [this](bool isVisible) {
        QSettings().setValue(QStringLiteral("renderGraph/shaderApiVisible"), isVisible);
        updateSourceInterfaceVisibility();
    });

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
    mViews->insertTab(PLAN_VIEW, mCanvas->GetExecutionPlan().GetPage(), QStringLiteral("Plan"));
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // Textures: any target or attachment of the previewed generation (native GPU builds).
    mTextureViewerOrNull = new TextureViewer(this);
    mViews->insertTab(TEXTURES_VIEW, mTextureViewerOrNull, QStringLiteral("Textures"));
#endif

    mStatus->setObjectName(QStringLiteral("renderGraphStatus"));
    mStatus->setTextFormat(Qt::PlainText);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Compiler messages may contain the entire multiline report. Keep it readable
    // without letting its height displace the graph/source editing area.
    mStatusPanel->setObjectName(QStringLiteral("renderGraphStatusPanel"));
    mStatusPanel->setFrameShape(QFrame::NoFrame);
    mStatusPanel->setWidgetResizable(true);
    mStatusPanel->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mStatusPanel->setFixedHeight(mStatus->fontMetrics().lineSpacing() * STATUS_VISIBLE_LINES + STATUS_PADDING_PIXELS);
    mStatusPanel->setWidget(mStatus);
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
    static_cast<void>(horizontal->restoreState(QSettings().value(SPLITTER_STATE_KEY).toByteArray()));
    mTree->setVisible(mSourcesToggle->isChecked());
    connect(mSourcesToggle, &QPushButton::toggled, this, [this, horizontal](bool isVisible) {
        if (isVisible == false)
        {
            QSettings().setValue(SPLITTER_STATE_KEY, horizontal->saveState());
        }
        mTree->setVisible(isVisible);
        if (isVisible == true)
        {
            static_cast<void>(horizontal->restoreState(QSettings().value(SPLITTER_STATE_KEY).toByteArray()));
            if (horizontal->sizes().value(SOURCE_TREE_COLUMN) == 0)
            {
                horizontal->setSizes({MINIMUM_SOURCE_TREE_WIDTH, horizontal->width() - MINIMUM_SOURCE_TREE_WIDTH});
            }
        }
        QSettings().setValue(SOURCES_VISIBLE_KEY, isVisible);
    });
    connect(horizontal, &QSplitter::splitterMoved, this, [this, horizontal]() {
        const bool isVisible = horizontal->sizes().value(SOURCE_TREE_COLUMN) > 0;
        const QSignalBlocker blocker(mSourcesToggle);
        mSourcesToggle->setChecked(isVisible);
        QSettings().setValue(SOURCES_VISIBLE_KEY, isVisible);
        if (isVisible == true)
        {
            QSettings().setValue(SPLITTER_STATE_KEY, horizontal->saveState());
        }
    });
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
    layout->addWidget(mNavigation);
    layout->addWidget(mContent, 1);
    layout->addWidget(mStatusPanel);

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
            mTree->setCurrentItem(itemOrNull,
                                  0,
                                  itemOrNull->isSelected() == true ? QItemSelectionModel::NoUpdate
                                                                   : QItemSelectionModel::ClearAndSelect);
        }
        else
        {
            mSelectedNode.clear();
            mSelectedNodes.clear();
        }
        showNodeContextMenu(mTree->viewport()->mapToGlobal(position), mSelectedNodes.isEmpty() == false);
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
        static_cast<void>(buildAndPreview(RenderGraphSource::Saved));
    });
    connect(mTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (mIsRendering == true || current == nullptr)
        {
            return;
        }
        mSelectedFile = current->data(0, FILE_ROLE).toString();
        mSelectedConfiguration = current->data(0, CONFIGURATION_ROLE).toString();
        mSelectedNode = current->data(0, NODE_ROLE).toString();
        syncTreeSelection();
    });
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this]() { activateSelectedNode(); });
    setupResourceInspection();
    connect(mCanvas, &RenderGraphCanvas::InterfaceEditRequested, this, &RendererWorkspace::editConfigurationInterface);
    connect(mCanvas, &RenderGraphCanvas::NodeDetailsRequested, this, [this](const QString& node) {
        static_cast<void>(SelectNode(mCanvas->GetFile(), mCanvas->GetConfiguration(), node));
        editNodeOnCanvas();
    });
    connect(mCanvas, &RenderGraphCanvas::NodeSelected, this, [this](const QString& node) {
        mSelectedFile = mCanvas->GetFile();
        mSelectedConfiguration = mCanvas->GetConfiguration();
        mSelectedNode = node;
        renderInspector();
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        // A node that writes or publishes textures shows them in the texture viewer.
        QJsonObject graph;
        if (mTextureViewerOrNull != nullptr && mResourceInspectorOrNull->IsPinned() == false &&
            mDocument.GetGraph(mSelectedFile, graph) == true)
        {
            static_cast<void>(
                mTextureViewerOrNull->SelectCanvasNode(graph.value(QStringLiteral("id")).toString(), node));
        }
#endif
    });
    // Quick Add lists the catalog's snippets and nodes; ports are computed once from default node records.
    QHash<QString, RenderGraphNodePorts> quickAddPorts;
    for (const RenderGraphTemplate& entry :
         RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Node))
    {
        const QJsonObject node = MakeTemplateNode(entry, QStringLiteral("quickadd"));
        RenderGraphNodePorts ports;
        if (GetRenderGraphNodePorts(mDocument, node, ports) == true)
        {
            quickAddPorts.insert(GetRenderGraphTemplateKey(entry), ports);
        }
    }
    mQuickAdd->SetNodePorts(quickAddPorts);
    mQuickAdd->SetUnavailableRequirements({{QStringLiteral("denoiser"), GetDenoiseUnavailableMessage()}});
    mQuickAdd->SetRecent(settings.value(QStringLiteral("renderGraph/quickAddRecent")).toStringList());
    mQuickAdd->SetPinned(settings.value(QStringLiteral("renderGraph/quickAddPinned")).toStringList());
    connect(mQuickAdd, &RenderGraphQuickAdd::HistoryChanged, this, [this]() {
        QSettings settings;
        settings.setValue(QStringLiteral("renderGraph/quickAddRecent"), mQuickAdd->GetRecent());
        settings.setValue(QStringLiteral("renderGraph/quickAddPinned"), mQuickAdd->GetPinned());
    });
    connect(mQuickAdd, &RenderGraphQuickAdd::InsertRequested, this, &RendererWorkspace::insertQuickAddTemplate);
    connect(mQuickAdd, &RenderGraphQuickAdd::NewGraphRequested, this, [this](const QString& templateIdentity) {
        static_cast<void>(PromptNewGraph(templateIdentity));
    });
    connect(mCanvas, &RenderGraphCanvas::QuickAddRequested, this, [this](QPointF scenePosition) {
        static_cast<void>(ShowQuickAdd(scenePosition));
    });
    connect(mCanvas,
            &RenderGraphCanvas::ConnectQuickAddRequested,
            this,
            [this](const QString& from, QPointF inputPosition) {
                static_cast<void>(ShowConnectQuickAdd(from, inputPosition));
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
    connect(mCanvas,
            &RenderGraphCanvas::ReplaceConnectionRequested,
            this,
            [this](const QString& from, const QString& to) {
                documentEdited(mDocument.ReplaceConnection(mCanvas->GetFile(), mCanvas->GetConfiguration(), from, to));
            });
    connect(mCanvas, &RenderGraphCanvas::DisconnectRequested, this, [this](const QString& from, const QString& to) {
        documentEdited(mDocument.Disconnect(mCanvas->GetFile(), mCanvas->GetConfiguration(), from, to));
    });
    connect(mCanvas, &RenderGraphCanvas::RemoveRequested, this, [this](const QString& node) {
        if (node == mSelectedNode)
        {
            mSelectedNode.clear();
            mSelectedNodes.clear();
        }
        documentEdited(mDocument.RemoveNode(mCanvas->GetFile(), mCanvas->GetConfiguration(), node));
    });
    connect(mCanvas, &RenderGraphCanvas::LinkRejected, this, [this](const QString& reason) {
        mStatus->setText(QStringLiteral("Link rejected - %1").arg(reason));
    });
    connect(mCanvas, &RenderGraphCanvas::LayoutRejected, this, [this](const QString& reason) {
        mStatus->setText(QStringLiteral("Layout rejected - %1").arg(reason));
    });
    connect(&mCanvas->GetReroutes(), &RenderGraphReroutes::EditRequested, this, [this](const QJsonArray& routes) {
        const RenderGraphDocumentStatus status =
            mDocument.ReplaceWireRoutes(mCanvas->GetFile(), mCanvas->GetConfiguration(), routes);
        if (status != RenderGraphDocumentStatus::Ok)
        {
            mCanvas->Rebuild();
        }
        documentEdited(status);
    });
    connect(&mCanvas->GetReroutes(), &RenderGraphReroutes::Rejected, this, [this](const QString& reason) {
        mStatus->setText(QStringLiteral("Wire reroutes - %1").arg(reason));
    });
    connect(&mCanvas->GetComments(),
            &RenderGraphComments::EditRequested,
            this,
            [this](const QJsonArray& frames, const QVector<RenderGraphNodePosition>& positions) {
                const RenderGraphDocumentStatus status =
                    mDocument.ReplaceCommentFrames(mCanvas->GetFile(), mCanvas->GetConfiguration(), frames, positions);
                if (status != RenderGraphDocumentStatus::Ok)
                {
                    mCanvas->Rebuild();
                }
                documentEdited(status);
            });
    connect(&mCanvas->GetComments(), &RenderGraphComments::Rejected, this, [this](const QString& reason) {
        mStatus->setText(QStringLiteral("Comment frame - %1").arg(reason));
    });
    connect(&mCanvas->GetDiagnostics(),
            &RenderGraphDiagnostics::RefreshRequested,
            this,
            &RendererWorkspace::updateCanvasDiagnostics);
    connect(&mCanvas->GetDiagnostics(), &RenderGraphDiagnostics::DiagnosticRequested, this, [this](int32 row) {
        static_cast<void>(ActivateDiagnostic(row));
    });
    connect(mCanvas, &RenderGraphCanvas::SubgraphActivated, this, &RendererWorkspace::openSubgraph);
    connect(&mCanvas->GetHistory(),
            &RenderGraphHistory::LocationRequested,
            this,
            [this](const QString& file, const QString& configuration, const QStringList& selection) {
                mSelectedFile = file;
                mSelectedConfiguration = configuration;
                mSelectedNodes = selection;
                mSelectedNode = selection.isEmpty() == true ? QString() : selection.first();
                renderTree();
                mSourceFile->setCurrentText(file);
                renderSource();
                mViews->setCurrentWidget(mCanvas);
            });
    connect(&mCanvas->GetHistory(), &RenderGraphHistory::NavigationFailed, mStatus, &QLabel::setText);
    connect(mCanvas,
            &RenderGraphCanvas::ContextMenuRequested,
            this,
            [this](const QString& node, const QPoint& globalPosition) {
                mSelectedFile = mCanvas->GetFile();
                mSelectedConfiguration = mCanvas->GetConfiguration();
                if (node.isEmpty() == false)
                {
                    mSelectedNode = node;
                }
                showNodeContextMenu(globalPosition, node.isEmpty() == false);
            });
    setupGraphSelection();
    connect(mPreviewButton, &QPushButton::clicked, this, [this]() { static_cast<void>(PreviewRuntime()); });
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mPreviewOrNull = new RenderGraphPreview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment(),
        this);
    connect(mPreviewOrNull, &RenderGraphPreview::Changed, this, &RendererWorkspace::renderPreview);
    connect(mPreviewOrNull, &RenderGraphPreview::PreparingSubmission, this, [this]() {
        if (mPreviewSourceOrNull != nullptr)
        {
            mPreviewSourceOrNull->PrepareGpuProfileFrame();
        }
    });
    connect(mPreviewOrNull, &RenderGraphPreview::SubmissionRecorded, this, [this]() {
        if (mPreviewSourceOrNull != nullptr)
        {
            mPreviewSourceOrNull->NotifyGpuProfileSubmission();
        }
    });
    connect(mPreviewOrNull, &RenderGraphPreview::FramePresented, this, [this]() {
        if (mPreviewSourceOrNull != nullptr)
        {
            mPreviewSourceOrNull->PollGpuProfiling();
        }
        renderPreview();
    });
    mTextureViewerOrNull->SetPreview(mPreviewOrNull);
    connect(mPreviewOrNull, &RenderGraphPreview::TargetsChanged, this, &RendererWorkspace::updateResourceInspection);
    connect(mPreviewOrNull, &RenderGraphPreview::FramePresented, this, &RendererWorkspace::updateResourceInspection);
    connect(mPreviewOrNull, &RenderGraphPreview::Changed, this, &RendererWorkspace::updateResourceInspection);
    mRayPathsOrNull = new RayPathInspection(this);
    mRayPathsOrNull->SetPreview(mPreviewOrNull);
    connect(mRayPathsOrNull, &RayPathInspection::BuildRequested, this, &RendererWorkspace::startRayPathBuild);
    connect(mPreviewOrNull, &RenderGraphPreview::RayPathCaptureChanged, &mPreviewPump, &ViewportFramePump::Wake);
    connect(&mPreviewPump, &ViewportFramePump::WorkRequested, this, &RendererWorkspace::ServicePreview);
    connect(mPreviewOrNull, &RenderGraphPreview::FramePresented, &mPreviewPump, &ViewportFramePump::Wake);
    connect(mPreviewOrNull, &RenderGraphPreview::Changed, &mPreviewPump, &ViewportFramePump::Wake);
    connect(&mPreviewOrNull->GetDisplayFrames(),
            &GraphDisplayFrames::CapacityAvailable,
            &mPreviewPump,
            &ViewportFramePump::Wake);
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
    connect(remove, &QPushButton::clicked, this, &RendererWorkspace::RemoveSelectedNodes);
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
        mSourceController->BreakUndoGroup();
        renderSource();
    });
    connect(mSource, &CodeEditor::UndoRequested, this, &RendererWorkspace::Undo);
    connect(mSource, &CodeEditor::RedoRequested, this, &RendererWorkspace::Redo);
    connect(mSourceController, &CodeDocumentController::Failed, mStatus, &QLabel::setText);
    connect(mSource, &CodeEditor::FormatRequested, this, [this]() {
        const QString sourcePath = QDir(mDocument.GetPackageRoot()).filePath(mSourceFile->currentText());
        const bool hasStarted = mSourceFormatter->Start(MakeShaderFormatter(FindShaderFormatter(), sourcePath));
        mStatus->setText(mSourceFormatter->GetMessage());
        if (hasStarted == true)
        {
            mSource->SetCanFormat(false);
        }
    });
    connect(mSourceFormatter, &CodeFormatter::Finished, this, [this](bool, const QString& message) {
        mStatus->setText(message);
        mSource->SetCanFormat(CodeFormatter::IsAvailable() == true && mSource->GetLanguage() == CodeLanguage::Slang);
    });
    connect(mSource, &CodeEditor::ChooseFormatterRequested, this, [this]() {
        const QString program =
            QFileDialog::getOpenFileName(this, QStringLiteral("Choose clang-format executable"), FindShaderFormatter());
        if (program.isEmpty() == false)
        {
            QSettings().setValue(QStringLiteral("codeEditor/clangFormat"), program);
        }
    });
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
    connect(mSourceController, &CodeDocumentController::TextAccepted, this, [this]() {
        // History is immediate. Expensive tree/interface/watch refresh stays coalesced.
        mSourceEditDebounce.start();
        mSourceStatus->setText(QStringLiteral("Draft r%1 · Unsaved · Save publishes files. Preview retained.")
                                   .arg(mDocument.GetRevision()));
        Q_EMIT CommandsChanged();
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
                static_cast<void>(buildAndPreview(mSourceAfterWorker));
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
        updateCanvasDiagnostics();
    });
    // Save must be available while a table cell or property text editor still owns focus.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget* previousOrNull, QWidget* currentOrNull) {
        if (IsNodeFieldControl(*this, *mInspector, previousOrNull) == true ||
            IsNodeFieldControl(*this, *mInspector, currentOrNull) == true)
        {
            Q_EMIT DocumentChanged();
        }
    });
    renderStatus();
    renderPreview();
}

void RendererWorkspace::SetChromeVisible(bool isVisible)
{
    if (isVisible == mIsChromeVisible)
    {
        return;
    }
    mIsChromeVisible = isVisible;
    if (isVisible == false)
    {
        mFocusRestoreView = mViews->currentIndex();
        if (mFocusRestoreView != SOURCE_VIEW)
        {
            mViews->setCurrentWidget(mCanvas);
        }
    }
    else
    {
        mViews->setCurrentIndex(mFocusRestoreView);
    }
    mSourceFocusToggle->setText(isVisible == true ? QStringLiteral("Focus Source") : QStringLiteral("Restore Layout"));
    updateSourceInterfaceVisibility();
    mNavigation->setVisible(isVisible);
    mStatusPanel->setVisible(isVisible);
    mTree->setVisible(isVisible == true && mSourcesToggle->isChecked() == true);
}

void RendererWorkspace::FocusActiveView()
{
    if (mViews->currentIndex() == SOURCE_VIEW)
    {
        mSource->setFocus(Qt::ShortcutFocusReason);
    }
    else
    {
        mCanvas->setFocus(Qt::ShortcutFocusReason);
    }
}

RendererWorkspace::~RendererWorkspace()
{
    delete mResourceInspectorOrNull;
    mResourceInspectorOrNull = nullptr;
    delete mSourceAssistance;
    delete mSourceFormatter;
    delete mSourceController;
}

void RendererWorkspace::SetProject(const QString& root, uint64 epoch)
{
    // Package and project roots share one canonical identity, including symlinked projects.
    const QString canonicalRoot = root.isEmpty() == true ? QString() : QFileInfo(root).canonicalFilePath();
    if (canonicalRoot != mProjectRoot || epoch != mEpoch)
    {
        mCreation.Cancel();
        mSourceFormatter->Cancel();
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
            renderSource();
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
    mSourceDocument.ResetBinding();
    mResourceInspectorOrNull->Reset();
    mCanvas->GetHistory().Reset();
    mDocument.Reset();
    mBuildProblem.clear();
    mSourceProblem.clear();
    mExtractionProblem.clear();
    mActivationProblem.clear();
    mSelectedFile.clear();
    mSelectedConfiguration.clear();
    mSelectedNode.clear();
    mSelectedNodes.clear();
    mLastResult = {};
    mHasResult = false;
    mIsValidated = false;
    mCanvas->GetExecutionPlan().Reset();
    mPath->setText(QStringLiteral("No render graph is open."));
    renderTree();
    renderSource();
    renderStatus();
    prepareDefaultPreview();
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
        // Keep the authored graph and its diagnostics; the viewport labels its fallback separately.
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

bool RendererWorkspace::PromptNewGraph(const QString& templateIdentity)
{
    if (CanCreateGraph() == false || ConfirmDiscard() == false)
    {
        return false;
    }
    const uint64 epoch = mEpoch;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("newRenderGraphDialog"));
    dialog.setWindowTitle(QStringLiteral("New Render Graph"));
    QFormLayout* fields = new QFormLayout(&dialog);
    QLabel* destination = new QLabel(QStringLiteral("Creates editable graph and shader sources in:\n%1/<name>/")
                                         .arg(QDir(mProjectRoot).filePath(QStringLiteral("graphs"))),
                                     &dialog);
    destination->setObjectName(QStringLiteral("newRenderGraphDestination"));
    fields->addRow(destination);
    QLineEdit* name = new QLineEdit(QStringLiteral("Main"), &dialog);
    name->setObjectName(QStringLiteral("newRenderGraphName"));
    fields->addRow(QStringLiteral("Name"), name);
    QComboBox* graphTemplate = new QComboBox(&dialog);
    graphTemplate->setObjectName(QStringLiteral("newRenderGraphTemplate"));
    for (const RenderGraphTemplate& entry :
         RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Graph))
    {
        graphTemplate->addItem(entry.Label, entry.Identity);
        graphTemplate->setItemData(graphTemplate->count() - 1, entry.Description, Qt::ToolTipRole);
    }
    if (templateIdentity.isEmpty() == false && graphTemplate->findData(templateIdentity) >= 0)
    {
        graphTemplate->setCurrentIndex(graphTemplate->findData(templateIdentity));
    }
    fields->addRow(QStringLiteral("Template"), graphTemplate);
    QLabel* description = new QLabel(&dialog);
    description->setWordWrap(true);
    fields->addRow(description);
    const auto describe = [description, graphTemplate, &dialog]() {
        description->setText(graphTemplate->currentData(Qt::ToolTipRole).toString());
        dialog.adjustSize();
    };
    describe();
    connect(graphTemplate, &QComboBox::currentIndexChanged, &dialog, describe);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    fields->addRow(buttons);
    return dialog.exec() == QDialog::Accepted && epoch == mEpoch &&
           CreateGraph(name->text().trimmed(), graphTemplate->currentData().toString()) == true;
}

bool RendererWorkspace::CreateGraph(const QString& name, const QString& templateIdentity)
{
    if (IsJobActive() == true)
    {
        return false;
    }
    QString error;
    if (mCreation.Start(mTooling, mProjectRoot, name, mEpoch, error, templateIdentity) == false)
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
    mSourceDocument.ResetBinding();
    mCanvas->ShowConfiguration({}, {});
    mResourceInspectorOrNull->Reset();
    mDocument = document;
    mBuildProblem.clear();
    mSourceProblem.clear();
    mExtractionProblem.clear();
    mActivationProblem.clear();
    mPreviewSceneIdentity.clear();
    mArePreviewInputsDirty = true;
    prepareDefaultPreview();
    mSelectedFile = mDocument.GetRootGraph();
    mSelectedConfiguration.clear();
    mSelectedNode.clear();
    mSelectedNodes.clear();
    mLastResult = {};
    mHasResult = false;
    mIsValidated = false;
    mCanvas->GetExecutionPlan().Reset();
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
    return mDocument.IsLoaded() == true &&
           (mDocument.IsDirty() == true ||
            IsNodeFieldControl(*this, *mInspector, QApplication::focusWidget()) == true ||
            mSourceEditDebounce.isActive() == true);
}

bool RendererWorkspace::Save()
{
    // Keyboard Save does not move focus. Finish the active delegate/property edit first.
    QWidget* focusedControlOrNull = QApplication::focusWidget();
    if (IsNodeFieldControl(*this, *mInspector, focusedControlOrNull) == true)
    {
        focusedControlOrNull->clearFocus();
    }
    mSourceController->BreakUndoGroup();
    if (isUneditedBuiltIn() == true)
    {
        // The unedited engine default is not project content; Save never copies it.
        return true;
    }
    return writeDocument();
}

bool RendererWorkspace::OpenSourceInCodeEditor()
{
    if (mDocument.IsLoaded() == false || IsJobActive() == true)
    {
        return false;
    }
    const QString file = mSourceFile->currentText();
    if (mDocument.IsBuiltIn() == true || mDocument.IsDirty() == true)
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
    if (IsJobActive() == true)
    {
        mStatus->setText(QStringLiteral("Finish the current job before reloading sources."));
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
    mSourceController->BreakUndoGroup();
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
    scheduleLiveBuild();
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
        restoreSourceHistory();
    }
}

void RendererWorkspace::Redo()
{
    if (mDocument.Redo() == true)
    {
        restoreSourceHistory();
    }
}

void RendererWorkspace::restoreSourceHistory()
{
    mSourceEditDebounce.stop();
    mSourceController->BreakUndoGroup();
    const RenderGraphNodeSelection selection = mDocument.GetHistoryNodeSelection();
    if (selection.File.isEmpty() == false)
    {
        mSelectedFile = selection.File;
        mSelectedConfiguration = selection.Configuration;
        mSelectedNode = selection.Node;
        mSelectedNodes = selection.Node.isEmpty() == true ? QStringList{} : QStringList{selection.Node};
    }
    const RenderGraphTextState state = mDocument.GetHistoryTextState();
    if (state.File.isEmpty() == false)
    {
        const QSignalBlocker blocker(mSourceFile);
        mSourceFile->setCurrentText(state.File);
    }
    documentEdited(RenderGraphDocumentStatus::Ok);
    if (state.File.isEmpty() == false)
    {
        mSourceController->RestoreSelection(state.Selection);
        mViews->setCurrentIndex(SOURCE_VIEW);
        mSource->setFocus();
    }
}

bool RendererWorkspace::startJob(const QString& action,
                                 const QStringList& arguments,
                                 bool shouldCaptureSnapshot,
                                 bool shouldPreview,
                                 RenderGraphSource source)
{
    if (mDocument.IsLoaded() == false || mProjectRoot.isEmpty() == true || IsJobActive() == true)
    {
        return false;
    }
    if (shouldCaptureSnapshot == true && source == RenderGraphSource::Draft && ReloadSourcesFromDisk() == false)
    {
        return false;
    }
    RenderGraphJobRequest request;
    request.Request = ++mNextRequest;
    request.ProjectEpoch = mEpoch;
    request.Revision = mDocument.GetRevision(source);
    request.Action = action;
    request.ShouldPreview = shouldPreview;
    request.Source = source;
    request.InputIdentity = getBuildIdentity(source);
    if (shouldCaptureSnapshot == true)
    {
        // Jobs read an immutable source snapshot, never the live package or widgets.
        request.SnapshotDirectory =
            QDir(mProjectRoot)
                .filePath(QStringLiteral("out/render-graph/drafts/%1-%2").arg(mEpoch).arg(request.Request));
        QDir(request.SnapshotDirectory).removeRecursively();
        if (QDir().mkpath(request.SnapshotDirectory) == false ||
            mDocument.WriteSnapshot(request.SnapshotDirectory, source) != RenderGraphDocumentStatus::Ok)
        {
            QDir(request.SnapshotDirectory).removeRecursively();
            mStatus->setText(QStringLiteral("The source snapshot could not be written under out/render-graph."));
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
        QStringLiteral("Running render-graph %1 on %2 revision %3...")
            .arg(action, source == RenderGraphSource::Saved ? QStringLiteral("saved") : QStringLiteral("draft"))
            .arg(request.Revision));
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
    return buildAndPreview(RenderGraphSource::Draft);
}

bool RendererWorkspace::buildAndPreview(RenderGraphSource source)
{
    prepareDefaultPreview();
    // The unedited default already has a generation built with the editor: no tools needed.
    if (mDocument.IsBuiltIn() == true && (source == RenderGraphSource::Saved || mDocument.IsDirty() == false) &&
        GetDefaultRenderGraphRuntime().isEmpty() == false && mPreviewOrNull != nullptr)
    {
        // Explicit draft previews also acknowledge the saved version, so the automatic
        // lane does not immediately replace them with an older saved build.
        mLiveBuild.MarkRequested(getBuildIdentity(RenderGraphSource::Saved));
        return PreviewRuntime(source);
    }
    if (startBuild(true, source) == false)
    {
        if (mHasPendingWorkerBuild == false)
        {
            setProblem(mBuildProblem, mSourceProblem.isEmpty() == true ? mStatus->text() : QString());
        }
        return false;
    }
    mLiveBuild.MarkRequested(getBuildIdentity(RenderGraphSource::Saved));
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
    if (mActivationProblem.isEmpty() == false)
    {
        return mActivationProblem;
    }
    return mExtractionProblem.isEmpty() == false ? mExtractionProblem : mBuildProblem;
}
QString RendererWorkspace::GetScenePreparationProblem() const
{
    if (mExtractionProblem.isEmpty() == false)
    {
        return mExtractionProblem;
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsWorldRenderingEnabled == true && mPreviewOrNull != nullptr)
    {
        const graphics::renderer::RenderGraphActivationReport report = mPreviewOrNull->GetReport();
        if (report.ActiveGeneration != 0)
        {
            return {};
        }
        if (mIsDefaultPreview == true)
        {
            if (report.Candidate == graphics::renderer::RenderGraphCandidateState::Failed ||
                report.Candidate == graphics::renderer::RenderGraphCandidateState::RestartRequired)
            {
                return mPreviewOrNull->GetDisplayStatus();
            }
            // Detach keeps the accepted package request; Attach reloads it on the next device.
            if (mPreviewOrNull->IsAttached() == false || mPreviewOrNull->GetTargetSize().isEmpty() == false)
            {
                return {};
            }
        }
    }
#endif
    return GetRenderProblem();
}
void RendererWorkspace::prepareDefaultPreview()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsWorldRenderingEnabled == false || mProjectRoot.isEmpty() == true || mPreviewOrNull == nullptr ||
        mPreviewOrNull->GetTargetSize().isEmpty() == false)
    {
        return;
    }
    const QString runtime = GetDefaultRenderGraphRuntime();
    if (runtime.isEmpty() == true)
    {
        return;
    }
    // Bootstrap the producer without changing the authored document or invoking shader tools.
    // A failed project build leaves this generation rendering the latest World extraction.
    mIsDefaultPreview = true;
    mPreviewInputIdentity.clear();
    const RenderGraphPreviewLoad loaded = mPreviewOrNull->Load(runtime, GetDefaultRenderGraphRuntime());
    if (loaded != RenderGraphPreviewLoad::Waiting && loaded != RenderGraphPreviewLoad::Staged)
    {
        mIsDefaultPreview = false;
        setProblem(mBuildProblem, mPreviewOrNull->GetDisplayStatus());
    }
#endif
}
void RendererWorkspace::setProblem(QString& inoutProblem, const QString& text)
{
    if (inoutProblem != text)
    {
        inoutProblem = text;
        renderStatus();
        Q_EMIT RenderProblemChanged();
    }
}
void RendererWorkspace::applySourceEdit()
{
    mSourceEditDebounce.stop();
    documentEdited(RenderGraphDocumentStatus::Ok);
}
void RendererWorkspace::EnableWorldRendering()
{
    mIsWorldRenderingEnabled = true;
    prepareDefaultPreview();
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
    if (mSourceEditDebounce.isActive() == true)
    {
        // Flush only the coalesced presentation refresh; text already belongs to the draft.
        applySourceEdit();
    }
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
            scheduleLiveBuild(true);
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

bool RendererWorkspace::startBuild(bool shouldPreview, RenderGraphSource source)
{
    if (source == RenderGraphSource::Draft)
    {
        if (ReloadSourcesFromDisk() == false)
        {
            return false;
        }
        const foundation::uint64 revision = mDocument.GetRevision();
        QString error;
        if (mDocument.PrepareTechniques(error) != RenderGraphDocumentStatus::Ok)
        {
            mStatus->setText(error);
            return false;
        }
        if (mDocument.GetRevision() != revision)
        {
            documentEdited(RenderGraphDocumentStatus::Ok);
        }
    }
    if (mDocument.GetFiles(source).contains(manifestFile()) == false)
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
    appendBuildToolArguments(arguments);
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
            mSourceAfterWorker = source;
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
    return startJob(QStringLiteral("build"), arguments, true, shouldPreview, source);
}

void RendererWorkspace::appendBuildToolArguments(QStringList& inoutArguments) const
{
    const bool shouldOverride = mBuildSettings.ShouldOverrideTools;
    const RenderGraphBuildTools& tools = mBuildSettings.Tools;
    if (shouldOverride == true)
    {
        inoutArguments += {QStringLiteral("--helper"), tools.Helper, QStringLiteral("--compiler"), tools.Compiler};
    }
    else
    {
        inoutArguments +=
            {QStringLiteral("--project"), mProjectRoot, QStringLiteral("--tooling-root"), mTooling.ToolingRoot};
    }
    if (shouldOverride == true && tools.Validator.isEmpty() == false)
    {
        inoutArguments += {QStringLiteral("--validator"), tools.Validator};
    }
    if (shouldOverride == true && tools.SpirvCross.isEmpty() == false)
    {
        inoutArguments += {QStringLiteral("--spirv-cross"), tools.SpirvCross};
    }
}

bool RendererWorkspace::BeginRayPathCapture(const QString& node)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mRayPathsOrNull == nullptr)
    {
        return false;
    }
    QJsonObject graph;
    static_cast<void>(mDocument.GetGraph(mCanvas->GetFile(), graph));
    const RayPathTarget target{graph.value(QStringLiteral("id")).toString(), node};
    if (mRayPathsOrNull->Begin(target) == false)
    {
        mStatus->setText(mRayPathsOrNull->GetStatus());
        return false;
    }
    mStatus->setText(mRayPathsOrNull->GetStatus());
    return true;
#else
    static_cast<void>(node);
    return false;
#endif
}

void RendererWorkspace::startRayPathBuild()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    const RayPathFreeze& freeze = mPreviewOrNull->GetRayPathFreeze();
    const QString build = buildDirectory();
    const QString generation = build.isEmpty() == true
                                   ? QString{}
                                   : QDir(build).filePath(QStringLiteral("generations/") +
                                                          QString::fromLatin1(freeze.GenerationIdentity.toHex()));
    // The capture variant is compiled from the drawing generation's own saved sources, never newer edits.
    if (generation.isEmpty() == true || QFileInfo(generation).isDir() == false)
    {
        mRayPathsOrNull->RejectBuild(QStringLiteral("The drawing generation has no build package in this project, as "
                                                    "for the prebuilt engine default. Build & Preview the project's "
                                                    "graph, then capture again."));
        return;
    }
    if (IsJobActive() == true)
    {
        mRayPathsOrNull->RejectBuild(QStringLiteral("Another render-graph job is running; capture again once it "
                                                    "finishes."));
        return;
    }
    QString name = mDocument.GetRootGraph();
    name.chop(static_cast<qsizetype>(QStringLiteral(".lurender.json").size()));
    mRayPathPackage = QDir::cleanPath(QDir(build).filePath(QStringLiteral("../") + name + QStringLiteral(".lurgrd")));
    const QString configuration = mConfiguration->currentText() == DEFAULT_CONFIGURATION
                                      ? QStringLiteral("Default")
                                      : mConfiguration->currentText();
    QStringList arguments{generation,
                          QDir::cleanPath(QDir(build).filePath(QStringLiteral("../ray-path-capture"))),
                          mRayPathPackage,
                          QStringLiteral("--configuration"),
                          configuration};
    appendBuildToolArguments(arguments);
    QString endpoint = mBuildSettings.WorkerSocket;
    if (endpoint.isEmpty() == true)
    {
        endpoint = qEnvironmentVariable("LUDUS_SHADER_WORKER_SOCKET");
    }
    if (endpoint.isEmpty() == true && mBuildSettings.ShouldOverrideTools == false &&
        mWorkerOrNull->GetState() == RenderGraphWorkerState::Ready)
    {
        endpoint = mWorkerOrNull->Prepare();
    }
    if (endpoint.isEmpty() == false)
    {
        arguments += {QStringLiteral("--worker-socket"), endpoint};
    }
    if (startJob(QStringLiteral("ray-path-capture"), arguments, false) == false)
    {
        mRayPathsOrNull->RejectBuild(mStatus->text());
    }
#endif
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
    renderSourceStatus();
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
    mSourceAssistance->Update(mProjectRoot, mEpoch, manifestFile(), mSourceFile->currentText(), mBuildSettings);
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
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        if (result.Request.Action == QStringLiteral("ray-path-capture") && mRayPathsOrNull != nullptr)
        {
            mRayPathsOrNull->RejectBuild(QStringLiteral("The project or graph changed while the capture variant "
                                                        "compiled; capture again."));
        }
#endif
        Q_EMIT JobFinished();
        return;
    }
    // The rows are the Error List's (MakeRenderGraphErrorEntries); ActivateDiagnostic maps
    // a row back to its authored location from this result.
    mCanvas->ClearDiagnostic();
    mLastResult = result;
    mHasResult = true;
    mCanvas->GetDiagnostics().SetResult(result);
    mCanvas->GetExecutionPlan().SetResult(result);
    updateCanvasDiagnostics();
    updateSourceDiagnostics(true);
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
        if (result.Request.Action == QStringLiteral("dump"))
        {
            mIsValidated = true;
            mValidatedRevision = result.Request.Revision;
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
        result.Request.InputIdentity == getBuildIdentity(result.Request.Source))
    {
        static_cast<void>(PreviewRuntime(result.Request.Source));
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (result.Request.Action == QStringLiteral("ray-path-capture") && mRayPathsOrNull != nullptr)
    {
        if (result.IsSuccessful == true)
        {
            mRayPathsOrNull->AcceptBuild(mRayPathPackage);
        }
        else
        {
            mRayPathsOrNull->RejectBuild(result.IsCancelled == true
                                             ? QStringLiteral("The capture build was cancelled.")
                                             : QStringLiteral("%1: %2").arg(result.Code, result.Message));
        }
    }
#endif
    renderStatus();
    if (result.IsSuccessful == false)
    {
        // The Error List shows every row; the status line names the first.
        mStatus->setText(
            QStringLiteral("%1 failed - %2: %3 [%4]. Preview unchanged.")
                .arg(result.Request.Action, result.Code, result.Message, DescribeLocation(result.Location)));
        if (result.Request.ShouldPreview == true &&
            result.Request.InputIdentity == getBuildIdentity(result.Request.Source))
        {
            setProblem(mBuildProblem, mStatus->text());
        }
    }
    Q_EMIT JobFinished();
}

QByteArray RendererWorkspace::getBuildIdentity(RenderGraphSource source) const
{
    return ComputeRenderGraphBuildIdentity(mDocument, mConfiguration->currentText(), source);
}

void RendererWorkspace::scheduleLiveBuild(bool hasDebouncedSource)
{
    if (mIsRendering == false && mDocument.IsLoaded() == true)
    {
        mLiveBuild.Schedule(getBuildIdentity(RenderGraphSource::Saved), hasDebouncedSource);
    }
}

void RendererWorkspace::documentEdited(RenderGraphDocumentStatus status,
                                       bool shouldRefreshInspector,
                                       QWidget* editOriginOrNull)
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
        mSelectedNodes.clear();
    }
    renderTree(shouldRefreshInspector, editOriginOrNull);
    renderSource();
    renderStatus();
    Q_EMIT DocumentChanged();
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

void RendererWorkspace::renderTree(bool shouldRefreshInspector, QWidget* editOriginOrNull)
{
    if (mSelectedFile != mCanvas->GetFile() || mSelectedConfiguration != mCanvas->GetConfiguration())
    {
        mSelectedNodes = mSelectedNode.isEmpty() == true ? QStringList{} : QStringList{mSelectedNode};
    }
    QJsonObject selectedGraph;
    if (mDocument.GetGraph(mSelectedFile, selectedGraph) == true && selectedConfiguration().isEmpty() == true)
    {
        mSelectedConfiguration = selectedGraph.value(QStringLiteral("default")).toString();
        mSelectedNode.clear();
        mSelectedNodes.clear();
    }
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
                nodeItem->setSelected(file == mSelectedFile && configurationId == mSelectedConfiguration &&
                                      mSelectedNodes.contains(nodeId) == true);
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
        mTree->setCurrentItem(currentOrNull, 0, QItemSelectionModel::NoUpdate);
        if (mSelectedNodes.isEmpty() == true)
        {
            currentOrNull->setSelected(true);
        }
    }
    else
    {
        mSelectedNode.clear();
        mSelectedNodes.clear();
    }
    mIsRendering = false;
    const QString selectedSource = mSourceFile->currentText();
    mSourceFile->blockSignals(true);
    mSourceFile->clear();
    mSourceFile->addItems(mDocument.GetFiles());
    const int32 sourceIndex = mSourceFile->findText(selectedSource);
    mSourceFile->setCurrentIndex(sourceIndex >= 0 ? sourceIndex : mSourceFile->findText(mDocument.GetRootGraph()));
    mSourceFile->blockSignals(false);
    if (shouldRefreshInspector == true)
    {
        renderInspector();
    }
    renderEdges();
    renderCanvas();
    Q_EMIT NodeFieldsChanged(editOriginOrNull);
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
                // Focus loss during teardown must not submit another edit from obsolete controls.
                disconnect(widgetOrNull, nullptr, this, nullptr);
                for (QObject* control : widgetOrNull->findChildren<QObject*>())
                {
                    disconnect(control, nullptr, this, nullptr);
                }
                widgetOrNull->hide();
                widgetOrNull->setParent(nullptr);
                widgetOrNull->deleteLater();
            }
            delete itemOrNull;
        }
    }
    const QJsonObject node = mSelectedNodes.size() > 1 ? QJsonObject{} : selectedNode();
    QPushButton* remove = mInspector->findChild<QPushButton*>(QStringLiteral("renderGraphRemoveNode"));
    LUDUS_REQUIRE(remove != nullptr);
    remove->setText(mSelectedNodes.size() > 1 ? QStringLiteral("Remove Selected Nodes")
                                              : QStringLiteral("Remove Node"));
    mLayoutX->setEnabled(node.isEmpty() == false);
    mLayoutY->setEnabled(node.isEmpty() == false);
    if (node.isEmpty() == true)
    {
        QLabel* hint = new QLabel(
            mSelectedNodes.size() > 1
                ? QStringLiteral("%1 nodes selected. Drag, copy, or remove the group.").arg(mSelectedNodes.size())
                : QStringLiteral("Select a node to inspect its authored fields."),
            mInspector);
        hint->setWordWrap(true);
        mFields->addRow(hint);
        populateGraphSettings(*mFields, *mInspector);
        return;
    }
    populateNodeFields(*mFields, *mInspector);
    populateGraphSettings(*mFields, *mInspector);
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

void RendererWorkspace::populateNodeFields(QFormLayout& outFields, QWidget& inoutParent)
{
    QJsonObject node = selectedNode();
    if (node.value(QStringLiteral("kind")) == QStringLiteral("Upscale") &&
        node.value(QStringLiteral("algorithm")) == QStringLiteral("metalfx-spatial") &&
        node.contains(QStringLiteral("color_processing")) == false)
    {
        node.insert(QStringLiteral("color_processing"), QStringLiteral("Linear"));
    }
    populateNodeNameFields(outFields, inoutParent);
    outFields.addRow(QStringLiteral("Kind"), new QLabel(node.value(QStringLiteral("kind")).toString(), &inoutParent));
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Upscale"))
    {
        QLabel* availability = new QLabel(GetUpscaleAvailabilityMessage(node), &inoutParent);
        availability->setObjectName(QStringLiteral("renderGraphUpscaleAvailability"));
        availability->setWordWrap(true);
        outFields.addRow(availability);
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Denoise") &&
        GetDenoiseUnavailableMessage().isEmpty() == false)
    {
        QLabel* availability = new QLabel(GetDenoiseUnavailableMessage(), &inoutParent);
        availability->setObjectName(QStringLiteral("renderGraphDenoiseAvailability"));
        availability->setWordWrap(true);
        outFields.addRow(availability);
    }
    if (HasSceneBundle(node) == true)
    {
        QPushButton* expand = new QPushButton(QStringLiteral("Expand Scene Bundle"), &inoutParent);
        expand->setObjectName(QStringLiteral("renderGraphExpandSceneBundle"));
        expand->setCheckable(true);
        expand->setChecked(mCanvas->IsSceneBundleExpanded(mSelectedNode));
        connect(expand, &QPushButton::clicked, this, &RendererWorkspace::expandSceneBundle);
        outFields.addRow(expand);
    }
    if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
    {
        RenderGraphPipelineEditor* editor = new RenderGraphPipelineEditor(node, &inoutParent);
        if (UsesRayLaunch(node) == true)
        {
            RenderGraphSceneQueryBudgets budgets;
            if (mDocument.GetSceneQueryBudgets(budgets) == true)
            {
                editor->AddSceneQueryBudgets(budgets);
                connect(editor,
                        &RenderGraphPipelineEditor::SceneQueryBudgetsEdited,
                        this,
                        [this, editor](const RenderGraphSceneQueryBudgets& candidate) {
                            QString error;
                            const RenderGraphDocumentStatus status = mDocument.SetSceneQueryBudgets(candidate, error);
                            if (status != RenderGraphDocumentStatus::Ok)
                            {
                                mStatus->setText(error);
                                renderInspector();
                                Q_EMIT NodeFieldsChanged(nullptr);
                                Q_EMIT NodeOperationFailed(error);
                                return;
                            }
                            documentEdited(status, mInspector->isAncestorOf(editor) == false, editor);
                        });
            }
        }
        connect(editor, &RenderGraphPipelineEditor::Edited, this, [this, editor](const QJsonObject& candidate) {
            QString error;
            const bool hasChangedProfile = IsSceneComputeNode(selectedNode()) != IsSceneComputeNode(candidate);
            const RenderGraphDocumentStatus status =
                mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, candidate, &error);
            if (status != RenderGraphDocumentStatus::Ok)
            {
                mStatus->setText(error);
                renderInspector();
                Q_EMIT NodeFieldsChanged(nullptr);
                Q_EMIT NodeOperationFailed(error);
                return;
            }
            if (hasChangedProfile == true)
            {
                // A Scene input changes the State and Invocation controls, so rebuild both surfaces.
                documentEdited(status);
                return;
            }
            editor->SetShaderAssignments(selectedNode().value(QStringLiteral("shaders")).toArray());
            // Refresh the other surface while retaining the controls that submitted this edit.
            documentEdited(status, mInspector->isAncestorOf(editor) == false, editor);
        });
        connect(editor, &RenderGraphPipelineEditor::ShaderOpenRequested, this, [this, editor](const QString& shaderId) {
            openNodeShader(shaderId, editor);
        });
        editor->AddOptionsEditor(createNodeOptionsEditor(inoutParent));
        if (qobject_cast<QDialog*>(inoutParent.window()) != nullptr)
        {
            editor->ShowParts();
        }
        outFields.addRow(editor);
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
    {
        outFields.addRow(QStringLiteral("Public Options"), createNodeOptionsEditor(inoutParent));
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("ComputePipeline") &&
        IsSceneComputeNode(node) == false)
    {
        QPushButton* trace = new QPushButton(QStringLiteral("Add Scene and View Inputs"), &inoutParent);
        trace->setObjectName(QStringLiteral("renderGraphAddSceneInputs"));
        trace->setToolTip(DescribeSceneComputeConversion());
        connect(trace, &QPushButton::clicked, this, &RendererWorkspace::makeSceneCompute);
        outFields.addRow(trace);
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline") &&
        HasSceneRayInterface(QJsonDocument::fromJson(mDocument.GetDraft(manifestFile())).object()) == false)
    {
        QPushButton* enable = new QPushButton(QStringLiteral("Enable Scene Materials"), &inoutParent);
        enable->setObjectName(QStringLiteral("renderGraphEnableSceneMaterials"));
        enable->setToolTip(
            QStringLiteral("Upgrade the package's shared ray callbacks in one undoable edit. "
                           "Original shaders keep their behavior through adapters. Save publishes the change."));
        connect(enable, &QPushButton::clicked, this, &RendererWorkspace::enableSceneMaterials);
        outFields.addRow(enable);
    }
    if (CanDisableRenderGraphNode(node.value(QStringLiteral("kind")).toString()) == true)
    {
        QCheckBox* enabled = new QCheckBox(&inoutParent);
        enabled->setObjectName(QStringLiteral("renderGraphField_enabled"));
        enabled->setChecked(IsRenderGraphNodeEnabled(node));
        enabled->setToolTip(DescribeDisabledNode(node.value(QStringLiteral("kind")).toString()));
        connect(enabled, &QCheckBox::toggled, this, &RendererWorkspace::setSelectedNodeEnabled);
        outFields.addRow(QStringLiteral("Enabled"), enabled);
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Raster"))
    {
        QPushButton* migrate = new QPushButton(QStringLiteral("Convert to Graphics Pipeline"), &inoutParent);
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
        outFields.addRow(migrate);
    }
    for (QJsonObject::const_iterator field = node.constBegin(); field != node.constEnd(); ++field)
    {
        const QString name = field.key();
        if (name == QStringLiteral("id") || name == QStringLiteral("kind") || name == QStringLiteral("enabled") ||
            name == QStringLiteral("overrides") || name == QStringLiteral("options") ||
            (name == QStringLiteral("scale") &&
             node.value(QStringLiteral("kind")).toString() == QStringLiteral("RenderSize")) ||
            IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
        {
            continue;
        }
        QStringList choices = GetChoices(name);
        if (choices.isEmpty() == true)
        {
            choices = GetNodeFieldChoices(mDocument, mSelectedFile, node, name);
        }
        if (choices.isEmpty() == false)
        {
            QComboBox* combo = new QComboBox(&inoutParent);
            combo->setObjectName(QStringLiteral("renderGraphField_") + name);
            combo->addItems(choices);
            combo->setCurrentText(field.value().toString());
            connect(combo, &QComboBox::activated, this, [this, name, combo]() {
                replaceSelectedField(name, combo->currentText());
            });
            outFields.addRow(name, combo);
        }
        else if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Accumulate") &&
                 name == QStringLiteral("samples"))
        {
            QSpinBox* samples = new QSpinBox(&inoutParent);
            samples->setObjectName(QStringLiteral("renderGraphField_samples"));
            samples->setRange(1, static_cast<int32>(content::render_graph_format::MAX_ACCUMULATION_SAMPLES));
            samples->setValue(field.value().toInt());
            samples->setToolTip(QStringLiteral("Compensated linear RGB mean before display conversion. "
                                               "Resets on camera, scene, size or shader replacement. "
                                               "Disable this node to display individual samples."));
            connect(samples, &QSpinBox::editingFinished, this, [this, samples]() {
                replaceSelectedField(QStringLiteral("samples"), samples->value());
            });
            outFields.addRow(QStringLiteral("Sample limit"), samples);
        }
        else if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("ToneMap") &&
                 name == QStringLiteral("exposure"))
        {
            QDoubleSpinBox* exposure = new QDoubleSpinBox(&inoutParent);
            exposure->setObjectName(QStringLiteral("renderGraphField_exposure"));
            exposure->setRange(static_cast<float64>(content::render_graph_format::MIN_TONE_MAPPING_EXPOSURE),
                               static_cast<float64>(content::render_graph_format::MAX_TONE_MAPPING_EXPOSURE));
            exposure->setValue(field.value().toDouble());
            exposure->setToolTip(
                QStringLiteral("Linear exposure multiplier, then Reinhard compression and sRGB "
                               "encoding. Connect after Accumulate; disable to clamp raw linear RGB."));
            connect(exposure, &QDoubleSpinBox::editingFinished, this, [this, exposure]() {
                replaceSelectedField(QStringLiteral("exposure"), exposure->value());
            });
            outFields.addRow(QStringLiteral("Exposure multiplier"), exposure);
        }
        else if (field.value().isBool() == true)
        {
            QCheckBox* check = new QCheckBox(&inoutParent);
            check->setObjectName(QStringLiteral("renderGraphField_") + name);
            check->setChecked(field.value().toBool());
            connect(check, &QCheckBox::toggled, this, [this, name](bool isChecked) {
                replaceSelectedField(name, isChecked);
            });
            outFields.addRow(name, check);
        }
        else if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("StructuredBuffer") &&
                 name == QStringLiteral("count"))
        {
            const QString value = field.value().isString() == true ? field.value().toString()
                                                                   : QString::number(field.value().toInteger());
            QLineEdit* line = new QLineEdit(value, &inoutParent);
            line->setObjectName(QStringLiteral("renderGraphField_count"));
            line->setToolTip(QStringLiteral(
                "ColorTarget gives one element per output pixel, or enter a positive fixed element count."));
            connect(line, &QLineEdit::editingFinished, this, [this, line]() {
                bool isNumber = false;
                const int32 count = line->text().toInt(&isNumber);
                replaceSelectedField(QStringLiteral("count"),
                                     isNumber == true ? QJsonValue(count) : QJsonValue(line->text()));
            });
            outFields.addRow(name, line);
        }
        else if (name == QStringLiteral("extent") && node.contains(QStringLiteral("bytes_per_pixel")) == true)
        {
            outFields.addRow(QStringLiteral("Sizing"),
                             new QLabel(QStringLiteral("From connected RenderSize"), &inoutParent));
        }
        else if (name == QStringLiteral("extent"))
        {
            QWidget* row = new QWidget(&inoutParent);
            QFormLayout* rowLayout = new QFormLayout(row);
            rowLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
            rowLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            QComboBox* mode = new QComboBox(row);
            mode->setObjectName(QStringLiteral("renderGraphExtentMode"));
            const bool isSizeNode = node.value(QStringLiteral("kind")).toString() == QStringLiteral("RenderSize");
            const bool isOutputSize = node.value(QStringLiteral("kind")).toString() == QStringLiteral("OutputSize");
            const QString source = isOutputSize == true ? QStringLiteral("Viewport")
                                   : isSizeNode == true ? QStringLiteral("Texture")
                                                        : QStringLiteral("RenderSize");
            mode->addItem(QStringLiteral("Fixed dimensions"));
            mode->addItem(isOutputSize == true ? QStringLiteral("Viewport pixels")
                          : isSizeNode == true ? QStringLiteral("Match texture")
                                               : QStringLiteral("From size input"));
            const bool isDerived = field.value().toString() == QStringLiteral("OutputSize");
            const QJsonArray scale = isDerived == true
                                         ? node.value(QStringLiteral("scale")).toArray()
                                         : field.value().toObject().value(QStringLiteral("scale")).toArray();
            constexpr int32 QUARTER_DENOMINATOR = 4;
            constexpr int32 FIRST_VIEW_MODE = 2;
            constexpr int32 VIEW_PRESET_COUNT = 3;
            constexpr int32 OUTPUT_SIZE_MODE = FIRST_VIEW_MODE + VIEW_PRESET_COUNT + 1;
            if (isSizeNode == true)
            {
                mode->addItems({QStringLiteral("Full view"),
                                QStringLiteral("Half view"),
                                QStringLiteral("Quarter view"),
                                QStringLiteral("Custom view scale"),
                                QStringLiteral("From OutputSize")});
            }
            const bool isFixed = field.value().isArray();
            int32 selectedMode = isFixed == true ? 0 : 1;
            if (field.value().isObject() == true)
            {
                selectedMode = FIRST_VIEW_MODE + VIEW_PRESET_COUNT;
                for (int32 preset = 0; preset < VIEW_PRESET_COUNT; ++preset)
                {
                    if (scale == QJsonArray{1, 1 << preset})
                    {
                        selectedMode = FIRST_VIEW_MODE + preset;
                    }
                }
            }
            if (isDerived == true)
            {
                selectedMode = OUTPUT_SIZE_MODE;
            }
            mode->setCurrentIndex(selectedMode);
            rowLayout->addRow(mode);
            QSpinBox* width = new QSpinBox(row);
            QSpinBox* height = new QSpinBox(row);
            width->setObjectName(QStringLiteral("renderGraphExtentWidth"));
            height->setObjectName(QStringLiteral("renderGraphExtentHeight"));
            for (QSpinBox* spin : {width, height})
            {
                spin->setRange(1, MAX_EXTENT);
                spin->setEnabled(isFixed);
                spin->setVisible(isFixed);
            }
            rowLayout->addRow(QStringLiteral("Width (pixels)"), width);
            rowLayout->addRow(QStringLiteral("Height (pixels)"), height);
            rowLayout->setRowVisible(width, isFixed);
            rowLayout->setRowVisible(height, isFixed);
            const QJsonArray extent =
                isFixed == true ? field.value().toArray() : QJsonArray{DEFAULT_RENDER_WIDTH, DEFAULT_RENDER_HEIGHT};
            width->setValue(extent.at(0).toInt());
            height->setValue(extent.at(1).toInt());
            QSpinBox* numerator = new QSpinBox(row);
            QSpinBox* denominator = new QSpinBox(row);
            numerator->setObjectName(QStringLiteral("renderGraphScaleNumerator"));
            denominator->setObjectName(QStringLiteral("renderGraphScaleDenominator"));
            numerator->setPrefix(QStringLiteral("× "));
            denominator->setPrefix(QStringLiteral("÷ "));
            numerator->setAccessibleName(QStringLiteral("Render scale numerator"));
            denominator->setAccessibleName(QStringLiteral("Render scale denominator"));
            const bool isCustom = isSizeNode == true && selectedMode >= FIRST_VIEW_MODE + VIEW_PRESET_COUNT;
            for (QSpinBox* spin : {numerator, denominator})
            {
                spin->setRange(1, MAX_EXTENT);
                spin->setVisible(isCustom);
                spin->setEnabled(isCustom);
            }
            rowLayout->addRow(QStringLiteral("Scale numerator"), numerator);
            rowLayout->addRow(QStringLiteral("Scale denominator"), denominator);
            rowLayout->setRowVisible(numerator, isCustom);
            rowLayout->setRowVisible(denominator, isCustom);
            numerator->setValue(scale.isEmpty() == true ? 1 : scale.at(0).toInt());
            denominator->setValue(scale.isEmpty() == true ? 1 : scale.at(1).toInt());
            const auto commitScale = [this, mode, numerator, denominator]() {
                if (mode->currentIndex() == OUTPUT_SIZE_MODE)
                {
                    replaceSelectedField(QStringLiteral("scale"), QJsonArray{numerator->value(), denominator->value()});
                    return;
                }
                replaceSelectedField(
                    QStringLiteral("extent"),
                    QJsonObject{{QStringLiteral("reference"), QStringLiteral("View")},
                                {QStringLiteral("scale"), QJsonArray{numerator->value(), denominator->value()}}});
            };
            connect(numerator, &QSpinBox::editingFinished, this, commitScale);
            connect(denominator, &QSpinBox::editingFinished, this, commitScale);
            connect(mode,
                    &QComboBox::activated,
                    this,
                    [this, source, width, height, numerator, denominator, rowLayout](int32 index) {
                        const bool isCustomScale = index >= FIRST_VIEW_MODE + VIEW_PRESET_COUNT;
                        // Selecting Custom may keep the same fraction as a preset, so there may
                        // be no document edit/rebuild. Reveal its controls in that case too.
                        numerator->setEnabled(isCustomScale);
                        denominator->setEnabled(isCustomScale);
                        rowLayout->setRowVisible(numerator, isCustomScale);
                        rowLayout->setRowVisible(denominator, isCustomScale);
                        QJsonValue extentValue = source;
                        if (index == 0)
                        {
                            extentValue = QJsonArray{width->value(), height->value()};
                        }
                        else if (index == OUTPUT_SIZE_MODE)
                        {
                            extentValue = QStringLiteral("OutputSize");
                        }
                        else if (index >= FIRST_VIEW_MODE)
                        {
                            const int32 divisor = index == FIRST_VIEW_MODE       ? 1
                                                  : index == FIRST_VIEW_MODE + 1 ? 2
                                                  : index == FIRST_VIEW_MODE + 2 ? QUARTER_DENOMINATOR
                                                                                 : denominator->value();
                            extentValue = QJsonObject{
                                {QStringLiteral("reference"), QStringLiteral("View")},
                                {QStringLiteral("scale"),
                                 QJsonArray{index < FIRST_VIEW_MODE + VIEW_PRESET_COUNT ? 1 : numerator->value(),
                                            divisor}}};
                        }
                        replaceSelectedField(QStringLiteral("extent"), extentValue);
                    });
            const auto commitExtent = [this, width, height]() {
                replaceSelectedField(QStringLiteral("extent"), QJsonArray{width->value(), height->value()});
            };
            connect(width, &QSpinBox::editingFinished, this, commitExtent);
            connect(height, &QSpinBox::editingFinished, this, commitExtent);
            outFields.addRow(isOutputSize == true ? QStringLiteral("Output resolution")
                             : isSizeNode == true ? QStringLiteral("Render resolution")
                                                  : name,
                             row);
        }
        else if (field.value().isDouble() == true)
        {
            QSpinBox* spin = new QSpinBox(&inoutParent);
            spin->setObjectName(QStringLiteral("renderGraphField_") + name);
            spin->setRange(1,
                           name == QStringLiteral("bytes") ? content::render_graph_format::MAX_RAY_BUFFER_BYTES
                                                           : MAX_EXTENT);
            if (name == QStringLiteral("bytes"))
            {
                spin->setSingleStep(content::render_graph_format::RAY_BUFFER_WORD_BYTES);
            }
            spin->setValue(field.value().toInt());
            connect(spin, &QSpinBox::editingFinished, this, [this, name, spin]() {
                replaceSelectedField(name, spin->value());
            });
            outFields.addRow(name, spin);
        }
        else
        {
            QLineEdit* line = new QLineEdit(field.value().toString(), &inoutParent);
            line->setObjectName(QStringLiteral("renderGraphField_") + name);
            connect(line, &QLineEdit::editingFinished, this, [this, name, line]() {
                replaceSelectedField(name, line->text());
            });
            outFields.addRow(name, line);
        }
    }
}

void RendererWorkspace::populateGraphSettings(QFormLayout& outFields, QWidget& inoutParent)
{
    const QJsonArray settings = QJsonDocument::fromJson(mDocument.GetDraft(manifestFile()))
                                    .object()
                                    .value(QStringLiteral("settings"))
                                    .toArray();
    if (settings.isEmpty() == true)
    {
        return;
    }
    RenderGraphSettingsEditor* editor = new RenderGraphSettingsEditor(settings, &inoutParent);
    editor->setEnabled(CanCreateSubgraph());
    connect(
        editor,
        &RenderGraphSettingsEditor::SettingEdited,
        this,
        [this, editor](const QString& identity, const QJsonValue& value) { setGraphSetting(identity, value, editor); });
    QToolButton* section = new QToolButton(&inoutParent);
    section->setObjectName(QStringLiteral("renderGraphToggleSettings"));
    section->setText(QStringLiteral("Graph settings"));
    section->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    section->setCheckable(true);
    section->setChecked(mIsGraphSettingsSectionExpanded);
    section->setArrowType(mIsGraphSettingsSectionExpanded == true ? Qt::DownArrow : Qt::RightArrow);
    editor->setVisible(mIsGraphSettingsSectionExpanded);
    connect(section, &QToolButton::toggled, this, [this, editor, section](bool isExpanded) {
        mIsGraphSettingsSectionExpanded = isExpanded;
        section->setArrowType(isExpanded == true ? Qt::DownArrow : Qt::RightArrow);
        editor->setVisible(isExpanded);
    });
    outFields.addRow(section);
    outFields.addRow(editor);
}

void RendererWorkspace::setGraphSetting(const QString& identity, const QJsonValue& value, QWidget* editOrigin)
{
    LUDUS_REQUIRE(editOrigin != nullptr);
    if (CanCreateSubgraph() == false)
    {
        return;
    }
    const QString file = manifestFile();
    QJsonObject manifest = QJsonDocument::fromJson(mDocument.GetDraft(file)).object();
    QJsonArray settings = manifest.value(QStringLiteral("settings")).toArray();
    for (qsizetype i = 0; i < settings.size(); ++i)
    {
        QJsonObject setting = settings[i].toObject();
        if (setting.value(QStringLiteral("id")).toString() != identity)
        {
            continue;
        }
        const QString type = setting.value(QStringLiteral("type")).toString();
        const bool isBoolean = type == QStringLiteral("Bool") && value.isBool() == true;
        const bool isNumber =
            (type == QStringLiteral("Float32") || type == QStringLiteral("UInt32")) && value.isDouble() == true &&
            value.toDouble() >= setting.value(QStringLiteral("minimum")).toDouble() &&
            value.toDouble() <= setting.value(QStringLiteral("maximum")).toDouble() &&
            (type != QStringLiteral("UInt32") || value.toDouble() == static_cast<float64>(value.toInteger()));
        if ((isBoolean == false && isNumber == false) || setting.value(QStringLiteral("value")) == value)
        {
            return;
        }
        setting.insert(QStringLiteral("value"), value);
        settings[i] = setting;
        manifest.insert(QStringLiteral("settings"), settings);
        documentEdited(mDocument.SetText(file, EncodeCanonicalJson(manifest)),
                       mInspector->isAncestorOf(editOrigin) == false,
                       editOrigin);
        return;
    }
}

void RendererWorkspace::editGraphSettingsOnCanvas()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("renderGraphSettingsDialog"));
    dialog.setWindowTitle(QStringLiteral("Graph Settings"));
    QFormLayout* fields = new QFormLayout(&dialog);
    populateGraphSettings(*fields, dialog);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    fields->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    static_cast<void>(dialog.exec());
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

void RendererWorkspace::updateSourceInterfaceVisibility()
{
    const bool isShader = mSourceFile->currentText().endsWith(QStringLiteral(".slang"));
    mSourceInterfaceToggle->setEnabled(isShader);
    QWidget* interfacePage = mSourceInterface->parentWidget();
    const bool shouldShow = isShader == true && mSourceInterfaceToggle->isChecked() == true && mIsChromeVisible == true;
    if (shouldShow == false && interfacePage->isHidden() == false && mSourceSplit->sizes().value(1) > 0)
    {
        QSettings().setValue(QStringLiteral("renderGraph/sourceSplit"), mSourceSplit->saveState());
    }
    const QSignalBlocker splitterBlocker(mSourceSplit);
    interfacePage->setVisible(shouldShow);
    if (shouldShow == true)
    {
        static_cast<void>(
            mSourceSplit->restoreState(QSettings().value(QStringLiteral("renderGraph/sourceSplit")).toByteArray()));
    }
}

void RendererWorkspace::renderSourceStatus()
{
    if (mDocument.IsLoaded() == false)
    {
        mSourceStatus->setText(QStringLiteral("Open a render graph to edit its sources."));
        mSourceStatus->setToolTip({});
        return;
    }
    QString state =
        mDocument.IsDirty() == true ? QStringLiteral("Unsaved · Save publishes files.") : QStringLiteral("Saved.");
    if (mDocument.IsBuiltIn() == true)
    {
        state = mDocument.IsDirty() == true ? QStringLiteral("Unsaved · Save creates a project copy.")
                                            : QStringLiteral("Engine default · edits start a project copy.");
    }
    if (mJob.IsActive() == true || mHasPendingWorkerBuild == true)
    {
        state += QStringLiteral(" Compiling / validating; preview retained.");
    }
    else if (mSourceProblem.isEmpty() == false)
    {
        state += QStringLiteral(" Source conflict; see Error List. Preview retained.");
    }
    else if (mHasResult == true && mLastResult.IsSuccessful == false && mLastResult.IsCancelled == false)
    {
        const bool isCurrent = mLastResult.Request.InputIdentity.isEmpty() == false &&
                               mLastResult.Request.InputIdentity == getBuildIdentity(RenderGraphSource::Draft);
        state += isCurrent == true ? QStringLiteral(" Build / validation failed; see Error List.")
                                   : QStringLiteral(" Earlier build / validation failed; validate current edits.");
    }
    else if (GetRenderProblem().isEmpty() == false)
    {
        state += QStringLiteral(" Preview blocked; see Error List. Last drawable view retained.");
    }
    else
    {
        state += mIsLivePreviewEnabled == true ? QStringLiteral(" Live Preview builds saved sources.")
                                               : QStringLiteral(" Live Preview off · use Build & Preview.");
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewOrNull != nullptr)
    {
        const graphics::renderer::RenderGraphCandidateState candidate = mPreviewOrNull->GetReport().Candidate;
        if (candidate == graphics::renderer::RenderGraphCandidateState::Preparing ||
            candidate == graphics::renderer::RenderGraphCandidateState::NeedsUpdate)
        {
            state += QStringLiteral(" Preparing preview; last drawable view retained.");
        }
    }
    if (mPreviewOrNull != nullptr && mPreviewOrNull->GetPresentedGeneration() != 0)
    {
        state += QStringLiteral(" Showing generation %1.").arg(mPreviewOrNull->GetPresentedGeneration());
        if (mPreviewOrNull->HasPresentedCatalogue() == true)
        {
            const RenderGraphPreviewSource& source = mPreviewOrNull->GetActiveSource();
            if (source.InputIdentity.isEmpty() == false)
            {
                state += QStringLiteral(" Source revision %1%2.")
                             .arg(source.SourceRevision)
                             .arg(source.InputIdentity == getBuildIdentity(RenderGraphSource::Draft)
                                      ? QStringLiteral(" · current renderer inputs")
                                      : QStringLiteral(" · earlier renderer inputs"));
            }
        }
    }
    else
    {
        state += QStringLiteral(" No graph frame presented yet.");
    }
#else
    state += QStringLiteral(" GPU preview unavailable in this editor build.");
#endif
    mSourceStatus->setText(QStringLiteral("Draft r%1 · %2").arg(mDocument.GetRevision()).arg(state));
    const QString problem = GetRenderProblem();
    mSourceStatus->setToolTip(problem.isEmpty() == true ? QString()
                                                        : QStringLiteral("<pre>%1</pre>").arg(problem.toHtmlEscaped()));
}

void RendererWorkspace::renderSource()
{
    const QString file = mSourceFile->currentText();
    mSourceAssistance->Update(mProjectRoot, mEpoch, manifestFile(), file, mBuildSettings);
    mSourceDocument.Bind(file, mEpoch);
    mSourceController->Bind(file.isEmpty() == false && mDocument.IsLoaded() == true ? &mSourceDocument : nullptr);
    if (file.isEmpty() == true)
    {
        mSource->setPlainText({});
    }
    mSource->SetLanguage(file.endsWith(QStringLiteral(".slang")) == true ? CodeLanguage::Slang
                                                                         : CodeLanguage::PlainText);
    mSource->setEnabled(file.isEmpty() == false);
    mSource->SetCanFormat(CodeFormatter::IsAvailable() == true && mSourceFormatter->IsRunning() == false &&
                          mSource->GetLanguage() == CodeLanguage::Slang);
    updateSourceInterfaceVisibility();
    QMap<QString, QByteArray> sources;
    for (const QString& draftFile : mDocument.GetFiles())
    {
        if (draftFile.endsWith(QStringLiteral(".slang")) == true)
        {
            sources.insert(QDir::cleanPath(draftFile), mDocument.GetDraft(draftFile));
        }
    }
    UpdateShaderInterfaceView(*mSourceInterface,
                              file,
                              DescribeShaderInterface(mDocument.GetDraft(manifestFile()), file, sources));
    updateSourceDiagnostics(false);
}

void RendererWorkspace::updateSourceDiagnostics(bool isNewResult)
{
    const QString file = mSourceFile->currentText();
    const uint64 request = mHasResult == true ? mLastResult.Request.Request : 0;
    if (isNewResult == false && request == mSourceDiagnosticsRequest && mEpoch == mSourceDiagnosticsEpoch &&
        file == mSourceDiagnosticsFile)
    {
        return;
    }
    mSourceDiagnosticsRequest = request;
    mSourceDiagnosticsEpoch = mEpoch;
    mSourceDiagnosticsFile = file;
    QList<CodeDiagnostic> diagnostics;
    const int32 count = mHasResult == true && mLastResult.IsSuccessful == false ? GetDiagnosticCount() : 0;
    // Without a structured batch, row 0 summarizes the compiler rows that follow it.
    const int32 first =
        mLastResult.Diagnostics.isEmpty() == false && count == mLastResult.Diagnostics.size() + 1 ? 1 : 0;
    for (int32 row = first; row < count && file.isEmpty() == false; ++row)
    {
        const QJsonObject diagnostic = mLastResult.GetDiagnostic(row);
        const QJsonObject location = diagnostic.value(QStringLiteral("location")).toObject();
        if (location.contains(QStringLiteral("generated")) == true ||
            QDir::cleanPath(location.value(QStringLiteral("source")).toString()) != QDir::cleanPath(file) ||
            location.value(QStringLiteral("line")).toInt() < 1)
        {
            continue;
        }
        const CodeDiagnostic entry =
        {
            .Line = location.value(QStringLiteral("line")).toInt(),
            .Column = location.value(QStringLiteral("column")).toInt(),
            .Length = location.value(QStringLiteral("length")).toInt(),
            .Severity = diagnostic.value(QStringLiteral("severity"))
                                    .toString()
                                    .compare(QLatin1StringView("warning"), Qt::CaseInsensitive) == 0
                            ? CodeDiagnosticSeverity::Warning
                            : CodeDiagnosticSeverity::Error,
            .Message = DescribeRenderGraphDiagnostic(diagnostic),
        };
        // Several programs and targets can compile one file and repeat the same message.
        bool isDuplicate = false;
        for (const CodeDiagnostic& existing : diagnostics)
        {
            isDuplicate = isDuplicate || (existing.Line == entry.Line && existing.Column == entry.Column &&
                                          existing.Length == entry.Length && existing.Severity == entry.Severity &&
                                          existing.Message == entry.Message);
        }
        if (isDuplicate == false)
        {
            diagnostics.append(entry);
        }
    }
    // The report matches this text only if its sources are unchanged and it compiled what the
    // view shows: a saved-source build does not describe unsaved edits.
    const bool isCurrent = (mLastResult.Request.InputIdentity.isEmpty() == true ||
                            mLastResult.Request.InputIdentity == getBuildIdentity(mLastResult.Request.Source)) &&
                           (mLastResult.Request.Source == RenderGraphSource::Draft ||
                            mDocument.GetDraft(file) == mDocument.GetSaved(file));
    mSource->SetDiagnostics(diagnostics, isCurrent);
}

void RendererWorkspace::updateCanvasDiagnostics()
{
    RenderGraphDiagnostics& diagnostics = mCanvas->GetDiagnostics();
    if (mHasResult == false)
    {
        diagnostics.Reset();
        return;
    }
    const QByteArray captured = mLastResult.Request.InputIdentity;
    RenderGraphDiagnosticFreshness freshness = RenderGraphDiagnosticFreshness::Unverified;
    if (captured.isEmpty() == false)
    {
        const bool isCurrent = captured == getBuildIdentity(mLastResult.Request.Source) &&
                               captured == getBuildIdentity(RenderGraphSource::Draft);
        freshness = isCurrent == true ? RenderGraphDiagnosticFreshness::Current : RenderGraphDiagnosticFreshness::Stale;
    }
    diagnostics.Refresh(freshness);
}

void RendererWorkspace::renderStatus()
{
    updateResourceInspection();
    updateCanvasDiagnostics();
    mCanvas->GetExecutionPlan().Refresh(mConfiguration->currentText());
    renderSourceStatus();
    if (mCreation.IsActive() == true)
    {
        mStatus->setText(QStringLiteral("Creating the graph and shader sources in your project..."));
    }
    else if (mJob.IsActive() == true)
    {
        mStatus->setText(QStringLiteral("Updating render graph; keeping the last working preview..."));
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
                           .arg(mDocument.IsDirty() == true ? QStringLiteral("unsaved changes; Save publishes files")
                                                            : QStringLiteral("matches saved sources"));
        if (mIsValidated == true)
        {
            text += mValidatedRevision == mDocument.GetRevision()
                        ? QStringLiteral(" Validated.")
                        : QStringLiteral(" Last validation describes revision %1.").arg(mValidatedRevision);
        }
        mStatus->setText(text);
    }
    if (GetRenderProblem().isEmpty() == false)
    {
        mStatus->setText(GetRenderProblem());
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
    if (field == QStringLiteral("extent") &&
        node.value(QStringLiteral("kind")).toString() == QStringLiteral("RenderSize"))
    {
        if (value.toString() == QStringLiteral("OutputSize"))
        {
            node.insert(QStringLiteral("scale"), QJsonArray{1, 1});
        }
        else
        {
            node.remove(QStringLiteral("scale"));
        }
    }
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
    replaceSelectedNode(node);
}

void RendererWorkspace::setSelectedNodeEnabled(bool isEnabled)
{
    QJsonObject node = selectedNode();
    if (node.isEmpty() == true || CanDisableRenderGraphNode(node.value(QStringLiteral("kind")).toString()) == false ||
        IsRenderGraphNodeEnabled(node) == isEnabled)
    {
        return;
    }
    // Enabled is the default, so the saved source records only a disabled node.
    if (isEnabled == true)
    {
        node.remove(QStringLiteral("enabled"));
    }
    else
    {
        node.insert(QStringLiteral("enabled"), false);
    }
    replaceSelectedNode(node);
}

void RendererWorkspace::replaceSelectedNode(const QJsonObject& node)
{
    QString error;
    const RenderGraphDocumentStatus status = mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, node, &error);
    documentEdited(status);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        renderInspector();
        Q_EMIT NodeFieldsChanged(nullptr);
        if (error.isEmpty() == false)
        {
            mStatus->setText(error);
        }
        Q_EMIT NodeOperationFailed(mStatus->text());
    }
}

bool RendererWorkspace::CanRemoveUnusedShaders() const
{
    return mDocument.IsLoaded() == true && IsJobActive() == false && mSourceProblem.isEmpty() == true;
}

bool RendererWorkspace::RemoveUnusedShaders()
{
    if (CanRemoveUnusedShaders() == false || ReloadSourcesFromDisk() == false)
    {
        return false;
    }
    QWidget* focusedControlOrNull = QApplication::focusWidget();
    if (IsNodeFieldControl(*this, *mInspector, focusedControlOrNull) == true)
    {
        focusedControlOrNull->clearFocus();
    }
    mSourceEditDebounce.stop();
    mSourceController->BreakUndoGroup();
    QStringList programs;
    QStringList sources;
    QString error;
    const RenderGraphDocumentStatus status = mDocument.RemoveUnusedShaders(programs, sources, error);
    documentEdited(status);
    mStatus->setText(status == RenderGraphDocumentStatus::Ok
                         ? QStringLiteral("Removed %1 unused shader programs and %2 source files from the draft. "
                                          "Shared or included sources were retained. Save to publish; Undo to restore.")
                               .arg(programs.size())
                               .arg(sources.size())
                         : error);
    if (status == RenderGraphDocumentStatus::Ok && programs.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("No unused shader programs found in the loaded package."));
    }
    return status == RenderGraphDocumentStatus::Ok;
}

bool RendererWorkspace::RemoveSelectedSubgraphLibrary()
{
    if (CanRemoveUnusedShaders() == false || ReloadSourcesFromDisk() == false)
    {
        mStatus->setText(QStringLiteral("Wait for the running job and resolve external source conflicts before "
                                        "deleting a Subgraph and its library."));
        return false;
    }
    const QString node = mSelectedNode;
    if (mSelectedNodes.size() > 1 ||
        selectedNode().value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph"))
    {
        mStatus->setText(QStringLiteral("Select one Subgraph node on the canvas to delete it with its library."));
        return false;
    }
    mSourceEditDebounce.stop();
    mSourceController->BreakUndoGroup();
    RenderGraphLibraryRemoval removal;
    QString error;
    const RenderGraphDocumentStatus status =
        mDocument.RemoveSubgraphLibrary(mSelectedFile, mCanvas->GetConfiguration(), node, removal, error);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        mSelectedNode.clear();
        mSelectedNodes.clear();
    }
    documentEdited(status);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
    }
    else if (removal.Graphs.isEmpty() == true)
    {
        mStatus->setText(
            QStringLiteral("Deleted Subgraph %1. Its library is still used by %2, so the library and its "
                           "shaders were kept. Save to publish; Undo to restore.")
                .arg(node, removal.RetainedBy.isEmpty() == true ? QStringLiteral("the package") : removal.RetainedBy));
    }
    else
    {
        mStatus->setText(QStringLiteral("Deleted Subgraph %1 with %2, %3 shader programs and %4 source files from the "
                                        "draft. Sources other shaders include were kept. Save to publish; Undo to "
                                        "restore.")
                             .arg(node, removal.Graphs.join(QStringLiteral(", ")))
                             .arg(removal.Programs.size())
                             .arg(removal.Sources.size()));
    }
    return status == RenderGraphDocumentStatus::Ok;
}

bool RendererWorkspace::CanCreateSubgraph() const
{
    return mDocument.IsLoaded() == true && mCanvas->GetConfiguration().isEmpty() == false && IsJobActive() == false &&
           mSourceProblem.isEmpty() == true;
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
    static_cast<void>(mCanvas->GetHistory().PrepareSubgraph(node, asset, QStringLiteral("Default")));
    mSelectedFile = asset;
    mSelectedConfiguration = QStringLiteral("Default");
    mSelectedNode.clear();
    mSelectedNodes.clear();
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
        QJsonObject effective;
        QString optionError;
        if (ResolveRenderGraphPipelineOptions(node, effective, optionError) == false)
        {
            return {};
        }
        const QJsonArray shaders = effective.value(QStringLiteral("shaders")).toArray();
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
    const QJsonObject node = selectedNode();
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
    {
        openSubgraph(node.value(QStringLiteral("asset")).toString(),
                     node.value(QStringLiteral("configuration")).toString(),
                     mSelectedNode);
        return;
    }
    editNodeOnCanvas();
}

void RendererWorkspace::expandSceneBundle(bool isExpanded)
{
    if (isExpanded == true)
    {
        const RenderGraphDocumentStatus status =
            mDocument.ExpandSceneBundle(mSelectedFile, mSelectedConfiguration, mSelectedNode);
        if (status != RenderGraphDocumentStatus::Ok)
        {
            documentEdited(status);
            return;
        }
    }
    mCanvas->SetSceneBundleExpanded(mSelectedNode, isExpanded);
    documentEdited(RenderGraphDocumentStatus::Ok);
}

void RendererWorkspace::enableSceneMaterials()
{
    QString error;
    const RenderGraphDocumentStatus status = mDocument.EnableSceneMaterials(error);
    documentEdited(status);
    mStatus->setText(
        status == RenderGraphDocumentStatus::Ok
            ? QStringLiteral("Scene materials enabled. Open the new closest-hit shader from View Shaders "
                             "to use EvaluateMaterial(scene, hit). Original callbacks are preserved. Save to publish.")
            : error);
}

void RendererWorkspace::makeSceneCompute()
{
    QJsonObject candidate;
    if (MakeSceneComputeNode(selectedNode(), candidate) == false)
    {
        mStatus->setText(QStringLiteral("Select a ComputePipeline without a Scene input. The draft was preserved."));
        return;
    }
    QString error;
    const RenderGraphDocumentStatus status =
        mDocument.ReplaceNode(mSelectedFile, mSelectedConfiguration, candidate, &error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error.isEmpty() == true ? DescribeStatus(status) : error);
        return;
    }
    documentEdited(status);
    mStatus->setText(QStringLiteral("%1 is a scene compute pass. Connect a Scene and a camera View, then add a "
                                    "compute shader for the scene-aware stub. Save to publish.")
                         .arg(mSelectedNode));
}

void RendererWorkspace::showNodeContextMenu(const QPoint& globalPosition, bool isNodeContext)
{
    QMenu menu(this);
    menu.setObjectName(isNodeContext == true ? QStringLiteral("renderGraphNodeMenu")
                                             : QStringLiteral("renderGraphBackgroundMenu"));
    if (isNodeContext == false)
    {
        const QPoint quickAddPoint = mCanvas->viewport()->mapFromGlobal(globalPosition);
        const QPointF quickAddPosition = mCanvas->mapToScene(mCanvas->viewport()->rect().contains(quickAddPoint) == true
                                                                 ? quickAddPoint
                                                                 : mCanvas->viewport()->rect().center());
        QAction* quickAdd = menu.addAction(QStringLiteral("Quick Add..."));
        quickAdd->setObjectName(QStringLiteral("renderGraphQuickAddAction"));
        quickAdd->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_A));
        quickAdd->setShortcutVisibleInContextMenu(true);
        quickAdd->setEnabled(CanCreateSubgraph());
        connect(quickAdd, &QAction::triggered, this, [this, quickAddPosition]() {
            static_cast<void>(ShowQuickAdd(quickAddPosition));
        });
        QAction* options = menu.addAction(QStringLiteral("Edit Exposed Options..."));
        options->setObjectName(QStringLiteral("renderGraphExposedOptionsAction"));
        options->setEnabled(CanCreateSubgraph());
        connect(options, &QAction::triggered, this, &RendererWorkspace::editExposedOptionsOnCanvas);
        menu.addSeparator();
        if (QJsonDocument::fromJson(mDocument.GetDraft(manifestFile()))
                .object()
                .value(QStringLiteral("settings"))
                .toArray()
                .isEmpty() == false)
        {
            QAction* settings = menu.addAction(QStringLiteral("Edit Graph Settings..."));
            settings->setObjectName(QStringLiteral("renderGraphEditSettingsAction"));
            settings->setEnabled(CanCreateSubgraph());
            connect(settings, &QAction::triggered, this, &RendererWorkspace::editGraphSettingsOnCanvas);
        }
        QAction* paste = menu.addAction(QStringLiteral("Paste Nodes"));
        paste->setObjectName(QStringLiteral("renderGraphPasteNodes"));
        paste->setEnabled(CanCreateSubgraph());
        const QPoint localPosition = mCanvas->viewport()->mapFromGlobal(globalPosition);
        const QPointF pastePosition = mCanvas->mapToScene(mCanvas->viewport()->rect().contains(localPosition) == true
                                                              ? localPosition
                                                              : mCanvas->viewport()->rect().center());
        connect(paste, &QAction::triggered, this, [this, pastePosition]() {
            static_cast<void>(PasteNodes(pastePosition));
        });
        QMenu* configurations = menu.addMenu(QStringLiteral("Configurations"));
        configurations->setObjectName(QStringLiteral("renderGraphConfigurationsMenu"));
        QAction* createConfiguration = configurations->addAction(QStringLiteral("Add Configuration..."));
        createConfiguration->setObjectName(QStringLiteral("renderGraphAddConfiguration"));
        createConfiguration->setEnabled(mDocument.IsLoaded() == true);
        connect(createConfiguration, &QAction::triggered, this, &RendererWorkspace::promptCreateConfiguration);
        QAction* duplicateConfiguration = configurations->addAction(QStringLiteral("Duplicate Configuration..."));
        duplicateConfiguration->setObjectName(QStringLiteral("renderGraphDuplicateConfiguration"));
        duplicateConfiguration->setEnabled(CanCreateSubgraph() == true && selectedConfiguration().isEmpty() == false);
        connect(duplicateConfiguration, &QAction::triggered, this, &RendererWorkspace::promptDuplicateConfiguration);
        QAction* configurationTemplate = configurations->addAction(QStringLiteral("Apply Configuration Template..."));
        configurationTemplate->setObjectName(QStringLiteral("renderGraphApplyConfigurationTemplate"));
        configurationTemplate->setEnabled(CanCreateSubgraph());
        connect(configurationTemplate, &QAction::triggered, this, [this]() { promptConfigurationTemplate(); });
        QAction* removeConfiguration = configurations->addAction(QStringLiteral("Delete Configuration"));
        removeConfiguration->setObjectName(QStringLiteral("renderGraphDeleteConfiguration"));
        removeConfiguration->setEnabled(CanCreateSubgraph() == true && selectedConfiguration().isEmpty() == false);
        connect(removeConfiguration, &QAction::triggered, this, [this]() {
            static_cast<void>(RemoveSelectedConfiguration());
        });
        QJsonObject selectedGraph;
        QAction* duplicateGraph = menu.addAction(QStringLiteral("Duplicate Graph..."));
        duplicateGraph->setObjectName(QStringLiteral("renderGraphDuplicateGraph"));
        duplicateGraph->setToolTip(QStringLiteral("Copy this graph file and all its configurations as a new graph in "
                                                  "the package."));
        duplicateGraph->setEnabled(CanCreateSubgraph() == true &&
                                   mDocument.GetGraph(mSelectedFile, selectedGraph) == true);
        connect(duplicateGraph, &QAction::triggered, this, &RendererWorkspace::promptDuplicateGraph);
        menu.addSeparator();
    }
    if (isNodeContext == true)
    {
        QAction* copy = menu.addAction(QStringLiteral("Copy Selected Nodes"));
        copy->setObjectName(QStringLiteral("renderGraphCopyNodes"));
        copy->setEnabled(mSelectedNodes.isEmpty() == false);
        connect(copy, &QAction::triggered, this, [this]() { static_cast<void>(CopySelectedNodes()); });
    }
    QAction* comment = menu.addAction(QStringLiteral("Add Comment Frame…"));
    comment->setObjectName(QStringLiteral("renderGraphAddComment"));
    comment->setEnabled(CanCreateSubgraph());
    const QPoint commentPoint = mCanvas->viewport()->mapFromGlobal(globalPosition);
    const QPointF commentPosition = mCanvas->mapToScene(commentPoint);
    const QString commentFile = mCanvas->GetFile();
    const QString commentConfiguration = mCanvas->GetConfiguration();
    const uint64 commentRevision = mDocument.GetRevision();
    connect(comment,
            &QAction::triggered,
            this,
            [this, commentPosition, commentFile, commentConfiguration, commentRevision]() {
                if (commentFile == mCanvas->GetFile() && commentConfiguration == mCanvas->GetConfiguration() &&
                    commentRevision == mDocument.GetRevision())
                {
                    mCanvas->GetComments().CreateFrame(commentPosition);
                }
            });
    mCanvas->AddLayoutMenu(menu);
    const QJsonObject node = isNodeContext == false || mSelectedNodes.size() > 1 ? QJsonObject{} : selectedNode();
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
    {
        const QString asset = node.value(QStringLiteral("asset")).toString();
        const QString configuration = node.value(QStringLiteral("configuration")).toString();
        QAction* open = menu.addAction(QStringLiteral("Open Subgraph"));
        open->setObjectName(QStringLiteral("renderGraphOpenSubgraph"));
        RenderGraphNodePorts ports;
        open->setEnabled(GetRenderGraphNodePorts(mDocument, node, ports) == true);
        const QString instance = node.value(QStringLiteral("id")).toString();
        connect(open, &QAction::triggered, this, [this, asset, configuration, instance]() {
            openSubgraph(asset, configuration, instance);
        });
    }
    else if (node.isEmpty() == false)
    {
        QMenu* shaders = menu.addMenu(QStringLiteral("View Shaders"));
        shaders->setObjectName(QStringLiteral("renderGraphViewShaders"));
        const QStringList sources = GetSelectedShaderSources();
        shaders->menuAction()->setVisible(sources.isEmpty() == false);
        shaders->setToolTip(QStringLiteral("Shader sources used by this pass's authored technique; scene and resource "
                                           "nodes have no shaders."));
        for (const QString& source : sources)
        {
            QAction* open = shaders->addAction(source);
            connect(open, &QAction::triggered, this, [this, source]() { openSourceLine(source, 1); });
        }
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // Canvas parity: capture is reachable here as well as from the Ray Paths panel.
    const QString kind = node.value(QStringLiteral("kind")).toString();
    const bool isRayNode = kind == QStringLiteral("RayTracingPipeline") ||
                           (kind == QStringLiteral("ComputePipeline") && IsSceneComputeNode(node) == true);
    if (mRayPathsOrNull != nullptr && (isNodeContext == false || isRayNode == true))
    {
        const QString captured = isNodeContext == true ? node.value(QStringLiteral("id")).toString() : QString{};
        QAction* capture = menu.addAction(isNodeContext == true ? QStringLiteral("Capture Ray Paths")
                                                                : QStringLiteral("Capture Ray Paths (All Ray Passes)"));
        capture->setObjectName(isNodeContext == true ? QStringLiteral("renderGraphCaptureRayPathsAction")
                                                     : QStringLiteral("renderGraphCaptureAllRayPathsAction"));
        const QString problem = mPreviewOrNull->DescribeRayPathCaptureProblem();
        capture->setToolTip(problem.isEmpty() == false
                                ? problem
                                : QStringLiteral("Click a pixel of the World viewport's graph output, then explore "
                                                 "the rays it traced with the free camera."));
        connect(capture, &QAction::triggered, this, [this, captured]() {
            static_cast<void>(BeginRayPathCapture(captured));
        });
    }
#endif
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline"))
    {
        QAction* enable = menu.addAction(QStringLiteral("Enable Scene Materials"));
        enable->setObjectName(QStringLiteral("renderGraphEnableSceneMaterialsAction"));
        const bool hasSceneMaterials =
            HasSceneRayInterface(QJsonDocument::fromJson(mDocument.GetDraft(manifestFile())).object());
        enable->setEnabled(hasSceneMaterials == false);
        enable->setToolTip(
            QStringLiteral("Upgrade the package's shared ray callbacks, preserving original shader behavior. "
                           "Undo restores the previous interface; Save publishes the change."));
        connect(enable, &QAction::triggered, this, &RendererWorkspace::enableSceneMaterials);
    }
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("ComputePipeline"))
    {
        QAction* trace = menu.addAction(QStringLiteral("Add Scene and View Inputs"));
        trace->setObjectName(QStringLiteral("renderGraphAddSceneInputsAction"));
        trace->setEnabled(IsSceneComputeNode(node) == false && CanCreateSubgraph() == true);
        trace->setToolTip(DescribeSceneComputeConversion());
        connect(trace, &QAction::triggered, this, &RendererWorkspace::makeSceneCompute);
    }
    if (HasSceneBundle(node) == true)
    {
        QAction* expand = menu.addAction(QStringLiteral("Expand Scene Bundle"));
        expand->setObjectName(QStringLiteral("renderGraphExpandSceneBundleAction"));
        expand->setCheckable(true);
        expand->setChecked(mCanvas->IsSceneBundleExpanded(mSelectedNode));
        connect(expand, &QAction::triggered, this, &RendererWorkspace::expandSceneBundle);
    }
    if (node.isEmpty() == false)
    {
        QAction* edit = menu.addAction(QStringLiteral("Show Details / Edit Node..."));
        edit->setObjectName(QStringLiteral("renderGraphEditNodeAction"));
        edit->setEnabled(CanCreateSubgraph());
        connect(edit, &QAction::triggered, this, [this]() { editNodeOnCanvas(); });
        QAction* rename = menu.addAction(QStringLiteral("Rename Node..."));
        rename->setObjectName(QStringLiteral("renderGraphRenameNodeAction"));
        rename->setEnabled(CanCreateSubgraph());
        connect(rename, &QAction::triggered, this, &RendererWorkspace::promptRenameNode);
    }
    if (CanDisableRenderGraphNode(node.value(QStringLiteral("kind")).toString()) == true)
    {
        QAction* disable = menu.addAction(QStringLiteral("Disable Node"));
        disable->setObjectName(QStringLiteral("renderGraphDisableNodeAction"));
        disable->setCheckable(true);
        disable->setChecked(IsRenderGraphNodeEnabled(node) == false);
        disable->setEnabled(CanCreateSubgraph());
        disable->setToolTip(DescribeDisabledNode(node.value(QStringLiteral("kind")).toString()) +
                            QStringLiteral(" One undoable edit, rebuilt like any other node edit."));
        connect(disable, &QAction::triggered, this, [this](bool isDisabled) {
            setSelectedNodeEnabled(isDisabled == false);
        });
    }
    if (isNodeContext == false)
    {
        QAction* editInterface = menu.addAction(QStringLiteral("Edit Inputs / Outputs..."));
        editInterface->setObjectName(QStringLiteral("renderGraphEditInterfaceAction"));
        editInterface->setEnabled(CanCreateSubgraph());
        connect(editInterface, &QAction::triggered, this, &RendererWorkspace::editConfigurationInterface);
        QAction* cleanup = menu.addAction(QStringLiteral("Remove Unused Shaders"));
        cleanup->setObjectName(QStringLiteral("renderGraphRemoveUnusedShadersAction"));
        cleanup->setEnabled(CanRemoveUnusedShaders());
        connect(cleanup, &QAction::triggered, this, [this]() { static_cast<void>(RemoveUnusedShaders()); });
        QMenu* createNodes = menu.addMenu(QStringLiteral("Create Node"));
        createNodes->setObjectName(QStringLiteral("renderGraphCreateNodeMenu"));
        createNodes->setEnabled(CanCreateSubgraph());
        const QVector<RenderGraphTemplate> entries =
            RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Node);
        for (const QString& category : GetRenderGraphTemplateCategories(RenderGraphTemplateKind::Node))
        {
            QMenu* group = createNodes->addMenu(GetRenderGraphTemplateCategoryLabel(category));
            for (int32 index = 0; index < entries.size(); ++index)
            {
                const RenderGraphTemplate& entry = entries.at(index);
                if (entry.Category != category)
                {
                    continue;
                }
                QAction* createNode = group->addAction(
                    entry.Identity == QStringLiteral("Subgraph") ? QStringLiteral("Subgraph Reference") : entry.Label);
                createNode->setData(mNewKind->itemText(index));
                createNode->setToolTip(entry.Description + QLatin1Char('\n') +
                                       mNewKind->itemData(index, Qt::ToolTipRole).toString());
                connect(createNode, &QAction::triggered, this, [this, index]() {
                    mNewKind->setCurrentIndex(index);
                    addNode();
                });
            }
        }
        QAction* create = menu.addAction(QStringLiteral("Create Subgraph..."));
        create->setObjectName(QStringLiteral("renderGraphCreateSubgraphAction"));
        create->setEnabled(CanCreateSubgraph());
        connect(create, &QAction::triggered, this, [this]() { static_cast<void>(PromptCreateSubgraph()); });
    }
    if (isNodeContext == true && mSelectedNodes.isEmpty() == false)
    {
        menu.addSeparator();
        QAction* remove = menu.addAction(mSelectedNodes.size() > 1 ? QStringLiteral("Remove Selected Nodes")
                                                                   : QStringLiteral("Remove Node"));
        remove->setObjectName(QStringLiteral("renderGraphRemoveNodeAction"));
        remove->setEnabled(true);
        connect(remove, &QAction::triggered, this, &RendererWorkspace::RemoveSelectedNodes);
        if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
        {
            QAction* removeLibrary = menu.addAction(QStringLiteral("Delete Subgraph and Library"));
            removeLibrary->setObjectName(QStringLiteral("renderGraphRemoveSubgraphLibraryAction"));
            removeLibrary->setEnabled(CanRemoveUnusedShaders());
            removeLibrary->setToolTip(
                QStringLiteral("Delete this node, its library graph unless another graph uses it, and the shader "
                               "programs and sources only that library uses, such as an applied template's files. "
                               "One undoable edit; Save publishes the deletions."));
            connect(removeLibrary, &QAction::triggered, this, [this]() {
                static_cast<void>(RemoveSelectedSubgraphLibrary());
            });
        }
    }
    if (isNodeContext == false && mCustomize->isVisible() == true)
    {
        menu.addAction(mCustomize);
    }
    menu.exec(globalPosition);
}

void RendererWorkspace::editConfigurationInterface()
{
    if (CanCreateSubgraph() == false)
    {
        mStatus->setText(
            QStringLiteral("Select a configuration and resolve external source conflicts before editing ports."));
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
        connect(&editor,
                &RenderGraphConfigurationEditor::ApplyRequested,
                this,
                [this, &editor, file, configuration, revision, epoch, packageRoot]() {
                    if (mDocument.GetRevision() != revision || mEpoch != epoch ||
                        mDocument.GetPackageRoot() != packageRoot)
                    {
                        editor.ShowError(QStringLiteral("The graph changed while this dialog was open. Cancel and "
                                                        "reopen it to edit the current interface."));
                        return;
                    }
                    QString error;
                    const RenderGraphDocumentStatus status =
                        mDocument.ReplaceConfigurationInterface(file,
                                                                configuration,
                                                                editor.GetInputs(),
                                                                editor.GetOutputs(),
                                                                error,
                                                                editor.GetInputRenames(),
                                                                editor.GetOutputRenames());
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
    const QVector<RenderGraphTemplate> nodes =
        RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Node);
    const int32 index = mNewKind->currentIndex();
    if (index < 0 || index >= nodes.size())
    {
        return;
    }
    addNodeTemplate(nodes.at(index), nullptr);
}

void RendererWorkspace::addNodeTemplate(const RenderGraphTemplate& entry,
                                        const QPoint* positionOrNull,
                                        const QString& from,
                                        const QString& input)
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
    const QString algorithm = entry.Algorithm;
    const bool isUpscalePreset = algorithm.isEmpty() == false;
    if (entry.Kind == RenderGraphTemplateKind::Graph)
    {
        const QString reason = CheckRenderGraphQuickAddGraph(mDocument, file, configuration, entry);
        if (reason.isEmpty() == false)
        {
            report(reason);
            return;
        }
    }
    const QString kind = entry.Kind == RenderGraphTemplateKind::Graph ? QStringLiteral("Subgraph") : entry.Identity;
    const QString identity = MakeNodeIdentity(entry, record);
    const QJsonObject node = MakeTemplateNode(entry, identity);
    const bool isConnected = from.isEmpty() == false && input.isEmpty() == false;
    const RenderGraphNodeConnection connection{from, identity + QLatin1Char('.') + input};
    QPoint position = positionOrNull == nullptr ? QPoint() : *positionOrNull;
    if (isConnected == true)
    {
        // Check the exact record being added with the same pre-check that offered this input.
        const RenderGraphLinkCheck check =
            CheckRenderGraphLinkToNewNode(mDocument, file, configuration, node, from, connection.To);
        if (check.IsLegal() == false)
        {
            report(QStringLiteral("Cannot connect %1 to a new %2: %3: %4").arg(from, kind, check.Code, check.Reason));
            return;
        }
        if (positionOrNull != nullptr)
        {
            position = mCanvas->GetNewNodePosition(node, from, input, QPointF(position)).toPoint();
        }
    }
    const QPoint* placementOrNull = positionOrNull == nullptr ? nullptr : &position;
    const RenderGraphNodeConnection* connectionOrNull = isConnected == true ? &connection : nullptr;
    const RenderGraphDocumentStatus status =
        isUpscalePreset == true
            ? mDocument.AddUpscalePreset(file, configuration, algorithm, identity, placementOrNull, connectionOrNull)
            : mDocument.AddNode(file, configuration, node, placementOrNull, connectionOrNull);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        report(QStringLiteral("Cannot add %1 to %2#%3: %4").arg(kind, file, configuration, DescribeStatus(status)));
        return;
    }
    mSelectedFile = file;
    mSelectedConfiguration = configuration;
    mSelectedNode = identity;
    mSelectedNodes = {identity};
    documentEdited(status);
    mViews->setCurrentIndex(CANVAS_VIEW);
    mCanvas->RevealNode(identity);
    report(isConnected == true
               ? QStringLiteral("Added %1 to %2#%3, connected from %4.").arg(identity, file, configuration, from)
               : QStringLiteral("Added %1 to %2#%3.").arg(identity, file, configuration));
}

bool RendererWorkspace::ShowQuickAdd(QPointF scenePosition)
{
    if (mDocument.IsLoaded() == false || mCanvas->GetConfiguration().isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("Open a render graph before using Quick Add."));
        return false;
    }
    mQuickAdd->SetGraphs(BuildRenderGraphQuickAddGraphs(mDocument, mCanvas->GetFile(), mCanvas->GetConfiguration()));
    mQuickAddPosition = scenePosition;
    mQuickAddSource.clear();
    mQuickAdd->Open(mCanvas->viewport()->mapToGlobal(mCanvas->mapFromScene(scenePosition)));
    return true;
}

bool RendererWorkspace::ShowConnectQuickAdd(const QString& from, QPointF inputPosition)
{
    if (mDocument.IsLoaded() == false || mCanvas->GetConfiguration().isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("Open a render graph before using Quick Add."));
        return false;
    }
    const QString file = mCanvas->GetFile();
    const QString configuration = mCanvas->GetConfiguration();
    RenderGraphQuickAddPortFilter filter{from, mCanvas->GetPortType(from, true), {}};
    QJsonObject graph;
    QJsonObject record;
    if (filter.Type.isEmpty() == false && mDocument.GetGraph(file, graph) == true)
    {
        const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
        for (const QJsonValueConstRef value : configurations)
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == configuration)
            {
                record = value.toObject();
                break;
            }
        }
    }
    if (record.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral("Cannot connect from %1: it is not an output on this canvas.").arg(from));
        return false;
    }
    const QVector<RenderGraphQuickAddGraph> graphs = BuildRenderGraphQuickAddGraphs(mDocument, file, configuration);
    mQuickAdd->SetGraphs(graphs);
    QVector<RenderGraphTemplate> candidates =
        RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Node);
    for (const RenderGraphQuickAddGraph& graphEntry : graphs)
    {
        if (graphEntry.UnavailableReason.isEmpty() == true)
        {
            candidates.append(graphEntry.Template);
        }
    }
    // Offer the first input accepted by the shared link pre-check, for nodes and graph references.
    for (const RenderGraphTemplate& entry : candidates)
    {
        const QString identity = MakeNodeIdentity(entry, record);
        const QJsonObject node = MakeTemplateNode(entry, identity);
        RenderGraphNodePorts ports;
        if (node.isEmpty() == true || GetRenderGraphNodePorts(mDocument, node, ports) == false)
        {
            continue;
        }
        for (const RenderGraphPort& port : ports.Inputs)
        {
            const QString to = identity + QLatin1Char('.') + port.Id;
            if (CheckRenderGraphLinkToNewNode(mDocument, file, configuration, node, from, to).IsLegal() == true)
            {
                filter.Inputs.insert(GetRenderGraphTemplateKey(entry), port.Id);
                break;
            }
        }
    }
    mQuickAddPosition = inputPosition;
    mQuickAddSource = from;
    mQuickAdd->Open(mCanvas->viewport()->mapToGlobal(mCanvas->mapFromScene(inputPosition)), filter);
    return true;
}

void RendererWorkspace::insertQuickAddTemplate(const RenderGraphTemplate& entry, const QString& input)
{
    if (entry.Kind == RenderGraphTemplateKind::Snippet)
    {
        // Snippets keep the Apply dialog's naming, budget confirmation and Subgraph insertion.
        promptConfigurationTemplate(entry.Identity);
        return;
    }
    const QPoint position = mQuickAddPosition.toPoint();
    addNodeTemplate(entry, &position, input.isEmpty() == true ? QString{} : mQuickAddSource, input);
}

bool RendererWorkspace::SelectNode(const QString& file, const QString& configuration, const QString& node)
{
    mSelectedFile = file;
    mSelectedConfiguration = configuration;
    mSelectedNode = node;
    mSelectedNodes = node.isEmpty() == true ? QStringList{} : QStringList{node};
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
    return mLastResult.GetDiagnosticCount();
}

QJsonObject RendererWorkspace::getDiagnosticLocation(int32 row) const
{
    return mLastResult.GetDiagnostic(row).value(QStringLiteral("location")).toObject();
}

void RendererWorkspace::openSourceLine(const QString& file, int32 line, int32 column)
{
    mViews->setCurrentIndex(SOURCE_VIEW);
    mSourceFile->setCurrentText(file);
    renderSource();
    mSource->GoToLine(line, column);
}

bool RendererWorkspace::ActivateDiagnostic(int32 row)
{
    if (row < 0 || row >= GetDiagnosticCount())
    {
        return false;
    }
    updateCanvasDiagnostics();
    const QJsonObject diagnostic = mLastResult.GetDiagnostic(row);
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
        mCanvas->GetDiagnostics().SetCurrentRow(row);
        openSourceLine(source,
                       location.value(QStringLiteral("line")).toInt(),
                       location.value(QStringLiteral("column")).toInt());
        return true;
    }
    const QJsonObject target = ResolveRenderGraphDiagnosticTarget(mDocument, location);
    if (target.isEmpty() == true)
    {
        mStatus->setText(QStringLiteral(
            "This diagnostic's canvas target is no longer available. See Error List or validate again."));
        return false;
    }
    mCanvas->GetDiagnostics().SetCurrentRow(row);
    mCanvas->GetHistory().PrepareDiagnosticVisit();
    mViews->setCurrentIndex(CANVAS_VIEW);
    const QString node = target.value(QStringLiteral("node")).toString();
    const bool isInterface = node == QStringLiteral("input") || node == QStringLiteral("#outputs");
    const bool isSelected = SelectNode(target.value(QStringLiteral("source")).toString(),
                                       target.value(QStringLiteral("configuration")).toString(),
                                       isInterface == true ? QString() : node);
    const bool isWarning = diagnostic.value(QStringLiteral("severity"))
                               .toString()
                               .compare(QLatin1StringView("warning"), Qt::CaseInsensitive) == 0;
    QString description = (isWarning == true ? QStringLiteral("Warning: ") : QStringLiteral("Error: ")) +
                          DescribeRenderGraphDiagnostic(diagnostic);
    const RenderGraphDiagnosticFreshness freshness = mCanvas->GetDiagnostics().GetFreshness();
    if (freshness == RenderGraphDiagnosticFreshness::Stale)
    {
        description.prepend(
            QStringLiteral("This report describes an earlier source version. Validate again after edits.\n"));
    }
    else if (freshness == RenderGraphDiagnosticFreshness::Unverified)
    {
        description.prepend(
            QStringLiteral("This report has no captured source identity; its freshness is unverified.\n"));
    }
    mStatus->setText(description);
    if (isSelected == true)
    {
        static_cast<void>(mCanvas->ShowDiagnostic(target, description, isWarning));
        const QString field = location.value(QStringLiteral("field")).toString();
        if (field.isEmpty() == false && selectedNode().isEmpty() == false)
        {
            editNodeOnCanvas(field);
        }
    }
    return isSelected;
}

QString RendererWorkspace::GetSourceFile() const
{
    return mSourceFile->currentText();
}

int32 RendererWorkspace::GetSourceLine() const
{
    return mSource->textCursor().blockNumber() + 1;
}

int32 RendererWorkspace::GetSourceColumn() const
{
    const QTextCursor cursor = mSource->textCursor();
    const QString prefix = cursor.block().text().left(cursor.positionInBlock());
    return static_cast<int32>(prefix.toUcs4().size()) + 1;
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
    mCanvas->SelectNodes(mSelectedNodes);
    mSelectedNodes = mCanvas->GetSelectedNodes();
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

void RendererWorkspace::openSubgraph(const QString& asset, const QString& configuration, const QString& instance)
{
    if (mDocument.GetFiles().contains(asset) == false)
    {
        mStatus->setText(QStringLiteral("Subgraph source %1 is not part of this package.").arg(asset));
        return;
    }
    if (mCanvas->GetHistory().PrepareSubgraph(instance, asset, configuration) == false)
    {
        return;
    }
    // Navigate canvas, tree and Source tab to the referenced asset's configuration.
    mSelectedFile = asset;
    mSelectedConfiguration = configuration;
    mSelectedNode.clear();
    mSelectedNodes.clear();
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
    mPreviewPump.Stop();
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
    mCanvas->SetReferenceSize({});
    mPreviewDeviceSerial = 0;
    mPreviewSceneIdentity.clear();
    mArePreviewInputsDirty = true;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mPreviewSourceOrNull != nullptr && mPreviewOrNull != nullptr)
    {
        mPreviewOrNull->SetForceRaster(mPreviewSourceOrNull->ShouldForceRaster());
        connect(mPreviewSourceOrNull, &RenderPreview::RendererPreferenceChanged, this, [this](bool shouldForceRaster) {
            mPreviewOrNull->SetForceRaster(shouldForceRaster);
            if (mPreviewSourceOrNull->HasDevice() == true)
            {
                mPreviewPump.Start();
            }
        });
        // Release generations before RenderPreview destroys the shared device.
        connect(mPreviewSourceOrNull, &RenderPreview::DeviceStopping, this, [this]() {
            mPreviewPump.Stop();
            mPreviewOrNull->Detach();
            mPreviewDeviceSerial = 0;
            if (mIsDefaultPreview == true)
            {
                prepareDefaultPreview();
            }
        });
    }
#endif
    if (mPreviewSourceOrNull != nullptr)
    {
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
        mCanvas->SetReferenceSize(mPreviewSourceOrNull->GetDrawableSize());
        connect(mPreviewSourceOrNull, &RenderPreview::DrawableSizeChanged, this, [this](const QSize& size) {
            mCanvas->SetReferenceSize(size);
            mArePreviewInputsDirty = true;
        });
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        connect(mPreviewSourceOrNull, &RenderPreview::NavigationChanged, this, [this]() {
            mArePreviewInputsDirty = true;
            mPreviewPump.Wake();
        });
        connect(mPreviewSourceOrNull, &RenderPreview::Changed, this, [this]() {
            mArePreviewInputsDirty = true;
            if (mPreviewSourceOrNull->HasDevice() == true)
            {
                mPreviewPump.Start();
                mPreviewPump.Wake();
            }
        });
        if (mPreviewSourceOrNull->HasDevice() == true)
        {
            mPreviewPump.Start();
        }
#endif
        connect(mPreviewSourceOrNull, &QObject::destroyed, this, [this]() {
            mPreviewPump.Stop();
            mPreviewSourceOrNull = nullptr;
            mCanvas->SetReferenceSize({});
            mPreviewDeviceSerial = 0;
        });
    }
    renderPreview();
}

bool RendererWorkspace::PreviewRuntime(RenderGraphSource source)
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
    const QString runtime = source == RenderGraphSource::Saved && mDocument.IsBuiltIn() == true
                                ? GetDefaultRenderGraphRuntime()
                                : GetPreviewPackagePath();
    if (runtime.isEmpty() == true)
    {
        return false;
    }
    const bool isPreviousPreviewDefault = mIsDefaultPreview;
    mIsDefaultPreview = runtime == GetDefaultRenderGraphRuntime();
    RenderGraphPreviewSource provenance;
    if (mHasResult == true && mLastResult.IsSuccessful == true &&
        (mLastResult.Request.Action == QStringLiteral("build") ||
         mLastResult.Request.Action == QStringLiteral("cook-runtime")) &&
        mLastResult.Request.InputIdentity.isEmpty() == false &&
        mLastResult.Request.InputIdentity == getBuildIdentity(source) && mLastResult.Request.Source == source)
    {
        provenance.SourceRevision = mLastResult.Request.Revision;
        const QByteArray rootBytes = source == RenderGraphSource::Saved ? mDocument.GetSaved(mDocument.GetRootGraph())
                                                                        : mDocument.GetDraft(mDocument.GetRootGraph());
        const QJsonObject root = QJsonDocument::fromJson(rootBytes).object();
        provenance.Configuration = mConfiguration->currentText() == DEFAULT_CONFIGURATION
                                       ? root.value(QStringLiteral("default")).toString()
                                       : mConfiguration->currentText();
        provenance.InputIdentity = ComputeRenderGraphBuildIdentity(mDocument, provenance.Configuration, source);
        provenance.RootFile = mDocument.GetRootGraph();
        const QJsonObject runtimeResult = mLastResult.Payload.value(QStringLiteral("runtime")).toObject();
        if (QDir::cleanPath(runtimeResult.value(QStringLiteral("runtime_package")).toString()) ==
                QDir::cleanPath(runtime) &&
            runtimeResult.value(QStringLiteral("configuration")).toString() == provenance.Configuration)
        {
            provenance.Generation = runtimeResult.value(QStringLiteral("generation")).toString().toLatin1();
            provenance.RuntimeIdentity = runtimeResult.value(QStringLiteral("sha256")).toString().toLatin1();
        }
    }
    const RenderGraphPreviewLoad loaded = mPreviewOrNull->Load(runtime, GetDefaultRenderGraphRuntime(), provenance);
    if (loaded == RenderGraphPreviewLoad::Staged || loaded == RenderGraphPreviewLoad::Waiting)
    {
        mPreviewInputIdentity = getBuildIdentity(source);
        mPreviewSourceVersion = source;
        setProblem(mBuildProblem, {});
    }
    else
    {
        mIsDefaultPreview = isPreviousPreviewDefault;
        setProblem(mBuildProblem, mPreviewOrNull->GetDisplayStatus());
    }
    if (mPreviewSourceOrNull != nullptr && mPreviewSourceOrNull->HasDevice() == true)
    {
        mPreviewPump.Start();
    }
    ServicePreview();
    return loaded == RenderGraphPreviewLoad::Staged || loaded == RenderGraphPreviewLoad::Waiting;
#else
    static_cast<void>(source);
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
    LUDUS_PROFILE_SCOPE(EditorGraphService);
    RenderGraphPreview& preview = *mPreviewOrNull;
    const ScopedViewportService traceService(
        preview.GetFrameTrace(),
        mPreviewSourceOrNull != nullptr ? mPreviewSourceOrNull->GetNextProfileFrameId() : 0,
        mPreviewSourceOrNull != nullptr ? mPreviewSourceOrNull->GetDeviceSerial() : 0);
    QElapsedTimer preparationTimer;
    preparationTimer.start();
    if (mPreviewSourceOrNull != nullptr && mPreviewSourceOrNull->HasDevice() == true &&
        (preview.IsAttached() == false || mPreviewSourceOrNull->GetDeviceSerial() != mPreviewDeviceSerial))
    {
        preview.Detach();
        if (preview.Attach(mPreviewSourceOrNull->GetDevice(), mPreviewSourceOrNull->GetSurface()) == true)
        {
            mPreviewDeviceSerial = mPreviewSourceOrNull->GetDeviceSerial();
            mPreviewSceneIdentity.clear();
            mArePreviewInputsDirty = true;
        }
    }
    if (mPreviewSourceOrNull != nullptr)
    {
        preview.SetReferenceSize(mPreviewSourceOrNull->GetDrawableSize());
    }
    const QSize target = preview.GetTargetSize();
    mArePreviewInputsDirty = mArePreviewInputsDirty == true || target != mPreviewTargetSize;
    mPreviewTargetSize = target;
    if (mPreviewSourceOrNull != nullptr && preview.IsAttached() == true && target.isEmpty() == false &&
        mArePreviewInputsDirty == true)
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
            setProblem(mExtractionProblem, error);
        }
        mArePreviewInputsDirty = false;
        if (hasView == true && (scene.Identity != mPreviewSceneIdentity ||
                                AreMatricesEqual(view.WorldToClip, mPreviewWorldToClip) == false))
        {
            LUDUS_PROFILE_SCOPE(EditorGraphWorldExtraction);
            bool hasUpdatedInputs = false;
            if (scene.Identity == mPreviewSceneIdentity)
            {
                preview.SetCamera(view, ++mPreviewRevision);
                hasUpdatedInputs = true;
            }
            else
            {
                RenderGraphExtraction extraction;
                if (MakeRenderGraphExtraction(scene, view, ++mPreviewRevision, extraction, error) == true)
                {
                    setProblem(mExtractionProblem, {});
                    extraction.ContentRevision = mPreviewRevision;
                    preview.SetExtraction(extraction);
                    hasUpdatedInputs = true;
                }
                else
                {
                    setProblem(mExtractionProblem, error);
                    mPreviewStatus->setText(error);
                }
            }
            if (hasUpdatedInputs == true)
            {
                mPreviewSceneIdentity = scene.Identity;
                mPreviewWorldToClip = view.WorldToClip;
            }
        }
    }
    constexpr foundation::float64 NANOSECONDS_PER_MILLISECOND = 1'000'000.0;
    preview.Tick(static_cast<foundation::float64>(preparationTimer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND);
    mPreviewPump.SetProducerPending(preview.HasPendingWork());
    if (mPreviewSourceOrNull == nullptr || mPreviewSourceOrNull->HasDevice() == false ||
        preview.GetTargetSize().isEmpty() == true)
    {
        mPreviewPump.Stop();
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
            (mPreviewInputIdentity == getBuildIdentity(mPreviewSourceVersion) ||
             (mIsDefaultPreview == true && mPreviewInputIdentity.isEmpty() == true)))
        {
            setProblem(mActivationProblem, mPreviewOrNull->GetDisplayStatus());
        }
        else if (candidate == graphics::renderer::RenderGraphCandidateState::Preparing ||
                 candidate == graphics::renderer::RenderGraphCandidateState::NeedsUpdate ||
                 (candidate == graphics::renderer::RenderGraphCandidateState::None &&
                  mPreviewOrNull->GetReport().ActiveGeneration != 0))
        {
            setProblem(mActivationProblem, {});
        }
        static_cast<PreviewImage*>(mPreviewImage)->SetImage(mPreviewOrNull->GetImage());
        const QString previewStatus = mPreviewOrNull->GetStatus();
        if (mPreviewStatus->text() != previewStatus)
        {
            mPreviewStatus->setText(previewStatus);
        }
        const bool hasPresentedCatalogue = mPreviewOrNull->HasPresentedCatalogue();
        const bool isPreparing = candidate == graphics::renderer::RenderGraphCandidateState::Preparing ||
                                 candidate == graphics::renderer::RenderGraphCandidateState::NeedsUpdate;
        // Camera revisions change the preview's detailed status every frame. Source currency
        // depends on publication, not navigation; avoid hashing the source package for each frame.
        if (mSourcePresentedGeneration != mPreviewOrNull->GetPresentedGeneration() ||
            mHasSourcePresentedCatalogue != hasPresentedCatalogue || mIsSourcePreviewPreparing != isPreparing)
        {
            mSourcePresentedGeneration = mPreviewOrNull->GetPresentedGeneration();
            mHasSourcePresentedCatalogue = hasPresentedCatalogue;
            mIsSourcePreviewPreparing = isPreparing;
            renderSourceStatus();
        }
        mPreviewButton->setEnabled(mDocument.IsLoaded());
        return;
    }
#endif
    mPreviewButton->setEnabled(false);
    mPreviewStatus->setText(QStringLiteral("The render-graph preview needs the native editor built with "
                                           "LUDUS_EDITOR_RENDER_PREVIEW and pinned shader tools."));
}
} // namespace ludus::editor
