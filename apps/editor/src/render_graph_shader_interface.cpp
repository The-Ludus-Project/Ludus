#include "internal/render_graph_shader_interface.h"

#include <ludus/foundation/base/types.h>

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QStringList>
#include <QStringView>
#include <QTextCursor>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
// These expressions index declarations in the bundled, renderer-owned Slang files.
// They are a source view, not a general Slang parser or compiler reflection.
QString ReadShaderApi(bool isScene)
{
    QFile file(isScene == true ? QStringLiteral(":/shader-api/scene_shader.slang")
                               : QStringLiteral(":/shader-api/ray_shader.slang"));
    return file.open(QIODevice::ReadOnly) == true ? QString::fromUtf8(file.readAll()) : QString{};
}

QString GetStructDeclaration(const QString& source, QStringView name)
{
    const QRegularExpression expression(
        QStringLiteral("\\bstruct\\s+%1\\s*\\{[^}]*\\};").arg(QRegularExpression::escape(name.toString())));
    return expression.match(source).captured();
}

/// A callback belongs to the technique that names it; a scene compute kernel shares the ray technique's
/// scene interface without being one of its callbacks.
bool UsesTechnique(const QJsonObject& technique, const QJsonObject& program)
{
    const QString stage = program.value(QStringLiteral("stage")).toString();
    return stage == QStringLiteral("compute")
               ? technique.value(QStringLiteral("id")).toString() == QStringLiteral("RendererTriangles.RayTracing")
               : technique.value(stage).toString() == program.value(QStringLiteral("id")).toString();
}

QString
DescribeRayInterface(const QJsonObject& program, const QJsonArray& techniques, const QMap<QString, QByteArray>& sources)
{
    const QString stage = program.value(QStringLiteral("stage")).toString();
    const QString input = program.value(QStringLiteral("input")).toString();
    const QString output = program.value(QStringLiteral("output")).toString();
    const QString function = program.value(QStringLiteral("function")).toString();
    const bool isScene = output == QStringLiteral("Radiance") || output == QStringLiteral("ScenePayload");
    const bool isLaunch = stage == QStringLiteral("ray_generation") || stage == QStringLiteral("compute");
    QString api = ReadShaderApi(isScene);
    for (const QJsonValueConstRef entry : techniques)
    {
        const QJsonObject technique = entry.toObject();
        if (UsesTechnique(technique, program) == true && technique.value(QStringLiteral("scene_interface"))
                                                                 .toObject()
                                                                 .value(QStringLiteral("visibility_buffer"))
                                                                 .toBool() == true)
        {
            QFile visibility(QStringLiteral(":/shader-api/visibility_buffer.slang"));
            if (visibility.open(QIODevice::ReadOnly) == true)
            {
                api += QString::fromUtf8(visibility.readAll());
            }
            break;
        }
    }
    if (api.isEmpty() == true)
    {
        return QStringLiteral("Shader API unavailable: bundled callback declarations are missing.");
    }
    QStringList lines{QStringLiteral("// %1 (%2)").arg(program.value(QStringLiteral("id")).toString(), stage)};
    if (isScene == true)
    {
        lines.append(
            stage == QStringLiteral("compute")
                ? QStringLiteral("// Scene compute pass: one invocation per launch pixel in the ordered ray sequence.\n"
                                 "// Trace with TraceClosest or TestVisibility; return linear radiance.\n"
                                 "// There are no hit or miss callbacks: TraceAndShade latches queries.HasFailed.")
                : QStringLiteral(
                      "// Scene materials enabled.\n"
                      "// Closest-hit material access: MaterialSample material = EvaluateMaterial(scene, hit);\n"
                      "// Author lighting and bounce policy in your callbacks."));
        lines.append(
            QStringLiteral("%1 %2(\n    SceneContext scene,\n    %3 %4,\n    inout RayQueryContext queries%5);")
                .arg(isLaunch == true ? QStringLiteral("float3") : QStringLiteral("void"),
                     function,
                     input,
                     isLaunch == true                  ? QStringLiteral("launch")
                     : stage == QStringLiteral("miss") ? QStringLiteral("ray")
                                                       : QStringLiteral("hit"),
                     isLaunch == true ? QString{} : QStringLiteral(",\n    inout ScenePayload payload")));
    }
    else
    {
        lines.append(QStringLiteral("%1 %2(%3 input);")
                         .arg(output == QStringLiteral("Ray") ? QStringLiteral("Ray") : QStringLiteral("float3"),
                              function,
                              input));
    }
    lines.append(GetStructDeclaration(api, input));
    if (isScene == false)
    {
        lines.append(
            QStringLiteral("// Legacy ray interface. Return linear RGB; no automatic lighting.\n"
                           "// For full material properties, right-click the RayTracingPipeline on the canvas\n"
                           "// and choose Enable Scene Materials. Original callbacks are preserved through adapters."));
        if (stage == QStringLiteral("closest_hit"))
        {
            lines.append(QStringLiteral("// Closest-hit material tint: return input.Color.xyz; Color.w is reserved."));
        }
        if (input != QStringLiteral("Ray"))
        {
            lines.append(GetStructDeclaration(api, QStringLiteral("Ray")));
        }
        return lines.join(QStringLiteral("\n\n"));
    }
    const QRegularExpression structures(QStringLiteral("\\bstruct\\s+(\\w+)\\s*\\{[^}]*\\};"));
    QRegularExpressionMatchIterator structureMatches = structures.globalMatch(api);
    while (structureMatches.hasNext() == true)
    {
        const QRegularExpressionMatch match = structureMatches.next();
        if (match.captured(1) != input)
        {
            lines.append(match.captured());
        }
    }
    QStringList payloadFiles;
    for (const QJsonValueConstRef entry : techniques)
    {
        const QJsonObject technique = entry.toObject();
        if (UsesTechnique(technique, program) == false)
        {
            continue;
        }
        const QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
        const bool hasCustomPayload = interface.contains(QStringLiteral("payload"));
        const QString payload = hasCustomPayload == true
                                    ? QDir::cleanPath(interface.value(QStringLiteral("payload")).toString())
                                    : QStringLiteral(":/shader-api/scene_payload.slang");
        if (payloadFiles.contains(payload) == true)
        {
            continue;
        }
        payloadFiles.append(payload);
        QByteArray payloadSource = sources.value(payload);
        if (hasCustomPayload == false)
        {
            QFile file(payload);
            if (file.open(QIODevice::ReadOnly) == true)
            {
                payloadSource = file.readAll();
            }
        }
        const QString declaration =
            GetStructDeclaration(QString::fromUtf8(payloadSource), QStringLiteral("ScenePayload"));
        lines.append(
            declaration.isEmpty() == false
                ? QStringLiteral("// %1\n%2")
                      .arg(hasCustomPayload == true ? payload : QStringLiteral("Pipeline default payload"), declaration)
                : QStringLiteral("// ScenePayload declaration unavailable: %1").arg(payload));
    }
    if (payloadFiles.isEmpty() == true)
    {
        lines.append(
            QStringLiteral("// ScenePayload declaration unavailable: no scene_interface payload is assigned."));
    }
    QStringList functions{
        QStringLiteral("uint TraceAndShade(\n    SceneContext scene,\n    inout RayQueryContext queries,\n"
                       "    Ray ray,\n    inout ScenePayload payload);")};
    const QRegularExpression signatures(QStringLiteral("^\\w+ [A-Z]\\w*\\([^)]*\\)(?=\\s*\\{)"),
                                        QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator signatureMatches = signatures.globalMatch(api);
    while (signatureMatches.hasNext() == true)
    {
        functions.append(signatureMatches.next().captured() + QLatin1Char(';'));
    }
    lines.append(functions.join(QLatin1Char('\n')));
    const QRegularExpression constants(QStringLiteral("^static const [^;]+;"), QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator constantMatches = constants.globalMatch(api);
    QStringList constantDeclarations;
    while (constantMatches.hasNext() == true)
    {
        constantDeclarations.append(constantMatches.next().captured());
    }
    lines.append(constantDeclarations.join(QLatin1Char('\n')));
    return lines.join(QStringLiteral("\n\n"));
}

QString DescribeSchema(const QJsonArray& schemas, const QString& identity)
{
    QStringList lines{identity};
    for (const QJsonValueConstRef entry : schemas)
    {
        const QJsonObject schema = entry.toObject();
        if (schema.value(QStringLiteral("id")).toString() != identity)
        {
            continue;
        }
        const QJsonArray fields = schema.value(QStringLiteral("fields")).toArray();
        for (const QJsonValueConstRef fieldEntry : fields)
        {
            const QJsonObject field = fieldEntry.toObject();
            QString line =
                QStringLiteral("  %1 : %2")
                    .arg(field.value(QStringLiteral("id")).toString(), field.value(QStringLiteral("type")).toString());
            if (schema.value(QStringLiteral("role")).toString() != QStringLiteral("Uniform"))
            {
                const QString semantic = field.value(QStringLiteral("semantic")).toString();
                line += semantic == QStringLiteral("SV_Position")
                            ? QStringLiteral("  [SV_Position, built-in]")
                            : QStringLiteral("  [%1, location %2]")
                                  .arg(semantic, field.value(QStringLiteral("location")).toVariant().toString());
            }
            lines.append(line);
        }
        return lines.join(QLatin1Char('\n'));
    }
    return identity + QStringLiteral("\n  Schema unavailable in this draft.");
}
} // namespace

QString
DescribeShaderInterface(const QByteArray& manifest, const QString& sourceFile, const QMap<QString, QByteArray>& sources)
{
    const QJsonDocument document = QJsonDocument::fromJson(manifest);
    if (document.isObject() == false)
    {
        return QStringLiteral("Shader interface unavailable: the draft build manifest is missing or invalid.");
    }
    const QJsonObject root = document.object();
    const QJsonArray schemas = root.value(QStringLiteral("schemas")).toArray();
    const QJsonArray programs = root.value(QStringLiteral("programs")).toArray();
    QStringList descriptions;
    for (const QJsonValueConstRef entry : programs)
    {
        const QJsonObject program = entry.toObject();
        if (QDir::cleanPath(program.value(QStringLiteral("source")).toString()) != QDir::cleanPath(sourceFile))
        {
            continue;
        }
        const QString stage = program.value(QStringLiteral("stage")).toString();
        const QString output = program.value(QStringLiteral("output")).toString();
        if (stage == QStringLiteral("ray_generation") || stage == QStringLiteral("closest_hit") ||
            stage == QStringLiteral("miss") ||
            (stage == QStringLiteral("compute") && output == QStringLiteral("Radiance")))
        {
            descriptions.append(
                DescribeRayInterface(program, root.value(QStringLiteral("techniques")).toArray(), sources));
            continue;
        }
        if (stage != QStringLiteral("vertex") && stage != QStringLiteral("fragment"))
        {
            descriptions.append(QStringLiteral("%1 — %2 (%3)\n"
                                               "Edit the stage interface in Slang source.\n"
                                               "This stage is an authoring draft; the current graph build profile "
                                               "does not execute it.")
                                    .arg(program.value(QStringLiteral("id")).toString(),
                                         program.value(QStringLiteral("function")).toString(),
                                         stage));
            continue;
        }
        QStringList lines{
            QStringLiteral("%1 — %2 (%3)")
                .arg(program.value(QStringLiteral("id")).toString(),
                     program.value(QStringLiteral("function")).toString(),
                     stage),
            QStringLiteral("Inputs"),
            DescribeSchema(schemas, program.value(QStringLiteral("input")).toString()),
            QStringLiteral("\nOutputs"),
        };
        if (stage == QStringLiteral("fragment") && output == QStringLiteral("Color"))
        {
            lines.append(QStringLiteral("Color : float4 → SV_Target0\n  Return RGBA color from the function."));
        }
        else if (stage == QStringLiteral("fragment") && output == QStringLiteral("Depth"))
        {
            lines.append(QStringLiteral(
                "Depth : fixed-function depth\n  The function returns void; rasterization supplies depth.\n"
                "  Depth writes depend on the pipeline recipe."));
        }
        else
        {
            lines.append(DescribeSchema(schemas, output));
        }
        lines.append(QStringLiteral("\nResource inputs (read-only uniforms)"));
        const QJsonArray bindings = program.value(QStringLiteral("bindings")).toArray();
        if (bindings.isEmpty() == true)
        {
            lines.append(QStringLiteral("None declared."));
        }
        for (const QJsonValueConstRef bindingEntry : bindings)
        {
            const QJsonObject binding = bindingEntry.toObject();
            lines.append(QStringLiteral("%1 — slot %2, set 0\n%3")
                             .arg(binding.value(QStringLiteral("id")).toString(),
                                  binding.value(QStringLiteral("slot")).toVariant().toString(),
                                  DescribeSchema(schemas, binding.value(QStringLiteral("schema")).toString())));
        }
        descriptions.append(lines.join(QLatin1Char('\n')));
    }
    return descriptions.isEmpty() == true
               ? QStringLiteral("No shader program declares this source in the draft build manifest.")
               : descriptions.join(QStringLiteral("\n\n"));
}
QWidget* CreateShaderInterfaceView(QPlainTextEdit* textView, QWidget* parent)
{
    QWidget* page = new QWidget(parent);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(QStringLiteral("Shader API"), page));
    QHBoxLayout* searchRow = new QHBoxLayout;
    QLineEdit* search = new QLineEdit(page);
    search->setObjectName(QStringLiteral("shaderApiSearch"));
    search->setPlaceholderText(QStringLiteral("Find type, field, or function"));
    search->setAccessibleName(QStringLiteral("Find in shader API"));
    search->setClearButtonEnabled(true);
    QPushButton* next = new QPushButton(QStringLiteral("Next"), page);
    next->setObjectName(QStringLiteral("shaderApiFindNext"));
    searchRow->addWidget(search, 1);
    searchRow->addWidget(next);
    layout->addLayout(searchRow);
    textView->setObjectName(QStringLiteral("renderGraphSourceInterface"));
    textView->setReadOnly(true);
    textView->setLineWrapMode(QPlainTextEdit::NoWrap);
    textView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    textView->setAccessibleName(QStringLiteral("Shader API declarations"));
    layout->addWidget(textView, 1);
    const auto find = [textView, search](bool shouldRestart) {
        if (shouldRestart == true)
        {
            textView->moveCursor(QTextCursor::Start);
        }
        search->setToolTip({});
        if (search->text().isEmpty() == true)
        {
            return;
        }
        if (textView->find(search->text()) == false)
        {
            textView->moveCursor(QTextCursor::Start);
            const bool hasMatch = textView->find(search->text());
            search->setToolTip(hasMatch == true ? QString{} : QStringLiteral("No matching declaration."));
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, page, [find]() { find(true); });
    QObject::connect(search, &QLineEdit::returnPressed, page, [find]() { find(false); });
    QObject::connect(next, &QPushButton::clicked, page, [find]() { find(false); });
    QShortcut* shortcut = new QShortcut(QKeySequence::Find, page);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(shortcut, &QShortcut::activated, page, [search]() {
        search->setFocus();
        search->selectAll();
    });
    return page;
}

void UpdateShaderInterfaceView(QPlainTextEdit& inoutTextView, const QString& source, const QString& declarations)
{
    const bool isSameSource = inoutTextView.property("shaderApiSource").toString() == source;
    if (isSameSource == true && inoutTextView.toPlainText() == declarations)
    {
        return;
    }
    const foundation::int32 verticalPosition = inoutTextView.verticalScrollBar()->value();
    const foundation::int32 horizontalPosition = inoutTextView.horizontalScrollBar()->value();
    inoutTextView.setPlainText(declarations);
    inoutTextView.setProperty("shaderApiSource", source);
    if (isSameSource == true)
    {
        inoutTextView.verticalScrollBar()->setValue(verticalPosition);
        inoutTextView.horizontalScrollBar()->setValue(horizontalPosition);
    }
}
} // namespace ludus::editor
