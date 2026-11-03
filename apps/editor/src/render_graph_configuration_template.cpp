#include "internal/render_graph_configuration_template.h"
#include "internal/render_graph_build_limits.h"

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
const QString PROGRAM_PREFIX = QStringLiteral("Restir");
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

QString GetProgramName(const TemplateInstance& instance, const QString& identity)
{
    return instance.ProgramPrefix + identity;
}

bool MergePayload(RenderGraphSnapshot& inoutSnapshot,
                  QJsonObject& inoutInterface,
                  const QString& templateRoot,
                  TemplateInstance& inoutInstance)
{
    const QString previous = inoutInterface.value(QStringLiteral("payload")).toString();
    const QString destination = inoutInstance.Directory + QStringLiteral("main_shaders/payload.slang");
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
    bool isExtended = false;
    if (LAUNCH_FIELD.match(match.captured(1)).hasMatch() == false)
    {
        if (LAUNCH_NAME.match(match.captured(1)).hasMatch() == true)
        {
            return false;
        }
        text.insert(match.capturedEnd(1), QStringLiteral("\n    RayLaunch Launch;\n"));
        payload = text.toUtf8();
        isExtended = true;
    }
    // A complete shared payload stays where the other ray callbacks already find it.
    const bool isShared = previous.isEmpty() == false && isExtended == false;
    if (isShared == false && InsertTemplateSource(inoutSnapshot, destination, payload, inoutInstance) == false)
    {
        return false;
    }
    inoutInterface.insert(QStringLiteral("payload"), isShared == true ? previous : destination);
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
                                            TemplateInstance& inoutInstance,
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
        const QByteArray bytes = RenameTemplateIdentifiers(ReadTemplate(relative, templateRoot), inoutInstance);
        const QString destination = inoutInstance.Directory + relative;
        if (bytes.isEmpty() == true)
        {
            outError = QStringLiteral("ReSTIR template source '%1' is unavailable; repair the editor installation. The "
                                      "draft was preserved.")
                           .arg(relative);
            return RenderGraphDocumentStatus::NotFound;
        }
        // PrepareConfigurationTemplate checks the package file limit once the whole template is in.
        if (InsertTemplateSource(inoutSnapshot, destination, bytes, inoutInstance) == false)
        {
            outError = QStringLiteral("Shader source '%1' already has different contents. The draft was preserved.")
                           .arg(destination);
            return RenderGraphDocumentStatus::Conflict;
        }
        QRegularExpressionMatchIterator includes = INCLUDE.globalMatch(QString::fromUtf8(bytes));
        while (includes.hasNext() == true)
        {
            pending.append(QDir(QFileInfo(relative).path()).filePath(includes.next().captured(1)));
        }
    }
    return RenderGraphDocumentStatus::Ok;
}

// Sources are copied before the program budget is checked, so a collision moves the import to the
// next instance before any capacity request is reported.
RenderGraphDocumentStatus MergePrograms(RenderGraphSnapshot& inoutSnapshot,
                                        QJsonObject& inoutManifest,
                                        const QJsonObject& source,
                                        const QString& templateRoot,
                                        TemplateInstance& inoutInstance,
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
        program.insert(QStringLiteral("id"),
                       GetProgramName(inoutInstance, program.value(QStringLiteral("id")).toString()));
        program.insert(QStringLiteral("source"),
                       inoutInstance.Directory + program.value(QStringLiteral("source")).toString());
        if (InsertTemplateProgram(programs, program, inoutInstance) == false)
        {
            outError =
                QStringLiteral("Shader program '%1' already has a different definition. The draft was preserved.")
                    .arg(program.value(QStringLiteral("id")).toString());
            return RenderGraphDocumentStatus::Conflict;
        }
    }
    const RenderGraphDocumentStatus sourceStatus =
        CopyShaderSources(inoutSnapshot, sources, templateRoot, inoutInstance, outError);
    if (sourceStatus != RenderGraphDocumentStatus::Ok)
    {
        return sourceStatus;
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
    return RenderGraphDocumentStatus::Ok;
}

// Settings reach shaders only as generated constants, so a setting that no authored shader names as a
// whole identifier changes no build output. Comments count as uses, which keeps a documented setting.
QStringList
GetUnusedSettings(const RenderGraphSnapshot& snapshot, const QJsonArray& settings, const QJsonArray& additions)
{
    QStringList sources;
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
    {
        if (file.key().endsWith(QStringLiteral(".slang")) == true)
        {
            sources.append(QString::fromUtf8(file.value()));
        }
    }
    QStringList unused;
    for (const QJsonValueConstRef value : settings)
    {
        const QString identity = value.toObject().value(QStringLiteral("id")).toString();
        bool isTemplateSetting = false;
        for (const QJsonValueConstRef addition : additions)
        {
            isTemplateSetting = isTemplateSetting || addition.toObject().value(QStringLiteral("id")) == identity;
        }
        const QRegularExpression use(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(identity)));
        bool isUsed = isTemplateSetting;
        for (const QString& source : sources)
        {
            isUsed = isUsed || use.match(source).hasMatch();
        }
        if (isUsed == false)
        {
            unused.append(identity);
        }
    }
    return unused;
}

} // namespace

QString GetInstanceDirectory(const QString& directory, const TemplateInstance& instance)
{
    if (instance.Number <= 1)
    {
        return directory;
    }
    return directory.chopped(1) + QStringLiteral("_%1/").arg(instance.Number);
}

QString GetInstancePrefix(const QString& prefix, const TemplateInstance& instance)
{
    return instance.Number <= 1 ? prefix : prefix + QString::number(instance.Number);
}

QByteArray RenameTemplateIdentifiers(const QByteArray& source, const TemplateInstance& instance)
{
    if (instance.Renames.isEmpty() == true || source.isEmpty() == true)
    {
        return source;
    }
    QString text = QString::fromUtf8(source);
    for (QMap<QString, QString>::const_iterator rename = instance.Renames.constBegin();
         rename != instance.Renames.constEnd();
         ++rename)
    {
        const QRegularExpression identifier(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(rename.key())));
        text.replace(identifier, rename.value());
    }
    return text.toUtf8();
}

bool InsertTemplateSource(RenderGraphSnapshot& inoutSnapshot,
                          const QString& path,
                          const QByteArray& bytes,
                          TemplateInstance& inoutInstance)
{
    if (inoutSnapshot.Files.contains(path) == true && inoutSnapshot.Files.value(path) != bytes)
    {
        inoutInstance.HasCollision = true;
        return false;
    }
    inoutSnapshot.Files.insert(path, bytes);
    return true;
}

bool InsertTemplateProgram(QJsonArray& inoutPrograms, const QJsonObject& program, TemplateInstance& inoutInstance)
{
    for (const QJsonValueConstRef existing : inoutPrograms)
    {
        if (existing.toObject().value(QStringLiteral("id")) != program.value(QStringLiteral("id")))
        {
            continue;
        }
        if (existing.toObject() != program)
        {
            inoutInstance.HasCollision = true;
            return false;
        }
        return true;
    }
    inoutPrograms.append(program);
    return true;
}

QJsonArray
RenameTemplateSettings(const QJsonObject& manifest, const QJsonArray& additions, TemplateInstance& inoutInstance)
{
    // Definitions are compared without their values: an authored value is kept by a shared setting.
    QMap<QString, QJsonObject> definitions;
    for (const QJsonValueConstRef value : manifest.value(QStringLiteral("settings")).toArray())
    {
        QJsonObject definition = value.toObject();
        definition.remove(QStringLiteral("value"));
        definitions.insert(definition.value(QStringLiteral("id")).toString(), definition);
    }
    QJsonArray renamed;
    for (const QJsonValueConstRef value : additions)
    {
        QJsonObject setting = value.toObject();
        QJsonObject definition = setting;
        definition.remove(QStringLiteral("value"));
        const QString identity = setting.value(QStringLiteral("id")).toString();
        if (definitions.contains(identity) == true && definitions.value(identity) != definition)
        {
            for (int32 suffix = 2; suffix <= MAX_TEMPLATE_INSTANCES; ++suffix)
            {
                const QString candidate = QStringLiteral("%1_%2").arg(identity).arg(suffix);
                definition.insert(QStringLiteral("id"), candidate);
                if (definitions.contains(candidate) == false || definitions.value(candidate) == definition)
                {
                    inoutInstance.Renames.insert(identity, candidate);
                    setting.insert(QStringLiteral("id"), candidate);
                    break;
                }
            }
        }
        renamed.append(setting);
    }
    return renamed;
}

RenderGraphDocumentStatus MergeTemplateSettings(const RenderGraphSnapshot& snapshot,
                                                QJsonObject& inoutManifest,
                                                const QJsonArray& additions,
                                                const QString& templateName,
                                                QString& outError,
                                                RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
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
                    outError = QStringLiteral("Setting '%1' and its numbered alternatives up to _%2 already have "
                                              "different definitions, so %3 has no free setting name. Delete "
                                              "settings this package no longer needs from the shader manifest and "
                                              "apply again. The draft was preserved.")
                                   .arg(incoming.value(QStringLiteral("id")).toString())
                                   .arg(MAX_TEMPLATE_INSTANCES)
                                   .arg(templateName);
                    return RenderGraphDocumentStatus::Conflict;
                }
            }
        }
        if (hasSetting == false)
        {
            settings.append(value);
        }
    }
    if (settings.size() <= LUDUS_EDITOR_MAX_SETTINGS)
    {
        inoutManifest.insert(QStringLiteral("settings"), settings);
        return RenderGraphDocumentStatus::Ok;
    }
    const qsizetype requiredSettings = settings.size();
    const QStringList unused = GetUnusedSettings(snapshot, settings, additions);
    if (requiredSettings - unused.size() > LUDUS_EDITOR_MAX_SETTINGS)
    {
        outError = QStringLiteral("%1 needs %2 settings, exceeding the editor limit of %3, and only %4 of them are "
                                  "unused by every shader. Delete settings or configurations this package no longer "
                                  "needs from the shader manifest, then retry. The draft was preserved.")
                       .arg(templateName)
                       .arg(requiredSettings)
                       .arg(LUDUS_EDITOR_MAX_SETTINGS)
                       .arg(unused.size());
        return RenderGraphDocumentStatus::Capacity;
    }
    QJsonArray kept;
    for (const QJsonValueConstRef value : settings)
    {
        const QString identity = value.toObject().value(QStringLiteral("id")).toString();
        if (unused.contains(identity) == false || inoutBudgetOrNull == nullptr ||
            inoutBudgetOrNull->ApprovedSettingRemovals.contains(identity) == false)
        {
            kept.append(value);
        }
    }
    if (kept.size() > LUDUS_EDITOR_MAX_SETTINGS)
    {
        if (inoutBudgetOrNull != nullptr)
        {
            inoutBudgetOrNull->RequiredSettings = static_cast<int32>(requiredSettings);
            inoutBudgetOrNull->UnusedSettings = unused;
        }
        outError = QStringLiteral("%1 needs %2 settings, exceeding the editor limit of %3. No shader uses %4; "
                                  "remove them and apply the template again. The draft was preserved.")
                       .arg(templateName)
                       .arg(requiredSettings)
                       .arg(LUDUS_EDITOR_MAX_SETTINGS)
                       .arg(unused.join(QStringLiteral(", ")));
        return RenderGraphDocumentStatus::Capacity;
    }
    inoutManifest.insert(QStringLiteral("settings"), kept);
    return RenderGraphDocumentStatus::Ok;
}

bool EnsureTemplateProgramBudget(QJsonObject& inoutManifest,
                                 int32 requiredPrograms,
                                 const QString& templateName,
                                 QString& outError,
                                 RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    // Target artifacts are programs times profiles and have no authored budget; only the supported
    // ceiling bounds them, and no approval can raise it.
    const qsizetype profileCount = inoutManifest.value(QStringLiteral("profiles")).toArray().size();
    if (profileCount > 0 && requiredPrograms > MAX_SHADER_ARTIFACTS / profileCount)
    {
        outError = QStringLiteral("%1 needs %2 shader programs, which across %3 target profiles exceed the supported "
                                  "limit of %4 target artifacts. Right-click the canvas and choose Remove Unused "
                                  "Shaders, or right-click an unneeded Subgraph and choose Delete Subgraph and "
                                  "Library, then apply again. To keep all programs, reduce profiles in the shader "
                                  "manifest. The draft was preserved.")
                       .arg(templateName)
                       .arg(requiredPrograms)
                       .arg(profileCount)
                       .arg(MAX_SHADER_ARTIFACTS);
        return false;
    }
    QJsonObject budgets = inoutManifest.value(QStringLiteral("budgets")).toObject();
    const int32 currentPrograms = budgets.value(QStringLiteral("programs")).toInt();
    if (requiredPrograms > currentPrograms)
    {
        if (inoutBudgetOrNull == nullptr || inoutBudgetOrNull->ApprovedPrograms < requiredPrograms)
        {
            if (inoutBudgetOrNull != nullptr)
            {
                inoutBudgetOrNull->CurrentPrograms = currentPrograms;
                inoutBudgetOrNull->RequiredPrograms = requiredPrograms;
            }
            outError = QStringLiteral("%1 needs %2 shader programs, but budgets.programs is %3. Increase "
                                      "budgets.programs in the shader manifest and retry. The draft was preserved.")
                           .arg(templateName)
                           .arg(requiredPrograms)
                           .arg(currentPrograms);
            return false;
        }
        budgets.insert(QStringLiteral("programs"), requiredPrograms);
    }
    RemoveDerivedBudgets(budgets);
    inoutManifest.insert(QStringLiteral("budgets"), budgets);
    return true;
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
                                                     RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull,
                                                     TemplateInstance* inoutInstanceOrNull)
{
    const QString sourceRoot = templateRoot.isEmpty() == true ? RESOURCE_ROOT : templateRoot;
    TemplateInstance localInstance;
    TemplateInstance& instance = inoutInstanceOrNull != nullptr ? *inoutInstanceOrNull : localInstance;
    instance.Directory = GetInstanceDirectory(SOURCE_ROOT, instance);
    instance.ProgramPrefix = GetInstancePrefix(PROGRAM_PREFIX, instance);
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
        outError = DescribeConfigurationCapacity(QStringLiteral("ReSTIR DI"), file);
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
    // Setting names are chosen first: the copied sources spell the names this import uses.
    const QJsonArray settings =
        RenameTemplateSettings(manifest, source.value(QStringLiteral("settings")).toArray(), instance);
    const RenderGraphDocumentStatus programStatus =
        MergePrograms(inoutSnapshot, manifest, source, sourceRoot, instance, outError, inoutBudgetOrNull);
    if (programStatus != RenderGraphDocumentStatus::Ok)
    {
        return programStatus;
    }
    const RenderGraphDocumentStatus settingsStatus = MergeTemplateSettings(inoutSnapshot,
                                                                           manifest,
                                                                           settings,
                                                                           QStringLiteral("ReSTIR DI"),
                                                                           outError,
                                                                           inoutBudgetOrNull);
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
            technique.insert(stage, GetProgramName(instance, technique.value(stage).toString()));
        }
        QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
        interface.remove(QStringLiteral("payload"));
        technique.insert(QStringLiteral("scene_interface"), interface);
    }
    QJsonObject interface = technique.value(QStringLiteral("scene_interface")).toObject();
    // visibility_buffer only permits visibility ray inputs in the package: each configuration selects
    // ordinary or visibility bindings from its own ray inputs, so ReSTIR's ordinary passes may share it.
    outError = QStringLiteral("ReSTIR DI needs ray scene interface version 1 and a float3 Radiance field in the shared "
                              "payload, which gains RayLaunch Launch. Existing sources were preserved.");
    if ((interface.contains(QStringLiteral("version")) == true &&
         interface.value(QStringLiteral("version")).toInt() != 1) ||
        MergePayload(inoutSnapshot, interface, sourceRoot, instance) == false)
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
    const qsizetype techniqueBudget =
        manifest.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("configurations")).toInt();
    if (techniques.size() > techniqueBudget)
    {
        outError = QStringLiteral("Adding the ReSTIR ray-tracing technique needs %1 techniques, but "
                                  "budgets.configurations in %2 is %3. Increase budgets.configurations and apply the "
                                  "template again. The draft was preserved.")
                       .arg(techniques.size())
                       .arg(manifestFile)
                       .arg(techniqueBudget);
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
                          GetProgramName(instance, shader.value(QStringLiteral("program")).toString()));
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
namespace
{
// One attempt at the given instance's names. Template identities are distinct authored records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareTemplateInstance(RenderGraphSnapshot& inoutSnapshot,
                                                  const QString& rootGraph,
                                                  const QString& targetFile,
                                                  const QString& configuration,
                                                  const QString& templateIdentity,
                                                  QString& outError,
                                                  RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull,
                                                  TemplateInstance& inoutInstance)
{
    if (templateIdentity == QStringLiteral("restir-di"))
    {
        return PrepareRestirConfiguration(inoutSnapshot,
                                          rootGraph,
                                          targetFile,
                                          configuration,
                                          outError,
                                          {},
                                          inoutBudgetOrNull,
                                          &inoutInstance);
    }
    if (templateIdentity == QStringLiteral("line-sampling"))
    {
        return PrepareLineSamplingConfiguration(inoutSnapshot,
                                                rootGraph,
                                                targetFile,
                                                configuration,
                                                outError,
                                                inoutBudgetOrNull,
                                                &inoutInstance);
    }
    if (templateIdentity == QStringLiteral("salesin-jarosz-lines"))
    {
        return PreparePointLineConfiguration(inoutSnapshot,
                                             rootGraph,
                                             targetFile,
                                             configuration,
                                             outError,
                                             inoutBudgetOrNull,
                                             &inoutInstance);
    }
    if (templateIdentity == QStringLiteral("compute-ray-tracing") ||
        templateIdentity == QStringLiteral("simple-compute-ray-tracing") ||
        templateIdentity == QStringLiteral("veach-guibas-mis") ||
        templateIdentity == QStringLiteral("ris-projected-solid-angle") ||
        templateIdentity == QStringLiteral("polygonal-light-sampling") ||
        templateIdentity == QStringLiteral("slice-sampling-luminaire") ||
        templateIdentity == QStringLiteral("steinberg-plt") ||
        templateIdentity == QStringLiteral("steinberg-physical-optics") ||
        templateIdentity == QStringLiteral("wave-tracing") ||
        templateIdentity == QStringLiteral("guided-restir-fg-plus") ||
        templateIdentity == QStringLiteral("restir-gi") || templateIdentity == QStringLiteral("rao-blackwell-mh") ||
        templateIdentity == QStringLiteral("conditional-restir") ||
        templateIdentity == QStringLiteral("one-more-vertex-nee") ||
        templateIdentity == QStringLiteral("light-transformer"))
    {
        return PrepareComputeRayTracingConfiguration(inoutSnapshot,
                                                     rootGraph,
                                                     targetFile,
                                                     configuration,
                                                     outError,
                                                     templateIdentity,
                                                     inoutBudgetOrNull,
                                                     &inoutInstance);
    }
    return PrepareVisibilityConfiguration(inoutSnapshot,
                                          rootGraph,
                                          targetFile,
                                          configuration,
                                          templateIdentity,
                                          outError,
                                          inoutBudgetOrNull,
                                          &inoutInstance);
}

QString DescribeTemplateInstance(const TemplateInstance& first, const TemplateInstance& used)
{
    QStringList parts;
    if (used.Number > 1)
    {
        parts.append(QStringLiteral("%1 already holds different sources or programs, so this import uses %2 and "
                                    "%3-prefixed programs.")
                         .arg(first.Directory, used.Directory, used.ProgramPrefix));
    }
    QStringList renames;
    for (QMap<QString, QString>::const_iterator rename = used.Renames.constBegin(); rename != used.Renames.constEnd();
         ++rename)
    {
        renames.append(QStringLiteral("%1 as %2").arg(rename.key(), rename.value()));
    }
    if (renames.isEmpty() == false)
    {
        parts.append(
            QStringLiteral("Existing definitions were kept, so it names %1.").arg(renames.join(QStringLiteral(", "))));
    }
    if (parts.isEmpty() == false)
    {
        parts.append(QStringLiteral("Existing configurations are unchanged."));
    }
    return parts.join(QLatin1Char(' '));
}

// Engine templates never ask authors to rename their content: each attempt runs on a copy of the
// candidate, and a name collision moves the import to the next numbered instance.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareTemplateInstances(RenderGraphSnapshot& inoutSnapshot,
                                                   const QString& rootGraph,
                                                   const QString& targetFile,
                                                   const QString& configuration,
                                                   const QString& templateIdentity,
                                                   QString& outError,
                                                   RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull,
                                                   QString* outNoticeOrNull)
{
    TemplateInstance first;
    for (int32 number = 1; number <= MAX_TEMPLATE_INSTANCES; ++number)
    {
        RenderGraphSnapshot candidate = inoutSnapshot;
        TemplateInstance instance;
        instance.Number = number;
        if (inoutBudgetOrNull != nullptr)
        {
            // Only the attempt that is reported may leave a capacity request.
            inoutBudgetOrNull->CurrentPrograms = 0;
            inoutBudgetOrNull->RequiredPrograms = 0;
            inoutBudgetOrNull->RequiredSettings = 0;
            inoutBudgetOrNull->UnusedSettings.clear();
        }
        const RenderGraphDocumentStatus status = PrepareTemplateInstance(candidate,
                                                                         rootGraph,
                                                                         targetFile,
                                                                         configuration,
                                                                         templateIdentity,
                                                                         outError,
                                                                         inoutBudgetOrNull,
                                                                         instance);
        if (number == 1)
        {
            first = instance;
        }
        if (instance.HasCollision == true)
        {
            continue;
        }
        if (status == RenderGraphDocumentStatus::Ok)
        {
            inoutSnapshot = candidate;
            if (outNoticeOrNull != nullptr)
            {
                *outNoticeOrNull = DescribeTemplateInstance(first, instance);
            }
        }
        return status;
    }
    outError = QStringLiteral("%1 and its numbered copies up to %2 all hold different sources or programs. Delete "
                              "copies this package no longer uses (right-click an unneeded Subgraph and choose Delete "
                              "Subgraph and Library) and apply again. The draft was preserved.")
                   .arg(first.Directory)
                   .arg(MAX_TEMPLATE_INSTANCES);
    return RenderGraphDocumentStatus::Conflict;
}

// All paths and configuration identities describe distinct parts of one insertion transaction.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus PrepareTemplateContents(RenderGraphSnapshot& inoutSnapshot,
                                                  const QString& rootGraph,
                                                  const QString& file,
                                                  const QString& configuration,
                                                  const QString& parentConfiguration,
                                                  const QString& packageRoot,
                                                  const QString& templateIdentity,
                                                  QString& outError,
                                                  RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull,
                                                  QString* outNoticeOrNull)
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
            outError = QStringLiteral("The template cannot add its Subgraph node: configuration %1 of %2 already has "
                                      "%3 nodes, the most one configuration can hold. Remove nodes you no longer use "
                                      "(select them and press Delete, or right-click and choose Remove Selected "
                                      "Nodes) and apply the template again. The draft was preserved.")
                           .arg(parent, file)
                           .arg(RenderGraphDocument::MAX_NODES);
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
    return PrepareTemplateInstances(inoutSnapshot,
                                    rootGraph,
                                    targetFile,
                                    configuration,
                                    templateIdentity,
                                    outError,
                                    inoutBudgetOrNull,
                                    outNoticeOrNull);
}
} // namespace

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
                                                       RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull,
                                                       QString* outNoticeOrNull)
{
    if (outNoticeOrNull != nullptr)
    {
        outNoticeOrNull->clear();
    }
    const usize existingFiles = static_cast<usize>(inoutSnapshot.Files.size());
    const RenderGraphDocumentStatus status = PrepareTemplateContents(inoutSnapshot,
                                                                     rootGraph,
                                                                     file,
                                                                     configuration,
                                                                     parentConfiguration,
                                                                     packageRoot,
                                                                     templateIdentity,
                                                                     outError,
                                                                     inoutBudgetOrNull,
                                                                     outNoticeOrNull);
    // One check after the whole template is in counts its library graph and every source it copies.
    const usize templateFiles = static_cast<usize>(inoutSnapshot.Files.size()) - existingFiles;
    if (status == RenderGraphDocumentStatus::Ok && existingFiles + templateFiles > RenderGraphDocument::MAX_FILES)
    {
        outError = DescribeTemplateFileCapacity(templateIdentity, existingFiles, templateFiles);
        return RenderGraphDocumentStatus::Capacity;
    }
    return status;
}

void AppendDraftPreserved(QString& inoutError)
{
    const QString preserved = QStringLiteral("The draft was preserved.");
    if (inoutError.endsWith(preserved) == false)
    {
        inoutError += QLatin1Char(' ') + preserved;
    }
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
