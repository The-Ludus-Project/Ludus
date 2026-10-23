#include "internal/render_graph_configuration_template.h"
#include "internal/render_graph_shader_creation.h"

#include <ludus/foundation/base/types.h>

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace ludus::editor
{
using namespace foundation;
namespace
{
const QString VISIBILITY_VARYING = QStringLiteral("VisibilityRasterVarying");
const QString RAY_TECHNIQUE = QStringLiteral("RendererTriangles.RayTracing");

QByteArray ReadVisibilityTemplate(const QString& identity, const QString& file)
{
    QFile source(QStringLiteral(":/render-graph/templates/") + identity + QLatin1Char('/') + file);
    if (source.open(QIODevice::ReadOnly) == false || source.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return {};
    }
    return source.readAll();
}

QString GetVisibilityProgramName(const QString& identity)
{
    return (identity == QStringLiteral("Geometry") || identity == QStringLiteral("Color")
                ? QStringLiteral("Visibility")
                : QStringLiteral("VisibilitySecondary")) +
           identity;
}

bool MergeVisibilitySchemas(QJsonObject& inoutManifest, const QJsonObject& source)
{
    QJsonArray schemas = inoutManifest.value(QStringLiteral("schemas")).toArray();
    for (const QJsonValueConstRef value : source.value(QStringLiteral("schemas")).toArray())
    {
        QJsonObject incoming = value.toObject();
        const bool isVertex = incoming.value(QStringLiteral("id")) == QStringLiteral("RendererStatic");
        if (isVertex == false)
        {
            incoming.insert(QStringLiteral("id"), VISIBILITY_VARYING);
        }
        bool hasSchema = false;
        for (qsizetype index = 0; index < schemas.size(); ++index)
        {
            QJsonObject existing = schemas.at(index).toObject();
            if (existing.value(QStringLiteral("id")) != incoming.value(QStringLiteral("id")))
            {
                continue;
            }
            hasSchema = true;
            if (existing.value(QStringLiteral("role")) != incoming.value(QStringLiteral("role")))
            {
                return false;
            }
            if (isVertex == true)
            {
                const QJsonArray expected = incoming.value(QStringLiteral("fields")).toArray();
                QJsonArray fields = existing.value(QStringLiteral("fields")).toArray();
                constexpr qsizetype IDENTITY_ATTRIBUTE_COUNT = 2;
                const qsizetype originalCount = expected.size() - IDENTITY_ATTRIBUTE_COUNT;
                if (fields.size() != originalCount && fields.size() != expected.size())
                {
                    return false;
                }
                for (qsizetype field = 0; field < fields.size(); ++field)
                {
                    if (fields.at(field) != expected.at(field))
                    {
                        return false;
                    }
                }
                for (qsizetype field = fields.size(); field < expected.size(); ++field)
                {
                    fields.append(expected.at(field));
                }
                existing.insert(QStringLiteral("fields"), fields);
                schemas.replace(index, existing);
            }
            else if (existing != incoming)
            {
                return false;
            }
        }
        if (hasSchema == false)
        {
            schemas.append(incoming);
        }
    }
    inoutManifest.insert(QStringLiteral("schemas"), schemas);
    return true;
}

bool MergeVisibilityPrograms(RenderGraphSnapshot& inoutSnapshot, QJsonObject& inoutManifest, const QJsonObject& source)
{
    QJsonArray programs = inoutManifest.value(QStringLiteral("programs")).toArray();
    for (const QJsonValueConstRef value : source.value(QStringLiteral("programs")).toArray())
    {
        QJsonObject incoming = value.toObject();
        const QString identity = incoming.value(QStringLiteral("id")).toString();
        if (identity == QStringLiteral("Depth"))
        {
            continue;
        }
        const bool isRaster = identity == QStringLiteral("Geometry") || identity == QStringLiteral("Color");
        const QString templateIdentity =
            isRaster == true ? QStringLiteral("visibility-buffer") : QStringLiteral("visibility-secondary-rays");
        const QString directory =
            isRaster == true ? QStringLiteral("visibility_buffer/") : QStringLiteral("visibility_secondary_rays/");
        const QString relative = incoming.value(QStringLiteral("source")).toString();
        const QString destination = directory + relative;
        const QByteArray bytes = ReadVisibilityTemplate(templateIdentity, relative);
        if (bytes.isEmpty() == true ||
            (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != bytes))
        {
            return false;
        }
        inoutSnapshot.Files.insert(destination, bytes);
        incoming.insert(QStringLiteral("id"), GetVisibilityProgramName(identity));
        incoming.insert(QStringLiteral("source"), destination);
        bool hasProgram = false;
        for (const QJsonValueConstRef existing : programs)
        {
            if (existing.toObject().value(QStringLiteral("id")) == incoming.value(QStringLiteral("id")))
            {
                hasProgram = true;
                if (existing.toObject() != incoming)
                {
                    return false;
                }
            }
        }
        if (hasProgram == false)
        {
            programs.append(incoming);
        }
    }
    if (programs.size() >
        inoutManifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("programs")).toInt())
    {
        return false;
    }
    inoutManifest.insert(QStringLiteral("programs"), programs);
    return true;
}

bool MergeVisibilityRayInterface(RenderGraphSnapshot& inoutSnapshot,
                                 QJsonObject& inoutManifest,
                                 const QJsonObject& source)
{
    QJsonArray techniques = inoutManifest.value(QStringLiteral("techniques")).toArray();
    for (const QJsonValueConstRef value : techniques)
    {
        const QJsonObject technique = value.toObject();
        if (technique.value(QStringLiteral("id")) != RAY_TECHNIQUE)
        {
            continue;
        }
        const QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
        // The compiler validates the full payload grammar. Accept this template's known
        // payload without changing a package's existing callbacks or query limits.
        const QByteArray payload = inoutSnapshot.Files.value(interface.value(QStringLiteral("payload")).toString());
        return interface.value(QStringLiteral("version")).toInt() == 1 &&
               interface.value(QStringLiteral("visibility_buffer")).toBool() == true &&
               interface.value(QStringLiteral("surface_queries")).toInt() >= 1 &&
               payload == ReadVisibilityTemplate(QStringLiteral("visibility-secondary-rays"),
                                                 QStringLiteral("main_shaders/payload.slang"));
    }
    QJsonObject technique = source.value(QStringLiteral("techniques")).toArray().last().toObject();
    for (const QString& stage :
         {QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")})
    {
        technique.insert(stage, GetVisibilityProgramName(technique.value(stage).toString()));
    }
    QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    const QString relative = interface.value(QStringLiteral("payload")).toString();
    const QString destination = QStringLiteral("visibility_secondary_rays/") + relative;
    const QByteArray payload = ReadVisibilityTemplate(QStringLiteral("visibility-secondary-rays"), relative);
    if (payload.isEmpty() == true ||
        (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != payload))
    {
        return false;
    }
    inoutSnapshot.Files.insert(destination, payload);
    interface.insert(QStringLiteral("payload"), destination);
    technique.insert(QStringLiteral("scene_interface"), interface);
    techniques.append(technique);
    inoutManifest.insert(QStringLiteral("techniques"), techniques);
    return true;
}
} // namespace

// File and configuration identify distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareVisibilityConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                                         const QString& rootGraph,
                                                         const QString& file,
                                                         const QString& configuration,
                                                         const QString& templateIdentity,
                                                         QString& outError)
{
    const bool hasSecondaryRays = templateIdentity == QStringLiteral("visibility-secondary-rays");
    if (hasSecondaryRays == false && templateIdentity != QStringLiteral("visibility-buffer"))
    {
        outError = QStringLiteral("Select a built-in configuration template.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject graph = QJsonDocument::fromJson(inoutSnapshot.Files.value(file)).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    static const QRegularExpression IDENTIFIER(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    if (graph.isEmpty() == true || IDENTIFIER.match(configuration).hasMatch() == false)
    {
        outError = QStringLiteral("Choose a valid configuration name in an open graph.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    qsizetype destinationIndex = -1;
    for (qsizetype index = 0; index < configurations.size(); ++index)
    {
        const QJsonObject record = configurations.at(index).toObject();
        if (record.value(QStringLiteral("id")) != configuration)
        {
            continue;
        }
        destinationIndex = index;
        for (const QString& field :
             {QStringLiteral("nodes"), QStringLiteral("edges"), QStringLiteral("inputs"), QStringLiteral("outputs")})
        {
            if (record.value(field).toArray().isEmpty() == false)
            {
                outError = QStringLiteral("Select a new or empty configuration. Existing contents are preserved.");
                return RenderGraphDocumentStatus::InvalidEdit;
            }
        }
    }
    if (destinationIndex < 0 && configurations.size() >= RenderGraphDocument::MAX_CONFIGURATIONS)
    {
        outError = QStringLiteral("The graph has reached its configuration capacity. The draft was preserved.");
        return RenderGraphDocumentStatus::Capacity;
    }
    const QString manifestFile =
        rootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile)).object();
    const QJsonArray profiles = manifest.value(QStringLiteral("profiles")).toArray();
    if (manifest.isEmpty() == true || profiles.isEmpty() == true ||
        (hasSecondaryRays == true && profiles.contains(QStringLiteral("metal")) == false &&
         profiles.contains(QStringLiteral("spirv")) == false))
    {
        outError =
            QStringLiteral("Visibility needs a Renderer manifest; secondary rays also need a Metal or Vulkan profile.");
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QByteArray bytes = ReadVisibilityTemplate(templateIdentity, QStringLiteral("main.lushader.json.in"));
    bytes.replace("@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@", QJsonDocument(profiles).toJson(QJsonDocument::Compact));
    const QJsonObject source = QJsonDocument::fromJson(bytes).object();
    const QJsonArray records =
        QJsonDocument::fromJson(ReadVisibilityTemplate(templateIdentity, QStringLiteral("library.lurender.json")))
            .object()
            .value(QStringLiteral("configurations"))
            .toArray();
    if (source.isEmpty() == true || records.isEmpty() == true)
    {
        outError = QStringLiteral("Visibility template resources are unavailable; repair the editor installation.");
        return RenderGraphDocumentStatus::NotFound;
    }
    if (MergeVisibilitySchemas(manifest, source) == false ||
        MergeVisibilityPrograms(inoutSnapshot, manifest, source) == false ||
        (hasSecondaryRays == true && MergeVisibilityRayInterface(inoutSnapshot, manifest, source) == false))
    {
        outError = QStringLiteral("Visibility conflicts with existing schemas, shader sources, program budgets or a "
                                  "non-visibility ray interface. Use compatible RendererStatic inputs and a visibility "
                                  "ray package; existing sources were preserved.");
        return RenderGraphDocumentStatus::Conflict;
    }
    QJsonObject record = records.first().toObject();
    record.insert(QStringLiteral("id"), configuration);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        QJsonObject node = nodes.at(index).toObject();
        QJsonArray shaders = node.value(QStringLiteral("shaders")).toArray();
        for (qsizetype shader = 0; shader < shaders.size(); ++shader)
        {
            QJsonObject entry = shaders.at(shader).toObject();
            entry.insert(QStringLiteral("program"),
                         GetVisibilityProgramName(entry.value(QStringLiteral("program")).toString()));
            shaders.replace(shader, entry);
        }
        if (shaders.isEmpty() == false)
        {
            node.insert(QStringLiteral("shaders"), shaders);
            nodes.replace(index, node);
        }
    }
    record.insert(QStringLiteral("nodes"), nodes);
    if (destinationIndex < 0)
    {
        configurations.append(record);
    }
    else
    {
        configurations.replace(destinationIndex, record);
    }
    graph.insert(QStringLiteral("version"), 2);
    graph.insert(QStringLiteral("configurations"), configurations);
    inoutSnapshot.Files.insert(file, EncodeCanonicalJson(graph));
    inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    return PrepareRenderGraphTechniques(inoutSnapshot, rootGraph, outError);
}
} // namespace ludus::editor
