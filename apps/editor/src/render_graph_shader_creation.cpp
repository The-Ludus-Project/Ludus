#include "internal/render_graph_shader_creation.h"
#include "internal/render_graph_pipeline_node.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace ludus::editor
{
namespace
{
constexpr qsizetype GENERATED_NAME_PART_LENGTH = 20;

bool IsIdentifier(const QString& text)
{
    static const QRegularExpression expression(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_]{0,62}\\z"));
    return expression.match(text).hasMatch();
}

QString FindSchema(const QJsonArray& schemas, const QString& role)
{
    for (const QJsonValueConstRef value : schemas)
    {
        const QJsonObject schema = value.toObject();
        if (schema.value(QStringLiteral("role")).toString() == role)
        {
            return schema.value(QStringLiteral("id")).toString();
        }
    }
    return {};
}

QByteArray MakeSource(QJsonObject& inoutProgram, const QJsonArray& schemas)
{
    const QString stage = inoutProgram.value(QStringLiteral("stage")).toString();
    QString source = QStringLiteral("// Edit this shader, then Save to publish it to the project.\n");
    QString input;
    QString output;
    if (stage == QStringLiteral("vertex") || stage == QStringLiteral("fragment"))
    {
        input = FindSchema(schemas,
                           stage == QStringLiteral("vertex") ? QStringLiteral("Vertex") : QStringLiteral("Varying"));
        output = stage == QStringLiteral("vertex") ? FindSchema(schemas, QStringLiteral("Varying"))
                                                   : QStringLiteral("Color");
        if (input.isEmpty() == true || output.isEmpty() == true)
        {
            return {};
        }
        source += QStringLiteral("// Interface types are supplied by the shader manifest.\n");
        if (stage == QStringLiteral("vertex"))
        {
            source += QStringLiteral("%1 ShaderMain(%2 input)\n{\n    %1 result = (%1)0;\n"
                                     "    // TODO: transform the input into clip space.\n    return result;\n}\n")
                          .arg(output, input);
        }
        else
        {
            source += QStringLiteral("float4 ShaderMain(%1 input)\n{\n    return float4(1, 1, 1, 1);\n}\n").arg(input);
        }
    }
    else
    {
        source += QStringLiteral("// Authoring draft: the current graph build profile does not execute this stage.\n");
        if (stage == QStringLiteral("compute"))
        {
            source += QStringLiteral("[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid ShaderMain()\n{\n"
                                     "    // TODO: implement the compute kernel.\n}\n");
        }
        else if (stage == QStringLiteral("ray_generation") || stage == QStringLiteral("intersection"))
        {
            source += QStringLiteral("[shader(\"%1\")]\nvoid ShaderMain()\n{\n    // TODO: implement this stage.\n}\n")
                          .arg(stage == QStringLiteral("ray_generation") ? QStringLiteral("raygeneration") : stage);
        }
        else if (stage == QStringLiteral("miss") || stage == QStringLiteral("closest_hit") ||
                 stage == QStringLiteral("any_hit") || stage == QStringLiteral("callable"))
        {
            const QString annotation = stage == QStringLiteral("closest_hit") ? QStringLiteral("closesthit")
                                       : stage == QStringLiteral("any_hit")   ? QStringLiteral("anyhit")
                                                                              : stage;
            source += QStringLiteral("struct ShaderPayload\n{\n    float4 Color;\n};\n");
            QString arguments = QStringLiteral("inout ShaderPayload inoutPayload");
            if (stage == QStringLiteral("closest_hit") || stage == QStringLiteral("any_hit"))
            {
                source += QStringLiteral("struct ShaderAttributes\n{\n    float2 Barycentrics;\n};\n");
                arguments += QStringLiteral(", ShaderAttributes attributes");
            }
            source +=
                QStringLiteral("[shader(\"%1\")]\nvoid ShaderMain(%2)\n{\n    // TODO: implement this stage.\n}\n")
                    .arg(annotation, arguments);
        }
        else
        {
            source +=
                QStringLiteral("// TODO: define the %1 stage interface and implementation.\nvoid ShaderMain()\n{\n}\n")
                    .arg(stage);
        }
    }
    inoutProgram.insert(QStringLiteral("input"), input);
    inoutProgram.insert(QStringLiteral("output"), output);
    return source.toUtf8();
}
} // namespace

RenderGraphDocumentStatus PreparePipelineShaders(RenderGraphSnapshot& inoutSnapshot,
                                                 QJsonObject& inoutNode,
                                                 const QString& manifestFile,
                                                 const QString& packageRoot)
{
    const QJsonArray originalShaders = inoutNode.value(QStringLiteral("shaders")).toArray();
    if (originalShaders.isEmpty() == true)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile));
    if (manifestDocument.isObject() == false ||
        manifestDocument.object().value(QStringLiteral("programs")).isArray() == false)
    {
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonObject manifest = manifestDocument.object();
    QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    QJsonArray shaders;
    QStringList identities;
    for (const QJsonValueConstRef value : originalShaders)
    {
        QJsonObject shader = value.toObject();
        const QString identity = shader.value(QStringLiteral("id")).toString();
        const QString stage = shader.value(QStringLiteral("stage")).toString();
        if (IsIdentifier(QString(identity).replace(QLatin1Char('-'), QLatin1Char('_'))) == false ||
            identities.contains(identity) == true ||
            GetPipelineShaderStages(inoutNode.value(QStringLiteral("kind")).toString()).contains(stage) == false)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        identities.append(identity);
        QString name = shader.value(QStringLiteral("program")).toString();
        if (name == QStringLiteral("Program"))
        {
            const QString base =
                QStringLiteral("Shader_%1_%2")
                    .arg(inoutNode.value(QStringLiteral("id")).toString().left(GENERATED_NAME_PART_LENGTH),
                         identity.left(GENERATED_NAME_PART_LENGTH))
                    .replace(QLatin1Char('-'), QLatin1Char('_'));
            QStringList names;
            for (const QJsonValueConstRef program : programs)
            {
                names.append(program.toObject().value(QStringLiteral("id")).toString());
            }
            name = base;
            for (qsizetype suffix = 2; names.contains(name) == true; ++suffix)
            {
                name = base + QLatin1Char('_') + QString::number(suffix);
            }
        }
        if (IsIdentifier(name) == false)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        bool hasProgram = false;
        for (const QJsonValueConstRef program : programs)
        {
            if (program.toObject().value(QStringLiteral("id")).toString() != name)
            {
                continue;
            }
            if (hasProgram == true || program.toObject().value(QStringLiteral("stage")).toString() != stage ||
                inoutSnapshot.Files.contains(program.toObject().value(QStringLiteral("source")).toString()) == false)
            {
                return RenderGraphDocumentStatus::InvalidEdit;
            }
            hasProgram = true;
        }
        if (hasProgram == false)
        {
            // Keep new files beside the root manifest, without creating directories.
            const QString sourceFile = QStringLiteral("shader_") + name + QStringLiteral(".slang");
            for (QMap<QString, QByteArray>::const_iterator file = inoutSnapshot.Files.constBegin();
                 file != inoutSnapshot.Files.constEnd();
                 ++file)
            {
                if (file.key().compare(sourceFile, Qt::CaseInsensitive) == 0)
                {
                    return RenderGraphDocumentStatus::Conflict;
                }
            }
            const QFileInfo destination(QDir(packageRoot).filePath(sourceFile));
            if (destination.exists() == true || destination.isSymLink() == true)
            {
                return RenderGraphDocumentStatus::Conflict;
            }
            QJsonObject program{{QStringLiteral("id"), name},
                                {QStringLiteral("stage"), stage},
                                {QStringLiteral("source"), sourceFile},
                                {QStringLiteral("function"), QStringLiteral("ShaderMain")},
                                {QStringLiteral("bindings"), QJsonArray{}}};
            const QByteArray source = MakeSource(program, manifest.value(QStringLiteral("schemas")).toArray());
            if (source.isEmpty() == true)
            {
                return RenderGraphDocumentStatus::InvalidEdit;
            }
            const foundation::int32 programBudget =
                manifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("programs")).toInt();
            if (programs.size() >= programBudget)
            {
                return RenderGraphDocumentStatus::Capacity;
            }
            programs.append(program);
            inoutSnapshot.Files.insert(sourceFile, source);
        }
        shader.insert(QStringLiteral("program"), name);
        shaders.append(shader);
    }
    manifest.insert(QStringLiteral("programs"), programs);
    if (programs != manifestDocument.object().value(QStringLiteral("programs")).toArray())
    {
        inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    }
    inoutNode.insert(QStringLiteral("shaders"), shaders);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
