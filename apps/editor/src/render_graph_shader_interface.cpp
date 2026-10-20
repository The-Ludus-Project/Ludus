#include "internal/render_graph_shader_interface.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace ludus::editor
{
namespace
{
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

QString DescribeShaderInterface(const QByteArray& manifest, const QString& sourceFile)
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
            stage == QStringLiteral("miss"))
        {
            descriptions.append(
                QStringLiteral(
                    "%1 — %2 (%3)\nInput: %4\nOutput: %5\n"
                    "Scene callbacks receive SceneContext and inout RayQueryContext. Raygen drives TraceAndShade; "
                    "closest-hit/miss share your ScenePayload. Read materials with EvaluateMaterial and sources with "
                    "GetLightInfo. SelectLight/SampleIncidentLight and TestVisibility provide proposals and queries; "
                    "your shaders own NEE, MIS and bounce policy. Legacy Ray/Color programs retain their original "
                    "signatures.")
                    .arg(program.value(QStringLiteral("id")).toString(),
                         program.value(QStringLiteral("function")).toString(),
                         stage,
                         program.value(QStringLiteral("input")).toString(),
                         output));
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
} // namespace ludus::editor
