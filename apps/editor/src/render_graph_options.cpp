#include "internal/render_graph_options.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"

#include <ludus/foundation/base/core.h>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_OPTIONS = LUDUS_EDITOR_OPTIONS_MAX_OPTIONS;
constexpr int32 MAX_VALUES = LUDUS_EDITOR_OPTIONS_MAX_VALUES;
constexpr int32 MAX_COMBINATIONS = LUDUS_EDITOR_OPTIONS_MAX_COMBINATIONS;
constexpr int32 MAX_LABEL_LENGTH = LUDUS_EDITOR_OPTIONS_MAX_LABEL_LENGTH;
constexpr int32 MAX_ENUM_LENGTH = LUDUS_EDITOR_OPTIONS_MAX_ENUM_LENGTH;
constexpr float64 MAX_OPTION_NUMBER = LUDUS_EDITOR_OPTIONS_MAX_NUMERIC_VALUE;
enum Column : int32
{
    ID,
    LABEL,
    TYPE,
    TARGET,
    VALUES,
    DEFAULT_VALUE,
    COLUMN_COUNT
};
QString DescribeValue(const QJsonValue& value)
{
    if (value.isString() == true)
    {
        return value.toString();
    }
    const QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}
bool ContainsValue(const QJsonArray& values, const QJsonValue& value)
{
    return values.contains(value);
}
QJsonObject FindConfiguration(const QJsonObject& graph, const QString& identity)
{
    for (const QJsonValueConstRef entry : graph.value(QStringLiteral("configurations")).toArray())
    {
        if (entry.toObject().value(QStringLiteral("id")) == identity)
        {
            return entry.toObject();
        }
    }
    return {};
}
QJsonArray FindNodeOptions(const QMap<QString, QByteArray>& files, const QJsonObject& node)
{
    if (node.value(QStringLiteral("kind")) == QStringLiteral("Subgraph"))
    {
        const QJsonObject graph =
            QJsonDocument::fromJson(files.value(node.value(QStringLiteral("asset")).toString())).object();
        return FindConfiguration(graph, node.value(QStringLiteral("configuration")).toString())
            .value(QStringLiteral("options"))
            .toArray();
    }
    return node.value(QStringLiteral("options")).toArray();
}
} // namespace
QStringList GetRenderGraphOptionTargets(const QJsonObject& node)
{
    QStringList targets;
    for (const QString& group : {QStringLiteral("state"), QStringLiteral("invocation")})
    {
        const QJsonObject fields = node.value(group).toObject();
        for (QJsonObject::const_iterator field = fields.constBegin(); field != fields.constEnd(); ++field)
        {
            if (field.value().isString() == true || field.value().isBool() == true || field.value().isDouble() == true)
            {
                targets.append(group + QLatin1Char('.') + field.key());
            }
        }
    }
    for (const QJsonValueConstRef shader : node.value(QStringLiteral("shaders")).toArray())
    {
        targets.append(QStringLiteral("shaders.") + shader.toObject().value(QStringLiteral("id")).toString() +
                       QStringLiteral(".program"));
    }
    return targets;
}
QStringList GetRenderGraphOptionPrograms(const QJsonObject& node)
{
    QStringList programs;
    for (const QJsonValueConstRef entry : node.value(QStringLiteral("options")).toArray())
    {
        const QJsonObject option = entry.toObject();
        const QString target = option.value(QStringLiteral("target")).toString();
        if (target.startsWith(QStringLiteral("shaders.")) == true &&
            target.endsWith(QStringLiteral(".program")) == true)
        {
            for (const QJsonValueConstRef value : option.value(QStringLiteral("values")).toArray())
            {
                programs.append(value.toString());
            }
        }
    }
    return programs;
}
bool HasRenderGraphOptionProgram(const QMap<QString, QByteArray>& files, const QString& program)
{
    for (QMap<QString, QByteArray>::const_iterator file = files.constBegin(); file != files.constEnd(); ++file)
    {
        if (file.key().endsWith(QStringLiteral(".lurender.json")) == false)
        {
            continue;
        }
        const QJsonObject graph = QJsonDocument::fromJson(file.value()).object();
        for (const QJsonValueConstRef configuration : graph.value(QStringLiteral("configurations")).toArray())
        {
            for (const QJsonValueConstRef node : configuration.toObject().value(QStringLiteral("nodes")).toArray())
            {
                if (GetRenderGraphOptionPrograms(node.toObject()).contains(program) == true)
                {
                    return true;
                }
            }
        }
    }
    return false;
}
QJsonArray GetRenderGraphNodeOptions(const RenderGraphDocument& document, const QJsonObject& node)
{
    QMap<QString, QByteArray> files;
    if (node.value(QStringLiteral("kind")) == QStringLiteral("Subgraph"))
    {
        const QString asset = node.value(QStringLiteral("asset")).toString();
        files.insert(asset, document.GetDraft(asset));
    }
    return FindNodeOptions(files, node);
}
bool ValidateRenderGraphOptions(const QJsonArray& declarations, const QJsonObject& overrides, QString& outError)
{
    outError = QStringLiteral(
        "Use unique option IDs and targets, a label, a type, 1–16 allowed values and a default from that list.");
    if (declarations.size() > MAX_OPTIONS)
    {
        return false;
    }
    const QRegularExpression identifier(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    int32 combinations = 1;
    QSet<QString> identities;
    QSet<QString> targets;
    for (const QJsonValueConstRef entry : declarations)
    {
        const QJsonObject option = entry.toObject();
        const QString identity = option.value(QStringLiteral("id")).toString();
        const QString target = option.value(QStringLiteral("target")).toString();
        const QString label = option.value(QStringLiteral("label")).toString();
        const QString type = option.value(QStringLiteral("type")).toString();
        const QJsonArray values = option.value(QStringLiteral("values")).toArray();
        if (option.size() != COLUMN_COUNT || identifier.match(identity).hasMatch() == false ||
            identities.contains(identity) == true || target.isEmpty() == true || targets.contains(target) == true ||
            label.isEmpty() == true || label.size() > MAX_LABEL_LENGTH || values.isEmpty() == true ||
            values.size() > MAX_VALUES || ContainsValue(values, option.value(QStringLiteral("default"))) == false)
        {
            return false;
        }
        combinations *= static_cast<int32>(values.size());
        if (combinations > MAX_COMBINATIONS)
        {
            outError =
                QStringLiteral("At most %1 combinations per interface. Prefer one named mode to independent switches.")
                    .arg(MAX_COMBINATIONS);
            return false;
        }
        QJsonArray seen;
        for (const QJsonValueConstRef value : values)
        {
            const bool isValid = type == QStringLiteral("Bool") ? value.isBool()
                                 : type == QStringLiteral("Enum")
                                     ? value.isString() == true && value.toString().isEmpty() == false &&
                                           value.toString().size() <= MAX_ENUM_LENGTH
                                 : type == QStringLiteral("Float32")
                                     ? value.isDouble() == true && value.toDouble() >= -MAX_OPTION_NUMBER &&
                                           value.toDouble() <= MAX_OPTION_NUMBER
                                     : type == QStringLiteral("UInt32") && value.isDouble() == true &&
                                           value.toDouble() >= 0 && value.toDouble() <= MAX_OPTION_NUMBER &&
                                           value.toDouble() == static_cast<float64>(value.toInteger());
            if (isValid == false || seen.contains(value) == true)
            {
                return false;
            }
            seen.append(value);
        }
        identities.insert(identity);
        targets.insert(target);
        if (overrides.contains(identity) == true && ContainsValue(values, overrides.value(identity)) == false)
        {
            outError =
                QStringLiteral("Override %1 is outside its allowed values. Reset it to the default.").arg(identity);
            return false;
        }
    }
    for (QJsonObject::const_iterator value = overrides.constBegin(); value != overrides.constEnd(); ++value)
    {
        if (identities.contains(value.key()) == false)
        {
            outError = QStringLiteral("Override %1 is no longer declared. Reset or remove it first.").arg(value.key());
            return false;
        }
    }
    outError.clear();
    return true;
}
bool ResolveRenderGraphPipelineOptions(const QJsonObject& node, QJsonObject& outPipeline, QString& outError)
{
    const QJsonArray options = node.value(QStringLiteral("options")).toArray();
    const QJsonObject overrides = node.value(QStringLiteral("overrides")).toObject();
    if (ValidateRenderGraphOptions(options, overrides, outError) == false)
    {
        return false;
    }
    QJsonObject result = node;
    const QStringList targets = GetRenderGraphOptionTargets(node);
    for (const QJsonValueConstRef entry : options)
    {
        const QJsonObject option = entry.toObject();
        const QString target = option.value(QStringLiteral("target")).toString();
        if (targets.contains(target) == false)
        {
            outError =
                QStringLiteral("Option target %1 no longer exists. Repair the binding before changing this node.")
                    .arg(target);
            return false;
        }
        const QString identity = option.value(QStringLiteral("id")).toString();
        const QJsonValue value =
            overrides.contains(identity) == true ? overrides.value(identity) : option.value(QStringLiteral("default"));
        const QStringList path = target.split(QLatin1Char('.'));
        if (path.first() == QStringLiteral("shaders"))
        {
            if (value.isString() == false)
            {
                outError = QStringLiteral("Shader options must use Enum program names.");
                return false;
            }
            QJsonArray shaders = result.value(QStringLiteral("shaders")).toArray();
            for (qsizetype index = 0; index < shaders.size(); ++index)
            {
                QJsonObject shader = shaders.at(index).toObject();
                if (shader.value(QStringLiteral("id")) == path.at(1))
                {
                    shader.insert(QStringLiteral("program"), value);
                    shaders[index] = shader;
                }
            }
            result.insert(QStringLiteral("shaders"), shaders);
        }
        else
        {
            QJsonObject fields = result.value(path.first()).toObject();
            if (fields.value(path.last()).type() != value.type())
            {
                outError = QStringLiteral("Option %1 has a different type from its target.").arg(identity);
                return false;
            }
            fields.insert(path.last(), value);
            result.insert(path.first(), fields);
        }
    }
    outPipeline = result;
    return true;
}
bool ValidateRenderGraphOptionInterfaces(const QMap<QString, QByteArray>& files, QString& outError)
{
    for (QMap<QString, QByteArray>::const_iterator file = files.constBegin(); file != files.constEnd(); ++file)
    {
        if (file.key().endsWith(QStringLiteral(".lurender.json")) == false)
        {
            continue;
        }
        const QJsonObject graph = QJsonDocument::fromJson(file.value()).object();
        for (const QJsonValueConstRef entry : graph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject configuration = entry.toObject();
            const QJsonArray declarations = configuration.value(QStringLiteral("options")).toArray();
            if (ValidateRenderGraphOptions(declarations, {}, outError) == false)
            {
                return false;
            }
            QMap<QString, QJsonObject> nodes;
            for (const QJsonValueConstRef value : configuration.value(QStringLiteral("nodes")).toArray())
            {
                const QJsonObject node = value.toObject();
                nodes.insert(node.value(QStringLiteral("id")).toString(), node);
                const QJsonArray options = FindNodeOptions(files, node);
                if (ValidateRenderGraphOptions(options, node.value(QStringLiteral("overrides")).toObject(), outError) ==
                    false)
                {
                    return false;
                }
                if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
                {
                    QJsonObject effective;
                    if (ResolveRenderGraphPipelineOptions(node, effective, outError) == false)
                    {
                        return false;
                    }
                }
            }
            for (const QJsonValueConstRef value : declarations)
            {
                const QJsonObject option = value.toObject();
                const QStringList target = option.value(QStringLiteral("target")).toString().split(QLatin1Char('.'));
                bool hasCompatibleTarget = false;
                if (target.size() == 2 && nodes.contains(target.first()) == true)
                {
                    for (const QJsonValueConstRef child : FindNodeOptions(files, nodes.value(target.first())))
                    {
                        const QJsonObject forwarded = child.toObject();
                        if (forwarded.value(QStringLiteral("id")) == target.last() &&
                            forwarded.value(QStringLiteral("type")) == option.value(QStringLiteral("type")))
                        {
                            hasCompatibleTarget = true;
                            for (const QJsonValueConstRef choice : option.value(QStringLiteral("values")).toArray())
                            {
                                hasCompatibleTarget =
                                    hasCompatibleTarget == true &&
                                    forwarded.value(QStringLiteral("values")).toArray().contains(choice) == true;
                            }
                        }
                    }
                }
                if (hasCompatibleTarget == false)
                {
                    outError =
                        QStringLiteral(
                            "Exposed option %1 needs a compatible node.OptionId target. Update its callers first.")
                            .arg(option.value(QStringLiteral("id")).toString());
                    return false;
                }
            }
        }
    }
    outError.clear();
    return true;
}
RenderGraphDocumentStatus RenderGraphDocument::SetConfigurationOptions(const QString& file,
                                                                       const QString& configuration,
                                                                       const QJsonArray& options,
                                                                       QString& outError)
{
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        outError = QStringLiteral("Graph source is unavailable.");
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    for (qsizetype index = 0; index < configurations.size(); ++index)
    {
        QJsonObject record = configurations.at(index).toObject();
        if (record.value(QStringLiteral("id")) != configuration)
        {
            continue;
        }
        if (options.isEmpty() == true)
        {
            record.remove(QStringLiteral("options"));
        }
        else
        {
            record.insert(QStringLiteral("options"), options);
        }
        configurations[index] = record;
        graph.insert(QStringLiteral("configurations"), configurations);
        RenderGraphSnapshot candidate = mDraft;
        candidate.Files.insert(file, EncodeCanonicalJson(graph));
        if (ValidateRenderGraphOptionInterfaces(candidate.Files, outError) == false)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        if (candidate == mDraft)
        {
            return RenderGraphDocumentStatus::Ok;
        }
        mLastMove.clear();
        return commit(candidate, false);
    }
    outError = QStringLiteral("Configuration is unavailable.");
    return RenderGraphDocumentStatus::NotFound;
}
RenderGraphOptionsEditor::RenderGraphOptionsEditor(const QJsonArray& declarations,
                                                   const QJsonObject& overrides,
                                                   const QStringList& targets,
                                                   bool canAuthor,
                                                   QWidget* parentOrNull)
    : QWidget(parentOrNull), mError(new QLabel(this)), mTargets(targets), mOverrides(overrides)
{
    setObjectName(QStringLiteral("renderGraphOptionsEditor"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QFormLayout* choices = new QFormLayout;
    choices->setRowWrapPolicy(QFormLayout::WrapLongRows);
    for (const QJsonValueConstRef entry : declarations)
    {
        const QJsonObject option = entry.toObject();
        const QString identity = option.value(QStringLiteral("id")).toString();
        QComboBox* choice = new QComboBox(this);
        choice->setObjectName(QStringLiteral("renderGraphOption_") + identity);
        choice->addItem(QStringLiteral("Default (%1)").arg(DescribeValue(option.value(QStringLiteral("default")))));
        const QJsonArray values = option.value(QStringLiteral("values")).toArray();
        for (const QJsonValueConstRef value : values)
        {
            choice->addItem(DescribeValue(value), value.toVariant());
        }
        if (overrides.contains(identity) == true)
        {
            for (int32 index = 0; index < values.size(); ++index)
            {
                if (values.at(index) == overrides.value(identity))
                {
                    choice->setCurrentIndex(index + 1);
                }
            }
        }
        choices->addRow(option.value(QStringLiteral("label")).toString(), choice);
        connect(choice, &QComboBox::activated, this, [this, choice, identity]() {
            if (choice->currentIndex() == 0)
            {
                mOverrides.remove(identity);
            }
            else
            {
                mOverrides.insert(identity, QJsonValue::fromVariant(choice->currentData()));
            }
            Q_EMIT OverridesEdited(mOverrides);
        });
    }
    layout->addLayout(choices);
    if (canAuthor == true)
    {
        mTableOrNull = new QTableWidget(0, COLUMN_COUNT, this);
        mTableOrNull->setObjectName(QStringLiteral("renderGraphOptionDeclarations"));
        mTableOrNull->setHorizontalHeaderLabels({QStringLiteral("ID"),
                                                 QStringLiteral("Label"),
                                                 QStringLiteral("Type"),
                                                 QStringLiteral("Target"),
                                                 QStringLiteral("Allowed values"),
                                                 QStringLiteral("Default")});
        mTableOrNull->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        for (const QJsonValueConstRef option : declarations)
        {
            addDeclaration(option.toObject());
        }
        layout->addWidget(mTableOrNull);
        QHBoxLayout* actions = new QHBoxLayout;
        QPushButton* add = new QPushButton(QStringLiteral("Add Option"), this);
        QPushButton* remove = new QPushButton(QStringLiteral("Remove Option"), this);
        QPushButton* apply = new QPushButton(QStringLiteral("Apply Options"), this);
        apply->setObjectName(QStringLiteral("renderGraphApplyOptions"));
        actions->addWidget(add);
        actions->addWidget(remove);
        actions->addWidget(apply);
        layout->addLayout(actions);
        connect(add, &QPushButton::clicked, this, [this]() {
            if (mTableOrNull->rowCount() < MAX_OPTIONS)
            {
                addDeclaration({});
            }
        });
        connect(remove, &QPushButton::clicked, this, [this]() { mTableOrNull->removeRow(mTableOrNull->currentRow()); });
        connect(apply, &QPushButton::clicked, this, &RenderGraphOptionsEditor::publishDeclarations);
    }
    QString declarationError;
    if (ValidateRenderGraphOptions(declarations, overrides, declarationError) == false)
    {
        mError->setText(declarationError);
    }
    mError->setWordWrap(true);
    layout->addWidget(mError);
    const QString helpText =
        canAuthor == true
            ? QStringLiteral(
                  "Choices affect this use only. Default follows the author's default. Save and rebuild to apply.\n"
                  "Allowed values are separated by commas. Targets select existing fields or shader programs; "
                  "exposed graph options forward to node.OptionId.")
        : declarations.isEmpty() == true
            ? QStringLiteral("This graph has no exposed options. Enter its graph to author its public choices.")
            : QStringLiteral(
                  "Choices affect this use only. Default follows the author's default. Save and rebuild to apply.");
    QLabel* help = new QLabel(helpText, this);
    help->setWordWrap(true);
    layout->addWidget(help);
}
void RenderGraphOptionsEditor::addDeclaration(const QJsonObject& declaration)
{
    LUDUS_REQUIRE(mTableOrNull != nullptr);
    const int32 row = mTableOrNull->rowCount();
    mTableOrNull->insertRow(row);
    for (const int32 column : {ID, LABEL, VALUES, DEFAULT_VALUE})
    {
        QString text;
        if (column == VALUES)
        {
            QStringList values;
            for (const QJsonValueConstRef value : declaration.value(QStringLiteral("values")).toArray())
            {
                values.append(DescribeValue(value));
            }
            text = values.join(QStringLiteral(", "));
        }
        else
        {
            text = column == DEFAULT_VALUE
                       ? DescribeValue(declaration.value(QStringLiteral("default")))
                       : declaration.value(column == ID ? QStringLiteral("id") : QStringLiteral("label")).toString();
        }
        mTableOrNull->setItem(row, column, new QTableWidgetItem(text));
    }
    QComboBox* type = new QComboBox(mTableOrNull);
    type->addItems(
        {QStringLiteral("Enum"), QStringLiteral("Bool"), QStringLiteral("UInt32"), QStringLiteral("Float32")});
    type->setCurrentText(declaration.value(QStringLiteral("type")).toString(QStringLiteral("Enum")));
    mTableOrNull->setCellWidget(row, TYPE, type);
    QComboBox* target = new QComboBox(mTableOrNull);
    target->addItems(mTargets);
    const QString binding = declaration.value(QStringLiteral("target")).toString();
    if (binding.isEmpty() == false && mTargets.contains(binding) == false)
    {
        target->addItem(binding);
    }
    if (binding.isEmpty() == false)
    {
        target->setCurrentText(binding);
    }
    mTableOrNull->setCellWidget(row, TARGET, target);
}
void RenderGraphOptionsEditor::publishDeclarations()
{
    LUDUS_REQUIRE(mTableOrNull != nullptr);
    QJsonArray declarations;
    for (int32 row = 0; row < mTableOrNull->rowCount(); ++row)
    {
        const QString type = static_cast<QComboBox*>(mTableOrNull->cellWidget(row, TYPE))->currentText();
        const auto parse = [&type](const QString& text) -> QJsonValue {
            if (type == QStringLiteral("Enum"))
            {
                return text.trimmed();
            }
            const QJsonArray value =
                QJsonDocument::fromJson((QLatin1Char('[') + text.trimmed() + QLatin1Char(']')).toUtf8()).array();
            return value.size() == 1 ? value.first() : QJsonValue{};
        };
        QJsonArray values;
        for (const QString& text : mTableOrNull->item(row, VALUES)->text().split(QLatin1Char(',')))
        {
            values.append(parse(text));
        }
        declarations.append(QJsonObject{
            {QStringLiteral("id"), mTableOrNull->item(row, ID)->text().trimmed()},
            {QStringLiteral("label"), mTableOrNull->item(row, LABEL)->text().trimmed()},
            {QStringLiteral("type"), type},
            {QStringLiteral("target"), static_cast<QComboBox*>(mTableOrNull->cellWidget(row, TARGET))->currentText()},
            {QStringLiteral("values"), values},
            {QStringLiteral("default"), parse(mTableOrNull->item(row, DEFAULT_VALUE)->text())}});
    }
    QString error;
    if (ValidateRenderGraphOptions(declarations, mOverrides, error) == false)
    {
        mError->setText(error);
        return;
    }
    mError->clear();
    Q_EMIT DeclarationsEdited(declarations);
}
} // namespace ludus::editor
