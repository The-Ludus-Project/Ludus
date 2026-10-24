#include "internal/render_graph_configuration_editor.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_node_ports.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 DIALOG_WIDTH = 760;
constexpr int32 DIALOG_HEIGHT = 460;
constexpr int32 TYPE_COLUMN = 1;
constexpr int32 SOURCE_COLUMN = 2;
constexpr int32 REQUIRED_COLUMN = 3;
constexpr int32 REPAIRS_HEIGHT = 144;

QJsonArray ReadPorts(const QTableWidget& table, bool isOutput)
{
    QJsonArray ports;
    for (int32 row = 0; row < table.rowCount(); ++row)
    {
        const QComboBox* type = qobject_cast<QComboBox*>(table.cellWidget(row, TYPE_COLUMN));
        QJsonObject port{{QStringLiteral("id"), table.item(row, 0)->text().trimmed()},
                         {QStringLiteral("type"), type->currentText()}};
        if (isOutput == true)
        {
            const QComboBox* source = qobject_cast<QComboBox*>(table.cellWidget(row, SOURCE_COLUMN));
            port.insert(QStringLiteral("source"), source->currentData().toString());
            port.insert(QStringLiteral("required"), table.item(row, REQUIRED_COLUMN)->checkState() == Qt::Checked);
        }
        ports.append(port);
    }
    return ports;
}
} // namespace

RenderGraphConfigurationEditor::RenderGraphConfigurationEditor(const RenderGraphDocument& document,
                                                               const QString& file,
                                                               const QJsonObject& configuration,
                                                               QWidget* parentOrNull)
    : QDialog(parentOrNull), mDocument(document), mFile(file), mConfiguration(configuration), mVersion(1),
      mInputs(createTable(false)), mOutputs(createTable(true)), mError(new QLabel(this)), mRepairs(new QLabel(this))
{
    QJsonObject graph;
    if (document.GetGraph(file, graph) == true)
    {
        mVersion = graph.value(QStringLiteral("version")).toInt(1);
    }
    setObjectName(QStringLiteral("renderGraphConfigurationEditor"));
    setWindowTitle(QStringLiteral("Configuration Inputs / Outputs — %1#%2")
                       .arg(file, configuration.value(QStringLiteral("id")).toString()));
    resize(DIALOG_WIDTH, DIALOG_HEIGHT);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* help = new QLabel(QStringLiteral("Inputs receive values from the containing graph. Outputs expose resource "
                                             "versions to it. Add ports here, then drag links on the canvas. "
                                             "Unconnected outputs must be connected before building."),
                              this);
    help->setWordWrap(true);
    layout->addWidget(help);
    QTabWidget* tabs = new QTabWidget(this);
    for (QTableWidget* table : {mInputs, mOutputs})
    {
        const bool isOutput = table == mOutputs;
        QWidget* page = new QWidget(tabs);
        QVBoxLayout* pageLayout = new QVBoxLayout(page);
        pageLayout->addWidget(table);
        QHBoxLayout* actions = new QHBoxLayout;
        QPushButton* add =
            new QPushButton(isOutput == true ? QStringLiteral("Add Output") : QStringLiteral("Add Input"), page);
        add->setObjectName(isOutput == true ? QStringLiteral("configurationAddOutput")
                                            : QStringLiteral("configurationAddInput"));
        QPushButton* remove = new QPushButton(QStringLiteral("Remove Selected"), page);
        remove->setObjectName(isOutput == true ? QStringLiteral("configurationRemoveOutput")
                                               : QStringLiteral("configurationRemoveInput"));
        actions->addWidget(add);
        actions->addWidget(remove);
        actions->addStretch();
        pageLayout->addLayout(actions);
        tabs->addTab(page, isOutput == true ? QStringLiteral("Outputs") : QStringLiteral("Inputs"));
        const QJsonArray ports =
            configuration.value(isOutput == true ? QStringLiteral("outputs") : QStringLiteral("inputs")).toArray();
        for (const QJsonValueConstRef port : ports)
        {
            addPort(*table, port.toObject(), isOutput, port.toObject().value(QStringLiteral("id")).toString());
        }
        add->setEnabled(table->rowCount() < MAX_CONFIGURATION_PORTS);
        connect(add, &QPushButton::clicked, this, [this, table, isOutput, add]() {
            if (table->rowCount() >= MAX_CONFIGURATION_PORTS)
            {
                return;
            }
            const QString stem = isOutput == true ? QStringLiteral("output") : QStringLiteral("input");
            QStringList names;
            for (int32 row = 0; row < table->rowCount(); ++row)
            {
                names.append(table->item(row, 0)->text().trimmed());
            }
            QString identity = stem;
            for (int32 suffix = 2; names.contains(identity) == true; ++suffix)
            {
                identity = stem + QString::number(suffix);
            }
            addPort(*table,
                    {{QStringLiteral("id"), identity}, {QStringLiteral("type"), QStringLiteral("ColorTexture")}},
                    isOutput);
            refreshSources();
            table->setCurrentCell(table->rowCount() - 1, 0);
            table->editItem(table->item(table->rowCount() - 1, 0));
            add->setEnabled(table->rowCount() < MAX_CONFIGURATION_PORTS);
        });
        connect(remove, &QPushButton::clicked, this, [this, table, add]() {
            // Remove from the bottom so row indices remain stable for multi-selection.
            for (int32 row = table->rowCount() - 1; row >= 0; --row)
            {
                if (table->selectionModel()->isRowSelected(row, QModelIndex()) == true)
                {
                    table->removeRow(row);
                }
            }
            refreshSources();
            add->setEnabled(table->rowCount() < MAX_CONFIGURATION_PORTS);
        });
    }
    connect(mInputs, &QTableWidget::itemChanged, this, [this]() { refreshSources(); });
    connect(mOutputs, &QTableWidget::itemChanged, this, [this]() { refreshSources(); });
    layout->addWidget(tabs);
    mError->setObjectName(QStringLiteral("configurationInterfaceError"));
    mError->setTextFormat(Qt::PlainText);
    mError->setWordWrap(true);
    layout->addWidget(mError);
    mRepairs->setObjectName(QStringLiteral("configurationInterfaceRepairs"));
    mRepairs->setTextFormat(Qt::PlainText);
    mRepairs->setWordWrap(true);
    mRepairs->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QScrollArea* repairs = new QScrollArea(this);
    repairs->setWidget(mRepairs);
    repairs->setWidgetResizable(true);
    repairs->setMaximumHeight(REPAIRS_HEIGHT);
    layout->addWidget(repairs);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("configurationApply"));
    connect(buttons->button(QDialogButtonBox::Apply),
            &QPushButton::clicked,
            this,
            &RenderGraphConfigurationEditor::ApplyRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    refreshSources();
}

QTableWidget* RenderGraphConfigurationEditor::createTable(bool isOutput)
{
    QTableWidget* table = new QTableWidget(this);
    table->setObjectName(isOutput == true ? QStringLiteral("configurationOutputs")
                                          : QStringLiteral("configurationInputs"));
    const QStringList labels = isOutput == true ? QStringList{QStringLiteral("Name"),
                                                              QStringLiteral("Type"),
                                                              QStringLiteral("Source"),
                                                              QStringLiteral("Requires consumer")}
                                                : QStringList{QStringLiteral("Name"), QStringLiteral("Type")};
    table->setColumnCount(static_cast<int32>(labels.size()));
    table->setHorizontalHeaderLabels(labels);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAccessibleName(isOutput == true ? QStringLiteral("Configuration outputs")
                                              : QStringLiteral("Configuration inputs"));
    return table;
}

void RenderGraphConfigurationEditor::addPort(QTableWidget& inoutTable,
                                             const QJsonObject& port,
                                             bool isOutput,
                                             const QString& originalIdentity)
{
    const bool hasBlockedSignals = inoutTable.blockSignals(true);
    const int32 row = inoutTable.rowCount();
    inoutTable.insertRow(row);
    QTableWidgetItem* name = new QTableWidgetItem(port.value(QStringLiteral("id")).toString());
    name->setData(Qt::UserRole, originalIdentity);
    inoutTable.setItem(row, 0, name);
    QComboBox* type = new QComboBox(&inoutTable);
    type->addItems(GetConfigurationPortTypes(mVersion, isOutput));
    const QString authoredType = port.value(QStringLiteral("type")).toString();
    if (type->findText(authoredType) < 0)
    {
        type->addItem(authoredType);
    }
    type->setCurrentText(authoredType);
    inoutTable.setCellWidget(row, TYPE_COLUMN, type);
    if (isOutput == true)
    {
        QComboBox* source = new QComboBox(&inoutTable);
        const QString endpoint = port.value(QStringLiteral("source")).toString();
        source->addItem(endpoint.isEmpty() == true ? QStringLiteral("Unconnected") : endpoint, endpoint);
        inoutTable.setCellWidget(row, SOURCE_COLUMN, source);
        connect(source, &QComboBox::currentIndexChanged, this, [this]() { refreshSources(); });
        QTableWidgetItem* required = new QTableWidgetItem;
        required->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        required->setCheckState(port.value(QStringLiteral("required")).toBool() == true ? Qt::Checked : Qt::Unchecked);
        required->setToolTip(QStringLiteral("Every containing Subgraph instance must consume this output."));
        inoutTable.setItem(row, REQUIRED_COLUMN, required);
    }
    connect(type, &QComboBox::currentTextChanged, this, [this]() { refreshSources(); });
    inoutTable.blockSignals(hasBlockedSignals);
}

void RenderGraphConfigurationEditor::refreshSources()
{
    const QJsonArray inputs = GetInputs();
    const QJsonArray nodes = mConfiguration.value(QStringLiteral("nodes")).toArray();
    for (int32 row = 0; row < mOutputs->rowCount(); ++row)
    {
        QComboBox* type = qobject_cast<QComboBox*>(mOutputs->cellWidget(row, TYPE_COLUMN));
        QComboBox* source = qobject_cast<QComboBox*>(mOutputs->cellWidget(row, SOURCE_COLUMN));
        const QSignalBlocker blocker(source);
        const QString previous = source->currentData().toString();
        source->clear();
        source->addItem(QStringLiteral("Unconnected"), QString());
        const auto addSource = [source,
                                type](const QString& endpoint, const QString& candidateType, const QString& origin) {
            if (type->currentText() == candidateType)
            {
                source->addItem(endpoint, origin);
            }
        };
        for (int32 inputRow = 0; inputRow < mInputs->rowCount(); ++inputRow)
        {
            const QJsonObject input = inputs.at(inputRow).toObject();
            const QString original = mInputs->item(inputRow, 0)->data(Qt::UserRole).toString();
            const QString identity = input.value(QStringLiteral("id")).toString();
            addSource(QStringLiteral("input.") + identity,
                      input.value(QStringLiteral("type")).toString(),
                      QStringLiteral("input.") + (original.isEmpty() == true ? identity : original));
        }
        for (const QJsonValueConstRef node : nodes)
        {
            RenderGraphNodePorts ports;
            if (GetRenderGraphNodePorts(mDocument, node.toObject(), ports) == true)
            {
                for (const RenderGraphPort& port : ports.Outputs)
                {
                    const QString endpoint =
                        node.toObject().value(QStringLiteral("id")).toString() + QLatin1Char('.') + port.Id;
                    addSource(endpoint, port.Type, endpoint);
                }
            }
        }
        int32 selected = source->findData(previous);
        if (previous.isEmpty() == false && selected < 0)
        {
            // Keep an authored source visible while names/types are being edited. Apply
            // diagnoses incompatibility; filtering choices must never silently disconnect it.
            source->addItem(previous + QStringLiteral(" (unavailable)"), previous);
            selected = source->count() - 1;
        }
        source->setCurrentIndex(qMax(0, selected));
    }
    QStringList repairs;
    QString error;
    const RenderGraphDocumentStatus status =
        mDocument.PreviewConfigurationInterface(mFile,
                                                mConfiguration.value(QStringLiteral("id")).toString(),
                                                GetInputs(),
                                                GetOutputs(),
                                                repairs,
                                                error,
                                                GetInputRenames(),
                                                GetOutputRenames());
    mRepairs->setText(status != RenderGraphDocumentStatus::Ok ? error
                      : repairs.isEmpty() == true ? QStringLiteral("No existing links need repair.")
                                                  : QStringLiteral("Apply will repair these uses in one undo step:\n") +
                                                        repairs.join(QLatin1Char('\n')));
}

QJsonArray RenderGraphConfigurationEditor::GetInputs() const
{
    return ReadPorts(*mInputs, false);
}

QJsonArray RenderGraphConfigurationEditor::GetOutputs() const
{
    return ReadPorts(*mOutputs, true);
}

namespace
{
QMap<QString, QString> ReadRenames(const QTableWidget& table)
{
    QMap<QString, QString> renames;
    for (int32 row = 0; row < table.rowCount(); ++row)
    {
        const QString previous = table.item(row, 0)->data(Qt::UserRole).toString();
        const QString next = table.item(row, 0)->text().trimmed();
        if (previous.isEmpty() == false && previous != next)
        {
            renames.insert(previous, next);
        }
    }
    return renames;
}
} // namespace
QMap<QString, QString> RenderGraphConfigurationEditor::GetInputRenames() const
{
    return ReadRenames(*mInputs);
}
QMap<QString, QString> RenderGraphConfigurationEditor::GetOutputRenames() const
{
    return ReadRenames(*mOutputs);
}

void RenderGraphConfigurationEditor::ShowError(const QString& message)
{
    mError->setText(message);
}
} // namespace ludus::editor
