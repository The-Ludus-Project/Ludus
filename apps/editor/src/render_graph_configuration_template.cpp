#include "internal/render_graph_configuration_template.h"

#include <ludus/foundation/base/types.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace ludus::editor
{
using namespace foundation;
namespace
{
const QString RESOURCE_ROOT = QStringLiteral(":/render-graph/templates/restir-di/");
const QString SOURCE_ROOT = QStringLiteral("restir_di/");
const QString RAY_TECHNIQUE = QStringLiteral("RendererTriangles.RayTracing");

QByteArray ReadTemplate(const QString& file, const QString& templateRoot)
{
    QFile source(QDir(templateRoot).filePath(file));
    if (source.open(QIODevice::ReadOnly) == false || source.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return {};
    }
    return source.readAll();
}

QString GetProgramName(const QString& identity)
{
    return QStringLiteral("Restir") + identity;
}

bool MergePayload(RenderGraphSnapshot& inoutSnapshot, QJsonObject& inoutInterface, const QString& templateRoot)
{
    const QString previous = inoutInterface.value(QStringLiteral("payload")).toString();
    const QString destination = SOURCE_ROOT + QStringLiteral("main_shaders/payload.slang");
    QByteArray payload = previous.isEmpty() == true
                             ? ReadTemplate(QStringLiteral("main_shaders/payload.slang"), templateRoot)
                             : inoutSnapshot.Files.value(previous);
    QString text = QString::fromUtf8(payload);
    // The build owns full payload grammar and byte-budget validation. Only extend a flat
    // existing payload; never rewrite a user's callback source or discard custom fields.
    static const QRegularExpression PAYLOAD_STRUCTURE(QStringLiteral("struct\\s+ScenePayload\\s*\\{([^{}]*)\\}\\s*;"));
    const QRegularExpressionMatch match = PAYLOAD_STRUCTURE.match(text);
    static const QRegularExpression RADIANCE_FIELD(QStringLiteral("\\bfloat3\\s+Radiance\\s*;"));
    static const QRegularExpression LAUNCH_FIELD(QStringLiteral("\\bRayLaunch\\s+Launch\\s*;"));
    static const QRegularExpression LAUNCH_NAME(QStringLiteral("\\bLaunch\\s*(?:;|\\[)"));
    if (match.hasMatch() == false || RADIANCE_FIELD.match(match.captured(1)).hasMatch() == false)
    {
        return false;
    }
    if (LAUNCH_FIELD.match(match.captured(1)).hasMatch() == false)
    {
        if (LAUNCH_NAME.match(match.captured(1)).hasMatch() == true)
        {
            return false;
        }
        text.insert(match.capturedEnd(1), QStringLiteral("\n    RayLaunch Launch;\n"));
        payload = text.toUtf8();
    }
    if (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != payload)
    {
        return false;
    }
    inoutSnapshot.Files.insert(destination, payload);
    inoutInterface.insert(QStringLiteral("payload"), destination);
    inoutInterface.insert(QStringLiteral("version"), 1);
    if (inoutInterface.contains(QStringLiteral("surface_queries")) == false)
    {
        inoutInterface.insert(QStringLiteral("surface_queries"), 1);
    }
    if (inoutInterface.value(QStringLiteral("visibility_queries")).toInt() == 0)
    {
        inoutInterface.insert(QStringLiteral("visibility_queries"), 1);
    }
    return true;
}

// Capture the declared entry sources and their literal include closure. A directory
// listing is not evidence that every required shader was packaged or copied.
RenderGraphDocumentStatus CopyShaderSources(RenderGraphSnapshot& inoutSnapshot,
                                            QStringList pending,
                                            const QString& templateRoot,
                                            QString& outError)
{
    QSet<QString> visited;
    static const QRegularExpression INCLUDE(QStringLiteral("^\\s*#\\s*include\\s+\"([^\"]+)\""),
                                            QRegularExpression::MultilineOption);
    while (pending.isEmpty() == false)
    {
        const QString relative = QDir::cleanPath(pending.takeFirst());
        if (relative.startsWith(QStringLiteral("../")) == true || QDir::isAbsolutePath(relative) == true)
        {
            outError =
                QStringLiteral("ReSTIR template source '%1' must remain inside the template directory.").arg(relative);
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        if (visited.contains(relative) == true)
        {
            continue;
        }
        visited.insert(relative);
        const QByteArray bytes = ReadTemplate(relative, templateRoot);
        const QString destination = SOURCE_ROOT + relative;
        if (bytes.isEmpty() == true)
        {
            outError = QStringLiteral("ReSTIR template source '%1' is unavailable; repair the editor installation. The "
                                      "draft was preserved.")
                           .arg(relative);
            return RenderGraphDocumentStatus::NotFound;
        }
        if (inoutSnapshot.Files.contains(destination) == true && inoutSnapshot.Files.value(destination) != bytes)
        {
            outError = QStringLiteral("Shader source '%1' already has different contents. Rename the conflicting "
                                      "source before importing ReSTIR DI. The draft was preserved.")
                           .arg(destination);
            return RenderGraphDocumentStatus::Conflict;
        }
        if (inoutSnapshot.Files.contains(destination) == false &&
            static_cast<usize>(inoutSnapshot.Files.size()) >= RenderGraphDocument::MAX_FILES)
        {
            outError = QStringLiteral("ReSTIR shader sources exceed the package file limit. The draft was preserved.");
            return RenderGraphDocumentStatus::Capacity;
        }
        inoutSnapshot.Files.insert(destination, bytes);
        QRegularExpressionMatchIterator includes = INCLUDE.globalMatch(QString::fromUtf8(bytes));
        while (includes.hasNext() == true)
        {
            pending.append(QDir(QFileInfo(relative).path()).filePath(includes.next().captured(1)));
        }
    }
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus MergePrograms(RenderGraphSnapshot& inoutSnapshot,
                                        QJsonObject& inoutManifest,
                                        const QJsonObject& source,
                                        const QString& templateRoot,
                                        QString& outError,
                                        RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    QJsonArray programs = inoutManifest.value(QStringLiteral("programs")).toArray();
    QStringList sources;
    for (const QJsonValueConstRef value : source.value(QStringLiteral("programs")).toArray())
    {
        QJsonObject program = value.toObject();
        const QString stage = program.value(QStringLiteral("stage")).toString();
        if (stage != QStringLiteral("ray_generation") && stage != QStringLiteral("miss") &&
            stage != QStringLiteral("closest_hit"))
        {
            continue;
        }
        sources.append(program.value(QStringLiteral("source")).toString());
        program.insert(QStringLiteral("id"), GetProgramName(program.value(QStringLiteral("id")).toString()));
        program.insert(QStringLiteral("source"), SOURCE_ROOT + program.value(QStringLiteral("source")).toString());
        bool hasProgram = false;
        for (const QJsonValueConstRef existing : programs)
        {
            if (existing.toObject().value(QStringLiteral("id")) == program.value(QStringLiteral("id")))
            {
                hasProgram = true;
                if (existing.toObject() != program)
                {
                    outError =
                        QStringLiteral("Shader program '%1' already has a different definition. Rename the "
                                       "conflicting program before importing ReSTIR DI. The draft was preserved.")
                            .arg(program.value(QStringLiteral("id")).toString());
                    return RenderGraphDocumentStatus::Conflict;
                }
            }
        }
        if (hasProgram == false)
        {
            programs.append(program);
        }
    }
    if (EnsureTemplateProgramBudget(inoutManifest,
                                    static_cast<int32>(programs.size()),
                                    QStringLiteral("ReSTIR DI"),
                                    outError,
                                    inoutBudgetOrNull) == false)
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    inoutManifest.insert(QStringLiteral("programs"), programs);
    return CopyShaderSources(inoutSnapshot, sources, templateRoot, outError);
}

} // namespace

RenderGraphDocumentStatus MergeTemplateSettings(QJsonObject& inoutManifest,
                                                const QJsonArray& additions,
                                                const QString& templateName,
                                                QString& outError)
{
    QJsonArray settings = inoutManifest.value(QStringLiteral("settings")).toArray();
    for (const QJsonValueConstRef value : additions)
    {
        QJsonObject incoming = value.toObject();
        bool hasSetting = false;
        for (const QJsonValueConstRef current : settings)
        {
            QJsonObject existing = current.toObject();
            if (existing.value(QStringLiteral("id")) == incoming.value(QStringLiteral("id")))
            {
                hasSetting = true;
                existing.remove(QStringLiteral("value"));
                incoming.remove(QStringLiteral("value"));
                if (existing != incoming)
                {
                    outError = QStringLiteral("Setting '%1' has a different definition. Rename the conflicting "
                                              "setting before importing %2. The draft was preserved.")
                                   .arg(incoming.value(QStringLiteral("id")).toString(), templateName);
                    return RenderGraphDocumentStatus::Conflict;
                }
            }
        }
        if (hasSetting == false)
        {
            settings.append(value);
        }
    }
    if (settings.size() > LUDUS_EDITOR_MAX_SETTINGS)
    {
        outError = QStringLiteral("%1 needs %2 settings, exceeding the editor limit of %3. "
                                  "Remove unused settings before importing. The draft was preserved.")
                       .arg(templateName)
                       .arg(settings.size())
                       .arg(LUDUS_EDITOR_MAX_SETTINGS);
        return RenderGraphDocumentStatus::Capacity;
    }
    inoutManifest.insert(QStringLiteral("settings"), settings);
    return RenderGraphDocumentStatus::Ok;
}

bool EnsureTemplateProgramBudget(QJsonObject& inoutManifest,
                                 int32 requiredPrograms,
                                 const QString& templateName,
                                 QString& outError,
                                 RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    QJsonObject budgets = inoutManifest.value(QStringLiteral("budgets")).toObject();
    const int32 currentPrograms = budgets.value(QStringLiteral("programs")).toInt();
    if (requiredPrograms <= currentPrograms)
    {
        return true;
    }
    if (inoutBudgetOrNull != nullptr && inoutBudgetOrNull->ApprovedPrograms >= requiredPrograms)
    {
        budgets.insert(QStringLiteral("programs"), requiredPrograms);
        inoutManifest.insert(QStringLiteral("budgets"), budgets);
        return true;
    }
    if (inoutBudgetOrNull != nullptr)
    {
        inoutBudgetOrNull->CurrentPrograms = currentPrograms;
        inoutBudgetOrNull->RequiredPrograms = requiredPrograms;
    }
    outError = QStringLiteral("%1 needs %2 shader programs, but budgets.programs is %3. "
                              "Increase budgets.programs in the shader manifest and retry. The draft was preserved.")
                   .arg(templateName)
                   .arg(requiredPrograms)
                   .arg(currentPrograms);
    return false;
}

bool UpgradeDefaultTemplateBudgets(QJsonObject& inoutManifest, const QJsonObject& source)
{
    QFile defaults(QStringLiteral(":/render-graph/default/main.lushader.json"));
    if (defaults.open(QIODevice::ReadOnly) == false || defaults.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return false;
    }
    const QJsonObject defaultBudgets =
        QJsonDocument::fromJson(defaults.readAll()).object().value(QStringLiteral("budgets")).toObject();
    const QJsonObject templateBudgets = source.value(QStringLiteral("budgets")).toObject();
    if (defaultBudgets.isEmpty() == true || templateBudgets.isEmpty() == true)
    {
        return false;
    }
    QJsonObject budgets = inoutManifest.value(QStringLiteral("budgets")).toObject();
    for (QJsonObject::const_iterator field = templateBudgets.constBegin(); field != templateBudgets.constEnd(); ++field)
    {
        // Importing the larger built-in template upgrades only unchanged defaults.
        // Custom admission limits remain authoritative, including tighter limits.
        if (budgets.value(field.key()) == defaultBudgets.value(field.key()) &&
            budgets.value(field.key()).toInt() < field.value().toInt())
        {
            budgets.insert(field.key(), field.value());
        }
    }
    inoutManifest.insert(QStringLiteral("budgets"), budgets);
    return true;
}

// File and configuration name distinct authored identities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareRestirConfiguration(RenderGraphSnapshot& inoutSnapshot,
                                                     const QString& rootGraph,
                                                     const QString& file,
                                                     const QString& configuration,
                                                     QString& outError,
                                                     const QString& templateRoot,
                                                     RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    const QString sourceRoot = templateRoot.isEmpty() == true ? RESOURCE_ROOT : templateRoot;
    outError = QStringLiteral(
        "Select a new or empty configuration for the ReSTIR DI template. Existing contents are preserved.");
    static const QRegularExpression CONFIGURATION_IDENTIFIER(QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,62}\\z"));
    QJsonObject graph = QJsonDocument::fromJson(inoutSnapshot.Files.value(file)).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    if (CONFIGURATION_IDENTIFIER.match(configuration).hasMatch() == false || graph.isEmpty() == true)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    qsizetype destinationIndex = -1;
    for (qsizetype index = 0; index < configurations.size(); ++index)
    {
        const QJsonObject record = configurations.at(index).toObject();
        if (record.value(QStringLiteral("id")).toString() == configuration)
        {
            destinationIndex = index;
            for (const QString& field : {QStringLiteral("nodes"),
                                         QStringLiteral("edges"),
                                         QStringLiteral("inputs"),
                                         QStringLiteral("outputs")})
            {
                if (record.value(field).toArray().isEmpty() == false)
                {
                    return RenderGraphDocumentStatus::InvalidEdit;
                }
            }
        }
    }
    if (destinationIndex < 0 && configurations.size() >= RenderGraphDocument::MAX_CONFIGURATIONS)
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    QString manifestFile = rootGraph;
    manifestFile.chop(QStringLiteral(".lurender.json").size());
    manifestFile += QStringLiteral(".lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifestFile)).object();
    const QJsonArray profiles = manifest.value(QStringLiteral("profiles")).toArray();
    outError = QStringLiteral("ReSTIR DI requires a Renderer manifest with a Metal or Vulkan profile.");
    if (profiles.contains(QStringLiteral("metal")) == false && profiles.contains(QStringLiteral("spirv")) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QByteArray sourceBytes = ReadTemplate(QStringLiteral("main.lushader.json.in"), sourceRoot);
    sourceBytes.replace("@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@",
                        QJsonDocument(profiles).toJson(QJsonDocument::Compact));
    const QJsonObject source = QJsonDocument::fromJson(sourceBytes).object();
    const QJsonArray library =
        QJsonDocument::fromJson(ReadTemplate(QStringLiteral("library.lurender.json"), sourceRoot))
            .object()
            .value(QStringLiteral("configurations"))
            .toArray();
    QJsonObject record = library.isEmpty() == true ? QJsonObject{} : library.first().toObject();
    outError = QStringLiteral("ReSTIR template resources are unavailable; repair the editor installation.");
    if (source.isEmpty() == true || record.isEmpty() == true ||
        UpgradeDefaultTemplateBudgets(manifest, source) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    const RenderGraphDocumentStatus programStatus =
        MergePrograms(inoutSnapshot, manifest, source, sourceRoot, outError, inoutBudgetOrNull);
    if (programStatus != RenderGraphDocumentStatus::Ok)
    {
        return programStatus;
    }
    const RenderGraphDocumentStatus settingsStatus =
        MergeTemplateSettings(manifest,
                              source.value(QStringLiteral("settings")).toArray(),
                              QStringLiteral("ReSTIR DI"),
                              outError);
    if (settingsStatus != RenderGraphDocumentStatus::Ok)
    {
        return settingsStatus;
    }
    QJsonArray techniques = manifest.value(QStringLiteral("techniques")).toArray();
    qsizetype rayIndex = -1;
    for (qsizetype index = 0; index < techniques.size(); ++index)
    {
        if (techniques.at(index).toObject().value(QStringLiteral("id")).toString() == RAY_TECHNIQUE)
        {
            rayIndex = index;
        }
    }
    QJsonObject technique;
    if (rayIndex >= 0)
    {
        technique = techniques.at(rayIndex).toObject();
    }
    else
    {
        technique = source.value(QStringLiteral("techniques")).toArray().last().toObject();
        for (const QString& stage :
             {QStringLiteral("ray_generation"), QStringLiteral("miss"), QStringLiteral("closest_hit")})
        {
            technique.insert(stage, GetProgramName(technique.value(stage).toString()));
        }
        QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
        interface.remove(QStringLiteral("payload"));
        technique.insert(QStringLiteral("scene_interface"), interface);
    }
    QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    outError = QStringLiteral("ReSTIR DI needs float3 Radiance and RayLaunch Launch in the shared payload, and cannot "
                              "share a visibility-buffer-only ray technique. Existing sources were preserved.");
    if ((interface.contains(QStringLiteral("version")) == true &&
         interface.value(QStringLiteral("version")).toInt() != 1) ||
        interface.value(QStringLiteral("visibility_buffer")).toBool() == true ||
        MergePayload(inoutSnapshot, interface, sourceRoot) == false)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    technique.insert(QStringLiteral("scene_interface"), interface);
    if (rayIndex >= 0)
    {
        techniques.replace(rayIndex, technique);
    }
    else
    {
        techniques.append(technique);
    }
    if (techniques.size() >
        manifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("configurations")).toInt())
    {
        outError = QStringLiteral(
            "The ReSTIR technique exceeds the manifest's configuration budget. The draft was preserved.");
        return RenderGraphDocumentStatus::Capacity;
    }
    manifest.insert(QStringLiteral("techniques"), techniques);
    record.insert(QStringLiteral("id"), configuration);
    QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (qsizetype index = 0; index < nodes.size(); ++index)
    {
        QJsonObject node = nodes.at(index).toObject();
        QJsonArray shaders = node.value(QStringLiteral("shaders")).toArray();
        if (shaders.isEmpty() == true)
        {
            continue;
        }
        for (qsizetype shaderIndex = 0; shaderIndex < shaders.size(); ++shaderIndex)
        {
            QJsonObject shader = shaders.at(shaderIndex).toObject();
            shader.insert(QStringLiteral("program"),
                          GetProgramName(shader.value(QStringLiteral("program")).toString()));
            shaders.replace(shaderIndex, shader);
        }
        node.insert(QStringLiteral("shaders"), shaders);
        nodes.replace(index, node);
    }
    record.insert(QStringLiteral("nodes"), nodes);
    if (destinationIndex >= 0)
    {
        configurations.replace(destinationIndex, record);
    }
    else
    {
        configurations.append(record);
    }
    graph.insert(QStringLiteral("version"), 2);
    graph.insert(QStringLiteral("configurations"), configurations);
    inoutSnapshot.Files.insert(file, EncodeCanonicalJson(graph));
    inoutSnapshot.Files.insert(manifestFile, EncodeCanonicalJson(manifest));
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}
// All paths and configuration identities describe distinct parts of one insertion transaction.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareConfigurationTemplate(RenderGraphSnapshot& inoutSnapshot,
                                                       const QString& rootGraph,
                                                       const QString& file,
                                                       const QString& configuration,
                                                       const QString& parentConfiguration,
                                                       const QString& packageRoot,
                                                       const QString& templateIdentity,
                                                       QString& outError,
                                                       RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    QString targetFile = file;
    if (file == rootGraph)
    {
        if (configuration == QStringLiteral("input"))
        {
            outError = QStringLiteral(
                "The name 'input' is reserved for configuration ports. Choose a different template name.");
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        targetFile = configuration + QStringLiteral(".lurender.json");
        outError = QStringLiteral("Choose a unique template name; its library file and Subgraph node must not exist.");
        const QFileInfo destination(QDir(packageRoot).filePath(targetFile));
        if (destination.exists() == true || destination.isSymLink() == true)
        {
            return RenderGraphDocumentStatus::Conflict;
        }
        for (const QString& existing : inoutSnapshot.Files.keys())
        {
            if (existing.compare(targetFile, Qt::CaseInsensitive) == 0)
            {
                return RenderGraphDocumentStatus::Conflict;
            }
        }
        QJsonObject graph = QJsonDocument::fromJson(inoutSnapshot.Files.value(file)).object();
        if (graph.isEmpty() == true)
        {
            return RenderGraphDocumentStatus::Malformed;
        }
        const QString parent = parentConfiguration.isEmpty() == true ? graph.value(QStringLiteral("default")).toString()
                                                                     : parentConfiguration;
        QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
        qsizetype parentIndex = -1;
        for (qsizetype index = 0; index < configurations.size(); ++index)
        {
            if (configurations.at(index).toObject().value(QStringLiteral("id")).toString() == parent)
            {
                parentIndex = index;
            }
        }
        if (parentIndex < 0)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        QJsonObject record = configurations.at(parentIndex).toObject();
        QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
        if (nodes.size() >= RenderGraphDocument::MAX_NODES)
        {
            return RenderGraphDocumentStatus::Capacity;
        }
        for (const QJsonValueConstRef node : nodes)
        {
            if (node.toObject().value(QStringLiteral("id")).toString() == configuration)
            {
                return RenderGraphDocumentStatus::Conflict;
            }
        }
        nodes.append(QJsonObject{{QStringLiteral("id"), configuration},
                                 {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                                 {QStringLiteral("asset"), targetFile},
                                 {QStringLiteral("configuration"), configuration}});
        record.insert(QStringLiteral("nodes"), nodes);
        configurations.replace(parentIndex, record);
        graph.insert(QStringLiteral("configurations"), configurations);
        inoutSnapshot.Files.insert(file, EncodeCanonicalJson(graph));
        inoutSnapshot.Files.insert(targetFile,
                                   EncodeCanonicalJson(QJsonObject{{QStringLiteral("version"), 2},
                                                                   {QStringLiteral("id"), configuration},
                                                                   {QStringLiteral("default"), configuration},
                                                                   {QStringLiteral("configurations"), QJsonArray{}}}));
    }
    if (templateIdentity == QStringLiteral("restir-di"))
    {
        return PrepareRestirConfiguration(inoutSnapshot,
                                          rootGraph,
                                          targetFile,
                                          configuration,
                                          outError,
                                          {},
                                          inoutBudgetOrNull);
    }
    if (templateIdentity == QStringLiteral("line-sampling"))
    {
        return PrepareLineSamplingConfiguration(inoutSnapshot,
                                                rootGraph,
                                                targetFile,
                                                configuration,
                                                outError,
                                                inoutBudgetOrNull);
    }
    if (templateIdentity == QStringLiteral("salesin-jarosz-lines"))
    {
        return PreparePointLineConfiguration(inoutSnapshot,
                                             rootGraph,
                                             targetFile,
                                             configuration,
                                             outError,
                                             inoutBudgetOrNull);
    }
    if (templateIdentity == QStringLiteral("compute-ray-tracing") ||
        templateIdentity == QStringLiteral("simple-compute-ray-tracing") ||
        templateIdentity == QStringLiteral("veach-guibas-mis") ||
        templateIdentity == QStringLiteral("ris-projected-solid-angle"))
    {
        return PrepareComputeRayTracingConfiguration(inoutSnapshot,
                                                     rootGraph,
                                                     targetFile,
                                                     configuration,
                                                     outError,
                                                     templateIdentity,
                                                     inoutBudgetOrNull);
    }
    return PrepareVisibilityConfiguration(inoutSnapshot,
                                          rootGraph,
                                          targetFile,
                                          configuration,
                                          templateIdentity,
                                          outError,
                                          inoutBudgetOrNull);
}
// Paths and configuration identities describe distinct parts of the insertion transaction.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareRestirTemplate(RenderGraphSnapshot& inoutSnapshot,
                                                const QString& rootGraph,
                                                const QString& file,
                                                const QString& configuration,
                                                const QString& parentConfiguration,
                                                const QString& packageRoot,
                                                QString& outError)
{
    return PrepareConfigurationTemplate(inoutSnapshot,
                                        rootGraph,
                                        file,
                                        configuration,
                                        parentConfiguration,
                                        packageRoot,
                                        QStringLiteral("restir-di"),
                                        outError);
}
} // namespace ludus::editor
