#include "internal/render_graph_document.h"

#include <ludus/foundation/base/core.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
namespace
{
// Inspect every authored branch, even disconnected nodes and inactive configurations.
// Technique strings are deliberately conservative: future stage names remain roots.
bool CollectProgramReferences(const QJsonValue& value, QStringList& inoutReferences, bool shouldCollectStrings)
{
    if (value.isString() == true && shouldCollectStrings == true)
    {
        inoutReferences.append(value.toString());
    }
    else if (value.isArray() == true)
    {
        for (const QJsonValueConstRef item : value.toArray())
        {
            if (CollectProgramReferences(item, inoutReferences, shouldCollectStrings) == false)
            {
                return false;
            }
        }
    }
    else if (value.isObject() == true)
    {
        const QJsonObject object = value.toObject();
        const QString target = object.value(QStringLiteral("target")).toString();
        if (target.startsWith(QStringLiteral("shaders.")) == true &&
            target.endsWith(QStringLiteral(".program")) == true)
        {
            for (const QJsonValueConstRef choice : object.value(QStringLiteral("values")).toArray())
            {
                if (choice.isString() == false)
                {
                    return false;
                }
                inoutReferences.append(choice.toString());
            }
        }

        for (QJsonObject::const_iterator field = object.constBegin(); field != object.constEnd(); ++field)
        {
            if (field.key() == QStringLiteral("program") && field.value().isString() == false)
            {
                return false;
            }
            if (CollectProgramReferences(field.value(),
                                         inoutReferences,
                                         shouldCollectStrings == true || field.key() == QStringLiteral("program")) ==
                false)
            {
                return false;
            }
        }
    }
    return true;
}

QString ResolveSourcePath(const QString& manifest, const QString& source)
{
    return QDir::cleanPath(QDir(QFileInfo(manifest).path()).filePath(source));
}

/// Add every snapshot file that a string in value names, resolved against document's directory
/// or, like traversal_source, against the package root. Remaining fields thereby keep sources
/// that no program registers, such as traversal sources and technique payloads, and graphs
/// that a Subgraph node or option imports.
void CollectNamedFiles(const QJsonValue& value,
                       const QString& document,
                       const RenderGraphSnapshot& snapshot,
                       QSet<QString>& inoutFiles)
{
    if (value.isString() == true)
    {
        for (const QString& path : {ResolveSourcePath(document, value.toString()), QDir::cleanPath(value.toString())})
        {
            if (snapshot.Files.contains(path) == true)
            {
                inoutFiles.insert(path);
            }
        }
    }
    else if (value.isArray() == true)
    {
        for (const QJsonValueConstRef item : value.toArray())
        {
            CollectNamedFiles(item, document, snapshot, inoutFiles);
        }
    }
    else if (value.isObject() == true)
    {
        const QJsonObject object = value.toObject();
        for (QJsonObject::const_iterator field = object.constBegin(); field != object.constEnd(); ++field)
        {
            CollectNamedFiles(field.value(), document, snapshot, inoutFiles);
        }
    }
}

bool HasCompleteGraphReferences(const QString& file, const QJsonObject& graph, const RenderGraphSnapshot& snapshot)
{
    if (graph.value(QStringLiteral("configurations")).isArray() == false)
    {
        return false;
    }
    for (const QJsonValueConstRef configuration : graph.value(QStringLiteral("configurations")).toArray())
    {
        if (configuration.isObject() == false ||
            configuration.toObject().value(QStringLiteral("nodes")).isArray() == false)
        {
            return false;
        }
        for (const QJsonValueConstRef value : configuration.toObject().value(QStringLiteral("nodes")).toArray())
        {
            if (value.isObject() == false)
            {
                return false;
            }
            const QJsonObject node = value.toObject();
            if (node.contains(QStringLiteral("shaders")) == true &&
                node.value(QStringLiteral("shaders")).isArray() == false)
            {
                return false;
            }
            for (const QJsonValueConstRef shader : node.value(QStringLiteral("shaders")).toArray())
            {
                if (shader.isObject() == false ||
                    shader.toObject().value(QStringLiteral("program")).isString() == false)
                {
                    return false;
                }
            }
            if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
            {
                const QString asset = node.value(QStringLiteral("asset")).toString();
                if (asset.endsWith(QStringLiteral(".lurender.json")) == false ||
                    snapshot.Files.contains(ResolveSourcePath(file, asset)) == false)
                {
                    return false;
                }
            }
        }
    }
    return true;
}

/// Resolve file's include directives into outIncludes: literal quoted includes, relative to file as
/// render_graph_build.py resolves them. False when a directive is computed or names a file outside
/// the snapshot; such an include can itself include any source.
bool GetTrackedIncludes(const RenderGraphSnapshot& snapshot, const QString& file, QStringList& outIncludes)
{
    static const QRegularExpression INCLUDES(QStringLiteral("^\\s*#\\s*include\\b([^\\n]*)"),
                                             QRegularExpression::MultilineOption);
    static const QRegularExpression QUOTED(QStringLiteral("^\\s*\"([^\"]+)\"\\s*$"));
    outIncludes.clear();
    bool isTracked = true;
    QRegularExpressionMatchIterator matches = INCLUDES.globalMatch(QString::fromUtf8(snapshot.Files.value(file)));
    while (matches.hasNext() == true)
    {
        const QRegularExpressionMatch match = QUOTED.match(matches.next().captured(1));
        const QString include = match.hasMatch() == true ? ResolveSourcePath(file, match.captured(1)) : QString();
        if (include.isEmpty() == true || snapshot.Files.contains(include) == false)
        {
            isTracked = false;
        }
        else
        {
            outIncludes.append(include);
        }
    }
    return isTracked;
}

/// Whether the source at survivor (bytes, tracked includes) could include or import candidate.
/// Besides its literal includes, a source reaches only its own directory tree: builds resolve
/// includes relative to the including file and reject paths through "..". Inside that tree any
/// file-name or module-name mention, even in a comment or a future import, conservatively counts.
bool CanReach(const QString& survivor, const QByteArray& bytes, const QStringList& includes, const QString& candidate)
{
    if (includes.contains(candidate) == true)
    {
        return true;
    }
    const QString directory = QFileInfo(survivor).path();
    if (directory != QStringLiteral(".") && candidate.startsWith(directory + QLatin1Char('/')) == false)
    {
        return false;
    }
    const QFileInfo information(candidate);
    return bytes.contains(information.fileName().toUtf8()) == true ||
           bytes.contains(information.completeBaseName().toUtf8()) == true;
}

/// Remove the program registrations of inoutSnapshot that no graph file (any configuration,
/// disconnected nodes included) or manifest technique names, limited to candidatesOrNull when
/// given, and the shader sources this frees. inoutSnapshot and the outputs are unchanged on failure.
RenderGraphDocumentStatus PruneShaderPrograms(RenderGraphSnapshot& inoutSnapshot,
                                              const QSet<QString>* candidatesOrNull,
                                              // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): programs, sources
                                              QStringList& outPrograms,
                                              QStringList& outSources,
                                              QString& outError)
{
    QStringList references;
    QMap<QString, QJsonObject> manifests;
    for (QMap<QString, QByteArray>::const_iterator file = inoutSnapshot.Files.constBegin();
         file != inoutSnapshot.Files.constEnd();
         ++file)
    {
        const bool isGraph = file.key().endsWith(QStringLiteral(".lurender.json"));
        const bool isManifest = file.key().endsWith(QStringLiteral(".lushader.json"));
        if (isGraph == false && isManifest == false)
        {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.value());
        const QJsonObject object = document.object();
        bool hasCompleteReferences =
            document.isObject() == true && (object.value(QStringLiteral("version")).toInt() == 1 ||
                                            (isGraph == true && object.value(QStringLiteral("version")).toInt() == 2));
        if (isGraph == true)
        {
            hasCompleteReferences = hasCompleteReferences == true &&
                                    HasCompleteGraphReferences(file.key(), object, inoutSnapshot) == true &&
                                    CollectProgramReferences(object, references, false) == true;
        }
        else
        {
            hasCompleteReferences = hasCompleteReferences == true &&
                                    object.value(QStringLiteral("programs")).isArray() == true &&
                                    object.value(QStringLiteral("techniques")).isArray() == true;
            for (const QJsonValueConstRef technique : object.value(QStringLiteral("techniques")).toArray())
            {
                hasCompleteReferences = hasCompleteReferences == true && technique.isObject() == true;
            }
            hasCompleteReferences =
                hasCompleteReferences == true &&
                CollectProgramReferences(object.value(QStringLiteral("techniques")), references, true) == true;
            for (const QJsonValueConstRef value : object.value(QStringLiteral("programs")).toArray())
            {
                const QJsonObject program = value.toObject();
                hasCompleteReferences = hasCompleteReferences == true && value.isObject() == true &&
                                        program.value(QStringLiteral("id")).toString().isEmpty() == false &&
                                        program.value(QStringLiteral("source")).toString().isEmpty() == false;
            }
            manifests.insert(file.key(), object);
        }
        if (hasCompleteReferences == false)
        {
            outError = QStringLiteral("Cannot safely check shader references in %1. Repair its JSON, shader tables or "
                                      "missing subgraphs first. The draft was preserved.")
                           .arg(file.key());
            return RenderGraphDocumentStatus::Malformed;
        }
    }
    RenderGraphSnapshot candidate = inoutSnapshot;
    QStringList programs;
    QStringList releasedSources;
    for (QMap<QString, QJsonObject>::const_iterator manifest = manifests.constBegin(); manifest != manifests.constEnd();
         ++manifest)
    {
        QJsonArray retained;
        const QJsonArray original = manifest.value().value(QStringLiteral("programs")).toArray();
        for (const QJsonValueConstRef value : original)
        {
            const QJsonObject program = value.toObject();
            const QString identity = program.value(QStringLiteral("id")).toString();
            if (references.contains(identity) == true ||
                (candidatesOrNull != nullptr && candidatesOrNull->contains(identity) == false))
            {
                retained.append(value);
            }
            else
            {
                programs.append(manifest.key() + QLatin1Char('#') + identity);
                releasedSources.append(
                    ResolveSourcePath(manifest.key(), program.value(QStringLiteral("source")).toString()));
            }
        }
        if (retained != original)
        {
            QJsonObject updated = manifest.value();
            updated.insert(QStringLiteral("programs"), retained);
            candidate.Files.insert(manifest.key(), EncodeCanonicalJson(updated));
        }
    }
    // Retained programs' sources and every file a remaining JSON field names are kept outright.
    QSet<QString> namedFiles;
    for (QMap<QString, QByteArray>::const_iterator file = candidate.Files.constBegin();
         file != candidate.Files.constEnd();
         ++file)
    {
        if (file.key().endsWith(QStringLiteral(".lurender.json")) == true ||
            file.key().endsWith(QStringLiteral(".lushader.json")) == true)
        {
            CollectNamedFiles(QJsonDocument::fromJson(file.value()).object(), file.key(), candidate, namedFiles);
        }
    }
    QMap<QString, QStringList> includes;
    QSet<QString> untracked;
    for (QMap<QString, QByteArray>::const_iterator file = candidate.Files.constBegin();
         file != candidate.Files.constEnd();
         ++file)
    {
        if (file.key().endsWith(QStringLiteral(".slang")) == true &&
            GetTrackedIncludes(candidate, file.key(), includes[file.key()]) == false)
        {
            untracked.insert(file.key());
        }
    }
    // Released sources, and the helpers they include, are removable unless named or registered.
    // Only registered or included package sources qualify; loose authored files never do.
    QSet<QString> removable;
    QStringList pending = releasedSources;
    while (pending.isEmpty() == false)
    {
        const QString source = pending.takeFirst();
        if (source.endsWith(QStringLiteral(".slang")) == false || candidate.Files.contains(source) == false ||
            namedFiles.contains(source) == true || removable.contains(source) == true)
        {
            continue;
        }
        removable.insert(source);
        pending.append(includes.value(source));
    }
    // Fixed point over surviving sources only, so sources removed together cannot keep each
    // other, while anything a survivor could reach, directly or through another survivor, stays.
    bool hasChanged = true;
    while (hasChanged == true)
    {
        hasChanged = false;
        for (QMap<QString, QStringList>::const_iterator survivor = includes.constBegin();
             survivor != includes.constEnd();
             ++survivor)
        {
            if (removable.contains(survivor.key()) == true)
            {
                continue;
            }
            const QStringList current = removable.values();
            for (const QString& source : current)
            {
                if (CanReach(survivor.key(), candidate.Files.value(survivor.key()), survivor.value(), source) == true)
                {
                    removable.remove(source);
                    hasChanged = true;
                }
            }
        }
    }
    for (const QString& file : untracked)
    {
        if (removable.contains(file) == false)
        {
            // An untracked header or computed include in a survivor can itself include any
            // source. Registrations may be pruned, files must survive.
            removable.clear();
            break;
        }
    }
    QStringList sources = removable.values();
    sources.sort();
    for (const QString& source : sources)
    {
        candidate.Files.remove(source);
    }
    inoutSnapshot = foundation::Move(candidate);
    outPrograms = programs;
    outSources = sources;
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}

/// The first graph file other than library whose fields name library, or empty when none does.
QString FindLibraryUser(const RenderGraphSnapshot& snapshot, const QString& library)
{
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
    {
        if (file.key() == library || file.key().endsWith(QStringLiteral(".lurender.json")) == false)
        {
            continue;
        }
        QSet<QString> named;
        CollectNamedFiles(QJsonDocument::fromJson(file.value()).object(), file.key(), snapshot, named);
        if (named.contains(library) == true)
        {
            return file.key();
        }
    }
    return {};
}
} // namespace

RenderGraphDocumentStatus
RenderGraphDocument::RemoveUnusedShaders(QStringList& outPrograms, QStringList& outSources, QString& outError)
{
    if (IsLoaded() == false)
    {
        outError = QStringLiteral("Open a render graph before removing unused shaders.");
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot candidate = mDraft;
    QStringList programs;
    QStringList sources;
    const RenderGraphDocumentStatus prepared = PruneShaderPrograms(candidate, nullptr, programs, sources, outError);
    if (prepared != RenderGraphDocumentStatus::Ok)
    {
        return prepared;
    }
    if (candidate == mDraft)
    {
        outPrograms.clear();
        outSources.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    const RenderGraphDocumentStatus status = commit(candidate, false);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        outPrograms = programs;
        outSources = sources;
    }
    else
    {
        outError = QStringLiteral("Unused shader cleanup was rejected; the draft was preserved.");
    }
    return status;
}

// File, configuration and node identify distinct parts of one authored record.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus RenderGraphDocument::RemoveSubgraphLibrary(const QString& file,
                                                                     const QString& configuration,
                                                                     const QString& node,
                                                                     RenderGraphLibraryRemoval& outRemoval,
                                                                     QString& outError)
{
    if (IsLoaded() == false)
    {
        outError = QStringLiteral("Open a render graph before deleting a Subgraph and its library.");
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        outError = QStringLiteral("Cannot read %1. Repair its JSON first. The draft was preserved.").arg(file);
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    qsizetype configurationIndex = -1;
    QJsonObject subgraph;
    for (qsizetype index = 0; index < configurations.size(); ++index)
    {
        const QJsonObject record = configurations.at(index).toObject();
        if (record.value(QStringLiteral("id")).toString() != configuration)
        {
            continue;
        }
        configurationIndex = index;
        for (const QJsonValueConstRef value : record.value(QStringLiteral("nodes")).toArray())
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == node)
            {
                subgraph = value.toObject();
            }
        }
    }
    if (configurationIndex < 0 || subgraph.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph"))
    {
        outError = QStringLiteral("%1 in %2#%3 is not a Subgraph node, so it has no library to delete. Select a "
                                  "Subgraph node, or use Remove Node. The draft was preserved.")
                       .arg(node, file, configuration);
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject record = configurations.at(configurationIndex).toObject();
    if (RemoveRenderGraphNodeRecord(record, node) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    configurations.replace(configurationIndex, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    // Remove the node's library, then libraries that only removed libraries imported. A graph
    // that any remaining graph names stays, and so do the root and loose graphs.
    const QString library = ResolveSourcePath(file, subgraph.value(QStringLiteral("asset")).toString());
    QStringList libraries{library};
    QStringList graphs;
    QSet<QString> programCandidates;
    bool hasChanged = true;
    while (hasChanged == true)
    {
        hasChanged = false;
        for (qsizetype index = 0; index < libraries.size(); ++index)
        {
            const QString current = libraries.at(index);
            if (current == mRootGraph || graphs.contains(current) == true ||
                current.endsWith(QStringLiteral(".lurender.json")) == false ||
                candidate.Files.contains(current) == false || FindLibraryUser(candidate, current).isEmpty() == false)
            {
                continue;
            }
            const QJsonObject removed = QJsonDocument::fromJson(candidate.Files.value(current)).object();
            QStringList references;
            // A partly malformed library only yields fewer candidates; remaining files are checked below.
            static_cast<void>(CollectProgramReferences(removed, references, false));
            for (const QString& reference : references)
            {
                programCandidates.insert(reference);
            }
            QSet<QString> imported;
            CollectNamedFiles(removed, current, candidate, imported);
            for (const QString& path : imported)
            {
                if (libraries.contains(path) == false)
                {
                    libraries.append(path);
                }
            }
            candidate.Files.remove(current);
            graphs.append(current);
            hasChanged = true;
        }
    }
    QStringList programs;
    QStringList sources;
    const RenderGraphDocumentStatus prepared =
        PruneShaderPrograms(candidate, &programCandidates, programs, sources, outError);
    if (prepared != RenderGraphDocumentStatus::Ok)
    {
        return prepared;
    }
    mLastMove.clear();
    const RenderGraphDocumentStatus status = commit(candidate, false);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        outError =
            QStringLiteral("Deleting Subgraph %1 and its library was rejected; the draft was preserved.").arg(node);
        return status;
    }
    outRemoval.Graphs = graphs;
    outRemoval.Programs = programs;
    outRemoval.Sources = sources;
    outRemoval.RetainedBy = graphs.isEmpty() == true ? FindLibraryUser(candidate, library) : QString();
    outError.clear();
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
