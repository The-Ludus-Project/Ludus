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

RenderGraphDocumentStatus ReconcileShader(RenderGraphSnapshot& inoutSnapshot,
                                          QJsonObject& inoutNode,
                                          QJsonObject& inoutManifest,
                                          QJsonArray& inoutPrograms,
                                          QJsonObject& inoutShader,
                                          const QJsonObject& previousShader,
                                          const QString& packageRoot,
                                          const RenderGraphSnapshot& originalSnapshot)
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
        name = previous;
    }
    const QString identity = inoutShader.value(QStringLiteral("id")).toString();
    const QString previousIdentity = previousShader.value(QStringLiteral("id")).toString();
    if (name == previous && identity != previousIdentity &&
        (previous == MakeGeneratedName(inoutNode, previousIdentity) ||
         previous.startsWith(MakeGeneratedName(inoutNode, previousIdentity) + QLatin1Char('_')) == true))
    {
        name = MakeGeneratedName(inoutNode, identity);
        if (name != previous && FindProgram(inoutPrograms, name) >= 0)
        {
            return RenderGraphDocumentStatus::Conflict;
        }
    }
    // Selecting an existing different program is an assignment, never an overwrite.
    if (name != previous && FindProgram(inoutPrograms, name) >= 0)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    const QString source = program.value(QStringLiteral("source")).toString();
    if (inoutSnapshot.Files.contains(source) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
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
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        program.insert(QStringLiteral("stage"), stage);
        const QByteArray replacement = MakeSource(program, schemas);
        if (replacement.isEmpty() == true)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        if (inoutSnapshot.Files.value(source) == originalTemplate)
        {
            inoutSnapshot.Files.insert(source, replacement);
        }
    }
    if (name != previous)
    {
        if (IsIdentifier(name) == false)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        const QString destination = QDir::cleanPath(
            QDir(QFileInfo(source).path()).filePath(QStringLiteral("shader_") + name + QStringLiteral(".slang")));
        if (HasSourceConflict(inoutSnapshot, destination, source, packageRoot, originalSnapshot) == true)
        {
            return RenderGraphDocumentStatus::Conflict;
        }
        // A source shared by multiple programs cannot be moved independently.
        for (const QJsonValueConstRef value : inoutPrograms)
        {
            const QJsonObject other = value.toObject();
            if (other.value(QStringLiteral("id")).toString() != previous &&
                other.value(QStringLiteral("source")).toString() == source)
            {
                return RenderGraphDocumentStatus::Conflict;
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
                                                 const QString& packageRoot)
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
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonObject manifest = manifestDocument.object();
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
    QStringList identities;
    for (qsizetype shaderIndex = 0; shaderIndex < originalShaders.size(); ++shaderIndex)
    {
        QJsonObject shader = inoutNode.value(QStringLiteral("shaders")).toArray().at(shaderIndex).toObject();
        const QString identity = shader.value(QStringLiteral("id")).toString();
        const QString stage = shader.value(QStringLiteral("stage")).toString();
        if (IsIdentifier(QString(identity).replace(QLatin1Char('-'), QLatin1Char('_'))) == false ||
            identities.contains(identity) == true ||
            GetPipelineShaderStages(inoutNode.value(QStringLiteral("kind")).toString()).contains(stage) == false)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
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
                                                                          originalSnapshot);
        if (reconcileStatus != RenderGraphDocumentStatus::Ok)
        {
            return reconcileStatus;
        }
        QString name = shader.value(QStringLiteral("program")).toString();
        if (name == QStringLiteral("Program"))
        {
            const QString base = MakeGeneratedName(inoutNode, identity);
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
            if (HasSourceConflict(inoutSnapshot, sourceFile, QString(), packageRoot, originalSnapshot) == true)
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
    if (manifest != manifestDocument.object())
    {
        inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    }
    inoutNode.insert(QStringLiteral("shaders"), shaders);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
