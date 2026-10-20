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
#include <QRegularExpression>
#include <QSignalBlocker>
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
constexpr int32 PAIRED_INPUT_ROLE = Qt::UserRole + 1;
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
                            QStringLiteral("format"),
                            QStringLiteral("load"),
                            QStringLiteral("store")},
                           {QStringLiteral("resource"),
                            QStringLiteral("ColorTexture"),
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
    for (const QString& name : {QStringLiteral("state"), QStringLiteral("invocation")})
    {
        QWidget* panel = new QWidget(tabs);
        QFormLayout* form = new QFormLayout(panel);
        addProperties(*form, name);
        tabs->addTab(panel,
                     name == QStringLiteral("state") ? QStringLiteral("Pipeline State") : QStringLiteral("Invocation"));
    }
    QLabel* help = new QLabel(QStringLiteral("Each writable resource creates a paired input and output. Connect the "
                                             "texture/buffer to its input and use its output for the updated resource. "
                                             "Target associations are automatic."),
                              this);
    help->setWordWrap(true);
    layout->addWidget(help);
    layout->addWidget(tabs);
    mError = new QLabel(this);
    mError->setObjectName(QStringLiteral("renderGraphPipelineError"));
    mError->setWordWrap(true);
    layout->addWidget(mError);
    QLabel* hint = new QLabel(
        QStringLiteral("New shaders create .slang sources automatically. "
                       "Renaming moves its source; stage edits reuse it. Edit sources in Source; Save writes files."),
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
    if (name == QStringLiteral("outputs"))
    {
        table->setHorizontalHeaderLabels({QStringLiteral("Resource"),
                                          QStringLiteral("Type"),
                                          QStringLiteral("Format"),
                                          QStringLiteral("Load"),
                                          QStringLiteral("Store")});
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
    layout->addWidget(table);
    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* add = new QPushButton(QStringLiteral("Add"), panel);
    QPushButton* remove = new QPushButton(QStringLiteral("Remove Selected"), panel);
    add->setObjectName(QStringLiteral("renderGraphAdd_") + name);
    remove->setObjectName(QStringLiteral("renderGraphRemove_") + name);
    const QString kind = mNode.value(QStringLiteral("kind")).toString();
    connect(add, &QPushButton::clicked, this, [this, table, defaults, fields, name, kind, connectResourceType]() {
        const int32 maximum = name == QStringLiteral("shaders") ? MAX_SHADERS : MAX_PORTS;
        if (table->rowCount() >= maximum)
        {
            return;
        }
        const QSignalBlocker blocker(table);
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
            publishEdit();
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
                    publishEdit();
                });
                layout->addWidget(control);
            }
            outForm.addRow(key, row);
        }
        else
        {
            QLineEdit* control = new QLineEdit(value.toString(), this);
            connect(control, &QLineEdit::editingFinished, this, [setProperty, control]() {
                setProperty(control->text());
            });
            outForm.addRow(key, control);
        }
    }
}

void RenderGraphPipelineEditor::SetShaderAssignments(const QJsonArray& shaders)
{
    const QSignalBlocker blocker(mShaders);
    mNode.insert(QStringLiteral("shaders"), shaders);
    for (int32 row = 0; row < shaders.size() && row < mShaders->rowCount(); ++row)
    {
        mShaders->item(row, 2)->setText(shaders.at(row).toObject().value(QStringLiteral("program")).toString());
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
    mNode = candidate;
    Q_EMIT Edited(candidate);
}
} // namespace ludus::editor
