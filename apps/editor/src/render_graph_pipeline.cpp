#include "internal/render_graph_pipeline.h"
#include "design_tokens.h"

#include <ludus/foundation/base/core.h>

#include <ludus/content/render_graph_format/plan.h>

#include <algorithm>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QMouseEvent>
#include <QObject>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_PORTS = 8;
constexpr int32 PAIRED_INPUT_ROLE = Qt::UserRole + 1;
constexpr int32 MAX_SHADERS = 16;
constexpr int32 MAX_PROPERTY_VALUE = 65535;
constexpr int32 DIMENSION_COUNT = 3;
constexpr int32 MAX_RAY_RECURSION = 31;
constexpr int32 MAX_RAY_INTERFACE_BYTES = 4096;
constexpr int32 AUTOMATIC_RAY_LIMIT = -1;
/// An assigned-shader chip shows at most this many characters' width; its tooltip carries the whole program name.
constexpr int32 SHADER_CHIP_MAX_CHARACTERS = 24;

/// Items left to right, wrapping onto a new row when the next one would not fit, so a narrow Properties dock shows
/// every part button instead of truncating them to "Read... Writable R...".
class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parentOrNull, int32 spacingPixels) : QLayout(parentOrNull)
    {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(spacingPixels);
    }
    ~FlowLayout() override
    {
        for (QLayoutItem* item : mItems)
        {
            delete item;
        }
    }
    void addItem(QLayoutItem* item) override
    {
        mItems.append(item);
    }
    int count() const override
    {
        return static_cast<int>(mItems.size());
    }
    QLayoutItem* itemAt(int index) const override
    {
        return mItems.value(index, nullptr);
    }
    QLayoutItem* takeAt(int index) override
    {
        return index >= 0 && index < mItems.size() ? mItems.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override
    {
        return {};
    }
    bool hasHeightForWidth() const override
    {
        return true;
    }
    int heightForWidth(int width) const override
    {
        return arrange(QRect(0, 0, width, 0), true);
    }
    void setGeometry(const QRect& rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }
    QSize sizeHint() const override
    {
        return minimumSize();
    }
    QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem* item : mItems)
        {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }

private:
    /// Place every item, or only measure when isTestOnly, and return the height used.
    int arrange(const QRect& rect, bool isTestOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect inner = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = inner.x();
        int y = inner.y();
        int rowHeight = 0;
        for (QLayoutItem* item : mItems)
        {
            // An item wider than the row takes the row, and its widget clips or elides.
            const QSize hint = item->sizeHint().boundedTo(QSize(inner.width(), item->sizeHint().height()));
            if (x > inner.x() && x + hint.width() > inner.right() + 1)
            {
                x = inner.x();
                y += rowHeight + spacing();
                rowHeight = 0;
            }
            if (isTestOnly == false)
            {
                item->setGeometry(QRect(QPoint(x, y), hint));
            }
            x += hint.width() + spacing();
            rowHeight = std::max(rowHeight, hint.height());
        }
        return y + rowHeight - rect.y() + margins.bottom();
    }
    QList<QLayoutItem*> mItems;
};

QJsonArray ReadRows(const QTableWidget& table, const QStringList& fields)
{
    QJsonArray rows;
    for (int32 row = 0; row < table.rowCount(); ++row)
    {
        QJsonObject record;
        for (int32 column = 0; column < fields.size(); ++column)
        {
            const QTableWidgetItem* itemOrNull = table.item(row, column);
            const QComboBox* choicesOrNull = qobject_cast<QComboBox*>(table.cellWidget(row, column));
            const QString value = choicesOrNull != nullptr ? choicesOrNull->currentText()
                                  : itemOrNull == nullptr  ? QString()
                                                           : itemOrNull->text();
            if (fields.at(column) == QStringLiteral("access"))
            {
                if (choicesOrNull != nullptr && choicesOrNull->currentData().isValid() == true)
                {
                    record.insert(fields.at(column), choicesOrNull->currentData().toString());
                }
                continue;
            }
            record.insert(fields.at(column),
                          fields.at(column) == QStringLiteral("required")
                              ? QJsonValue(itemOrNull != nullptr && itemOrNull->checkState() == Qt::Checked)
                              : QJsonValue(value));
        }
        rows.append(record);
    }
    return rows;
}

QString GetPairedInput(const QJsonObject& node, const QJsonObject& resource)
{
    const QString target = resource.value(QStringLiteral("target")).toString();
    for (const QJsonValueConstRef input : node.value(QStringLiteral("inputs")).toArray())
    {
        if (input.toObject().value(QStringLiteral("id")).toString() == target &&
            input.toObject().value(QStringLiteral("type")) == resource.value(QStringLiteral("type")))
        {
            return target;
        }
    }
    // A stale/missing association becomes a same-name pair on Apply, preserving a matching existing wire.
    return resource.value(QStringLiteral("id")).toString();
}

QJsonArray GetReadInputs(const QJsonObject& node)
{
    const QJsonArray resources = node.value(QStringLiteral("outputs")).toArray();
    QJsonArray inputs;
    for (const QJsonValueConstRef input : node.value(QStringLiteral("inputs")).toArray())
    {
        bool isPaired = false;
        for (const QJsonValueConstRef resource : resources)
        {
            isPaired =
                isPaired == true ||
                (GetPairedInput(node, resource.toObject()) == input.toObject().value(QStringLiteral("id")).toString() &&
                 resource.toObject().value(QStringLiteral("type")) == input.toObject().value(QStringLiteral("type")));
        }
        if (isPaired == false)
        {
            inputs.append(input);
        }
    }
    return inputs;
}

QString GetResourceFormat(const QString& type)
{
    return type == QStringLiteral("ColorTexture")   ? QStringLiteral("RGBA8Unorm")
           : type == QStringLiteral("DepthTexture") ? QStringLiteral("Depth32Float")
                                                    : QStringLiteral("Bytes");
}

QString ValidatePorts(const QJsonArray& ports, const QString& direction)
{
    if (ports.size() > MAX_PORTS)
    {
        return QStringLiteral("Pipeline has %1 %2 ports; at most %3 are supported. Remove a read input or writable "
                              "resource before continuing.")
            .arg(ports.size())
            .arg(direction)
            .arg(MAX_PORTS);
    }
    QStringList identities;
    const QRegularExpression identifier(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    for (const QJsonValueConstRef value : ports)
    {
        const QString identity = value.toObject().value(QStringLiteral("id")).toString();
        if (identifier.match(identity).hasMatch() == false)
        {
            return QStringLiteral("%1 port '%2' has an invalid name. Use 1–63 ASCII letters, digits, '_' or '-', "
                                  "starting with a letter.")
                .arg(direction, identity);
        }
        if (identities.contains(identity) == true)
        {
            return QStringLiteral("%1 port '%2' is declared more than once. Give each port a unique name. If this "
                                  "resource is in Writable Resources, remove its duplicate row from Read Inputs.")
                .arg(direction, identity);
        }
        identities.append(identity);
    }
    for (const QJsonValueConstRef value : ports)
    {
        const QJsonObject port = value.toObject();
        if (port.value(QStringLiteral("type")).toString() != QStringLiteral("Scene"))
        {
            continue;
        }
        for (const QString& part : {QStringLiteral("geometry"), QStringLiteral("materials"), QStringLiteral("lights")})
        {
            const QString member = port.value(QStringLiteral("id")).toString() + QLatin1Char('_') + part;
            if (identities.contains(member) == true)
            {
                return QStringLiteral("Scene member ID %1 is reserved by its bundle.").arg(member);
            }
        }
    }
    return {};
}

QStringList GetCellChoices(const QString& field, const QString& kind, const QString& table)
{
    if (field == QStringLiteral("type"))
    {
        QStringList types{QStringLiteral("ColorTexture"), QStringLiteral("DepthTexture"), QStringLiteral("Buffer")};
        if (table == QStringLiteral("inputs"))
        {
            types += {QStringLiteral("Scene"),
                      QStringLiteral("SceneGeometry"),
                      QStringLiteral("SceneMaterials"),
                      QStringLiteral("SceneLights"),
                      QStringLiteral("SceneBakedLighting"),
                      QStringLiteral("View"),
                      QStringLiteral("AccelerationStructure"),
                      QStringLiteral("Float32"),
                      QStringLiteral("Float2"),
                      QStringLiteral("Float3"),
                      QStringLiteral("Float4"),
                      QStringLiteral("UInt32")};
        }
        return types;
    }
    if (field == QStringLiteral("load"))
    {
        return {QStringLiteral("Clear"), QStringLiteral("Load"), QStringLiteral("Discard")};
    }
    if (field == QStringLiteral("store"))
    {
        return {QStringLiteral("Store"), QStringLiteral("Discard")};
    }
    if (field == QStringLiteral("format"))
    {
        return {QStringLiteral("RGBA8Unorm"), QStringLiteral("Depth32Float"), QStringLiteral("Bytes")};
    }
    if (field == QStringLiteral("stage"))
    {
        return GetPipelineShaderStages(kind);
    }
    return {};
}

QString GetNextShaderStage(const QTableWidget& table, const QString& kind)
{
    QStringList stages = GetPipelineShaderStages(kind);
    if (kind == QStringLiteral("RayTracingPipeline"))
    {
        return stages.value(0);
    }
    for (int32 row = 0; row < table.rowCount(); ++row)
    {
        const QComboBox* stage = qobject_cast<QComboBox*>(table.cellWidget(row, 1));
        LUDUS_REQUIRE(stage != nullptr);
        if (kind == QStringLiteral("ComputePipeline"))
        {
            if (stage->currentText() == QStringLiteral("compute"))
            {
                return {};
            }
            stages.removeAll(QStringLiteral("compute"));
        }
        stages.removeAll(stage->currentText());
    }
    return stages.value(0);
}

void AddCell(QTableWidget& inoutTable,
             int32 row,
             int32 column,
             const QString& field,
             const QString& value,
             const QString& kind)
{
    if (field == QStringLiteral("access"))
    {
        QComboBox* access = new QComboBox(&inoutTable);
        access->setObjectName(QStringLiteral("renderGraphWritableTextureAccess"));
        access->setAccessibleName(QStringLiteral("Texture access"));
        access->addItem(QStringLiteral("Store returned RGB"));
        access->addItem(QStringLiteral("Shader read/write"), QStringLiteral("ShaderReadWrite"));
        access->setCurrentIndex(value == QStringLiteral("ShaderReadWrite") ? 1 : 0);
        access->setToolTip(QStringLiteral("Store returned RGB at the launch pixel, or bind this texture by its "
                                          "input port name for shader reads and writes."));
        inoutTable.setCellWidget(row, column, access);
        return;
    }
    const QStringList choices = GetCellChoices(field, kind, inoutTable.objectName());
    if (choices.isEmpty() == false)
    {
        QComboBox* control = new QComboBox(&inoutTable);
        control->addItems(choices);
        if (choices.contains(value) == false)
        {
            control->addItem(value);
        }
        control->setCurrentText(value);
        inoutTable.setCellWidget(row, column, control);
        return;
    }
    QTableWidgetItem* item = new QTableWidgetItem(value);
    if (field == QStringLiteral("required"))
    {
        item->setText({});
        item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        item->setCheckState(value == QStringLiteral("true") ? Qt::Checked : Qt::Unchecked);
    }
    inoutTable.setItem(row, column, item);
}
} // namespace

RenderGraphPipelineEditor::RenderGraphPipelineEditor(const QJsonObject& node, QWidget* parentOrNull)
    : QWidget(parentOrNull), mNode(node),
      mInputs(createTable(QStringLiteral("inputs"),
                          {QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("required")},
                          {QStringLiteral("input"), QStringLiteral("ColorTexture"), QStringLiteral("true")})),
      mOutputs(createTable(QStringLiteral("outputs"),
                           QStringList{QStringLiteral("id"),
                                       QStringLiteral("type"),
                                       QStringLiteral("format"),
                                       QStringLiteral("load"),
                                       QStringLiteral("store")} +
                               (UsesRayLaunch(node) == true ? QStringList{QStringLiteral("access")} : QStringList{}),
                           QStringList{QStringLiteral("resource"),
                                       QStringLiteral("ColorTexture"),
                                       QStringLiteral("RGBA8Unorm"),
                                       QStringLiteral("Clear"),
                                       QStringLiteral("Store")} +
                               (UsesRayLaunch(node) == true ? QStringList{QString()} : QStringList{}))),
      mShaders(createTable(QStringLiteral("shaders"),
                           {QStringLiteral("id"), QStringLiteral("stage"), QStringLiteral("program")},
                           {QStringLiteral("shader"),
                            node.value(QStringLiteral("kind")).toString() == QStringLiteral("GraphicsPipeline")
                                ? QStringLiteral("vertex")
                            : node.value(QStringLiteral("kind")).toString() == QStringLiteral("ComputePipeline")
                                ? QStringLiteral("compute")
                                : QStringLiteral("ray_generation"),
                            QStringLiteral("Program")}))
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    // The editor sits inside a form or dialog that already has margins; its own would only narrow the content.
    layout->setContentsMargins(0, 0, 0, 0);
    QTabWidget* tabs = new QTabWidget(this);
    mTabs = tabs;
    mTabs->setObjectName(QStringLiteral("renderGraphPipelineParts"));
    // A narrow dialog or dock scrolls the part tabs instead of growing to fit every one of them.
    mTabs->setUsesScrollButtons(true);
    for (QTableWidget* table : {mInputs, mOutputs, mShaders})
    {
        const QString name = table->objectName();
        tabs->addTab(table->parentWidget(),
                     name == QStringLiteral("outputs")  ? QStringLiteral("Writable Resources")
                     : name == QStringLiteral("inputs") ? QStringLiteral("Read Inputs")
                                                        : QStringLiteral("Shaders"));
        for (int32 row = 0; row < table->rowCount(); ++row)
        {
            connectChoices(*table, row);
        }
        connect(table, &QTableWidget::cellChanged, this, [this]() { publishEdit(); });
    }
    if (UsesRayLaunch(mNode) == true &&
        mNode.value(QStringLiteral("invocation")).toObject().value(QStringLiteral("storage_texture")).toBool() == true)
    {
        addColorOutputEditor();
    }
    for (const QString& name : {QStringLiteral("state"), QStringLiteral("invocation")})
    {
        QWidget* panel = new QWidget(tabs);
        QFormLayout* form = new QFormLayout(panel);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        addProperties(*form, name);
        tabs->addTab(panel,
                     name == QStringLiteral("state") ? QStringLiteral("Pipeline State") : QStringLiteral("Invocation"));
    }
    // Each part explains itself inside its own page, so a paragraph shows only with the part it describes.
    QLabel* help = new QLabel(QStringLiteral("Each writable resource creates a paired input and output. Connect the "
                                             "texture/buffer to its input and use its output for the updated resource. "
                                             "Target associations are automatic."),
                              mOutputs->parentWidget());
    help->setObjectName(QStringLiteral("renderGraphWritableResourcesHelp"));
    help->setWordWrap(true);
    static_cast<QVBoxLayout*>(mOutputs->parentWidget()->layout())->insertWidget(0, help);
    layout->addWidget(tabs);
    mError = new QLabel(this);
    mError->setObjectName(QStringLiteral("renderGraphPipelineError"));
    mError->setWordWrap(true);
    layout->addWidget(mError);
    QLabel* hint = new QLabel(
        QStringLiteral("Choose the language for new shaders. LUSL (.lusl) is the default. "
                       "Renaming moves its source; stage edits reuse it. Edit sources in Source; Save writes files."),
        mShaders->parentWidget());
    hint->setObjectName(QStringLiteral("renderGraphShadersHelp"));
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline"))
    {
        hint->setText(hint->text() +
                      QStringLiteral(" New ray techniques expose materials through EvaluateMaterial(scene, hit). "
                                     "For legacy ray pipelines, use Enable Scene Materials on the canvas."));
    }
    else if (IsSceneComputeNode(node) == true)
    {
        hint->setText(
            hint->text() +
            QStringLiteral(" Scene compute shaders implement float3 Name(SceneContext scene, RayLaunch "
                           "launch, inout RayQueryContext queries) and trace with TraceClosest or "
                           "TestVisibility. Alternatively assign ray_generation, miss and closest_hit programs "
                           "from a ray pipeline; TraceAndShade then invokes those callbacks."));
    }
    hint->setWordWrap(true);
    // Under the program-scope note at the top of the Shaders part.
    static_cast<QVBoxLayout*>(mShaders->parentWidget()->layout())->insertWidget(1, hint);
}

void RenderGraphPipelineEditor::AddOptionsEditor(QWidget* editor)
{
    LUDUS_REQUIRE(editor != nullptr);
    mTabs->addTab(editor, QStringLiteral("Public Options"));
}

void RenderGraphPipelineEditor::ShowParts()
{
    constexpr int32 SHADER_TAB = 2;
    if (mPartsOrNull != nullptr)
    {
        mPartsOrNull->hide();
        mPartsOrNull->setParent(&mRetiredParts);
        mPartsOrNull->deleteLater();
    }
    // The parts are one row of text buttons in the editor's own style, and the assigned shaders a second, quieter
    // row of chips beneath it. Neither carries a file or folder icon: they are sections of one form, not files.
    QWidget* overview = new QWidget(this);
    overview->setObjectName(QStringLiteral("renderGraphNodeParts"));
    mPartsOrNull = overview;
    QVBoxLayout* parts = new QVBoxLayout(overview);
    parts->setContentsMargins(0, 0, 0, 0);
    parts->setSpacing(design::SPACING_SMALL);
    FlowLayout* partRow = new FlowLayout(nullptr, design::SPACING_EXTRA_SMALL);
    QButtonGroup* selection = new QButtonGroup(overview);
    selection->setExclusive(true);
    for (int32 index = 0; index < mTabs->count(); ++index)
    {
        QToolButton* part = new QToolButton(overview);
        part->setObjectName(QStringLiteral("renderGraphPart_") + QString::number(index));
        part->setText(mTabs->tabText(index));
        part->setToolButtonStyle(Qt::ToolButtonTextOnly);
        part->setCheckable(true);
        part->setChecked(index == mTabs->currentIndex());
        part->setToolTip(QStringLiteral("Edit %1 for this pipeline").arg(mTabs->tabText(index)));
        selection->addButton(part, index);
        partRow->addWidget(part);
    }
    parts->addLayout(partRow);
    const QJsonArray shaders = mNode.value(QStringLiteral("shaders")).toArray();
    if (shaders.isEmpty() == false)
    {
        FlowLayout* shaderRow = new FlowLayout(nullptr, design::SPACING_EXTRA_SMALL);
        QLabel* caption = new QLabel(QStringLiteral("Assigned shaders"), overview);
        caption->setForegroundRole(QPalette::PlaceholderText);
        shaderRow->addWidget(caption);
        for (int32 index = 0; index < shaders.size(); ++index)
        {
            const QJsonObject shader = shaders.at(index).toObject();
            QToolButton* part = new QToolButton(overview);
            part->setObjectName(QStringLiteral("renderGraphShaderPart_") +
                                shader.value(QStringLiteral("id")).toString());
            part->setText(part->fontMetrics().elidedText(
                QStringLiteral("%1 · %2").arg(shader.value(QStringLiteral("stage")).toString(),
                                              shader.value(QStringLiteral("program")).toString()),
                Qt::ElideMiddle,
                part->fontMetrics().averageCharWidth() * SHADER_CHIP_MAX_CHARACTERS));
            part->setToolTip(
                QStringLiteral("Program: %1\nClick to edit the assignment; double-click to open the shader source")
                    .arg(shader.value(QStringLiteral("program")).toString()));
            part->setProperty("renderGraphShaderId", shader.value(QStringLiteral("id")).toString());
            part->installEventFilter(this);
            part->setToolButtonStyle(Qt::ToolButtonTextOnly);
            part->setAutoRaise(true);
            shaderRow->addWidget(part);
            connect(part, &QToolButton::clicked, this, [this, index]() {
                mTabs->setCurrentIndex(SHADER_TAB);
                mShaders->setCurrentCell(index, 2);
                mShaders->setFocus(Qt::OtherFocusReason);
            });
        }
        parts->addLayout(shaderRow);
    }
    connect(selection, &QButtonGroup::idClicked, mTabs, &QTabWidget::setCurrentIndex);
    connect(mTabs, &QTabWidget::currentChanged, selection, [selection](int32 index) {
        if (selection->button(index) != nullptr)
        {
            selection->button(index)->setChecked(true);
        }
    });
    mTabs->tabBar()->hide();
    static_cast<QVBoxLayout*>(layout())->insertWidget(0, overview);
}

bool RenderGraphPipelineEditor::eventFilter(QObject* watched, QEvent* event)
{
    LUDUS_REQUIRE(watched != nullptr && event != nullptr);
    if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
    {
        const QString shaderId = watched->property("renderGraphShaderId").toString();
        if (shaderId.isEmpty() == false)
        {
            Q_EMIT ShaderOpenRequested(shaderId);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void RenderGraphPipelineEditor::AddSceneQueryBudgets(const RenderGraphSceneQueryBudgets& budgets)
{
    QGroupBox* group = new QGroupBox(QStringLiteral("Scene query budgets"), this);
    group->setObjectName(QStringLiteral("renderGraphSceneQueryBudgets"));
    QFormLayout* form = new QFormLayout(group);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    QSpinBox* surface = new QSpinBox(group);
    surface->setObjectName(QStringLiteral("renderGraphSurfaceQueries"));
    surface->setRange(RenderGraphSceneQueryBudgets::MIN_SURFACE_QUERIES,
                      RenderGraphSceneQueryBudgets::MAX_SURFACE_QUERIES);
    surface->setValue(budgets.SurfaceQueries);
    surface->setKeyboardTracking(false);
    surface->setToolTip(QStringLiteral("Maximum closest-hit queries per camera sample, including the camera ray. "
                                       "Camera plus one BSDF ray for MIS requires at least two."));
    form->addRow(QStringLiteral("Surface queries"), surface);
    QSpinBox* visibility = new QSpinBox(group);
    visibility->setObjectName(QStringLiteral("renderGraphVisibilityQueries"));
    visibility->setRange(RenderGraphSceneQueryBudgets::MIN_VISIBILITY_QUERIES,
                         RenderGraphSceneQueryBudgets::MAX_VISIBILITY_QUERIES);
    visibility->setValue(budgets.VisibilityQueries);
    visibility->setKeyboardTracking(false);
    visibility->setToolTip(
        QStringLiteral("Maximum shadow visibility queries per camera sample. "
                       "Zero permits no visibility queries; exceeding a budget rejects the sample."));
    form->addRow(QStringLiteral("Visibility queries"), visibility);
    QLabel* hint = new QLabel(QStringLiteral("Shared by all scene-aware ray pipelines in this package. "
                                             "Save publishes changes."),
                              group);
    hint->setWordWrap(true);
    form->addRow(hint);
    const auto publishBudgets = [this, surface, visibility]() {
        const RenderGraphSceneQueryBudgets edited
        {
            .SurfaceQueries = surface->value(),
            .VisibilityQueries = visibility->value(),
        };
        Q_EMIT SceneQueryBudgetsEdited(edited);
    };
    connect(surface, &QSpinBox::editingFinished, this, publishBudgets);
    connect(visibility, &QSpinBox::editingFinished, this, publishBudgets);
    static_cast<QVBoxLayout*>(layout())->addWidget(group);
}

QTableWidget*
RenderGraphPipelineEditor::createTable(const QString& name, const QStringList& fields, const QStringList& defaults)
{
    QWidget* panel = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(panel);
    QTableWidget* table = new QTableWidget(panel);
    table->setObjectName(name);
    table->setProperty("renderGraphField", name);
    table->setColumnCount(static_cast<int32>(fields.size()));
    table->setHorizontalHeaderLabels(fields);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    // The table is as tall as its rows (at least one), and the buttons sit right under it, instead of the table
    // holding a large empty box under a single row. An empty part shows a one-line note instead (below).
    table->setMinimumHeight(table->horizontalHeader()->sizeHint().height() +
                            table->verticalHeader()->defaultSectionSize() + 2 * table->frameWidth());
    table->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setProperty("fields", fields);
    if (name == QStringLiteral("outputs"))
    {
        connect(table, &QTableWidget::itemChanged, table, [table](QTableWidgetItem* item) {
            if (item->column() != 0)
            {
                return;
            }
            const bool hasPreviousName = item->data(Qt::UserRole).isValid();
            const QString previous = item->data(Qt::UserRole).toString();
            if (previous == item->text())
            {
                return;
            }
            const QSignalBlocker blocker(table);
            item->setData(Qt::UserRole, item->text());
            if (item->data(PAIRED_INPUT_ROLE).toString() == previous && hasPreviousName == true)
            {
                item->setData(PAIRED_INPUT_ROLE, item->text());
            }
            item->setToolTip(QStringLiteral("Input port: %1\nOutput port: %2\nThe resource association is automatic.")
                                 .arg(item->data(PAIRED_INPUT_ROLE).toString(), item->text()));
        });
    }
    const QJsonArray rows = name == QStringLiteral("inputs") ? GetReadInputs(mNode) : mNode.value(name).toArray();
    table->setRowCount(static_cast<int32>(rows.size()));
    for (int32 row = 0; row < rows.size(); ++row)
    {
        for (int32 column = 0; column < fields.size(); ++column)
        {
            const QJsonValue value = rows.at(row).toObject().value(fields.at(column));
            AddCell(*table,
                    row,
                    column,
                    fields.at(column),
                    value.isBool() == true ? (value.toBool() == true ? QStringLiteral("true") : QStringLiteral("false"))
                                           : value.toString(),
                    mNode.value(QStringLiteral("kind")).toString());
        }
    }
    if (name == QStringLiteral("shaders"))
    {
        QLabel* scope = new QLabel(
            QStringLiteral(
                "Program IDs are shared by every graph and configuration in the root shader manifest. "
                "Enter an existing program ID to reuse it, or Program to generate a unique ID. "
                "For shared structs and helpers, create a shared shader file and include it from your program."),
            panel);
        scope->setWordWrap(true);
        layout->addWidget(scope);
        QLineEdit* programName = new QLineEdit(panel);
        programName->setObjectName(QStringLiteral("renderGraphProgramName"));
        programName->setReadOnly(true);
        programName->setPlaceholderText(QStringLiteral("Select a shader to inspect and copy its full program ID"));
        layout->addWidget(programName);
        const auto showProgram = [table, programName]() {
            const QTableWidgetItem* itemOrNull = table->item(table->currentRow(), 2);
            programName->setText(itemOrNull != nullptr ? itemOrNull->text() : QString());
            programName->setCursorPosition(0);
        };
        connect(table, &QTableWidget::currentCellChanged, programName, showProgram);
        connect(table, &QTableWidget::itemChanged, programName, showProgram);
    }
    if (name == QStringLiteral("outputs"))
    {
        QStringList headers{QStringLiteral("Resource"),
                            QStringLiteral("Type"),
                            QStringLiteral("Format"),
                            QStringLiteral("Load"),
                            QStringLiteral("Store")};
        const int32 accessColumn = static_cast<int32>(fields.indexOf(QStringLiteral("access")));
        if (accessColumn >= 0)
        {
            headers.append(QStringLiteral("Texture access"));
            // Keep the access choice next to the port name in a narrow Properties dock.
            table->horizontalHeader()->moveSection(accessColumn, 1);
        }
        table->setHorizontalHeaderLabels(headers);
    }
    const auto connectResourceType = [table, fields](int32 row) {
        const int32 typeColumn = static_cast<int32>(fields.indexOf(QStringLiteral("type")));
        const int32 formatColumn = static_cast<int32>(fields.indexOf(QStringLiteral("format")));
        QComboBox* type = qobject_cast<QComboBox*>(table->cellWidget(row, typeColumn));
        QComboBox* format = qobject_cast<QComboBox*>(table->cellWidget(row, formatColumn));
        const QString previous = format->currentText();
        format->clear();
        format->addItem(GetResourceFormat(type->currentText()));
        if (format->findText(previous) < 0)
        {
            format->addItem(previous);
        }
        format->setCurrentText(previous);
        connect(type, &QComboBox::currentTextChanged, format, [format](const QString& value) {
            const QSignalBlocker blocker(format);
            format->clear();
            format->addItem(GetResourceFormat(value));
        });
        const int32 accessColumn = static_cast<int32>(fields.indexOf(QStringLiteral("access")));
        if (accessColumn >= 0)
        {
            QComboBox* access = qobject_cast<QComboBox*>(table->cellWidget(row, accessColumn));
            QComboBox* load = qobject_cast<QComboBox*>(
                table->cellWidget(row, static_cast<int32>(fields.indexOf(QStringLiteral("load")))));
            QComboBox* store = qobject_cast<QComboBox*>(
                table->cellWidget(row, static_cast<int32>(fields.indexOf(QStringLiteral("store")))));
            const auto showAccess = [access, load, store]() {
                const bool isShaderWritable = access->isEnabled() == true && access->currentData().isValid() == true;
                load->setEnabled(isShaderWritable == false);
                store->setEnabled(isShaderWritable == false);
            };
            access->setEnabled(type->currentText() == QStringLiteral("ColorTexture"));
            showAccess();
            connect(type, &QComboBox::currentTextChanged, access, [access, load, showAccess](const QString& value) {
                const QSignalBlocker accessBlocker(access);
                const QSignalBlocker loadBlocker(load);
                if (value != QStringLiteral("ColorTexture") && access->currentData().isValid() == true)
                {
                    access->setCurrentIndex(0);
                    load->setCurrentText(QStringLiteral("Clear"));
                }
                access->setEnabled(value == QStringLiteral("ColorTexture"));
                showAccess();
            });
            connect(access, &QComboBox::currentIndexChanged, load, [access, load, store, showAccess]() {
                const QSignalBlocker loadBlocker(load);
                const QSignalBlocker storeBlocker(store);
                load->setCurrentText(access->currentData().isValid() == true ? QStringLiteral("Load")
                                                                             : QStringLiteral("Clear"));
                store->setCurrentText(QStringLiteral("Store"));
                showAccess();
            });
        }
    };
    if (name == QStringLiteral("outputs"))
    {
        for (int32 row = 0; row < table->rowCount(); ++row)
        {
            QTableWidgetItem* item = table->item(row, 0);
            const QString input = GetPairedInput(mNode, rows.at(row).toObject());
            item->setData(PAIRED_INPUT_ROLE, input);
            item->setToolTip(QStringLiteral("Input port: %1\nOutput port: %2\nThe resource association is automatic.")
                                 .arg(input, item->text()));
            connectResourceType(row);
        }
    }
    // An empty part says so in one line instead of showing an empty table.
    QLabel* empty = new QLabel(name == QStringLiteral("inputs")
                                   ? QStringLiteral("No read inputs. Add one to read a texture or buffer.")
                               : name == QStringLiteral("outputs")
                                   ? QStringLiteral("No writable resources. Add one to write a texture or buffer.")
                                   : QStringLiteral("No shaders. Add one to create its source."),
                               panel);
    empty->setObjectName(QStringLiteral("renderGraphEmpty_") + name);
    empty->setWordWrap(true);
    empty->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(empty);
    layout->addWidget(table);
    // The buttons wrap under each other in a narrow dock rather than widening the panel past its edge.
    FlowLayout* buttons = new FlowLayout(nullptr, design::SPACING_SMALL);
    QPushButton* add = new QPushButton(name == QStringLiteral("shaders") ? QStringLiteral("Add Shader Program")
                                                                         : QStringLiteral("Add"),
                                       panel);
    QPushButton* remove = new QPushButton(QStringLiteral("Remove Selected"), panel);
    add->setObjectName(QStringLiteral("renderGraphAdd_") + name);
    remove->setObjectName(QStringLiteral("renderGraphRemove_") + name);
    const QString kind = mNode.value(QStringLiteral("kind")).toString();
    if (name == QStringLiteral("shaders"))
    {
        QComboBox* language = new QComboBox(panel);
        language->setObjectName(QStringLiteral("renderGraphNewShaderLanguage"));
        language->setAccessibleName(QStringLiteral("New shader language"));
        language->addItem(QStringLiteral("LUSL (.lusl)"), QVariant::fromValue(RenderGraphShaderLanguage::Lusl));
        language->addItem(QStringLiteral("Slang (.slang)"), QVariant::fromValue(RenderGraphShaderLanguage::Slang));
        buttons->addWidget(language);
    }
    connect(add, &QPushButton::clicked, this, [this, table, defaults, fields, name, kind, connectResourceType]() {
        const bool isShaders = name == QStringLiteral("shaders");
        const int32 maximum = isShaders == true ? MAX_SHADERS : MAX_PORTS;
        const QString stage = isShaders == true ? GetNextShaderStage(*table, kind) : QString();
        if (table->rowCount() >= maximum || (isShaders == true && stage.isEmpty() == true))
        {
            return;
        }
        const QSignalBlocker blocker(table);
        const int32 row = table->rowCount();
        table->insertRow(row);
        for (int32 column = 0; column < defaults.size(); ++column)
        {
            QString value = fields.at(column) == QStringLiteral("stage") ? stage : defaults.at(column);
            if (fields.at(column) == QStringLiteral("load") && mNode.value(QStringLiteral("invocation"))
                                                                       .toObject()
                                                                       .value(QStringLiteral("storage_texture"))
                                                                       .toBool() == true)
            {
                value = QStringLiteral("Load");
            }
            if (column == 0)
            {
                QStringList identities;
                for (int32 index = 0; index < row; ++index)
                {
                    const QTableWidgetItem* itemOrNull = table->item(index, 0);
                    if (itemOrNull != nullptr)
                    {
                        identities.append(itemOrNull->text());
                    }
                }
                for (int32 suffix = 2; identities.contains(value) == true; ++suffix)
                {
                    value = defaults.at(column) + QString::number(suffix);
                }
            }
            AddCell(*table, row, column, fields.at(column), value, kind);
        }
        if (name == QStringLiteral("outputs"))
        {
            QTableWidgetItem* item = table->item(row, 0);
            item->setData(Qt::UserRole, item->text());
            item->setData(PAIRED_INPUT_ROLE, item->text());
            item->setToolTip(QStringLiteral("Input port: %1\nOutput port: %1\nThe resource association is automatic.")
                                 .arg(item->text()));
            connectResourceType(row);
        }
        connectChoices(*table, row);
        table->setCurrentCell(row, 0);
        publishEdit();
    });
    connect(remove, &QPushButton::clicked, this, [this, table]() {
        if (table->currentRow() >= 0)
        {
            const QSignalBlocker blocker(table);
            table->removeRow(table->currentRow());
            publishEdit();
        }
    });
    buttons->addWidget(add);
    buttons->addWidget(remove);
    if (name == QStringLiteral("shaders"))
    {
        QPushButton* shared = new QPushButton(QStringLiteral("New shared shader file…"), panel);
        shared->setObjectName(QStringLiteral("renderGraphNewSharedShaderFromShaders"));
        connect(shared, &QPushButton::clicked, this, &RenderGraphPipelineEditor::SharedShaderFileRequested);
        buttons->addWidget(shared);
    }
    layout->addLayout(buttons);
    layout->addStretch(1);
    if (name == QStringLiteral("shaders"))
    {
        const auto updateAdd = [table, add, kind]() {
            const bool canAdd = table->rowCount() < MAX_SHADERS && GetNextShaderStage(*table, kind).isEmpty() == false;
            add->setEnabled(canAdd);
            add->setToolTip(canAdd == true ? QStringLiteral("Assign a shader program to this pipeline.")
                                           : QStringLiteral("No additional stage is available. Edit an existing "
                                                            "program, or create a shared shader file in Source."));
        };
        updateAdd();
        connect(this, &RenderGraphPipelineEditor::Edited, add, updateAdd);
    }
    const auto showRows = [table, empty, remove, name]() {
        const bool isEmpty = table->rowCount() == 0;
        empty->setVisible(isEmpty);
        table->setVisible(isEmpty == false);
        remove->setEnabled(isEmpty == false);
        // A diagnostic about this part points at whichever of the two is showing.
        table->setProperty("renderGraphField", isEmpty == true ? QVariant() : QVariant(name));
        empty->setProperty("renderGraphField", isEmpty == true ? QVariant(name) : QVariant());
    };
    showRows();
    connect(table->model(), &QAbstractItemModel::rowsInserted, empty, showRows);
    connect(table->model(), &QAbstractItemModel::rowsRemoved, empty, showRows);
    return table;
}

void RenderGraphPipelineEditor::addColorOutputEditor()
{
    QWidget* panel = mOutputs->parentWidget();
    QWidget* controls = new QWidget(panel);
    QFormLayout* form = new QFormLayout(controls);
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    QComboBox* access = new QComboBox(controls);
    access->setObjectName(QStringLiteral("renderGraphColorOutputAccess"));
    access->setProperty("renderGraphField", QStringLiteral("invocation.storage_texture"));
    access->addItem(QStringLiteral("Store returned RGB"));
    access->addItem(QStringLiteral("Shader read/write"));
    const QJsonObject current = mNode.value(QStringLiteral("invocation")).toObject();
    access->setCurrentIndex(current.value(QStringLiteral("storage_texture")).toBool() == true ? 1 : 0);
    access->setEnabled(current.contains(QStringLiteral("work_items")) == false);
    access->setToolTip(
        QStringLiteral("Store returned RGB at the launch pixel, or read and write the connected "
                       "color target from Slang using GraphStorageTexture. Set launch size in Invocation."));
    form->addRow(QStringLiteral("Legacy color output"), access);
    static_cast<QVBoxLayout*>(panel->layout())->insertWidget(0, controls);
    connect(access, &QComboBox::currentIndexChanged, this, [this](int32 index) {
        QJsonObject invocation = mNode.value(QStringLiteral("invocation")).toObject();
        updateColorOutputAccess(invocation, index == 1);
        mNode.insert(QStringLiteral("invocation"), invocation);
        publishEdit();
    });
}

void RenderGraphPipelineEditor::updateColorOutputAccess(QJsonObject& inoutInvocation, bool isShaderWritable)
{
    const QString key = QStringLiteral("storage_texture");
    if (inoutInvocation.value(key).toBool() == isShaderWritable)
    {
        return;
    }
    if (isShaderWritable == true)
    {
        inoutInvocation.insert(key, true);
    }
    else
    {
        inoutInvocation.remove(key);
    }
    const QStringList fields = mOutputs->property("fields").toStringList();
    const int32 typeColumn = static_cast<int32>(fields.indexOf(QStringLiteral("type")));
    const int32 loadColumn = static_cast<int32>(fields.indexOf(QStringLiteral("load")));
    const int32 accessColumn = static_cast<int32>(fields.indexOf(QStringLiteral("access")));
    for (int32 row = 0; row < mOutputs->rowCount(); ++row)
    {
        const QComboBox* typeOrNull = qobject_cast<QComboBox*>(mOutputs->cellWidget(row, typeColumn));
        QComboBox* loadOrNull = qobject_cast<QComboBox*>(mOutputs->cellWidget(row, loadColumn));
        const QComboBox* accessOrNull = qobject_cast<QComboBox*>(mOutputs->cellWidget(row, accessColumn));
        if ((accessOrNull == nullptr || accessOrNull->currentData().isValid() == false) && typeOrNull != nullptr &&
            loadOrNull != nullptr && typeOrNull->currentText() == QStringLiteral("ColorTexture"))
        {
            const QSignalBlocker blocker(loadOrNull);
            loadOrNull->setCurrentText(isShaderWritable == true ? QStringLiteral("Load") : QStringLiteral("Clear"));
        }
    }
}

void RenderGraphPipelineEditor::addProperties(QFormLayout& outForm, const QString& name)
{
    if (name == QStringLiteral("state") &&
        mNode.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline"))
    {
        addRayStateProperties(outForm);
        return;
    }
    if (name == QStringLiteral("state") && IsSceneComputeNode(mNode) == true)
    {
        addSceneComputeStateProperties(outForm);
        return;
    }
    if (name == QStringLiteral("invocation") && UsesRayLaunch(mNode) == true)
    {
        const QJsonObject invocation = mNode.value(name).toObject();
        QComboBox* execution = new QComboBox(this);
        execution->setObjectName(QStringLiteral("renderGraphRayInvocationType"));
        execution->setProperty("renderGraphField", QStringLiteral("invocation.type"));
        execution->addItem(QStringLiteral("Compute pipeline"), QStringLiteral("Compute"));
        if (mNode.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline"))
        {
            execution->addItem(QStringLiteral("Hardware RT pipeline"), QStringLiteral("RayTracing"));
        }
        execution->setCurrentIndex(
            execution->findData(invocation.value(QStringLiteral("type")).toString(QStringLiteral("Compute"))));
        execution->setToolTip(QStringLiteral("Select how this logical ray node executes. Hardware RT requires a "
                                             "native ray-stage backend and never silently lowers to compute."));
        outForm.addRow(QStringLiteral("Invocation type"), execution);
        QComboBox* traversal = new QComboBox(this);
        traversal->setObjectName(QStringLiteral("renderGraphTraversal"));
        traversal->setProperty("renderGraphField", QStringLiteral("invocation.traversal"));
        traversal->addItem(QStringLiteral("Automatic"), QString());
        traversal->addItem(QStringLiteral("Triangle loop"), QStringLiteral("TriangleLoop"));
        traversal->addItem(QStringLiteral("Compute BVH"), QStringLiteral("ComputeBvh"));
        traversal->addItem(QStringLiteral("Native ray query"), QStringLiteral("NativeQuery"));
        traversal->addItem(QStringLiteral("Custom traversal"), QStringLiteral("Custom"));
        const QString selection = mNode.value(name).toObject().value(QStringLiteral("traversal")).toString();
        traversal->setCurrentIndex(traversal->findData(selection));
        traversal->setToolTip(QStringLiteral(
            "Compute invocation uses native hardware BVH queries, the engine triangle loop, the renderer's BVH in "
            "ordinary compute (Compute BVH, for larger scenes without ray hardware), or your traversal source. "
            "Automatic on an explicit Compute invocation uses the triangle loop. Legacy graphs retain their prior "
            "default. Hardware RT invocation owns its traversal."));
        QLineEdit* source = new QLineEdit(invocation.value(QStringLiteral("traversal_source")).toString(), this);
        source->setObjectName(QStringLiteral("renderGraphTraversalSource"));
        source->setProperty("renderGraphField", QStringLiteral("invocation.traversal_source"));
        source->setPlaceholderText(QStringLiteral("Package-relative .slang traversal source"));
        source->setToolTip(QStringLiteral("Defines Hit TraceWorld(float3 originWorld, float3 directionWorld, "
                                          "float nearDistanceWorld, float farDistanceWorld, bool isVisibility). "
                                          "The same source handles closest and visibility queries."));
        traversal->setEnabled(execution->currentData().toString() == QStringLiteral("Compute"));
        source->setEnabled(traversal->isEnabled() == true && selection == QStringLiteral("Custom"));
        connect(execution, &QComboBox::currentIndexChanged, this, [this, execution, traversal, source]() {
            QJsonObject candidate = mNode.value(QStringLiteral("invocation")).toObject();
            const bool isCompute = execution->currentData().toString() == QStringLiteral("Compute");
            candidate.insert(QStringLiteral("type"), execution->currentData().toString());
            if (isCompute == false)
            {
                candidate.remove(QStringLiteral("traversal"));
                candidate.remove(QStringLiteral("traversal_source"));
            }
            else
            {
                candidate.insert(QStringLiteral("traversal"), QStringLiteral("TriangleLoop"));
            }
            const QSignalBlocker blocker(traversal);
            traversal->setCurrentIndex(
                traversal->findData(isCompute == true ? QStringLiteral("TriangleLoop") : QString()));
            traversal->setEnabled(isCompute);
            source->setEnabled(false);
            mNode.insert(QStringLiteral("invocation"), candidate);
            publishEdit();
        });
        connect(traversal, &QComboBox::currentIndexChanged, this, [this, traversal, source](int32 index) {
            QJsonObject invocation = mNode.value(QStringLiteral("invocation")).toObject();
            const QString strategy = traversal->itemData(index).toString();
            if (strategy.isEmpty() == true)
            {
                invocation.remove(QStringLiteral("traversal"));
            }
            else
            {
                invocation.insert(QStringLiteral("traversal"), strategy);
            }
            const bool isCustom = strategy == QStringLiteral("Custom");
            source->setEnabled(isCustom);
            if (isCustom == true)
            {
                invocation.insert(QStringLiteral("traversal_source"), source->text().trimmed());
            }
            else
            {
                invocation.remove(QStringLiteral("traversal_source"));
            }
            mNode.insert(QStringLiteral("invocation"), invocation);
            publishEdit();
        });
        outForm.addRow(QStringLiteral("Ray traversal"), traversal);
        connect(source, &QLineEdit::editingFinished, this, [this, source]() {
            if (source->isEnabled() == false)
            {
                return;
            }
            QJsonObject candidate = mNode.value(QStringLiteral("invocation")).toObject();
            candidate.insert(QStringLiteral("traversal_source"), source->text().trimmed());
            mNode.insert(QStringLiteral("invocation"), candidate);
            publishEdit();
        });
        outForm.addRow(QStringLiteral("Traversal source"), source);
    }
    const QJsonObject properties = mNode.value(name).toObject();
    for (QJsonObject::const_iterator property = properties.begin(); property != properties.end(); ++property)
    {
        const QString key = property.key();
        if (name == QStringLiteral("invocation") && UsesRayLaunch(mNode) == true &&
            (key == QStringLiteral("type") || key == QStringLiteral("traversal") ||
             key == QStringLiteral("traversal_source") || key == QStringLiteral("storage_texture")))
        {
            continue;
        }
        const QJsonValue value = property.value();
        const auto setProperty = [this, name, key](const QJsonValue& field) {
            QJsonObject record = mNode.value(name).toObject();
            record.insert(key, field);
            mNode.insert(name, record);
            publishEdit();
        };
        if (name == QStringLiteral("invocation") && UsesRayLaunch(mNode) == true &&
            (key == QStringLiteral("dimensions") || key == QStringLiteral("work_items")))
        {
            constexpr int32 MATCH_COLOR_TARGET_INDEX = 0;
            constexpr int32 EXPLICIT_SIZE_INDEX = 1;
            constexpr int32 WORK_ITEMS_INDEX = 2;
            QWidget* row = new QWidget(this);
            QVBoxLayout* layout = new QVBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            QComboBox* mode = new QComboBox(row);
            mode->setObjectName(QStringLiteral("rayLaunchSizeMode"));
            mode->addItems({QStringLiteral("Match color target"), QStringLiteral("Explicit size")});
            const bool isSceneCompute = IsSceneComputeNode(mNode);
            if (isSceneCompute == true)
            {
                mode->addItem(QStringLiteral("Work items"));
            }
            const bool hasWorkItemLaunch = key == QStringLiteral("work_items");
            const bool isAutomatic = value.toString() == QStringLiteral("ColorTarget");
            mode->setCurrentIndex(hasWorkItemLaunch == true ? WORK_ITEMS_INDEX
                                  : isAutomatic == true     ? MATCH_COLOR_TARGET_INDEX
                                                            : EXPLICIT_SIZE_INDEX);
            layout->addWidget(mode);

            QWidget* dimensions = new QWidget(row);
            // The three sizes share one row while they fit and wrap in the narrowest Properties dock, whose width
            // depends on the platform font, instead of pushing the panel past the dock's edge.
            FlowLayout* dimensionLayout = new FlowLayout(dimensions, design::SPACING_EXTRA_SMALL);
            for (int32 dimension = 0; dimension < DIMENSION_COUNT; ++dimension)
            {
                QSpinBox* control = new QSpinBox(dimensions);
                // A vector reads as plain number fields (typed, arrow keys or the wheel), so three fit one row.
                control->setButtonSymbols(QAbstractSpinBox::NoButtons);
                control->setObjectName(QStringLiteral("rayLaunchDimension%1").arg(dimension));
                const bool isDepth = dimension == DIMENSION_COUNT - 1;
                control->setRange(1, isDepth == true ? 1 : MAX_PROPERTY_VALUE);
                control->setValue(value.isArray() == true ? value.toArray().at(dimension).toInt() : 1);
                control->setEnabled(isDepth == false);
                dimensionLayout->addWidget(control);
            }
            QSpinBox* workItems = new QSpinBox(row);
            workItems->setObjectName(QStringLiteral("sceneWorkItemCount"));
            workItems->setRange(1, static_cast<int32>(content::render_graph_format::MAX_SCENE_WORK_ITEMS));
            workItems->setValue(hasWorkItemLaunch == true ? value.toInt() : 1);
            layout->addWidget(dimensions);
            layout->addWidget(workItems);

            QLabel* help = new QLabel(row);
            help->setWordWrap(true);
            layout->addWidget(help);
            const auto showMode = [dimensions, workItems, help, isSceneCompute](int32 index) {
                const bool isBufferDispatch = isSceneCompute == true && index == WORK_ITEMS_INDEX;
                dimensions->setVisible(index == EXPLICIT_SIZE_INDEX);
                workItems->setVisible(isBufferDispatch);
                help->setText(
                    isBufferDispatch == true
                        ? QStringLiteral("Dispatch this many work items with Scene, buffers and shader-writable "
                                         "textures. Remove View and automatic color output.")
                    : index == EXPLICIT_SIZE_INDEX
                        ? QStringLiteral("Dispatch width × height invocations. Bound textures keep their own sizes.")
                        : QStringLiteral("Dispatch one invocation per texel in the connected color target."));
            };
            QComboBox* colorOutputOrNull = findChild<QComboBox*>(QStringLiteral("renderGraphColorOutputAccess"));
            const auto applyLaunch = [this, mode, dimensions, workItems, isSceneCompute, colorOutputOrNull]() {
                QJsonObject invocation = mNode.value(QStringLiteral("invocation")).toObject();
                const int32 index = mode->currentIndex();
                const bool isBufferDispatch = isSceneCompute == true && index == WORK_ITEMS_INDEX;
                if (isBufferDispatch == true)
                {
                    invocation.remove(QStringLiteral("dimensions"));
                    invocation.insert(QStringLiteral("work_items"), workItems->value());
                    updateColorOutputAccess(invocation, false);
                }
                else
                {
                    invocation.remove(QStringLiteral("work_items"));
                    QJsonValue size(QStringLiteral("ColorTarget"));
                    if (index == EXPLICIT_SIZE_INDEX)
                    {
                        QJsonArray values;
                        for (const QSpinBox* field : dimensions->findChildren<QSpinBox*>())
                        {
                            values.append(field->value());
                        }
                        size = values;
                    }
                    invocation.insert(QStringLiteral("dimensions"), size);
                }
                if (colorOutputOrNull != nullptr)
                {
                    const QSignalBlocker blocker(colorOutputOrNull);
                    colorOutputOrNull->setCurrentIndex(
                        invocation.value(QStringLiteral("storage_texture")).toBool() == true ? 1 : 0);
                    colorOutputOrNull->setEnabled(isBufferDispatch == false);
                }
                mNode.insert(QStringLiteral("invocation"), invocation);
                publishEdit();
            };
            showMode(mode->currentIndex());
            connect(mode, &QComboBox::currentIndexChanged, this, [showMode, applyLaunch](int32 index) {
                showMode(index);
                applyLaunch();
            });
            for (QSpinBox* field : dimensions->findChildren<QSpinBox*>())
            {
                connect(field, &QSpinBox::valueChanged, this, applyLaunch);
            }
            connect(workItems, &QSpinBox::valueChanged, this, applyLaunch);
            row->setProperty("renderGraphField", name + QLatin1Char('.') + key);
            outForm.addRow(QStringLiteral("Launch size"), row);
        }
        else if (value.isBool() == true)
        {
            QCheckBox* control = new QCheckBox(this);
            control->setChecked(value.toBool());
            connect(control, &QCheckBox::toggled, this, [setProperty](bool isChecked) { setProperty(isChecked); });
            control->setProperty("renderGraphField", name + QLatin1Char('.') + key);
            outForm.addRow(key, control);
        }
        else if (value.isDouble() == true)
        {
            QSpinBox* control = new QSpinBox(this);
            control->setRange(0, MAX_PROPERTY_VALUE);
            control->setValue(value.toInt());
            connect(control, &QSpinBox::valueChanged, this, [setProperty](int32 number) { setProperty(number); });
            control->setProperty("renderGraphField", name + QLatin1Char('.') + key);
            outForm.addRow(key, control);
        }
        else if (value.isArray() == true)
        {
            QWidget* row = new QWidget(this);
            QHBoxLayout* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(design::SPACING_EXTRA_SMALL);
            for (int32 dimension = 0; dimension < DIMENSION_COUNT; ++dimension)
            {
                QSpinBox* control = new QSpinBox(row);
                // A vector reads as plain number fields (typed, arrow keys or the wheel), so three fit one row.
                control->setButtonSymbols(QAbstractSpinBox::NoButtons);
                control->setRange(1, MAX_PROPERTY_VALUE);
                control->setValue(value.toArray().at(dimension).toInt());
                connect(control, &QSpinBox::valueChanged, this, [this, name, key, dimension](int32 number) {
                    QJsonObject record = mNode.value(name).toObject();
                    QJsonArray dimensions = record.value(key).toArray();
                    dimensions.replace(dimension, number);
                    record.insert(key, dimensions);
                    mNode.insert(name, record);
                    publishEdit();
                });
                layout->addWidget(control);
            }
            row->setProperty("renderGraphField", name + QLatin1Char('.') + key);
            outForm.addRow(key, row);
        }
        else
        {
            QLineEdit* control = new QLineEdit(value.toString(), this);
            connect(control, &QLineEdit::editingFinished, this, [setProperty, control]() {
                setProperty(control->text());
            });
            control->setProperty("renderGraphField", name + QLatin1Char('.') + key);
            outForm.addRow(key, control);
        }
    }
}

void RenderGraphPipelineEditor::addRayStateProperties(QFormLayout& outForm)
{
    QLabel* explanation = new QLabel(
        QStringLiteral(
            "Automatic: the triangle profile supplies hit attributes (float2 barycentrics) and trace limits. "
            "ScenePayload is a customizable shader struct; Slang determines its local layout. "
            "Native payload bytes do not size ScenePayload."),
        this);
    explanation->setWordWrap(true);
    outForm.addRow(explanation);
    QGroupBox* advanced = new QGroupBox(QStringLiteral("Overrides"), this);
    advanced->setObjectName(QStringLiteral("renderGraphRayAdvanced"));
    advanced->setProperty("renderGraphField", QStringLiteral("state"));
    advanced->setCheckable(true);
    advanced->setChecked(mNode.value(QStringLiteral("state")).toObject().isEmpty() == false);
    QVBoxLayout* advancedLayout = new QVBoxLayout(advanced);
    QWidget* fields = new QWidget(advanced);
    fields->setObjectName(QStringLiteral("renderGraphRayLimits"));
    QFormLayout* form = new QFormLayout(fields);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    fields->setVisible(advanced->isChecked());
    connect(advanced, &QGroupBox::toggled, fields, &QWidget::setVisible);
    for (const QString& key :
         {QStringLiteral("max_recursion"), QStringLiteral("payload_bytes"), QStringLiteral("attribute_bytes")})
    {
        QSpinBox* control = new QSpinBox(fields);
        control->setObjectName(QStringLiteral("renderGraphRay_") + key);
        const int32 automaticValue = key == QStringLiteral("max_recursion") ? 0 : AUTOMATIC_RAY_LIMIT;
        control->setRange(automaticValue,
                          key == QStringLiteral("max_recursion") ? MAX_RAY_RECURSION : MAX_RAY_INTERFACE_BYTES);
        control->setSpecialValueText(QStringLiteral("Automatic"));
        control->setValue(mNode.value(QStringLiteral("state")).toObject().value(key).toInt(automaticValue));
        connect(control, &QSpinBox::valueChanged, this, [this, key, automaticValue](int32 number) {
            QJsonObject state = mNode.value(QStringLiteral("state")).toObject();
            if (number == automaticValue)
            {
                state.remove(key);
            }
            else
            {
                state.insert(key, number);
            }
            mNode.insert(QStringLiteral("state"), state);
            publishEdit();
        });
        const QString label = key == QStringLiteral("max_recursion")   ? QStringLiteral("Trace recursion limit")
                              : key == QStringLiteral("payload_bytes") ? QStringLiteral("Native payload bytes")
                                                                       : QStringLiteral("Hit attribute bytes");
        form->addRow(label, control);
    }
    QLabel* limits = new QLabel(
        QStringLiteral(
            "Overrides must match the selected profile. Custom intersection attributes need procedural "
            "geometry, which this triangle profile does not execute. Collapsing this section retains overrides."),
        fields);
    limits->setWordWrap(true);
    form->addRow(limits);
    advancedLayout->addWidget(fields);
    outForm.addRow(advanced);
    QPushButton* reset = new QPushButton(QStringLiteral("Use automatic settings"), this);
    reset->setObjectName(QStringLiteral("renderGraphRayAutomatic"));
    connect(reset, &QPushButton::clicked, this, [this, fields, advanced]() {
        for (QSpinBox* control : fields->findChildren<QSpinBox*>())
        {
            const QSignalBlocker blocker(control);
            control->setValue(control->minimum());
        }
        mNode.insert(QStringLiteral("state"), QJsonObject{});
        advanced->setChecked(false);
        publishEdit();
    });
    outForm.addRow(reset);
}

void RenderGraphPipelineEditor::addSceneComputeStateProperties(QFormLayout& outForm)
{
    const QJsonArray defaultGroupSize = GetSceneComputeGroupSize();
    const QJsonArray supported = GetSupportedSceneComputeGroupSizes();
    const QString defaultText = QStringLiteral("%1 × %2 × %3")
                                    .arg(defaultGroupSize.at(0).toInteger())
                                    .arg(defaultGroupSize.at(1).toInteger())
                                    .arg(defaultGroupSize.at(2).toInteger());
    QLabel* explanation = new QLabel(
        QStringLiteral("Automatic: inherit the shader bundle's explicit workgroup, or use %1. "
                       "All explicit scene compute overrides in one bundle must agree. Query budgets are shared.")
            .arg(defaultText),
        this);
    explanation->setWordWrap(true);
    outForm.addRow(explanation);
    QGroupBox* advanced = new QGroupBox(QStringLiteral("Overrides"), this);
    advanced->setObjectName(QStringLiteral("renderGraphComputeAdvanced"));
    advanced->setProperty("renderGraphField", QStringLiteral("state"));
    advanced->setCheckable(true);
    QFormLayout* form = new QFormLayout(advanced);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    QComboBox* groupSize = new QComboBox(advanced);
    groupSize->setObjectName(QStringLiteral("renderGraphComputeGroupSize"));
    for (const QJsonValueConstRef shape : supported)
    {
        const QJsonArray dimensions = shape.toArray();
        groupSize->addItem(QStringLiteral("%1 × %2 × %3")
                               .arg(dimensions.at(0).toInteger())
                               .arg(dimensions.at(1).toInteger())
                               .arg(dimensions.at(2).toInteger()));
    }
    form->addRow(QStringLiteral("Group size"), groupSize);
    QLabel* warning = new QLabel(advanced);
    warning->setObjectName(QStringLiteral("renderGraphComputeGroupSizeWarning"));
    warning->setWordWrap(true);
    form->addRow(warning);
    const auto showState = [this, advanced, groupSize, warning, supported]() {
        const QJsonValue authored = mNode.value(QStringLiteral("state")).toObject().value(QStringLiteral("group_size"));
        const QSignalBlocker advancedBlocker(advanced);
        const QSignalBlocker groupBlocker(groupSize);
        advanced->setChecked(authored.isUndefined() == false);
        foundation::int32 index = authored.isUndefined() == true ? 0 : -1;
        for (foundation::int32 candidate = 0; candidate < supported.size(); ++candidate)
            if (supported.at(candidate) == authored)
                index = candidate;
        groupSize->setCurrentIndex(index);
        warning->setVisible(index < 0);
        warning->setText(QStringLiteral("Authored group size %1 is unsupported and the build rejects it. "
                                        "Select a supported shape or use automatic settings to repair it.")
                             .arg(QString::fromUtf8(QJsonDocument(authored.toArray()).toJson(QJsonDocument::Compact))));
    };
    showState();
    connect(groupSize, &QComboBox::currentIndexChanged, this, [this, supported, showState](foundation::int32 index) {
        if (index < 0 || index >= supported.size())
            return;
        QJsonObject state = mNode.value(QStringLiteral("state")).toObject();
        state.insert(QStringLiteral("group_size"), supported.at(index));
        mNode.insert(QStringLiteral("state"), state);
        showState();
        publishEdit();
    });
    connect(advanced, &QGroupBox::toggled, this, [this, defaultGroupSize, showState](bool isChecked) {
        QJsonObject state = mNode.value(QStringLiteral("state")).toObject();
        if (isChecked == true)
            state.insert(QStringLiteral("group_size"), defaultGroupSize);
        else
            state.remove(QStringLiteral("group_size"));
        mNode.insert(QStringLiteral("state"), state);
        showState();
        publishEdit();
    });
    outForm.addRow(advanced);
    QPushButton* reset = new QPushButton(QStringLiteral("Use automatic settings"), this);
    reset->setObjectName(QStringLiteral("renderGraphComputeAutomatic"));
    connect(reset, &QPushButton::clicked, this, [this, showState]() {
        mNode.insert(QStringLiteral("state"), QJsonObject{});
        showState();
        publishEdit();
    });
    outForm.addRow(reset);
}

void RenderGraphPipelineEditor::SetShaderAssignments(const QJsonArray& shaders)
{
    const QSignalBlocker blocker(mShaders);
    mNode.insert(QStringLiteral("shaders"), shaders);
    for (int32 row = 0; row < shaders.size() && row < mShaders->rowCount(); ++row)
    {
        mShaders->item(row, 2)->setText(shaders.at(row).toObject().value(QStringLiteral("program")).toString());
    }
    QLineEdit* programNameOrNull =
        mShaders->parentWidget()->findChild<QLineEdit*>(QStringLiteral("renderGraphProgramName"));
    if (programNameOrNull != nullptr)
    {
        const int32 currentRow = mShaders->currentRow();
        programNameOrNull->setText(currentRow >= 0 && currentRow < shaders.size()
                                       ? shaders.at(currentRow).toObject().value(QStringLiteral("program")).toString()
                                       : QString());
        programNameOrNull->setCursorPosition(0);
    }
    if (mPartsOrNull != nullptr)
    {
        ShowParts();
    }
}

void RenderGraphPipelineEditor::connectChoices(QTableWidget& inoutTable, int32 row)
{
    for (int32 column = 0; column < inoutTable.columnCount(); ++column)
    {
        QComboBox* controlOrNull = qobject_cast<QComboBox*>(inoutTable.cellWidget(row, column));
        if (controlOrNull != nullptr)
        {
            connect(controlOrNull, &QComboBox::currentTextChanged, this, [this]() { publishEdit(); });
        }
    }
}

void RenderGraphPipelineEditor::publishEdit()
{
    QJsonObject candidate = mNode;
    for (const QTableWidget* table : {mInputs, mOutputs, mShaders})
    {
        candidate.insert(table->objectName(), ReadRows(*table, table->property("fields").toStringList()));
    }
    QJsonArray inputs = candidate.value(QStringLiteral("inputs")).toArray();
    QJsonArray outputs;
    const QJsonArray resources = candidate.value(QStringLiteral("outputs")).toArray();
    for (int32 row = 0; row < resources.size(); ++row)
    {
        QJsonObject resource = resources.at(row).toObject();
        const QString input = mOutputs->item(row, 0)->data(PAIRED_INPUT_ROLE).toString();
        QJsonValue required(true);
        for (const QJsonValueConstRef previous : mNode.value(QStringLiteral("inputs")).toArray())
        {
            if (previous.toObject().value(QStringLiteral("id")).toString() == input)
            {
                required = previous.toObject().value(QStringLiteral("required"));
            }
        }
        inputs.append(QJsonObject{{QStringLiteral("id"), input},
                                  {QStringLiteral("type"), resource.value(QStringLiteral("type"))},
                                  {QStringLiteral("required"), required}});
        resource.insert(QStringLiteral("target"), input);
        outputs.append(resource);
    }
    const QString inputError = ValidatePorts(inputs, QStringLiteral("Input"));
    const QString outputError = ValidatePorts(outputs, QStringLiteral("Output"));
    mError->setText(inputError.isEmpty() == false ? inputError : outputError);
    if (mError->text().isEmpty() == false)
    {
        return;
    }
    // Preserve existing input order and all endpoint names when opening older graphs.
    QJsonArray orderedInputs;
    for (const QJsonValueConstRef previous : mNode.value(QStringLiteral("inputs")).toArray())
    {
        for (int32 index = 0; index < inputs.size(); ++index)
        {
            if (inputs.at(index).toObject().value(QStringLiteral("id")) ==
                previous.toObject().value(QStringLiteral("id")))
            {
                orderedInputs.append(inputs.takeAt(index));
                break;
            }
        }
    }
    for (const QJsonValueConstRef input : inputs)
    {
        orderedInputs.append(input);
    }
    candidate.insert(QStringLiteral("inputs"), orderedInputs);
    candidate.insert(QStringLiteral("outputs"), outputs);
    // Adding or removing a Scene input changes a ComputePipeline's execution profile.
    candidate = ApplyComputeProfile(mNode, candidate);
    mNode = candidate;
    const QComboBox* language =
        mShaders->parentWidget()->findChild<QComboBox*>(QStringLiteral("renderGraphNewShaderLanguage"));
    LUDUS_REQUIRE(language != nullptr);
    Q_EMIT Edited(candidate, language->currentData().value<RenderGraphShaderLanguage>());
}
} // namespace ludus::editor
