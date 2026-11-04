#include "internal/code_recovery_journal.h"

#include <ludus/foundation/base/types.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

// The bounded code-buffer recovery journal (CE03), owned by CodeDocumentRegistry
// and stored under the project's machine-local state directory. One atomic file
// per canonical path, carrying a sha256-prefixed JSON envelope. Writes and
// removes run on the background pool over a copied snapshot; reads validate the
// checksum, schema and scope before any draft is handed back. No project code is
// executed and nothing is ever written into the project tree.

namespace ludus::editor
{
namespace
{
using foundation::int32;
using foundation::isize;

constexpr int32 JOURNAL_VERSION = 1;
constexpr int32 HASH_HEX_BYTES = 64;

[[nodiscard]] QByteArray HexDigest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// A stable, filesystem-safe slot name for a project or a file path. The hash
// keys the file so an arbitrary path never leaks into the journal directory.
[[nodiscard]] QString SlotName(const QString& value)
{
    return QString::fromLatin1(HexDigest(value.toUtf8()));
}
} // namespace

CodeRecoveryJournal::CodeRecoveryJournal(const QString& stateDirectory, const QString& projectRoot)
    : mStateDirectory(stateDirectory.isEmpty() == false ? QDir::cleanPath(QFileInfo(stateDirectory).absoluteFilePath())
                                                        : QString{})
{
    SetProject(projectRoot);
}

void CodeRecoveryJournal::SetProject(const QString& projectRoot)
{
    mProjectRoot = projectRoot;
    if (mStateDirectory.isEmpty() == true || projectRoot.isEmpty() == true)
    {
        mDirectory.clear();
        return;
    }
    mDirectory = QDir(mStateDirectory).filePath(QStringLiteral("code-recovery/") + SlotName(projectRoot));
}

QString CodeRecoveryJournal::slotPath(const QString& canonicalPath) const
{
    if (mDirectory.isEmpty() == true || canonicalPath.isEmpty() == true)
    {
        return QString{};
    }
    return QDir(mDirectory).filePath(SlotName(canonicalPath) + QStringLiteral(".journal"));
}

bool CodeRecoveryJournal::isLocalFile(const QString& path) const
{
    if (path.isEmpty() == true || mDirectory.isEmpty() == true)
    {
        return false;
    }
    const QFileInfo directory(mDirectory);
    const QFileInfo file(path);
    return directory.isSymLink() == false && directory.isDir() == true && file.isSymLink() == false &&
           file.absolutePath() == QDir::cleanPath(directory.absoluteFilePath());
}

QStringList CodeRecoveryJournal::journalFiles() const
{
    if (mDirectory.isEmpty() == true || QFileInfo(mDirectory).isSymLink() == true ||
        QFileInfo(mDirectory).isDir() == false)
    {
        return {};
    }
    QStringList paths;
    const QFileInfoList files =
        QDir(mDirectory).entryInfoList({QStringLiteral("*.journal")}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    for (const QFileInfo& file : files)
    {
        paths.append(file.absoluteFilePath());
        if (paths.size() >= MAX_RECOVERY_ENTRIES)
        {
            break;
        }
    }
    return paths;
}

bool CodeRecoveryJournal::Write(const CodeRecoveryEntry& entry) const
{
    if (mDirectory.isEmpty() == true || entry.CanonicalPath.isEmpty() == true)
    {
        return false;
    }
    // Reject an oversized draft before base64 expansion. The envelope (base64
    // draft plus the small JSON header and checksum) is always larger than the
    // raw draft, so a draft already past the cap can never fit.
    if (static_cast<isize>(entry.Draft.size()) > MAX_RECOVERY_ENTRY_BYTES)
    {
        return false;
    }
    const QByteArray payload =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("version"), JOURNAL_VERSION},
                          {QStringLiteral("path"), entry.CanonicalPath},
                          {QStringLiteral("savedHash"), QString::fromLatin1(entry.SavedHash.toHex())},
                          {QStringLiteral("draft"), QString::fromLatin1(entry.Draft.toBase64())},
                      })
            .toJson(QJsonDocument::Compact);
    const QByteArray envelope = HexDigest(payload) + '\n' + payload;
    if (static_cast<isize>(envelope.size()) > MAX_RECOVERY_ENTRY_BYTES)
    {
        return false;
    }
    if (QFileInfo(mDirectory).isSymLink() == true || QDir().mkpath(mDirectory) == false)
    {
        return false;
    }
    const QString path = slotPath(entry.CanonicalPath);
    if (isLocalFile(path) == false && QFileInfo::exists(path) == true)
    {
        // The slot exists but is not a plain local file (a link or a directory);
        // refuse to follow or overwrite it.
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
        return false;
    }
    return true;
}

bool CodeRecoveryJournal::Remove(const QString& canonicalPath) const
{
    const QString path = slotPath(canonicalPath);
    if (path.isEmpty() == true)
    {
        return false;
    }
    if (QFileInfo::exists(path) == false && QFileInfo(path).isSymLink() == false)
    {
        return true;
    }
    const QString lockPath = path + QStringLiteral(".lock");
    QLockFile lock(lockPath);
    if (QFileInfo(lockPath).isSymLink() == true || lock.tryLock(0) == false)
    {
        return false;
    }
    if (isLocalFile(path) == false)
    {
        return false;
    }
    return QFile::remove(path) == true;
}

QVector<CodeRecoveryEntry> CodeRecoveryJournal::Read() const
{
    QVector<CodeRecoveryEntry> entries;
    for (const QString& path : journalFiles())
    {
        if (isLocalFile(path) == false)
        {
            continue;
        }
        const QFileInfo info(path);
        if (info.size() > MAX_RECOVERY_ENTRY_BYTES)
        {
            continue;
        }
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) == false)
        {
            continue;
        }
        const QByteArray bytes = file.read(MAX_RECOVERY_ENTRY_BYTES + 1);
        if (file.error() != QFileDevice::NoError || static_cast<isize>(bytes.size()) > MAX_RECOVERY_ENTRY_BYTES ||
            bytes.size() <= HASH_HEX_BYTES || bytes.at(HASH_HEX_BYTES) != '\n')
        {
            continue;
        }
        const QByteArray digest = bytes.first(HASH_HEX_BYTES);
        const QByteArray payload = bytes.mid(HASH_HEX_BYTES + 1);
        if (HexDigest(payload) != digest)
        {
            continue;
        }
        const QJsonDocument parsed = QJsonDocument::fromJson(payload);
        if (parsed.isObject() == false)
        {
            continue;
        }
        const QJsonObject object = parsed.object();
        if (object.value(QStringLiteral("version")).toInt() != JOURNAL_VERSION ||
            object.value(QStringLiteral("path")).isString() == false ||
            object.value(QStringLiteral("savedHash")).isString() == false ||
            object.value(QStringLiteral("draft")).isString() == false)
        {
            continue;
        }
        const QByteArray savedHashHex = object.value(QStringLiteral("savedHash")).toString().toLatin1();
        const QByteArray draftEncoded = object.value(QStringLiteral("draft")).toString().toLatin1();
        const QByteArray savedHash = QByteArray::fromHex(savedHashHex);
        const QByteArray draft = QByteArray::fromBase64(draftEncoded);
        if (savedHash.toHex() != savedHashHex || draft.toBase64() != draftEncoded)
        {
            continue;
        }
        CodeRecoveryEntry entry;
        entry.CanonicalPath = object.value(QStringLiteral("path")).toString();
        entry.SavedHash = savedHash;
        entry.Draft = draft;
        entries.append(entry);
    }
    return entries;
}

void CodeRecoveryJournal::Clear() const
{
    for (const QString& path : journalFiles())
    {
        if (isLocalFile(path) == false)
        {
            continue;
        }
        const QString lockPath = path + QStringLiteral(".lock");
        QLockFile lock(lockPath);
        if (QFileInfo(lockPath).isSymLink() == true || lock.tryLock(0) == false)
        {
            continue;
        }
        static_cast<void>(QFile::remove(path));
    }
}
} // namespace ludus::editor
