#include "internal/render_graph_pipeline.h"

#include <ludus/foundation/base/types.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_PORTS = 8;
constexpr int32 MAX_SHADERS = 16;
constexpr int32 TABLE_HEIGHT = 160;
constexpr int32 MAX_PROPERTY_VALUE = 65535;
constexpr int32 DIMENSION_COUNT = 3;

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
            record.insert(fields.at(column),
                          fields.at(column) == QStringLiteral("required")
                              ? QJsonValue(itemOrNull != nullptr && itemOrNull->checkState() == Qt::Checked)
                              : QJsonValue(value));
        }
        rows.append(record);
    }
    return rows;
}

QStringList GetCellChoices(const QString& field, const QString& kind, const QString& table)
{
    if (field == QStringLiteral("type"))
    {
        QStringList types{QStringLiteral("ColorTexture"), QStringLiteral("DepthTexture"), QStringLiteral("Buffer")};
        if (table == QStringLiteral("inputs"))
        {
            types += {QStringLiteral("Scene"),
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

void AddCell(QTableWidget& inoutTable,
             int32 row,
             int32 column,
             const QString& field,
             const QString& value,
             const QString& kind)
{
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
                           {QStringLiteral("id"),
                            QStringLiteral("type"),
                            QStringLiteral("target"),
                            QStringLiteral("format"),
                            QStringLiteral("load"),
                            QStringLiteral("store")},
                           {QStringLiteral("output"),
                            QStringLiteral("ColorTexture"),
                            QStringLiteral("input"),
                            QStringLiteral("RGBA8Unorm"),
                            QStringLiteral("Clear"),
                            QStringLiteral("Store")})),
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
    QTabWidget* tabs = new QTabWidget(this);
    for (QTableWidget* table : {mInputs, mOutputs, mShaders})
    {
        const QString name = table->objectName();
        tabs->addTab(table->parentWidget(), name.left(1).toUpper() + name.mid(1));
    }
    for (const QString& name : {QStringLiteral("state"), QStringLiteral("invocation")})
    {
        QWidget* panel = new QWidget(tabs);
        QFormLayout* form = new QFormLayout(panel);
        addProperties(*form, name);
        tabs->addTab(panel,
                     name == QStringLiteral("state") ? QStringLiteral("Pipeline State") : QStringLiteral("Invocation"));
    }
    layout->addWidget(tabs);
    QPushButton* applyButton = new QPushButton(QStringLiteral("Apply Pipeline"), this);
    applyButton->setObjectName(QStringLiteral("renderGraphApplyPipeline"));
    connect(applyButton, &QPushButton::clicked, this, &RenderGraphPipelineEditor::apply);
    layout->addWidget(applyButton);
    QLabel* hint = new QLabel(QStringLiteral("Apply Pipeline creates source files for new program names. "
                                             "Program receives a name based on the node and shader ID. "
                                             "Sources appear in Source; Save writes them to the project."),
                              this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
}

QTableWidget*
RenderGraphPipelineEditor::createTable(const QString& name, const QStringList& fields, const QStringList& defaults)
{
    QWidget* panel = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(panel);
    QTableWidget* table = new QTableWidget(panel);
    table->setObjectName(name);
    table->setColumnCount(static_cast<int32>(fields.size()));
    table->setHorizontalHeaderLabels(fields);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setMinimumHeight(TABLE_HEIGHT);
    table->setProperty("fields", fields);
    const QJsonArray rows = mNode.value(name).toArray();
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
    layout->addWidget(table);
    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* add = new QPushButton(QStringLiteral("Add"), panel);
    QPushButton* remove = new QPushButton(QStringLiteral("Remove Selected"), panel);
    add->setObjectName(QStringLiteral("renderGraphAdd_") + name);
    remove->setObjectName(QStringLiteral("renderGraphRemove_") + name);
    const QString kind = mNode.value(QStringLiteral("kind")).toString();
    connect(add, &QPushButton::clicked, this, [table, defaults, fields, name, kind]() {
        const int32 maximum = name == QStringLiteral("shaders") ? MAX_SHADERS : MAX_PORTS;
        if (table->rowCount() >= maximum)
        {
            return;
        }
        const int32 row = table->rowCount();
        table->insertRow(row);
        for (int32 column = 0; column < defaults.size(); ++column)
        {
            QString value = defaults.at(column);
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
        table->setCurrentCell(row, 0);
    });
    connect(remove, &QPushButton::clicked, this, [table]() {
        if (table->currentRow() >= 0)
        {
            table->removeRow(table->currentRow());
        }
    });
    buttons->addWidget(add);
    buttons->addWidget(remove);
    layout->addLayout(buttons);
    return table;
}

void RenderGraphPipelineEditor::addProperties(QFormLayout& outForm, const QString& name)
{
    const QJsonObject properties = mNode.value(name).toObject();
    for (QJsonObject::const_iterator property = properties.begin(); property != properties.end(); ++property)
    {
        const QString key = property.key();
        const QJsonValue value = property.value();
        const auto setProperty = [this, name, key](const QJsonValue& field) {
            QJsonObject record = mNode.value(name).toObject();
            record.insert(key, field);
            mNode.insert(name, record);
        };
        if (value.isBool() == true)
        {
            QCheckBox* control = new QCheckBox(this);
            control->setChecked(value.toBool());
            connect(control, &QCheckBox::toggled, this, [setProperty](bool isChecked) { setProperty(isChecked); });
            outForm.addRow(key, control);
        }
        else if (value.isDouble() == true)
        {
            QSpinBox* control = new QSpinBox(this);
            control->setRange(0, MAX_PROPERTY_VALUE);
            control->setValue(value.toInt());
            connect(control, &QSpinBox::valueChanged, this, [setProperty](int32 number) { setProperty(number); });
            outForm.addRow(key, control);
        }
        else if (value.isArray() == true)
        {
            QWidget* row = new QWidget(this);
            QHBoxLayout* layout = new QHBoxLayout(row);
            for (int32 dimension = 0; dimension < DIMENSION_COUNT; ++dimension)
            {
                QSpinBox* control = new QSpinBox(row);
                control->setRange(1, MAX_PROPERTY_VALUE);
                control->setValue(value.toArray().at(dimension).toInt());
                connect(control, &QSpinBox::valueChanged, this, [this, name, key, dimension](int32 number) {
                    QJsonObject record = mNode.value(name).toObject();
                    QJsonArray dimensions = record.value(key).toArray();
                    dimensions.replace(dimension, number);
                    record.insert(key, dimensions);
                    mNode.insert(name, record);
                });
                layout->addWidget(control);
            }
            outForm.addRow(key, row);
        }
        else
        {
            QLineEdit* control = new QLineEdit(value.toString(), this);
            connect(control, &QLineEdit::textChanged, this, [setProperty](const QString& text) { setProperty(text); });
            outForm.addRow(key, control);
        }
    }
}

void RenderGraphPipelineEditor::apply()
{
    QJsonObject candidate = mNode;
    for (const QTableWidget* table : {mInputs, mOutputs, mShaders})
    {
        candidate.insert(table->objectName(), ReadRows(*table, table->property("fields").toStringList()));
    }
    Q_EMIT Applied(candidate);
}
} // namespace ludus::editor
