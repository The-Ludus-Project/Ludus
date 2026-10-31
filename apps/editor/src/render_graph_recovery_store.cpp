#include "internal/render_graph_recovery_store.h"

#include <ludus/foundation/base/types.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 RECOVERY_VERSION = 1;
constexpr int32 HASH_HEX_BYTES = 64;
constexpr int32 MAX_CANDIDATES = 64;
constexpr qsizetype METADATA_BYTES = 1024 * 1024;
// Encoded bytes have a separate limit from decoded package bytes. Room for base64
// expansion, path metadata and the envelope; decoded admission stays with the document.
constexpr qsizetype MAX_ENVELOPE_BYTES = 2 * RenderGraphDocument::MAX_TOTAL_BYTES + METADATA_BYTES;
constexpr qsizetype BASE64_GROUP_BYTES = 3;
constexpr qsizetype BASE64_GROUP_CHARACTERS = 4;

QByteArray GetDigest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool ReadEnvelope(const QString& path, QByteArray& outPayload, QByteArray& outDigest)
{
    const QFileInfo information(path);
    QFile file(path);
    if (information.isSymLink() == true || information.isFile() == false || information.size() > MAX_ENVELOPE_BYTES ||
        file.open(QIODevice::ReadOnly) == false)
    {
        return false;
    }
    const QByteArray bytes = file.read(MAX_ENVELOPE_BYTES + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > MAX_ENVELOPE_BYTES || bytes.size() <= HASH_HEX_BYTES ||
        bytes.at(HASH_HEX_BYTES) != '\n')
    {
        return false;
    }
    const QByteArray payload = bytes.mid(HASH_HEX_BYTES + 1);
    const QByteArray digest = bytes.first(HASH_HEX_BYTES);
    if (GetDigest(payload) != digest)
    {
        return false;
    }
    outPayload = payload;
    outDigest = digest;
    return true;
}
} // namespace

bool WriteRecoveryCopy(const QByteArray& bytes, const QString& path, QString& outError)
{
    outError.clear();
    QFile output(path);
    if (bytes.size() > RenderGraphDocument::MAX_FILE_BYTES ||
        output.open(QIODevice::WriteOnly | QIODevice::NewOnly) == false)
    {
        outError = QStringLiteral("Choose a new writable file. Existing files are never overwritten.");
        return false;
    }
    if (output.write(bytes) != bytes.size() || output.flush() == false)
    {
        static_cast<void>(output.remove());
        outError = QStringLiteral("Copy failed. The checkpoint is retained; check free space and permissions.");
        return false;
    }
    return true;
}

RenderGraphRecoveryStore::RenderGraphRecoveryStore(const QString& directory)
    : mDirectory(directory.isEmpty() == false ? QDir::cleanPath(QFileInfo(directory).absoluteFilePath()) : QString{}),
      mSession(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

void RenderGraphRecoveryStore::BeginBinding()
{
    mSession = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString RenderGraphRecoveryStore::getIdentity(const RenderGraphDocument& document) const
{
    return QString::fromLatin1(GetDigest(document.GetPackageRoot().toUtf8() + '\0' + document.GetRootGraph().toUtf8()));
}

QString RenderGraphRecoveryStore::GetPath(const RenderGraphDocument& document) const
{
    return document.IsLoaded() == true && mDirectory.isEmpty() == false
               ? QDir(mDirectory)
                     .filePath(getIdentity(document) + QLatin1Char('-') + mSession + QStringLiteral(".recovery"))
               : QString{};
}

bool RenderGraphRecoveryStore::isLocalFile(const QString& path) const
{
    const QFileInfo directory(mDirectory);
    const QFileInfo file(path);
    return directory.isSymLink() == false && directory.isDir() == true && file.isSymLink() == false &&
           file.absolutePath() == mDirectory &&
           QFileInfo(file.absolutePath()).canonicalFilePath() == directory.canonicalFilePath();
}

QStringList RenderGraphRecoveryStore::Find(const RenderGraphDocument& document) const
{
    QStringList paths;
    if (document.IsLoaded() == false || mDirectory.isEmpty() == true || QFileInfo(mDirectory).isSymLink() == true)
    {
        return paths;
    }
    const QFileInfoList files = QDir(mDirectory)
                                    .entryInfoList({getIdentity(document) + QStringLiteral("-*.recovery")},
                                                   QDir::Files | QDir::NoSymLinks,
                                                   QDir::Time);
    const QString ownPath = GetPath(document);
    for (const QFileInfo& file : files)
    {
        if (file.absoluteFilePath() != ownPath)
        {
            paths.append(file.absoluteFilePath());
            if (paths.size() >= MAX_CANDIDATES)
            {
                break;
            }
        }
    }
    return paths;
}

bool RenderGraphRecoveryStore::Write(const RenderGraphDocument& document, QString& outError)
{
    outError.clear();
    if (document.IsLoaded() == false || mDirectory.isEmpty() == true)
    {
        outError = QStringLiteral("Recovery storage is unavailable. Save to protect edits.");
        return false;
    }
    QJsonObject files;
    const RenderGraphSnapshot draft = document.CaptureRecoverySnapshot(RenderGraphSource::Draft);
    for (QMap<QString, QByteArray>::const_iterator file = draft.Files.constBegin(); file != draft.Files.constEnd();
         ++file)
    {
        if (IsRecoverySourcePath(file.key()) == false)
        {
            outError = QStringLiteral("Recovery cannot encode a non-portable source path. Save to protect edits: %1")
                           .arg(file.key());
            return false;
        }
        files.insert(file.key(), QString::fromLatin1(file.value().toBase64()));
    }
    QJsonObject hashes;
    const RenderGraphSnapshot saved = document.CaptureRecoverySnapshot(RenderGraphSource::Saved);
    for (QMap<QString, QByteArray>::const_iterator file = saved.Files.constBegin(); file != saved.Files.constEnd();
         ++file)
    {
        hashes.insert(file.key(), QString::fromLatin1(GetDigest(file.value())));
    }
    const QByteArray payload = QJsonDocument(QJsonObject{
                                                 {QStringLiteral("version"), RECOVERY_VERSION},
                                                 {QStringLiteral("package"), document.GetPackageRoot()},
                                                 {QStringLiteral("root"), document.GetRootGraph()},
                                                 {QStringLiteral("builtIn"), document.IsBuiltIn()},
                                                 {QStringLiteral("saved"), hashes},
                                                 {QStringLiteral("draft"), files},
                                             })
                                   .toJson(QJsonDocument::Compact);
    const QByteArray envelope = GetDigest(payload) + '\n' + payload;
    const QString path = GetPath(document);
    if (envelope.size() > MAX_ENVELOPE_BYTES || QFileInfo(mDirectory).isSymLink() == true ||
        QDir().mkpath(mDirectory) == false || isLocalFile(path) == false)
    {
        outError = QStringLiteral("Recovery storage is unavailable. Save to protect edits: %1").arg(path);
        return false;
    }
    const QString lockPath = path + QStringLiteral(".lock");
    QLockFile lock(lockPath);
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (QFileInfo(lockPath).isSymLink() == true || lock.tryLock(0) == false ||
        output.open(QIODevice::WriteOnly) == false || output.write(envelope) != envelope.size() ||
        output.commit() == false)
    {
        outError =
            QStringLiteral("Recovery checkpoint failed. Save to protect edits. Check free space and write access: %1")
                .arg(path);
        return false;
    }
    return true;
}

bool RenderGraphRecoveryStore::Read(const QString& path,
                                    const RenderGraphDocument& document,
                                    RenderGraphRecoveryCheckpoint& outCheckpoint,
                                    QString& outError) const
{
    outError =
        QStringLiteral("Recovery is damaged, too large, linked, or belongs to another package. Nothing was changed: %1")
            .arg(path);
    QByteArray payload;
    RenderGraphRecoveryCheckpoint candidate;
    if (isLocalFile(path) == false || ReadEnvelope(path, payload, candidate.Digest) == false)
    {
        return false;
    }
    const QJsonDocument parsed = QJsonDocument::fromJson(payload);
    const QJsonObject object = parsed.object();
    if (parsed.isObject() == false || object.value(QStringLiteral("version")).toInt() != RECOVERY_VERSION ||
        object.value(QStringLiteral("package")).toString() != document.GetPackageRoot() ||
        object.value(QStringLiteral("root")).toString() != document.GetRootGraph() ||
        object.value(QStringLiteral("builtIn")).isBool() == false ||
        object.value(QStringLiteral("saved")).isObject() == false ||
        object.value(QStringLiteral("draft")).isObject() == false)
    {
        return false;
    }
    const QJsonObject files = object.value(QStringLiteral("draft")).toObject();
    const QJsonObject hashes = object.value(QStringLiteral("saved")).toObject();
    if (static_cast<foundation::usize>(files.size()) > RenderGraphDocument::MAX_FILES ||
        static_cast<foundation::usize>(hashes.size()) > RenderGraphDocument::MAX_FILES)
    {
        return false;
    }
    qsizetype total = 0;
    for (QJsonObject::const_iterator file = files.constBegin(); file != files.constEnd(); ++file)
    {
        const QString name = file.key();
        if (file.value().isString() == false || IsRecoverySourcePath(name) == false)
        {
            return false;
        }
        const QByteArray encoded = file.value().toString().toLatin1();
        constexpr qsizetype MAX_ENCODED_FILE =
            ((RenderGraphDocument::MAX_FILE_BYTES + BASE64_GROUP_BYTES - 1) / BASE64_GROUP_BYTES) *
            BASE64_GROUP_CHARACTERS;
        if (encoded.size() > MAX_ENCODED_FILE)
        {
            return false;
        }
        const QByteArray bytes = QByteArray::fromBase64(encoded);
        total += bytes.size();
        if (bytes.toBase64() != encoded || bytes.size() > RenderGraphDocument::MAX_FILE_BYTES ||
            total > RenderGraphDocument::MAX_TOTAL_BYTES)
        {
            return false;
        }
        candidate.Draft.Files.insert(name, bytes);
    }
    if (candidate.Draft.Files.contains(document.GetRootGraph()) == false)
    {
        return false;
    }
    for (QJsonObject::const_iterator hash = hashes.constBegin(); hash != hashes.constEnd(); ++hash)
    {
        const QByteArray value = hash.value().toString().toLatin1();
        if (hash.value().isString() == false || value.size() != HASH_HEX_BYTES ||
            QByteArray::fromHex(value).toHex() != value)
        {
            return false;
        }
        candidate.SavedHashes.insert(hash.key(), value);
    }
    candidate.IsBuiltIn = object.value(QStringLiteral("builtIn")).toBool();
    outCheckpoint = candidate;
    outError.clear();
    return true;
}

bool RenderGraphRecoveryStore::Remove(const QString& path, const QByteArray& expectedDigest, QString& outError) const
{
    outError.clear();
    if (QFileInfo::exists(path) == false && QFileInfo(path).isSymLink() == false)
    {
        return true;
    }
    const QString lockPath = path + QStringLiteral(".lock");
    QLockFile lock(lockPath);
    if (isLocalFile(path) == false || QFileInfo(lockPath).isSymLink() == true || lock.tryLock(0) == false)
    {
        outError = QStringLiteral("Recovery is busy or linked; the checkpoint was retained: %1").arg(path);
        return false;
    }
    QByteArray payload;
    QByteArray digest;
    if (isLocalFile(path) == false || ReadEnvelope(path, payload, digest) == false || digest != expectedDigest ||
        QFile::remove(path) == false)
    {
        outError =
            QStringLiteral("Recovery changed or could not be removed; the checkpoint was retained: %1").arg(path);
        return false;
    }
    return true;
}
} // namespace ludus::editor
