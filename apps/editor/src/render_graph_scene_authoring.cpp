#include "internal/render_graph_shader_creation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>

namespace ludus::editor
{
namespace
{
constexpr qsizetype ADAPTER_PROGRAM_BASE_LENGTH = 40;
const QString RAY_TECHNIQUE = QStringLiteral("RendererTriangles.RayTracing");
const QStringList RAY_STAGES{QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")};

QString MakeSourcePath(const RenderGraphSnapshot& snapshot, const QString& packageRoot, const QString& base)
{
    QString name = base + QStringLiteral(".slang");
    for (qsizetype suffix = 2;; ++suffix)
    {
        bool hasConflict = false;
        for (const QString& existing : snapshot.Files.keys())
        {
            hasConflict = hasConflict == true || existing.compare(name, Qt::CaseInsensitive) == 0;
        }
        const QFileInfo destination(QDir(packageRoot).filePath(name));
        if (hasConflict == false && destination.exists() == false && destination.isSymLink() == false)
        {
            return name;
        }
        name = base + QLatin1Char('_') + QString::number(suffix) + QStringLiteral(".slang");
    }
}

QJsonObject MakeSceneInterface(RenderGraphSnapshot& inoutSnapshot, const QString& packageRoot)
{
    const QString payload = MakeSourcePath(inoutSnapshot, packageRoot, QStringLiteral("scene_payload"));
    inoutSnapshot.Files.insert(payload,
                               "#pragma once\n// Shared by the scene-aware ray callbacks.\n"
                               "struct ScenePayload\n{\n    float3 Radiance;\n};\n");
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("payload"), payload},
                       {QStringLiteral("surface_queries"), 1},
                       {QStringLiteral("visibility_queries"), 1}};
}

QJsonValue ReplacePrograms(const QJsonValue& value, const QMap<QString, QString>& replacements)
{
    if (value.isArray() == true)
    {
        QJsonArray result;
        for (const QJsonValueConstRef entry : value.toArray())
        {
            result.append(ReplacePrograms(entry, replacements));
        }
        return result;
    }
    if (value.isObject() == true)
    {
        QJsonObject result = value.toObject();
        for (QJsonObject::iterator field = result.begin(); field != result.end(); ++field)
        {
            if (field.key() == QStringLiteral("program") && replacements.contains(field.value().toString()) == true)
            {
                field.value() = replacements.value(field.value().toString());
            }
            else
            {
                field.value() = ReplacePrograms(field.value(), replacements);
            }
        }
        return result;
    }
    return value;
}
} // namespace

bool HasSceneRayInterface(const QJsonObject& manifest)
{
    for (const QJsonValueConstRef value : manifest.value(QStringLiteral("techniques")).toArray())
    {
        const QJsonObject technique = value.toObject();
        if (technique.value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            return technique.value(QStringLiteral("scene_interface")).isObject();
        }
    }
    return false;
}

QByteArray MakeSceneRaySource(QJsonObject& inoutProgram)
{
    const QString stage = inoutProgram.value(QStringLiteral("stage")).toString();
    const QString function = inoutProgram.value(QStringLiteral("function")).toString();
    QString source = QStringLiteral("// Edit this shader, then Save to publish it to the project.\n"
                                    "// Scene interface types are supplied by the shader manifest.\n");
    if (stage == QStringLiteral("ray_generation"))
    {
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("RayLaunch"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("Radiance"));
        source += QStringLiteral("float3 %1(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)\n"
                                 "{\n    ScenePayload payload = (ScenePayload)0;\n"
                                 "    TraceAndShade(scene, queries, launch.CameraRay, payload);\n"
                                 "    return payload.Radiance;\n}\n")
                      .arg(function);
    }
    else if (stage == QStringLiteral("closest_hit"))
    {
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("SurfaceHit"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("ScenePayload"));
        source += QStringLiteral("void %1(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, "
                                 "inout ScenePayload payload)\n{\n"
                                 "    // MaterialSample exposes BaseColor, Metallic, Roughness and Emission.\n"
                                 "    MaterialSample material = EvaluateMaterial(scene, hit);\n"
                                 "    // Display material color; author lighting here when needed.\n"
                                 "    payload.Radiance = material.BaseColor;\n}\n")
                      .arg(function);
    }
    else if (stage == QStringLiteral("miss"))
    {
        inoutProgram.insert(QStringLiteral("input"), QStringLiteral("Ray"));
        inoutProgram.insert(QStringLiteral("output"), QStringLiteral("ScenePayload"));
        source += QStringLiteral("void %1(SceneContext scene, Ray ray, inout RayQueryContext queries, "
                                 "inout ScenePayload payload)\n{\n    payload.Radiance = float3(0, 0, 0);\n}\n")
                      .arg(function);
    }
    else
    {
        return {};
    }
    return source.toUtf8();
}

RenderGraphDocumentStatus PrepareSceneRayDefaults(RenderGraphSnapshot& inoutSnapshot,
                                                  QJsonObject& inoutManifest,
                                                  const QJsonObject& node,
                                                  const QString& packageRoot)
{
    if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("RayTracingPipeline"))
    {
        return RenderGraphDocumentStatus::Ok;
    }
    QJsonArray techniques = inoutManifest.value(QStringLiteral("techniques")).toArray();
    for (const QJsonValueConstRef technique : techniques)
    {
        if (technique.toObject().value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            return RenderGraphDocumentStatus::Ok;
        }
    }
    // Only new placeholders opt into the default. Never reinterpret authored callbacks.
    for (const QJsonValueConstRef shader : node.value(QStringLiteral("shaders")).toArray())
    {
        if (shader.toObject().value(QStringLiteral("program")).toString() != QStringLiteral("Program"))
        {
            return RenderGraphDocumentStatus::Ok;
        }
    }
    if (techniques.size() >=
        inoutManifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("configurations")).toInt())
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    techniques.append(QJsonObject{{QStringLiteral("id"), RAY_TECHNIQUE},
                                  {QStringLiteral("configuration"), QStringLiteral("RayTracing")},
                                  {QStringLiteral("recipes"), QJsonArray{}},
                                  {QStringLiteral("scene_interface"), MakeSceneInterface(inoutSnapshot, packageRoot)}});
    inoutManifest.insert(QStringLiteral("techniques"), techniques);
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus ConvertSceneRayInterface(RenderGraphSnapshot& inoutSnapshot,
                                                   const QString& manifestFile,
                                                   const QString& packageRoot,
                                                   QString& outError)
{
    outError = QStringLiteral("Scene materials require a complete ray-generation, miss and closest-hit technique. "
                              "The draft was preserved.");
    QJsonObject manifest = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile)).object();
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    qsizetype techniqueIndex = -1;
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        if (techniques.at(index).toObject().value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            techniqueIndex = index;
        }
    }
    if (techniqueIndex < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (HasSceneRayInterface(manifest) == true)
    {
        outError.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    const qsizetype programCount = programs.size() + RAY_STAGES.size();
    const QJsonObject budgets = manifest.value(QStringLiteral("budgets")).toObject();
    if (programCount > budgets.value(QStringLiteral("programs")).toInt() ||
        programCount * manifest.value(QStringLiteral("profiles")).toArray().size() >
            budgets.value(QStringLiteral("artifacts")).toInt())
    {
        outError = QStringLiteral("Scene material conversion needs three adapter programs and their profile artifacts. "
                                  "Increase the manifest program/artifact budgets first. The draft was preserved.");
        return RenderGraphDocumentStatus::Capacity;
    }
    QJsonObject technique = techniques.at(techniqueIndex).toObject();
    QMap<QString, QJsonObject> originals;
    QStringList sources;
    for (const QString& stage : RAY_STAGES)
    {
        const QString expectedInput = stage == QStringLiteral("ray_generation") ? QStringLiteral("RayLaunch")
                                      : stage == QStringLiteral("closest_hit")  ? QStringLiteral("RayHit")
                                                                                : QStringLiteral("Ray");
        for (const QJsonValueConstRef entry : programs)
        {
            const QJsonObject program = entry.toObject();
            if (program.value(QStringLiteral("id")) == technique.value(stage) &&
                program.value(QStringLiteral("stage")).toString() == stage &&
                program.value(QStringLiteral("input")).toString() == expectedInput &&
                program.value(QStringLiteral("bindings")).toArray().isEmpty() == true &&
                program.value(QStringLiteral("function")).toString().isEmpty() == false &&
                program.value(QStringLiteral("output")).toString() ==
                    (stage == QStringLiteral("ray_generation") ? QStringLiteral("Ray") : QStringLiteral("Color")) &&
                inoutSnapshot.Files.contains(program.value(QStringLiteral("source")).toString()) == true)
            {
                originals.insert(stage, program);
                const QString source = program.value(QStringLiteral("source")).toString();
                if (sources.contains(source) == false)
                {
                    sources.append(source);
                }
            }
        }
    }
    if (originals.size() != RAY_STAGES.size())
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    // Copy the declaration from the renderer-owned API bundled with this editor.
    // Generated authored sources cannot include reserved build-time helper paths.
    QFile api(QStringLiteral(":/shader-api/ray_shader.slang"));
    if (api.open(QIODevice::ReadOnly) == false)
    {
        outError = QStringLiteral("Cannot enable scene materials: the bundled legacy shader API is unavailable. "
                                  "The draft was preserved.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const QRegularExpression hitDeclaration(QStringLiteral("\\bstruct\\s+RayHit\\s*\\{[^}]*\\};"));
    const QString declaration = hitDeclaration.match(QString::fromUtf8(api.readAll())).captured();
    if (declaration.isEmpty() == true)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const QString bridge = MakeSourcePath(inoutSnapshot, packageRoot, QStringLiteral("scene_legacy_callbacks"));
    QByteArray legacy =
        "#pragma once\n// Compatibility inputs for the original authored callbacks.\n" + declaration.toUtf8() + "\n";
    sources.sort();
    for (const QString& source : sources)
    {
        legacy += "#include \"" + source.toUtf8() + "\"\n";
    }
    inoutSnapshot.Files.insert(bridge, legacy);
    QMap<QString, QString> replacements;
    for (const QString& stage : RAY_STAGES)
    {
        const QJsonObject original = originals.value(stage);
        const QString previous = original.value(QStringLiteral("id")).toString();
        const QString base = previous.left(ADAPTER_PROGRAM_BASE_LENGTH);
        QString identity = QStringLiteral("Scene_") + base;
        QStringList identities;
        for (const QJsonValueConstRef value : programs)
        {
            identities.append(value.toObject().value(QStringLiteral("id")).toString());
        }
        for (qsizetype suffix = 2; identities.contains(identity) == true; ++suffix)
        {
            identity = QStringLiteral("Scene_%1_%2").arg(base, QString::number(suffix));
        }
        const QString source = MakeSourcePath(inoutSnapshot, packageRoot, QStringLiteral("shader_") + identity);
        QJsonObject program{{QStringLiteral("id"), identity},
                            {QStringLiteral("stage"), stage},
                            {QStringLiteral("source"), source},
                            {QStringLiteral("function"), QStringLiteral("SceneShaderMain")},
                            {QStringLiteral("bindings"), QJsonArray{}}};
        QString adapter = QString::fromUtf8(MakeSceneRaySource(program));
        if (stage == QStringLiteral("ray_generation"))
        {
            adapter.replace(QStringLiteral("launch.CameraRay, payload"),
                            original.value(QStringLiteral("function")).toString() +
                                QStringLiteral("(launch), payload"));
        }
        else if (stage == QStringLiteral("miss"))
        {
            adapter.replace(QStringLiteral("float3(0, 0, 0)"),
                            original.value(QStringLiteral("function")).toString() + QStringLiteral("(ray)"));
        }
        else
        {
            adapter =
                QStringLiteral(
                    "// Original shader remains in %1. Edit this callback to access full materials.\n"
                    "void SceneShaderMain(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, "
                    "inout ScenePayload payload)\n{\n"
                    "    // Full material: MaterialSample material = EvaluateMaterial(scene, hit);\n"
                    "    RayHit input;\n    input.IncomingRay = hit.IncomingRay;\n"
                    "    input.PositionWorld = hit.PositionWorld;\n    input.DistanceWorld = hit.DistanceWorld;\n"
                    "    input.NormalWorld = hit.GeometricNormalWorld;\n    input.Primitive = hit.TriangleIndex;\n"
                    "    input.Barycentric = hit.Barycentric;\n    input.Color = Surface(hit.TriangleIndex, 0);\n"
                    "    payload.Radiance = %2(input);\n}\n")
                    .arg(original.value(QStringLiteral("source")).toString(),
                         original.value(QStringLiteral("function")).toString());
        }
        inoutSnapshot.Files.insert(source, (QStringLiteral("#include \"%1\"\n").arg(bridge) + adapter).toUtf8());
        programs.append(program);
        replacements.insert(previous, identity);
        technique.insert(stage, identity);
    }
    technique.insert(QStringLiteral("scene_interface"), MakeSceneInterface(inoutSnapshot, packageRoot));
    techniques.replace(techniqueIndex, technique);
    manifest.insert(QStringLiteral("techniques"), techniques);
    manifest.insert(QStringLiteral("programs"), programs);
    inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    for (QMap<QString, QByteArray>::iterator file = inoutSnapshot.Files.begin(); file != inoutSnapshot.Files.end();
         ++file)
    {
        if (file.key().endsWith(QStringLiteral(".lurender.json")) == true)
        {
            const QJsonDocument graph = QJsonDocument::fromJson(file.value());
            const QJsonObject replaced = ReplacePrograms(graph.object(), replacements).toObject();
            if (replaced != graph.object())
            {
                file.value() = EncodeCanonicalJson(replaced);
            }
        }
    }
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
