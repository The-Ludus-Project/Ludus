#include "internal/render_graph_shader_creation.h"
#include "internal/render_graph_options.h"
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

RenderGraphDocumentStatus
ReportShaderFailure(RenderGraphDocumentStatus status, QString* outErrorOrNull, const QString& message)
{
    if (outErrorOrNull != nullptr)
    {
        *outErrorOrNull = message + QStringLiteral(" The draft was preserved.");
    }
    return status;
}

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

bool IsSceneRayStage(const QString& stage)
{
    return stage == QStringLiteral("ray_generation") || stage == QStringLiteral("miss") ||
           stage == QStringLiteral("closest_hit");
}

bool IsSceneComputeProgram(const QJsonObject& program)
{
    return program.value(QStringLiteral("stage")).toString() == QStringLiteral("compute") &&
           program.value(QStringLiteral("output")).toString() == QStringLiteral("Radiance");
}

QByteArray MakeSource(QJsonObject& inoutProgram, const QJsonArray& schemas)
{
    const QString stage = inoutProgram.value(QStringLiteral("stage")).toString();
    if ((IsSceneRayStage(stage) == true &&
         (inoutProgram.value(QStringLiteral("output")).toString() == QStringLiteral("Radiance") ||
          inoutProgram.value(QStringLiteral("output")).toString() == QStringLiteral("ScenePayload"))) ||
        IsSceneComputeProgram(inoutProgram) == true)
    {
        return MakeSceneRaySource(inoutProgram);
    }
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
    else if (stage == QStringLiteral("ray_generation"))
    {
        input = QStringLiteral("RayLaunch");
        output = QStringLiteral("Ray");
        source += QStringLiteral("Ray ShaderMain(RayLaunch launch)\n{\n    return launch.CameraRay;\n}\n");
    }
    else if (stage == QStringLiteral("miss") || stage == QStringLiteral("closest_hit"))
    {
        input = stage == QStringLiteral("miss") ? QStringLiteral("Ray") : QStringLiteral("RayHit");
        output = QStringLiteral("Color");
        source += QStringLiteral("float3 ShaderMain(%1 input)\n{\n    return float3(1, 1, 1);\n}\n").arg(input);
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
RenderGraphDocumentStatus GrowShaderBudgets(QJsonObject& inoutManifest,
                                            qsizetype programCount,
                                            const QString& manifestFile,
                                            QString* outErrorOrNull)
{
    constexpr qsizetype MAX_SHADER_ARTIFACTS = LUDUS_EDITOR_MAX_SHADER_ARTIFACTS;
    const qsizetype profileCount = inoutManifest.value(QStringLiteral("profiles")).toArray().size();
    if (profileCount == 0)
    {
        if (outErrorOrNull != nullptr)
        {
            *outErrorOrNull = QStringLiteral("Cannot add shader: %1 needs at least one target profile. "
                                             "The draft was preserved.")
                                  .arg(manifestFile);
        }
        return RenderGraphDocumentStatus::Malformed;
    }
    if (programCount > MAX_SHADER_ARTIFACTS / profileCount)
    {
        if (outErrorOrNull != nullptr)
        {
            *outErrorOrNull =
                QStringLiteral("Cannot add shader: %1 would contain %2 programs across %3 target profiles, "
                               "exceeding the supported limit of %4 target artifacts. Reuse existing programs, "
                               "remove unused shaders or reduce target profiles. The draft was preserved.")
                    .arg(manifestFile)
                    .arg(programCount)
                    .arg(profileCount)
                    .arg(MAX_SHADER_ARTIFACTS);
        }
        return RenderGraphDocumentStatus::Capacity;
    }
    const qsizetype artifactCount = programCount * profileCount;
    QJsonObject budgets = inoutManifest.value(QStringLiteral("budgets")).toObject();
    if (budgets.value(QStringLiteral("programs")).toDouble() < static_cast<foundation::float64>(programCount))
    {
        budgets.insert(QStringLiteral("programs"), programCount);
    }
    if (budgets.value(QStringLiteral("artifacts")).toDouble() < static_cast<foundation::float64>(artifactCount))
    {
        budgets.insert(QStringLiteral("artifacts"), artifactCount);
    }
    inoutManifest.insert(QStringLiteral("budgets"), budgets);
    return RenderGraphDocumentStatus::Ok;
}

QString MakeGeneratedName(const QJsonObject& node, const QString& identity)
{
    return QStringLiteral("Shader_%1_%2")
        .arg(node.value(QStringLiteral("id")).toString().left(GENERATED_NAME_PART_LENGTH),
             identity.left(GENERATED_NAME_PART_LENGTH))
        .replace(QLatin1Char('-'), QLatin1Char('_'));
}

qsizetype FindProgram(const QJsonArray& programs, const QString& name)
{
    for (qsizetype index = 0; index < programs.size(); ++index)
    {
        if (programs.at(index).toObject().value(QStringLiteral("id")).toString() == name)
        {
            return index;
        }
    }
    return -1;
}

QJsonValue RenameReferences(const QJsonValue& value, const QString& previous, const QString& name)
{
    if (value.isArray() == true)
    {
        QJsonArray result;
        for (const QJsonValueConstRef item : value.toArray())
        {
            result.append(RenameReferences(item, previous, name));
        }
        return result;
    }
    if (value.isObject() == true)
    {
        QJsonObject result = value.toObject();
        for (QJsonObject::iterator field = result.begin(); field != result.end(); ++field)
        {
            if (field.key() == QStringLiteral("program") && field.value().toString() == previous)
            {
                field.value() = name;
            }
            else
            {
                field.value() = RenameReferences(field.value(), previous, name);
            }
        }
        return result;
    }
    return value;
}

QStringList GetReferencedPrograms(const RenderGraphSnapshot& snapshot, const QJsonArray& techniques)
{
    QStringList result;
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
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
                result.append(GetRenderGraphOptionPrograms(node.toObject()));
                for (const QJsonValueConstRef shader : node.toObject().value(QStringLiteral("shaders")).toArray())
                {
                    result.append(shader.toObject().value(QStringLiteral("program")).toString());
                }
            }
        }
    }
    for (const QJsonValueConstRef value : techniques)
    {
        const QJsonObject technique = value.toObject();
        for (const QString& stage : {QStringLiteral("vertex"),
                                     QStringLiteral("fragment"),
                                     QStringLiteral("ray_generation"),
                                     QStringLiteral("miss"),
                                     QStringLiteral("closest_hit")})
        {
            result.append(technique.value(stage).toString());
        }
    }
    return result;
}

bool HasSourceConflict(const RenderGraphSnapshot& snapshot,
                       const QString& source,
                       const QString& previous,
                       const QString& packageRoot,
                       const RenderGraphSnapshot& originalSnapshot)
{
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
    {
        if (file.key() != previous && file.key().compare(source, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }
    const QFileInfo destination(QDir(packageRoot).filePath(source));
    return source != previous && originalSnapshot.Files.contains(source) == false &&
           (destination.exists() == true || destination.isSymLink() == true);
}

QString MakeAvailableGeneratedName(const QJsonObject& node,
                                   const QString& identity,
                                   const QJsonArray& programs,
                                   const RenderGraphSnapshot& snapshot,
                                   const RenderGraphSnapshot& originalSnapshot,
                                   const QString& packageRoot,
                                   const QString& previous = {},
                                   const QString& previousSource = {})
{
    const QString directory = previousSource.isEmpty() == true ? QString() : QFileInfo(previousSource).path();
    const QString base = MakeGeneratedName(node, identity);
    QString name = base;
    for (qsizetype suffix = 2;
         name != previous && (FindProgram(programs, name) >= 0 ||
                              HasSourceConflict(snapshot,
                                                QDir::cleanPath(QDir(directory).filePath(
                                                    QStringLiteral("shader_") + name + QStringLiteral(".slang"))),
                                                previousSource,
                                                packageRoot,
                                                originalSnapshot) == true);
         ++suffix)
    {
        name = base + QLatin1Char('_') + QString::number(suffix);
    }
    return name;
}

RenderGraphDocumentStatus ReconcileShader(RenderGraphSnapshot& inoutSnapshot,
                                          QJsonObject& inoutNode,
                                          QJsonObject& inoutManifest,
                                          QJsonArray& inoutPrograms,
                                          QJsonObject& inoutShader,
                                          const QJsonObject& previousShader,
                                          const QString& packageRoot,
                                          const RenderGraphSnapshot& originalSnapshot,
                                          QString* outErrorOrNull)
{
    const QString previous = previousShader.value(QStringLiteral("program")).toString();
    const qsizetype index = FindProgram(inoutPrograms, previous);
    if (index < 0)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    QJsonObject program = inoutPrograms.at(index).toObject();
    QString name = inoutShader.value(QStringLiteral("program")).toString();
    if (name == QStringLiteral("Program"))
    {
        return RenderGraphDocumentStatus::Ok;
    }
    const QString identity = inoutShader.value(QStringLiteral("id")).toString();
    const QString previousIdentity = previousShader.value(QStringLiteral("id")).toString();
    if (name == previous && identity != previousIdentity &&
        (previous == MakeGeneratedName(inoutNode, previousIdentity) ||
         previous.startsWith(MakeGeneratedName(inoutNode, previousIdentity) + QLatin1Char('_')) == true))
    {
        name = MakeAvailableGeneratedName(inoutNode,
                                          identity,
                                          inoutPrograms,
                                          inoutSnapshot,
                                          originalSnapshot,
                                          packageRoot,
                                          previous,
                                          program.value(QStringLiteral("source")).toString());
    }
    // Selecting an existing different program is an assignment, never an overwrite.
    if (name != previous && FindProgram(inoutPrograms, name) >= 0)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    const QString source = program.value(QStringLiteral("source")).toString();
    if (inoutSnapshot.Files.contains(source) == false)
    {
        return ReportShaderFailure(
            RenderGraphDocumentStatus::InvalidEdit,
            outErrorOrNull,
            QStringLiteral(
                "Program '%1' references missing source '%2'. Restore that file or assign a different program.")
                .arg(previous, source));
    }
    const QJsonArray schemas = inoutManifest.value(QStringLiteral("schemas")).toArray();
    QJsonObject templateProgram = program;
    const QByteArray originalTemplate = MakeSource(templateProgram, schemas);
    const QString stage = inoutShader.value(QStringLiteral("stage")).toString();
    if (stage != program.value(QStringLiteral("stage")).toString())
    {
        // Shared callbacks and declared techniques cannot silently change stage.
        const QStringList references =
            GetReferencedPrograms(inoutSnapshot, inoutManifest.value(QStringLiteral("techniques")).toArray());
        if (references.count(previous) > 1)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral(
                    "Cannot change program '%1' from %2 to %3: it is shared by other assignments or techniques. "
                    "Set this row's program to Program to create an independent shader, then change its stage.")
                    .arg(previous, program.value(QStringLiteral("stage")).toString(), stage));
        }
        program.insert(QStringLiteral("stage"), stage);
        const QByteArray replacement = MakeSource(program, schemas);
        if (replacement.isEmpty() == true)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral(
                    "Cannot generate program '%1' for stage '%2': the manifest needs Vertex and Varying schemas for "
                    "graphics stages. Add those schemas in Source or assign an existing compatible program.")
                    .arg(name, stage));
        }
        if (inoutSnapshot.Files.value(source) == originalTemplate)
        {
            inoutSnapshot.Files.insert(source, replacement);
        }
    }
    // A compute node that gains or loses its Scene input swaps an untouched, unshared generated kernel
    // between the generic and C03 forms. Authored kernels keep their bytes and manifest signature.
    const bool shouldUseSceneCompute = stage == QStringLiteral("compute") && IsSceneComputeNode(inoutNode) == true &&
                                       HasSceneRayInterface(inoutManifest) == true;
    if (stage == QStringLiteral("compute") && IsSceneComputeProgram(program) != shouldUseSceneCompute &&
        inoutSnapshot.Files.value(source) == originalTemplate &&
        GetReferencedPrograms(inoutSnapshot, inoutManifest.value(QStringLiteral("techniques")).toArray())
                .count(previous) <= 1)
    {
        program.insert(QStringLiteral("output"),
                       shouldUseSceneCompute == true ? QStringLiteral("Radiance") : QString());
        const QByteArray replacement = MakeSource(program, schemas);
        if (replacement.isEmpty() == true)
        {
            return ReportShaderFailure(RenderGraphDocumentStatus::InvalidEdit,
                                       outErrorOrNull,
                                       QStringLiteral("Cannot regenerate compute program '%1'. Restore its manifest "
                                                      "interface or assign a new Program shader.")
                                           .arg(name));
        }
        inoutSnapshot.Files.insert(source, replacement);
    }
    if (name != previous)
    {
        if (HasRenderGraphOptionProgram(inoutSnapshot.Files, previous) == true)
        {
            return ReportShaderFailure(RenderGraphDocumentStatus::Conflict,
                                       outErrorOrNull,
                                       QStringLiteral("Cannot rename program '%1': a public option references it. "
                                                      "Update or remove that option before renaming.")
                                           .arg(previous));
        }
        if (IsIdentifier(name) == false)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral(
                    "Program name '%1' is invalid. Use 1–63 ASCII letters, digits or '_', starting with a letter.")
                    .arg(name));
        }
        const QString destination = QDir::cleanPath(
            QDir(QFileInfo(source).path()).filePath(QStringLiteral("shader_") + name + QStringLiteral(".slang")));
        if (HasSourceConflict(inoutSnapshot, destination, source, packageRoot, originalSnapshot) == true)
        {
            return ReportShaderFailure(RenderGraphDocumentStatus::Conflict,
                                       outErrorOrNull,
                                       QStringLiteral("Cannot rename program '%1' to '%2': source '%3' already exists. "
                                                      "Choose a different program name or move the conflicting file.")
                                           .arg(previous, name, destination));
        }
        // A source shared by multiple programs cannot be moved independently.
        for (const QJsonValueConstRef value : inoutPrograms)
        {
            const QJsonObject other = value.toObject();
            if (other.value(QStringLiteral("id")).toString() != previous &&
                other.value(QStringLiteral("source")).toString() == source)
            {
                return ReportShaderFailure(
                    RenderGraphDocumentStatus::Conflict,
                    outErrorOrNull,
                    QStringLiteral("Cannot rename program '%1': source '%2' is also used by '%3'. Keep the shared "
                                   "source path and edit the program ID and references in Source.")
                        .arg(previous, source, other.value(QStringLiteral("id")).toString()));
            }
        }
        inoutSnapshot.Files.insert(destination, inoutSnapshot.Files.take(source));
        program.insert(QStringLiteral("id"), name);
        program.insert(QStringLiteral("source"), destination);
        for (QMap<QString, QByteArray>::iterator file = inoutSnapshot.Files.begin(); file != inoutSnapshot.Files.end();
             ++file)
        {
            if (file.key().endsWith(QStringLiteral(".lurender.json")) == true)
            {
                const QJsonObject original = QJsonDocument::fromJson(file.value()).object();
                const QJsonObject renamed = RenameReferences(original, previous, name).toObject();
                if (renamed != original)
                {
                    file.value() = EncodeCanonicalJson(renamed);
                }
            }
        }
        // Keep package-local includes valid when a referenced source is moved.
        static const QRegularExpression includeExpression(QStringLiteral("(^[ \t]*#[ \t]*include[ \t]*\")([^\"]+)(\")"),
                                                          QRegularExpression::MultilineOption);
        for (QMap<QString, QByteArray>::iterator file = inoutSnapshot.Files.begin(); file != inoutSnapshot.Files.end();
             ++file)
        {
            if (file.key().endsWith(QStringLiteral(".slang")) == false)
            {
                continue;
            }
            const QString text = QString::fromUtf8(file.value());
            QString replaced;
            qsizetype offset = 0;
            QRegularExpressionMatchIterator matches = includeExpression.globalMatch(text);
            while (matches.hasNext() == true)
            {
                const QRegularExpressionMatch match = matches.next();
                const QDir directory(QFileInfo(file.key()).path());
                if (QDir::cleanPath(directory.filePath(match.captured(2))) == source)
                {
                    replaced += text.mid(offset, match.capturedStart(2) - offset);
                    replaced += directory.relativeFilePath(QFileInfo(destination).absoluteFilePath());
                    offset = match.capturedEnd(2);
                }
            }
            if (offset > 0)
            {
                replaced += text.mid(offset);
                file.value() = replaced.toUtf8();
            }
        }
        inoutNode = RenameReferences(inoutNode, previous, name).toObject();
        QJsonArray techniques = inoutManifest.value(QStringLiteral("techniques")).toArray();
        for (qsizetype techniqueIndex = 0; techniqueIndex < techniques.size(); ++techniqueIndex)
        {
            QJsonObject technique = techniques.at(techniqueIndex).toObject();
            const QString previousStage = previousShader.value(QStringLiteral("stage")).toString();
            if (technique.value(previousStage).toString() == previous)
            {
                technique.insert(previousStage, name);
                techniques.replace(techniqueIndex, technique);
            }
        }
        inoutManifest.insert(QStringLiteral("techniques"), techniques);
    }
    inoutPrograms.replace(index, program);
    inoutShader.insert(QStringLiteral("program"), name);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace

RenderGraphDocumentStatus PreparePipelineShaders(RenderGraphSnapshot& inoutSnapshot,
                                                 QJsonObject& inoutNode,
                                                 const QJsonObject& previousNode,
                                                 const QString& manifestFile,
                                                 const QString& packageRoot,
                                                 QString* outErrorOrNull)
{
    const RenderGraphSnapshot originalSnapshot = inoutSnapshot;
    const QJsonArray originalShaders = inoutNode.value(QStringLiteral("shaders")).toArray();
    if (originalShaders.isEmpty() == true && previousNode.value(QStringLiteral("shaders")).toArray().isEmpty() == true)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile));
    if (manifestDocument.isObject() == false ||
        manifestDocument.object().value(QStringLiteral("programs")).isArray() == false)
    {
        return ReportShaderFailure(
            RenderGraphDocumentStatus::Malformed,
            outErrorOrNull,
            QStringLiteral(
                "Shader manifest '%1' has no valid programs array. Repair its JSON in Source before editing shaders.")
                .arg(manifestFile));
    }
    QJsonObject manifest = manifestDocument.object();
    const RenderGraphDocumentStatus defaultStatus =
        PrepareSceneRayDefaults(inoutSnapshot, manifest, inoutNode, packageRoot);
    if (defaultStatus != RenderGraphDocumentStatus::Ok)
    {
        const qsizetype requiredCount = manifest.value(QStringLiteral("techniques")).toArray().size() + 1;
        return ReportShaderFailure(
            defaultStatus,
            outErrorOrNull,
            QStringLiteral(
                "Cannot add the scene-ray technique to '%1': budgets.configurations is %2, but at least %3 are needed. "
                "Open this manifest in Source and increase budgets.configurations to %3, or remove an unused "
                "technique.")
                .arg(manifestFile)
                .arg(manifest.value(QStringLiteral("budgets"))
                         .toObject()
                         .value(QStringLiteral("configurations"))
                         .toInt())
                .arg(requiredCount));
    }
    QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    // Reclaim only untouched generated leftovers. Authored or shared sources survive.
    QStringList references =
        GetReferencedPrograms(inoutSnapshot, manifest.value(QStringLiteral("techniques")).toArray());
    for (const QJsonValueConstRef shader : previousNode.value(QStringLiteral("shaders")).toArray())
    {
        const QJsonObject previous = shader.toObject();
        bool hasReplacement = false;
        for (const QJsonValueConstRef replacement : originalShaders)
        {
            if (replacement.toObject().value(QStringLiteral("id")) == previous.value(QStringLiteral("id")) ||
                replacement.toObject().value(QStringLiteral("program")) == previous.value(QStringLiteral("program")))
            {
                hasReplacement = true;
            }
        }
        if (hasReplacement == false)
        {
            references.removeOne(previous.value(QStringLiteral("program")).toString());
        }
    }
    for (const QJsonValueConstRef shader : originalShaders)
    {
        references.append(shader.toObject().value(QStringLiteral("program")).toString());
    }
    const QString prefix = MakeGeneratedName(inoutNode, QString());
    for (qsizetype index = programs.size(); index > 0; --index)
    {
        QJsonObject program = programs.at(index - 1).toObject();
        const QString name = program.value(QStringLiteral("id")).toString();
        const QString source = program.value(QStringLiteral("source")).toString();
        bool isSharedSource = false;
        for (const QJsonValueConstRef other : programs)
        {
            const QJsonObject entry = other.toObject();
            if (entry.value(QStringLiteral("id")).toString() != name &&
                entry.value(QStringLiteral("source")).toString() == source)
            {
                isSharedSource = true;
            }
        }
        // Conservative include protection also retains comments mentioning the file.
        for (QMap<QString, QByteArray>::const_iterator file = inoutSnapshot.Files.constBegin();
             file != inoutSnapshot.Files.constEnd();
             ++file)
        {
            if (file.key() != source && file.key().endsWith(QStringLiteral(".slang")) == true &&
                file.value().contains(QFileInfo(source).fileName().toUtf8()) == true)
            {
                isSharedSource = true;
            }
        }
        if (isSharedSource == false && name.startsWith(prefix) == true && references.contains(name) == false &&
            source == QStringLiteral("shader_") + name + QStringLiteral(".slang") &&
            program.value(QStringLiteral("function")).toString() == QStringLiteral("ShaderMain") &&
            program.value(QStringLiteral("bindings")).toArray().isEmpty() == true &&
            inoutSnapshot.Files.value(source) ==
                MakeSource(program, manifest.value(QStringLiteral("schemas")).toArray()))
        {
            programs.removeAt(index - 1);
            inoutSnapshot.Files.remove(source);
        }
    }
    QJsonArray shaders;
    bool hasCreatedProgram = false;
    QStringList identities;
    for (qsizetype shaderIndex = 0; shaderIndex < originalShaders.size(); ++shaderIndex)
    {
        QJsonObject shader = inoutNode.value(QStringLiteral("shaders")).toArray().at(shaderIndex).toObject();
        const QString identity = shader.value(QStringLiteral("id")).toString();
        const QString stage = shader.value(QStringLiteral("stage")).toString();
        if (IsIdentifier(QString(identity).replace(QLatin1Char('-'), QLatin1Char('_'))) == false)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral(
                    "Shader ID '%1' is invalid. Use 1–63 ASCII letters, digits, '_' or '-', starting with a letter.")
                    .arg(identity));
        }
        if (identities.contains(identity) == true)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral("Shader ID '%1' appears more than once in this pipeline. Give this row a unique ID.")
                    .arg(identity));
        }
        if (GetPipelineShaderStages(inoutNode.value(QStringLiteral("kind")).toString()).contains(stage) == false)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral("Stage '%1' is unsupported by %2. Choose a supported stage from the stage menu.")
                    .arg(stage, inoutNode.value(QStringLiteral("kind")).toString()));
        }
        identities.append(identity);
        QJsonObject previousShader;
        for (const QJsonValueConstRef previous : previousNode.value(QStringLiteral("shaders")).toArray())
        {
            const QJsonObject row = previous.toObject();
            if (row.value(QStringLiteral("id")).toString() == identity)
            {
                previousShader = row;
                break;
            }
            if (row.value(QStringLiteral("program")) == shader.value(QStringLiteral("program")))
            {
                previousShader = row;
            }
        }
        const RenderGraphDocumentStatus reconcileStatus = ReconcileShader(inoutSnapshot,
                                                                          inoutNode,
                                                                          manifest,
                                                                          programs,
                                                                          shader,
                                                                          previousShader,
                                                                          packageRoot,
                                                                          originalSnapshot,
                                                                          outErrorOrNull);
        if (reconcileStatus != RenderGraphDocumentStatus::Ok)
        {
            return reconcileStatus;
        }
        QString name = shader.value(QStringLiteral("program")).toString();
        if (name == QStringLiteral("Program"))
        {
            name =
                MakeAvailableGeneratedName(inoutNode, identity, programs, inoutSnapshot, originalSnapshot, packageRoot);
        }
        if (IsIdentifier(name) == false)
        {
            return ReportShaderFailure(
                RenderGraphDocumentStatus::InvalidEdit,
                outErrorOrNull,
                QStringLiteral(
                    "Program name '%1' is invalid. Use 1–63 ASCII letters, digits or '_', starting with a letter.")
                    .arg(name));
        }
        bool hasProgram = false;
        for (const QJsonValueConstRef program : programs)
        {
            if (program.toObject().value(QStringLiteral("id")).toString() != name)
            {
                continue;
            }
            const QJsonObject existing = program.toObject();
            const QString existingStage = existing.value(QStringLiteral("stage")).toString();
            const QString source = existing.value(QStringLiteral("source")).toString();
            if (existingStage != stage)
            {
                return ReportShaderFailure(
                    RenderGraphDocumentStatus::InvalidEdit,
                    outErrorOrNull,
                    QStringLiteral("Cannot assign program '%1' (%2) to shader '%3' (%4) in '%5'. "
                                   "Program IDs are package-wide across graphs and configurations. Set program to "
                                   "Program for a new unique shader, "
                                   "or select an existing %4 program.")
                        .arg(name, existingStage, identity, stage, manifestFile));
            }
            if (hasProgram == true)
            {
                return ReportShaderFailure(RenderGraphDocumentStatus::InvalidEdit,
                                           outErrorOrNull,
                                           QStringLiteral("Program '%1' is declared more than once in '%2'. Remove or "
                                                          "rename the duplicate manifest entry in Source.")
                                               .arg(name, manifestFile));
            }
            if (inoutSnapshot.Files.contains(source) == false)
            {
                return ReportShaderFailure(RenderGraphDocumentStatus::InvalidEdit,
                                           outErrorOrNull,
                                           QStringLiteral("Program '%1' references missing source '%2' in '%3'. "
                                                          "Restore that file or assign a different program.")
                                               .arg(name, source, manifestFile));
            }
            hasProgram = true;
        }
        if (hasProgram == false)
        {
            // Keep new files beside the root manifest, without creating directories.
            const QString sourceFile = QStringLiteral("shader_") + name + QStringLiteral(".slang");
            if (HasSourceConflict(inoutSnapshot, sourceFile, QString(), packageRoot, originalSnapshot) == true)
            {
                return ReportShaderFailure(
                    RenderGraphDocumentStatus::Conflict,
                    outErrorOrNull,
                    QStringLiteral("Cannot create program '%1': source '%2' already exists. Choose a different program "
                                   "name or register the existing file in '%3'.")
                        .arg(name, sourceFile, manifestFile));
            }
            QJsonObject program{{QStringLiteral("id"), name},
                                {QStringLiteral("stage"), stage},
                                {QStringLiteral("source"), sourceFile},
                                {QStringLiteral("function"), QStringLiteral("ShaderMain")},
                                {QStringLiteral("bindings"), QJsonArray{}}};
            if (HasSceneRayInterface(manifest) == true && IsSceneRayStage(stage) == true &&
                (inoutNode.value(QStringLiteral("kind")).toString() == QStringLiteral("RayTracingPipeline") ||
                 IsSceneComputeNode(inoutNode) == true))
            {
                program.insert(QStringLiteral("output"), QStringLiteral("ScenePayload"));
            }
            else if (HasSceneRayInterface(manifest) == true && stage == QStringLiteral("compute") &&
                     IsSceneComputeNode(inoutNode) == true)
            {
                program.insert(QStringLiteral("output"), QStringLiteral("Radiance"));
            }
            const QByteArray source = MakeSource(program, manifest.value(QStringLiteral("schemas")).toArray());
            if (source.isEmpty() == true)
            {
                return ReportShaderFailure(
                    RenderGraphDocumentStatus::InvalidEdit,
                    outErrorOrNull,
                    QStringLiteral(
                        "Cannot generate program '%1' for stage '%2': '%3' needs Vertex and Varying schemas for "
                        "graphics shaders. Add those schemas in Source or assign an existing compatible program.")
                        .arg(name, stage, manifestFile));
            }
            hasCreatedProgram = true;
            programs.append(program);
            inoutSnapshot.Files.insert(sourceFile, source);
        }
        shader.insert(QStringLiteral("program"), name);
        shaders.append(shader);
    }
    if (hasCreatedProgram == true)
    {
        const RenderGraphDocumentStatus budgetStatus =
            GrowShaderBudgets(manifest, programs.size(), manifestFile, outErrorOrNull);
        if (budgetStatus != RenderGraphDocumentStatus::Ok)
        {
            return budgetStatus;
        }
    }
    manifest.insert(QStringLiteral("programs"), programs);
    if (manifest != manifestDocument.object())
    {
        inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    }
    inoutNode.insert(QStringLiteral("shaders"), shaders);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
