#include "internal/render_graph_document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>

#include <cmath>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 INDENT_WIDTH = 2;
constexpr char32_t FIRST_PRINTABLE = 0x20;
constexpr char32_t LAST_PRINTABLE = 0x7E;
constexpr float64 MAX_EXACT_INTEGER = 9007199254740992.0;
const QString LOCK_NAME = QStringLiteral(".ludus-editor-render-graph.lock");

bool IsAuthoredSource(const QString& path)
{
    return path.endsWith(QStringLiteral(".lurender.json")) || path.endsWith(QStringLiteral(".lushader.json")) ||
           path.endsWith(QStringLiteral(".slang"));
}

void AppendString(const QString& text, QByteArray& inoutBytes)
{
    inoutBytes.append('"');
    for (const QChar character : text)
    {
        const char16_t code = character.unicode();
        switch (code)
        {
            case u'"':
                inoutBytes.append("\\\"");
                break;
            case u'\\':
                inoutBytes.append("\\\\");
                break;
            case u'\n':
                inoutBytes.append("\\n");
                break;
            case u'\r':
                inoutBytes.append("\\r");
                break;
            case u'\t':
                inoutBytes.append("\\t");
                break;
            case u'\b':
                inoutBytes.append("\\b");
                break;
            case u'\f':
                inoutBytes.append("\\f");
                break;
            default:
                if (code >= FIRST_PRINTABLE && code <= LAST_PRINTABLE)
                {
                    inoutBytes.append(static_cast<char>(code));
                }
                else
                {
                    // Python's ensure_ascii writes UTF-16 code units in lowercase hex.
                    inoutBytes.append("\\u");
                    inoutBytes.append(QByteArray::number(code, 16).rightJustified(4, '0'));
                }
                break;
        }
    }
    inoutBytes.append('"');
}

void AppendValue(const QJsonValue& value, int32 depth, QByteArray& inoutBytes);

void AppendIndent(int32 depth, QByteArray& inoutBytes)
{
    inoutBytes.append('\n');
    inoutBytes.append(QByteArray(static_cast<qsizetype>(depth) * INDENT_WIDTH, ' '));
}

void AppendObject(const QJsonObject& object, int32 depth, QByteArray& inoutBytes)
{
    if (object.isEmpty() == true)
    {
        inoutBytes.append("{}");
        return;
    }
    inoutBytes.append('{');
    bool isFirst = true;
    // QJsonObject iterates keys in sorted order, matching sort_keys for ASCII identifiers.
    for (QJsonObject::const_iterator item = object.constBegin(); item != object.constEnd(); ++item)
    {
        if (isFirst == false)
        {
            inoutBytes.append(',');
        }
        isFirst = false;
        AppendIndent(depth + 1, inoutBytes);
        AppendString(item.key(), inoutBytes);
        inoutBytes.append(": ");
        AppendValue(item.value(), depth + 1, inoutBytes);
    }
    AppendIndent(depth, inoutBytes);
    inoutBytes.append('}');
}

void AppendValue(const QJsonValue& value, int32 depth, QByteArray& inoutBytes)
{
    switch (value.type())
    {
        case QJsonValue::Null:
        case QJsonValue::Undefined:
            inoutBytes.append("null");
            break;
        case QJsonValue::Bool:
            inoutBytes.append(value.toBool() == true ? "true" : "false");
            break;
        case QJsonValue::Double: {
            const float64 number = value.toDouble();
            if (std::trunc(number) == number && std::fabs(number) < MAX_EXACT_INTEGER)
            {
                inoutBytes.append(QByteArray::number(static_cast<qint64>(number)));
            }
            else
            {
                inoutBytes.append(QByteArray::number(number, 'g', QLocale::FloatingPointShortest));
            }
            break;
        }
        case QJsonValue::String:
            AppendString(value.toString(), inoutBytes);
            break;
        case QJsonValue::Array: {
            const QJsonArray array = value.toArray();
            if (array.isEmpty() == true)
            {
                inoutBytes.append("[]");
                break;
            }
            inoutBytes.append('[');
            for (qsizetype i = 0; i < array.size(); ++i)
            {
                if (i != 0)
                {
                    inoutBytes.append(',');
                }
                AppendIndent(depth + 1, inoutBytes);
                AppendValue(array.at(i), depth + 1, inoutBytes);
            }
            AppendIndent(depth, inoutBytes);
            inoutBytes.append(']');
            break;
        }
        case QJsonValue::Object:
            AppendObject(value.toObject(), depth, inoutBytes);
            break;
    }
}

bool ReadBounded(const QString& path, QByteArray& outBytes)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() == true || file.open(QIODevice::ReadOnly) == false)
    {
        return false;
    }
    QByteArray bytes = file.read(RenderGraphDocument::MAX_FILE_BYTES + 1);
    if (bytes.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return false;
    }
    outBytes = bytes;
    return true;
}

qsizetype FindConfiguration(const QJsonArray& configurations, const QString& identity)
{
    for (qsizetype i = 0; i < configurations.size(); ++i)
    {
        if (configurations.at(i).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return i;
        }
    }
    return -1;
}

qsizetype FindNode(const QJsonArray& nodes, const QString& identity)
{
    for (qsizetype i = 0; i < nodes.size(); ++i)
    {
        if (nodes.at(i).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return i;
        }
    }
    return -1;
}

bool ReplaceNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    const QJsonObject node = QJsonDocument::fromJson(arguments.at(0).toUtf8()).object();
    const qsizetype index = FindNode(nodes, node.value(QStringLiteral("id")).toString());
    if (index < 0 || node.value(QStringLiteral("kind")) != nodes.at(index).toObject().value(QStringLiteral("kind")))
    {
        return false;
    }
    nodes.replace(index, node);
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    return true;
}

bool AddNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    const QJsonObject node = QJsonDocument::fromJson(arguments.at(0).toUtf8()).object();
    const QString identity = node.value(QStringLiteral("id")).toString();
    if (identity.isEmpty() == true || identity == QStringLiteral("input") || FindNode(nodes, identity) >= 0)
    {
        return false;
    }
    nodes.append(node);
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    return true;
}

bool IsEndpointOf(const QString& endpoint, const QString& node)
{
    return endpoint.section(QLatin1Char('.'), 0, 0) == node;
}

bool RemoveNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QString& identity = arguments.at(0);
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    const qsizetype index = FindNode(nodes, identity);
    if (index < 0)
    {
        return false;
    }
    nodes.removeAt(index);
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    // Removing a node also removes its connections and layout position by stable ID.
    QJsonArray edges;
    const QJsonArray previousEdges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : previousEdges)
    {
        const QJsonObject record = edge.toObject();
        if (IsEndpointOf(record.value(QStringLiteral("from")).toString(), identity) == false &&
            IsEndpointOf(record.value(QStringLiteral("to")).toString(), identity) == false)
        {
            edges.append(record);
        }
    }
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    if (inoutConfiguration.contains(QStringLiteral("layout")) == true)
    {
        QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
        QJsonArray positions;
        const QJsonArray previousPositions = layout.value(QStringLiteral("positions")).toArray();
        for (const QJsonValueConstRef position : previousPositions)
        {
            if (position.toObject().value(QStringLiteral("node")).toString() != identity)
            {
                positions.append(position);
            }
        }
        layout.insert(QStringLiteral("positions"), positions);
        inoutConfiguration.insert(QStringLiteral("layout"), layout);
    }
    return true;
}

bool ConnectEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    QJsonArray edges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    const QJsonArray existing = edges;
    for (const QJsonValueConstRef edge : existing)
    {
        if (edge.toObject().value(QStringLiteral("from")).toString() == arguments.at(0) &&
            edge.toObject().value(QStringLiteral("to")).toString() == arguments.at(1))
        {
            return false;
        }
    }
    QJsonObject edge;
    edge.insert(QStringLiteral("from"), arguments.at(0));
    edge.insert(QStringLiteral("to"), arguments.at(1));
    edges.append(edge);
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    return true;
}

bool DisconnectEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    QJsonArray edges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    for (qsizetype i = 0; i < edges.size(); ++i)
    {
        const QJsonObject edge = edges.at(i).toObject();
        if (edge.value(QStringLiteral("from")).toString() == arguments.at(0) &&
            edge.value(QStringLiteral("to")).toString() == arguments.at(1))
        {
            edges.removeAt(i);
            inoutConfiguration.insert(QStringLiteral("edges"), edges);
            return true;
        }
    }
    return false;
}

bool MoveNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    if (FindNode(inoutConfiguration.value(QStringLiteral("nodes")).toArray(), arguments.at(0)) < 0)
    {
        return false;
    }
    QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
    QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
    QJsonObject position;
    position.insert(QStringLiteral("node"), arguments.at(0));
    position.insert(QStringLiteral("x"), arguments.at(1).toInt());
    position.insert(QStringLiteral("y"), arguments.at(2).toInt());
    bool isReplaced = false;
    for (qsizetype i = 0; i < positions.size(); ++i)
    {
        if (positions.at(i).toObject().value(QStringLiteral("node")).toString() == arguments.at(0))
        {
            positions.replace(i, position);
            isReplaced = true;
        }
    }
    if (isReplaced == false)
    {
        positions.append(position);
    }
    layout.insert(QStringLiteral("positions"), positions);
    inoutConfiguration.insert(QStringLiteral("layout"), layout);
    return true;
}
bool MoveNodesEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    // Arguments are node, x, y triples; one failed node rejects the whole gesture.
    for (qsizetype i = 0; i + 2 < arguments.size(); i += 3)
    {
        if (MoveNodeEdit(inoutConfiguration, arguments.mid(i, 3)) == false)
        {
            return false;
        }
    }
    return arguments.isEmpty() == false && arguments.size() % 3 == 0;
}
} // namespace

QByteArray EncodeCanonicalJson(const QJsonObject& value)
{
    QByteArray bytes;
    AppendObject(value, 0, bytes);
    bytes.append('\n');
    return bytes;
}

namespace
{
/// Read every authored source below root into outSnapshot (bounded, no symlinks, no hidden or
/// `out` directories). root may be a Qt resource directory. outSnapshot is unchanged on failure.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named package directory and root-graph identities.
RenderGraphDocumentStatus ReadPackage(const QString& root, const QString& rootGraph, RenderGraphSnapshot& outSnapshot)
{
    RenderGraphSnapshot snapshot;
    qsizetype total = 0;
    QStringList directories{root};
    while (directories.isEmpty() == false)
    {
        const QString directory = directories.takeFirst();
        const QFileInfoList entries =
            QDir(directory).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                          QDir::Name);
        for (const QFileInfo& entry : entries)
        {
            if (entry.fileName().startsWith(QLatin1Char('.')) == true)
            {
                continue;
            }
            if (entry.isDir() == true)
            {
                // Build products and generated drafts never become authored sources.
                if (entry.fileName() != QStringLiteral("out"))
                {
                    directories.append(entry.absoluteFilePath());
                }
                continue;
            }
            const QString relative = QDir(root).relativeFilePath(entry.absoluteFilePath());
            if (IsAuthoredSource(relative) == false)
            {
                continue;
            }
            QByteArray bytes;
            if (static_cast<usize>(snapshot.Files.size()) >= RenderGraphDocument::MAX_FILES ||
                ReadBounded(entry.absoluteFilePath(), bytes) == false)
            {
                return RenderGraphDocumentStatus::Capacity;
            }
            total += bytes.size();
            if (total > RenderGraphDocument::MAX_TOTAL_BYTES)
            {
                return RenderGraphDocumentStatus::Capacity;
            }
            snapshot.Files.insert(relative, bytes);
        }
    }
    if (rootGraph.endsWith(QStringLiteral(".lurender.json")) == false || snapshot.Files.contains(rootGraph) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    outSnapshot.Files.swap(snapshot.Files);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named package/file/configuration identities.
RenderGraphDocumentStatus RenderGraphDocument::Load(const QString& packageRoot, const QString& rootGraph)
{
    const QString root = QFileInfo(packageRoot).canonicalFilePath();
    if (root.isEmpty() == true || QFileInfo(root).isDir() == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot snapshot;
    const RenderGraphDocumentStatus status = ReadPackage(root, rootGraph, snapshot);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    adopt(root, rootGraph, snapshot, false);
    return RenderGraphDocumentStatus::Ok;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named source/file/destination identities.
RenderGraphDocumentStatus
RenderGraphDocument::LoadBuiltIn(const QString& sourceRoot, const QString& rootGraph, const QString& destinationRoot)
{
    if (QFileInfo(sourceRoot).isDir() == false || QDir::isAbsolutePath(destinationRoot) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot snapshot;
    const RenderGraphDocumentStatus status = ReadPackage(sourceRoot, rootGraph, snapshot);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    adopt(QDir::cleanPath(destinationRoot), rootGraph, snapshot, true);
    return RenderGraphDocumentStatus::Ok;
}

void RenderGraphDocument::adopt(const QString& packageRoot,
                                const QString& rootGraph,
                                const RenderGraphSnapshot& snapshot,
                                bool isBuiltIn)
{
    mPackageRoot = packageRoot;
    mRootGraph = rootGraph;
    mSaved = snapshot;
    mDraft = snapshot;
    mIsBuiltIn = isBuiltIn;
    mLastMove.clear();
    // A new document identity is the only reason to reset the monotonic revision.
    mHistory.Reset();
}

void RenderGraphDocument::Reset()
{
    mIsBuiltIn = false;
    mPackageRoot.clear();
    mRootGraph.clear();
    mSaved = {};
    mDraft = {};
    mLastMove.clear();
    mHistory.Reset();
}

QStringList RenderGraphDocument::GetFiles() const
{
    return mDraft.Files.keys();
}

QByteArray RenderGraphDocument::GetDraft(const QString& file) const
{
    return mDraft.Files.value(file);
}

QByteArray RenderGraphDocument::GetSaved(const QString& file) const
{
    return mSaved.Files.value(file);
}

bool RenderGraphDocument::IsFileDirty(const QString& file) const
{
    return mDraft.Files.value(file) != mSaved.Files.value(file);
}

bool RenderGraphDocument::IsDirty() const
{
    return (mDraft == mSaved) == false;
}

bool RenderGraphDocument::GetGraph(const QString& file, QJsonObject& outGraph) const
{
    if (file.endsWith(QStringLiteral(".lurender.json")) == false || mDraft.Files.contains(file) == false)
    {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(mDraft.Files.value(file), &error);
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        return false;
    }
    outGraph = document.object();
    return true;
}

RenderGraphDocumentStatus RenderGraphDocument::commit(const RenderGraphSnapshot& candidate, bool shouldMerge)
{
    qsizetype total = 0;
    for (const QByteArray& bytes : candidate.Files)
    {
        if (bytes.size() > MAX_FILE_BYTES)
        {
            return RenderGraphDocumentStatus::Capacity;
        }
        total += bytes.size();
    }
    if (total > MAX_TOTAL_BYTES)
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    if (mHistory.Commit(mDraft, candidate, shouldMerge) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    mDraft = candidate;
    return RenderGraphDocumentStatus::Ok;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named package/file/configuration identities.
RenderGraphDocumentStatus RenderGraphDocument::editConfiguration(const QString& file,
                                                                 const QString& configuration,
                                                                 const QStringList& arguments,
                                                                 EditFunction edit,
                                                                 bool shouldMerge)
{
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindConfiguration(configurations, configuration);
    if (index < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject record = configurations.at(index).toObject();
    if (edit(record, arguments) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    configurations.replace(index, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    return commit(candidate, shouldMerge);
}

RenderGraphDocumentStatus
RenderGraphDocument::ReplaceNode(const QString& file, const QString& configuration, const QJsonObject& node)
{
    mLastMove.clear();
    return editConfiguration(file,
                             configuration,
                             {QString::fromUtf8(QJsonDocument(node).toJson(QJsonDocument::Compact))},
                             &ReplaceNodeEdit,
                             false);
}

RenderGraphDocumentStatus
RenderGraphDocument::AddNode(const QString& file, const QString& configuration, const QJsonObject& node)
{
    mLastMove.clear();
    return editConfiguration(file,
                             configuration,
                             {QString::fromUtf8(QJsonDocument(node).toJson(QJsonDocument::Compact))},
                             &AddNodeEdit,
                             false);
}

RenderGraphDocumentStatus
RenderGraphDocument::RemoveNode(const QString& file, const QString& configuration, const QString& node)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {node}, &RemoveNodeEdit, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::Connect(const QString& file, const QString& configuration, const QString& from, const QString& to)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {from, to}, &ConnectEdit, false);
}

RenderGraphDocumentStatus RenderGraphDocument::Disconnect(const QString& file,
                                                          const QString& configuration,
                                                          const QString& from,
                                                          const QString& to)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {from, to}, &DisconnectEdit, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::MoveNode(const QString& file, const QString& configuration, const QString& node, int32 x, int32 y)
{
    const QString key = file + QLatin1Char('#') + configuration + QLatin1Char('/') + node;
    const bool shouldMerge = mLastMove == key;
    const RenderGraphDocumentStatus status = editConfiguration(file,
                                                               configuration,
                                                               {node, QString::number(x), QString::number(y)},
                                                               &MoveNodeEdit,
                                                               shouldMerge);
    mLastMove = status == RenderGraphDocumentStatus::Ok ? key : QString();
    return status;
}

RenderGraphDocumentStatus RenderGraphDocument::MoveNodes(const QString& file,
                                                         const QString& configuration,
                                                         const QVector<RenderGraphNodePosition>& positions)
{
    mLastMove.clear();
    QStringList arguments;
    for (const RenderGraphNodePosition& position : positions)
    {
        arguments += {position.Node, QString::number(position.X), QString::number(position.Y)};
    }
    return editConfiguration(file, configuration, arguments, &MoveNodesEdit, false);
}

RenderGraphDocumentStatus RenderGraphDocument::SetText(const QString& file, const QByteArray& bytes)
{
    mLastMove.clear();
    if (mDraft.Files.contains(file) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, bytes);
    return commit(candidate, false);
}

bool RenderGraphDocument::Undo()
{
    mLastMove.clear();
    return mHistory.Undo(mDraft);
}

bool RenderGraphDocument::Redo()
{
    mLastMove.clear();
    return mHistory.Redo(mDraft);
}

RenderGraphDocumentStatus RenderGraphDocument::WriteSnapshot(const QString& directory) const
{
    QDir target(directory);
    if (IsLoaded() == false || target.exists() == false ||
        target.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty() == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin(); file != mDraft.Files.constEnd();
         ++file)
    {
        const QString path = target.filePath(file.key());
        QFile output(path);
        if (QDir().mkpath(QFileInfo(path).absolutePath()) == false || output.open(QIODevice::WriteOnly) == false ||
            output.write(file.value()) != file.value().size())
        {
            return RenderGraphDocumentStatus::WriteFailed;
        }
    }
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus RenderGraphDocument::RefreshFromDisk(bool& outHasChanged)
{
    outHasChanged = false;
    if (IsLoaded() == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    if (mIsBuiltIn == true)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    if (QFileInfo(mPackageRoot).canonicalFilePath() != mPackageRoot)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    RenderGraphSnapshot candidate;
    const RenderGraphDocumentStatus status = ReadPackage(mPackageRoot, mRootGraph, candidate);
    if (status != RenderGraphDocumentStatus::Ok || candidate == mSaved)
    {
        return status;
    }
    if (IsDirty() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    const RenderGraphDocumentStatus committed = commit(candidate, false);
    if (committed == RenderGraphDocumentStatus::Ok)
    {
        mSaved = candidate;
        mLastMove.clear();
        outHasChanged = true;
    }
    return committed;
}

RenderGraphDocumentStatus RenderGraphDocument::Save(QStringList& outSavedFiles)
{
    outSavedFiles.clear();
    if (IsLoaded() == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    if (mIsBuiltIn == true)
    {
        return createPackage(outSavedFiles);
    }
    QLockFile lock(QDir(mPackageRoot).filePath(LOCK_NAME));
    if (lock.tryLock(0) == false)
    {
        return RenderGraphDocumentStatus::Locked;
    }
    QStringList dirty;
    for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin(); file != mDraft.Files.constEnd();
         ++file)
    {
        if (IsFileDirty(file.key()) == false)
        {
            continue;
        }
        // Another writer changed the file since it was loaded or saved: write nothing.
        QByteArray disk;
        if (ReadBounded(QDir(mPackageRoot).filePath(file.key()), disk) == false ||
            disk != mSaved.Files.value(file.key()))
        {
            return RenderGraphDocumentStatus::Conflict;
        }
        dirty.append(file.key());
    }
    for (const QString& file : dirty)
    {
        const QString path = QDir(mPackageRoot).filePath(file);
        const QByteArray bytes = mDraft.Files.value(file);
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (QFileInfo(path).isSymLink() == true || output.open(QIODevice::WriteOnly) == false ||
            output.write(bytes) != bytes.size() || output.commit() == false)
        {
            return RenderGraphDocumentStatus::WriteFailed;
        }
        mSaved.Files.insert(file, bytes);
        outSavedFiles.append(file);
    }
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus RenderGraphDocument::createPackage(QStringList& outSavedFiles)
{
    // A customized built-in becomes a new package: every file is created, none replaced.
    const QDir destination(mPackageRoot);
    const auto hasExistingFile = [this, &destination]() {
        for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin();
             file != mDraft.Files.constEnd();
             ++file)
        {
            if (QFileInfo::exists(destination.filePath(file.key())) == true ||
                QFileInfo(destination.filePath(file.key())).isSymLink() == true)
            {
                return true;
            }
        }
        return false;
    };
    if (hasExistingFile() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    if (QDir().mkpath(mPackageRoot) == false)
    {
        return RenderGraphDocumentStatus::WriteFailed;
    }
    QLockFile lock(destination.filePath(LOCK_NAME));
    if (lock.tryLock(0) == false)
    {
        return RenderGraphDocumentStatus::Locked;
    }
    // Another writer may have created a file between the first check and the lock.
    if (hasExistingFile() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    QStringList created;
    for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin(); file != mDraft.Files.constEnd();
         ++file)
    {
        const QString path = destination.filePath(file.key());
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (QDir().mkpath(QFileInfo(path).absolutePath()) == false || output.open(QIODevice::WriteOnly) == false ||
            output.write(file.value()) != file.value().size() || output.commit() == false)
        {
            // Remove this call's files so a retry still finds an empty destination.
            for (const QString& written : created)
            {
                static_cast<void>(QFile::remove(destination.filePath(written)));
            }
            return RenderGraphDocumentStatus::WriteFailed;
        }
        created.append(file.key());
    }
    // The package root keeps the destination's spelling, so project-relative paths are unchanged.
    mSaved = mDraft;
    mIsBuiltIn = false;
    outSavedFiles = created;
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
