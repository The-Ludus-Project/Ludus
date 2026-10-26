#include "internal/render_graph_document.h"

#include <ludus/foundation/base/types.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QRegularExpression>
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

bool HasUntrackedIncludes(const RenderGraphSnapshot& snapshot)
{
    const QRegularExpression includes(QStringLiteral("^\\s*#\\s*include\\b([^\\n]*)"),
                                      QRegularExpression::MultilineOption);
    const QRegularExpression quoted(QStringLiteral("^\\s*\"([^\"]+)\"\\s*$"));
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
    {
        if (file.key().endsWith(QStringLiteral(".slang")) == false)
        {
            continue;
        }
        QRegularExpressionMatchIterator matches = includes.globalMatch(QString::fromUtf8(file.value()));
        while (matches.hasNext() == true)
        {
            const QRegularExpressionMatch match = quoted.match(matches.next().captured(1));
            if (match.hasMatch() == false ||
                snapshot.Files.contains(ResolveSourcePath(file.key(), match.captured(1))) == false)
            {
                // An untracked header or computed include can itself include any
                // candidate source. Registrations may be pruned, files must survive.
                return true;
            }
        }
    }
    return false;
}

bool IsMentionedBySource(const RenderGraphSnapshot& snapshot, const QString& source)
{
    const QFileInfo information(source);
    const QByteArray fileName = information.fileName().toUtf8();
    const QByteArray moduleName = information.completeBaseName().toUtf8();
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
    {
        // Search all original sources, including those being pruned. This preserves
        // transitive includes, Slang module imports and ambiguous/commented mentions.
        if (file.key() != source && file.key().endsWith(QStringLiteral(".slang")) == true &&
            (file.value().contains(fileName) == true || file.value().contains(moduleName) == true))
        {
            return true;
        }
    }
    return false;
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
    QStringList references;
    QMap<QString, QJsonObject> manifests;
    QMap<QString, foundation::int32> sourceUses;
    for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin(); file != mDraft.Files.constEnd();
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
                                    HasCompleteGraphReferences(file.key(), object, mDraft) == true &&
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
                const QString identity = program.value(QStringLiteral("id")).toString();
                const QString source = program.value(QStringLiteral("source")).toString();
                hasCompleteReferences = hasCompleteReferences == true && value.isObject() == true &&
                                        identity.isEmpty() == false && source.isEmpty() == false;
                ++sourceUses[ResolveSourcePath(file.key(), source)];
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
    RenderGraphSnapshot candidate = mDraft;
    QStringList programs;
    QStringList sources;
    QStringList retainedSources;
    for (QMap<QString, QJsonObject>::const_iterator manifest = manifests.constBegin(); manifest != manifests.constEnd();
         ++manifest)
    {
        QJsonArray retained;
        const QJsonArray original = manifest.value().value(QStringLiteral("programs")).toArray();
        for (const QJsonValueConstRef value : original)
        {
            const QJsonObject program = value.toObject();
            const QString identity = program.value(QStringLiteral("id")).toString();
            const QString source =
                ResolveSourcePath(manifest.key(), program.value(QStringLiteral("source")).toString());
            if (references.contains(identity) == true)
            {
                retained.append(value);
                retainedSources.append(source);
            }
            else
            {
                programs.append(manifest.key() + QLatin1Char('#') + identity);
                sources.append(source);
            }
        }
        if (retained != original)
        {
            QJsonObject updated = manifest.value();
            updated.insert(QStringLiteral("programs"), retained);
            candidate.Files.insert(manifest.key(), EncodeCanonicalJson(updated));
        }
    }
    const bool hasUntrackedIncludes = HasUntrackedIncludes(mDraft);
    QStringList removedSources;
    sources.removeDuplicates();
    for (const QString& source : sources)
    {
        // Only delete registered package-owned .slang files. Shared files remain
        // even if all their programs are unused; loose authored files are untouched.
        if (hasUntrackedIncludes == false && sourceUses.value(source) == 1 &&
            retainedSources.contains(source) == false && source.endsWith(QStringLiteral(".slang")) == true &&
            candidate.Files.contains(source) == true && IsMentionedBySource(mDraft, source) == false)
        {
            candidate.Files.remove(source);
            removedSources.append(source);
        }
    }
    if (candidate == mDraft)
    {
        outPrograms.clear();
        outSources.clear();
        outError.clear();
        return RenderGraphDocumentStatus::Ok;
    }
    const RenderGraphDocumentStatus status = commit(candidate, false);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        outPrograms = programs;
        outSources = removedSources;
        outError.clear();
    }
    else
    {
        outError = QStringLiteral("Unused shader cleanup was rejected; the draft was preserved.");
    }
    return status;
}
} // namespace ludus::editor
