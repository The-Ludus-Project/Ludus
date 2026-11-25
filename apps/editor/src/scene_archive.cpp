#include "internal/scene_archive.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <QVector>

#include <span>
#include <utility>

extern "C" int stbi_zlib_decode_noheader_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr uint32 END_OF_DIRECTORY_SIGNATURE = 0x06054b50U;
constexpr uint32 DIRECTORY_ENTRY_SIGNATURE = 0x02014b50U;
constexpr uint32 LOCAL_ENTRY_SIGNATURE = 0x04034b50U;
constexpr usize END_OF_DIRECTORY_SIZE = 22;
/// Largest archive comment, which bounds the backwards search for the end record.
constexpr usize MAX_COMMENT_SIZE = 0xFFFF;
constexpr usize DIRECTORY_ENTRY_SIZE = 46;
constexpr usize LOCAL_ENTRY_SIZE = 30;
constexpr uint16 METHOD_STORED = 0;
constexpr uint16 METHOD_DEFLATED = 8;
constexpr uint16 FLAG_ENCRYPTED = 1;
/// Marker value meaning a ZIP64 extension holds the real size or offset.
constexpr uint32 ZIP64_MARKER = 0xFFFFFFFFU;
/// Upper bound on expanded bytes so a hostile archive cannot exhaust browser memory.
constexpr uint64 MAX_EXPANDED_BYTES = 1024ULL * 1024ULL * 1024ULL;
constexpr usize MAX_ENTRIES = 16384;
/// Field offsets within the end record, central entry and local entry.
constexpr usize END_ENTRY_COUNT_OFFSET = 10;
constexpr usize END_DIRECTORY_OFFSET = 16;
constexpr usize ENTRY_FLAGS_OFFSET = 8;
constexpr usize ENTRY_METHOD_OFFSET = 10;
constexpr usize ENTRY_COMPRESSED_OFFSET = 20;
constexpr usize ENTRY_EXPANDED_OFFSET = 24;
constexpr usize ENTRY_NAME_LENGTH_OFFSET = 28;
constexpr usize ENTRY_EXTRA_LENGTH_OFFSET = 30;
constexpr usize ENTRY_COMMENT_LENGTH_OFFSET = 32;
constexpr usize ENTRY_LOCAL_OFFSET = 42;
constexpr usize LOCAL_NAME_LENGTH_OFFSET = 26;
constexpr usize LOCAL_EXTRA_LENGTH_OFFSET = 28;

template <typename ValueType>
[[nodiscard]] bool readAt(std::span<const uint8> bytes, usize offset, ValueType& out)
{
    if (offset > bytes.size())
    {
        return false;
    }
    return TryReadLittleEndian(ludus::foundation::MakeBorrowedView(bytes.subspan(offset)), out);
}

[[nodiscard]] bool isSafeEntryName(const QString& name)
{
    if (name.isEmpty() == true || name.startsWith(QLatin1Char('/')) == true ||
        name.contains(QLatin1Char('\\')) == true || name.contains(QLatin1Char(':')) == true)
    {
        return false;
    }
    const QStringList parts = name.split(QLatin1Char('/'));
    for (const QString& part : parts)
    {
        if (part == QStringLiteral(".."))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool findEndOfDirectory(std::span<const uint8> bytes, usize& outOffset)
{
    if (bytes.size() < END_OF_DIRECTORY_SIZE)
    {
        return false;
    }
    const usize last = bytes.size() - END_OF_DIRECTORY_SIZE;
    const usize first = last > MAX_COMMENT_SIZE ? last - MAX_COMMENT_SIZE : 0;
    for (usize offset = last + 1; offset > first; --offset)
    {
        uint32 signature = 0;
        if (readAt(bytes, offset - 1, signature) == true && signature == END_OF_DIRECTORY_SIGNATURE)
        {
            outOffset = offset - 1;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool writeEntry(const QString& path, const QByteArray& contents)
{
    if (QDir().mkpath(QFileInfo(path).absolutePath()) == false)
    {
        return false;
    }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) == false || file.write(contents) != contents.size())
    {
        return false;
    }
    return file.commit();
}
struct ArchiveEntry final
{
    QString Name;
    usize DataOffset = 0;
    uint32 CompressedSize = 0;
    uint32 ExpandedSize = 0;
    uint16 Method = METHOD_STORED;
};

/// Read and validate the central directory without decompressing anything.
[[nodiscard]] bool readDirectory(std::span<const uint8> bytes,
                                 uint64 maximumExpandedBytes,
                                 QVector<ArchiveEntry>& outEntries,
                                 uint64& outExpandedBytes,
                                 QString& outError)
{
    usize endOffset = 0;
    if (findEndOfDirectory(bytes, endOffset) == false)
    {
        outError = QStringLiteral("The file is not a .zip archive.");
        return false;
    }
    uint16 entryCount = 0;
    uint32 directoryOffset = 0;
    if (readAt(bytes, endOffset + END_ENTRY_COUNT_OFFSET, entryCount) == false ||
        readAt(bytes, endOffset + END_DIRECTORY_OFFSET, directoryOffset) == false || directoryOffset == ZIP64_MARKER ||
        entryCount > MAX_ENTRIES)
    {
        outError = QStringLiteral("The .zip archive directory is unsupported (ZIP64 or too many entries).");
        return false;
    }
    QVector<ArchiveEntry> entries;
    uint64 expandedTotal = 0;
    usize cursor = directoryOffset;
    for (uint16 index = 0; index < entryCount; ++index)
    {
        uint32 signature = 0;
        uint16 flags = 0;
        uint16 method = 0;
        uint32 compressedSize = 0;
        uint32 expandedSize = 0;
        uint16 nameLength = 0;
        uint16 extraLength = 0;
        uint16 commentLength = 0;
        uint32 localOffset = 0;
        if (readAt(bytes, cursor, signature) == false || signature != DIRECTORY_ENTRY_SIGNATURE ||
            readAt(bytes, cursor + ENTRY_FLAGS_OFFSET, flags) == false ||
            readAt(bytes, cursor + ENTRY_METHOD_OFFSET, method) == false ||
            readAt(bytes, cursor + ENTRY_COMPRESSED_OFFSET, compressedSize) == false ||
            readAt(bytes, cursor + ENTRY_EXPANDED_OFFSET, expandedSize) == false ||
            readAt(bytes, cursor + ENTRY_NAME_LENGTH_OFFSET, nameLength) == false ||
            readAt(bytes, cursor + ENTRY_EXTRA_LENGTH_OFFSET, extraLength) == false ||
            readAt(bytes, cursor + ENTRY_COMMENT_LENGTH_OFFSET, commentLength) == false ||
            readAt(bytes, cursor + ENTRY_LOCAL_OFFSET, localOffset) == false ||
            cursor + DIRECTORY_ENTRY_SIZE + nameLength > bytes.size())
        {
            outError = QStringLiteral("The .zip archive directory is damaged.");
            return false;
        }
        const QString name =
            QString::fromUtf8(reinterpret_cast<const char*>(bytes.data() + cursor + DIRECTORY_ENTRY_SIZE), nameLength);
        cursor += DIRECTORY_ENTRY_SIZE + nameLength + extraLength + commentLength;
        if (name.endsWith(QLatin1Char('/')) == true)
        {
            continue;
        }
        if (isSafeEntryName(name) == false)
        {
            outError = QStringLiteral("The .zip archive has an unsafe entry name: %1").arg(name);
            return false;
        }
        if ((flags & FLAG_ENCRYPTED) != 0 || (method != METHOD_STORED && method != METHOD_DEFLATED) ||
            compressedSize == ZIP64_MARKER || expandedSize == ZIP64_MARKER || localOffset == ZIP64_MARKER ||
            (method == METHOD_STORED && compressedSize != expandedSize))
        {
            outError = QStringLiteral("The .zip entry %1 uses an unsupported method, encryption or ZIP64.").arg(name);
            return false;
        }
        expandedTotal += expandedSize;
        if (expandedTotal > maximumExpandedBytes)
        {
            outError = QStringLiteral("Import refused: the .zip archive expands beyond the session import limit.");
            return false;
        }
        uint32 localSignature = 0;
        uint16 localNameLength = 0;
        uint16 localExtraLength = 0;
        if (readAt(bytes, localOffset, localSignature) == false || localSignature != LOCAL_ENTRY_SIGNATURE ||
            readAt(bytes, localOffset + LOCAL_NAME_LENGTH_OFFSET, localNameLength) == false ||
            readAt(bytes, localOffset + LOCAL_EXTRA_LENGTH_OFFSET, localExtraLength) == false)
        {
            outError = QStringLiteral("The .zip entry %1 is damaged.").arg(name);
            return false;
        }
        const usize dataOffset =
            static_cast<usize>(localOffset) + LOCAL_ENTRY_SIZE + localNameLength + localExtraLength;
        if (dataOffset > bytes.size() || compressedSize > bytes.size() - dataOffset)
        {
            outError = QStringLiteral("The .zip entry %1 is truncated.").arg(name);
            return false;
        }
        entries.push_back({name, dataOffset, compressedSize, expandedSize, method});
    }
    outEntries = std::move(entries);
    outExpandedBytes = expandedTotal;
    return true;
}
} // namespace

bool ExpandSceneArchive(const QByteArray& archive,
                        const QString& directory,
                        uint64 maximumExpandedBytes,
                        uint64& outExpandedBytes,
                        QString& outError)
{
    const std::span<const uint8> bytes(reinterpret_cast<const uint8*>(archive.constData()),
                                       static_cast<usize>(archive.size()));
    QVector<ArchiveEntry> entries;
    uint64 expandedTotal = 0;
    if (readDirectory(bytes, math::Min(maximumExpandedBytes, MAX_EXPANDED_BYTES), entries, expandedTotal, outError) ==
        false)
    {
        return false;
    }
    for (const ArchiveEntry& entry : entries)
    {
        const char* data = archive.constData() + entry.DataOffset;
        QByteArray contents;
        if (entry.Method == METHOD_STORED)
        {
            contents = QByteArray(data, static_cast<qsizetype>(entry.CompressedSize));
        }
        else
        {
            contents.resize(static_cast<qsizetype>(entry.ExpandedSize));
            const int written = stbi_zlib_decode_noheader_buffer(contents.data(),
                                                                 static_cast<int>(entry.ExpandedSize),
                                                                 data,
                                                                 static_cast<int>(entry.CompressedSize));
            if (written < 0 || static_cast<uint32>(written) != entry.ExpandedSize)
            {
                outError = QStringLiteral("The .zip entry %1 could not be decompressed.").arg(entry.Name);
                return false;
            }
        }
        if (writeEntry(QDir(directory).filePath(entry.Name), contents) == false)
        {
            outError = QStringLiteral("Could not write %1 from the .zip archive.").arg(entry.Name);
            return false;
        }
    }
    outExpandedBytes = expandedTotal;
    return true;
}
} // namespace ludus::editor
