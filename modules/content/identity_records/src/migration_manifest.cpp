#include <ludus/content/identity_records/migration_manifest.h>

#include "internal/record_json.h"

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/parsing/json.hpp>

namespace ludus::content::identity_records
{
namespace
{
using foundation::identity::AssetId;
using foundation::identity::SourceObjectId;
using foundation::identity::Uuid;
using foundation::parsing::JsonValue;
using internal::Fail;

/// Bytes reserved per manifest entry beyond its strings: keys, UUID text and indentation.
constexpr usize ENTRY_OVERHEAD_BYTES = 256;
/// Worst-case expansion of one string byte by JSON escaping (`\u00XX`).
constexpr usize ESCAPED_BYTES_PER_BYTE = 6;

[[nodiscard]] bool IsLess(std::string_view left, std::string_view right) noexcept
{
    return left < right;
}

[[nodiscard]] bool IsUuidLess(const Uuid& left, const Uuid& right) noexcept
{
    for (usize i = 0; i < foundation::identity::UUID_BYTE_COUNT; ++i)
    {
        if (left.Bytes[i] != right.Bytes[i])
        {
            return left.Bytes[i] < right.Bytes[i];
        }
    }
    return false;
}

[[nodiscard]] bool IsValidDocumentPath(std::string_view path) noexcept
{
    return path.size() <= MAX_RESOURCE_PATH_BYTES && ValidPath(path) == true;
}

constexpr usize NO_INDEX = ~usize{0};
constexpr usize DIAGNOSTIC_LOCATION_BYTES = 80;

struct ManifestLocation final
{
    usize Document = NO_INDEX;
    usize Object = NO_INDEX;
};

[[nodiscard]] IdentityRecordStatus FailManifest(IdentityRecordError& outError,
                                                IdentityRecordStatus status,
                                                std::string_view reason,
                                                ManifestLocation location = {},
                                                std::string_view member = {}) noexcept
{
    foundation::StaticArray<uint8, DIAGNOSTIC_LOCATION_BYTES> storage;
    foundation::parsing::JsonWriter writer({storage.GetData(), storage.GetSize()});
    writer.Raw("$");
    if (location.Document != NO_INDEX)
    {
        writer.Raw(".documents[");
        writer.Integer(location.Document);
        writer.Raw("]");
    }
    if (location.Object != NO_INDEX)
    {
        writer.Raw(".objects[");
        writer.Integer(location.Object);
        writer.Raw("]");
    }
    if (member.empty() == false)
    {
        writer.Raw(".");
        writer.Raw(member);
    }
    std::span<const uint8> bytes;
    if (writer.Finish(bytes) != foundation::parsing::ParseStatus::Ok)
    {
        return Fail(outError, status, reason);
    }
    return Fail(outError, status, reason, {reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1});
}

[[nodiscard]] IdentityRecordStatus FailManifestParse(IdentityRecordError& outError,
                                                     const foundation::parsing::ParseError& error) noexcept
{
    using foundation::parsing::ParseStatus;
    const IdentityRecordStatus status = error.Status == ParseStatus::OutOfMemory     ? IdentityRecordStatus::OutOfMemory
                                        : error.Status == ParseStatus::LimitExceeded ? IdentityRecordStatus::Limit
                                                                                     : IdentityRecordStatus::Malformed;
    const std::string_view reason = status == IdentityRecordStatus::OutOfMemory ? "cannot allocate the manifest parser"
                                    : status == IdentityRecordStatus::Limit
                                        ? "manifest exceeds a parser limit; reduce input size or nesting"
                                        : "manifest is not valid strict JSON";
    if (error.Offset == foundation::parsing::UNKNOWN_BYTE_OFFSET)
    {
        return FailManifest(outError, status, reason);
    }
    foundation::StaticArray<uint8, DIAGNOSTIC_LOCATION_BYTES> storage;
    foundation::parsing::JsonWriter writer({storage.GetData(), storage.GetSize()});
    writer.Raw("byte ");
    writer.Integer(error.Offset);
    std::span<const uint8> bytes;
    if (writer.Finish(bytes) != ParseStatus::Ok)
    {
        return Fail(outError, status, reason);
    }
    return Fail(outError, status, reason, {reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1});
}

struct ManifestIdentity final
{
    Uuid Identity;
    ManifestLocation Location;
};

struct ManifestDecodeMemory final
{
    usize Budget;
    usize Live;
    usize& Peak;

    /// Reserve each empty table once, with no growth overlap.
    template <typename Record>
    [[nodiscard]] IdentityRecordStatus
    Reserve(foundation::Array<Record>& inoutTable, usize count, IdentityRecordError& outError) noexcept
    {
        if (count > (Budget - Live) / sizeof(Record))
        {
            return FailManifest(outError,
                                IdentityRecordStatus::Limit,
                                "decode budget exceeded; reduce manifest size",
                                {},
                                "documents");
        }
        if (inoutTable.TryEnsureCapacity(count) == false)
        {
            return FailManifest(outError,
                                IdentityRecordStatus::OutOfMemory,
                                "cannot allocate manifest table",
                                {},
                                "documents");
        }
        Live += inoutTable.GetCapacity() * sizeof(Record);
        if (Live > Peak)
        {
            Peak = Live;
        }
        return IdentityRecordStatus::Success;
    }
};

[[nodiscard]] IdentityRecordStatus CheckUniqueIdentities(const foundation::Array<MigrationDocument>& documents,
                                                         const foundation::Array<MigrationObject>& objects,
                                                         ManifestDecodeMemory& inoutMemory,
                                                         IdentityRecordError& outError) noexcept
{
    foundation::Array<ManifestIdentity> values;
    const IdentityRecordStatus reserved =
        inoutMemory.Reserve(values, documents.GetSize() + objects.GetSize(), outError);
    if (reserved != IdentityRecordStatus::Success)
    {
        return reserved;
    }
    for (usize i = 0; i < documents.GetSize(); ++i)
    {
        const MigrationDocument& document = documents[i];
        [[maybe_unused]] const bool isAdded = values.TryAdd(ManifestIdentity{document.Document.Value, {i, NO_INDEX}});
        LUDUS_ASSERT(isAdded == true);
        for (usize j = 0; j < document.ObjectCount; ++j)
        {
            const MigrationObject& object = objects[document.FirstObject + j];
            [[maybe_unused]] const bool isObjectAdded = values.TryAdd(ManifestIdentity{object.Object.Value, {i, j}});
            LUDUS_ASSERT(isObjectAdded == true);
        }
    }
    foundation::Sort(values.GetData(),
                     values.GetSize(),
                     [](const ManifestIdentity& left, const ManifestIdentity& right) {
                         if (left.Identity != right.Identity)
                         {
                             return IsUuidLess(left.Identity, right.Identity);
                         }
                         if (left.Location.Document != right.Location.Document)
                         {
                             return left.Location.Document < right.Location.Document;
                         }
                         if (left.Location.Object == NO_INDEX || right.Location.Object == NO_INDEX)
                         {
                             return left.Location.Object == NO_INDEX && right.Location.Object != NO_INDEX;
                         }
                         return left.Location.Object < right.Location.Object;
                     });
    for (usize i = 1; i < values.GetSize(); ++i)
    {
        if (values[i].Identity == values[i - 1].Identity)
        {
            const ManifestLocation location = values[i].Location;
            return FailManifest(outError,
                                IdentityRecordStatus::DuplicateIdentity,
                                "UUID repeats an earlier identity in the manifest",
                                location,
                                location.Object == NO_INDEX ? "document" : "object");
        }
    }
    return IdentityRecordStatus::Success;
}

/// Mint one identity that differs from every identity already in `minted`.
[[nodiscard]] IdentityRecordStatus MintIdentity(const foundation::identity::EntropySource& entropy,
                                                const foundation::Array<Uuid>& minted,
                                                Uuid& outValue,
                                                IdentityRecordError& outError) noexcept
{
    for (usize attempt = 0; attempt < MAX_UUID_GENERATION_ATTEMPTS; ++attempt)
    {
        Uuid candidate;
        if (foundation::identity::TryGenerateUuidV4(entropy, candidate) != foundation::identity::UuidStatus::Success)
        {
            return Fail(outError,
                        IdentityRecordStatus::EntropyUnavailable,
                        "platform entropy is unavailable; no identity was minted");
        }
        bool isTaken = false;
        for (const Uuid& existing : minted)
        {
            if (existing == candidate)
            {
                isTaken = true;
                break;
            }
        }
        if (isTaken == false)
        {
            outValue = candidate;
            return IdentityRecordStatus::Success;
        }
    }
    return Fail(outError, IdentityRecordStatus::IdentityConflict, "every generated UUID candidate was already in use");
}

/// Collect the entity IDs of one version-1 World document, sorted.
[[nodiscard]] IdentityRecordStatus ReadLegacyIds(const MigrationSource& source,
                                                 foundation::Array<MigrationObject>& inoutObjects,
                                                 IdentityRecordError& outError) noexcept
{
    if (source.WorldJson.size() > MAX_WORLD_DOCUMENT_BYTES)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "World document is too large", source.Path);
    }
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError parseError;
    if (document.Read(source.WorldJson, parseError, internal::MakeLimits(MAX_WORLD_DOCUMENT_BYTES)) !=
        foundation::parsing::ParseStatus::Ok)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World document is not valid JSON", source.Path);
    }
    const JsonValue root = document.Root();
    uint64 version = 0;
    if (root.Object() == false || root.Get("version").Integer(version) == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "World document has no version", source.Path);
    }
    if (version != 1)
    {
        return Fail(outError,
                    IdentityRecordStatus::UnsupportedVersion,
                    "only version-1 World documents are migrated",
                    source.Path);
    }
    const JsonValue entities = root.Get("entities");
    if (root.Fields({"version", "schema", "entities"}) == false || entities.Array() == false)
    {
        return Fail(outError,
                    IdentityRecordStatus::Unsupported,
                    "World root must have exactly version, schema and entities",
                    source.Path);
    }
    if (entities.Count() > MAX_DOCUMENT_OBJECTS)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "World document has too many entities", source.Path);
    }
    const usize first = inoutObjects.GetSize();
    if (inoutObjects.TryEnsureCapacity(first + entities.Count()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the object table");
    }
    for (usize i = 0; i < entities.Count(); ++i)
    {
        const JsonValue entity = entities.At(i);
        std::string_view legacyId;
        if (entity.Object() == false || entity.Fields({"id", "name", "components"}) == false)
        {
            return Fail(outError,
                        IdentityRecordStatus::Unsupported,
                        "World entity must have exactly id, name and components",
                        source.Path);
        }
        MigrationObject object;
        if (entity.Get("id").String(legacyId) == false || IsLegacyId(legacyId) == false ||
            object.LegacyId.Set(legacyId) == false)
        {
            return Fail(outError, IdentityRecordStatus::InvalidLegacyId, "invalid entity id", source.Path);
        }
        [[maybe_unused]] const bool isAdded = inoutObjects.TryAdd(object);
        LUDUS_ASSERT(isAdded == true);
    }
    MigrationObject* const begin = inoutObjects.GetData() + first;
    const usize count = inoutObjects.GetSize() - first;
    foundation::Sort(begin, count, [](const MigrationObject& left, const MigrationObject& right) noexcept {
        return IsLess(left.LegacyId.View(), right.LegacyId.View());
    });
    for (usize i = 1; i < count; ++i)
    {
        if (begin[i].LegacyId.View() == begin[i - 1].LegacyId.View())
        {
            return Fail(outError,
                        IdentityRecordStatus::InvalidLegacyId,
                        "entity id repeated in the document",
                        begin[i].LegacyId.View());
        }
    }
    return IdentityRecordStatus::Success;
}
} // namespace

/// Private assembly access for the reader and generator.
struct ManifestBuilder final
{
    static void Assign(MigrationManifest& outManifest,
                       foundation::Array<MigrationDocument>&& documents,
                       foundation::Array<MigrationObject>&& objects) noexcept
    {
        outManifest.mDocuments = Move(documents);
        outManifest.mObjects = Move(objects);
    }
};

std::string_view GetIdentityRecordStatusName(IdentityRecordStatus status) noexcept
{
    switch (status)
    {
        case IdentityRecordStatus::Success:
            return "success";
        case IdentityRecordStatus::Malformed:
            return "malformed";
        case IdentityRecordStatus::UnsupportedVersion:
            return "unsupported-version";
        case IdentityRecordStatus::Unsupported:
            return "unsupported";
        case IdentityRecordStatus::InvalidIdentity:
            return "invalid-identity";
        case IdentityRecordStatus::DuplicateIdentity:
            return "duplicate-identity";
        case IdentityRecordStatus::InvalidLegacyId:
            return "invalid-legacy-id";
        case IdentityRecordStatus::UnknownDocument:
            return "unknown-document";
        case IdentityRecordStatus::StaleManifest:
            return "stale-manifest";
        case IdentityRecordStatus::UnresolvedReference:
            return "unresolved-reference";
        case IdentityRecordStatus::Limit:
            return "limit";
        case IdentityRecordStatus::OutOfMemory:
            return "out-of-memory";
        case IdentityRecordStatus::EntropyUnavailable:
            return "entropy-unavailable";
        case IdentityRecordStatus::IdentityConflict:
            return "identity-conflict";
        case IdentityRecordStatus::AlreadyExists:
            return "already-exists";
        case IdentityRecordStatus::IoError:
            return "io-error";
    }
    return "unknown";
}

bool IsLegacyId(std::string_view text) noexcept
{
    if (text.empty() == true || text.size() > MAX_LEGACY_ID_BYTES)
    {
        return false;
    }
    for (const char item : text)
    {
        const bool isLetter = (item >= 'a' && item <= 'z') || (item >= 'A' && item <= 'Z');
        const bool isDigit = item >= '0' && item <= '9';
        if (isLetter == false && isDigit == false && item != '_' && item != '-' && item != '.' && item != ':')
        {
            return false;
        }
    }
    return true;
}

std::span<const MigrationDocument> MigrationManifest::GetDocuments() const noexcept
{
    return {mDocuments.GetData(), mDocuments.GetSize()};
}

std::span<const MigrationObject> MigrationManifest::GetObjects(const MigrationDocument& document) const noexcept
{
    LUDUS_ASSERT(document.FirstObject + document.ObjectCount <= mObjects.GetSize());
    return {mObjects.GetData() + document.FirstObject, document.ObjectCount};
}

const MigrationDocument* MigrationManifest::FindDocument(std::string_view path) const noexcept
{
    for (const MigrationDocument& document : mDocuments)
    {
        if (document.Path.View() == path)
        {
            return &document;
        }
    }
    return nullptr;
}

const MigrationObject* MigrationManifest::FindObject(const MigrationDocument& document,
                                                     std::string_view legacyId) const noexcept
{
    // Objects are sorted by legacy ID: binary search.
    const std::span<const MigrationObject> objects = GetObjects(document);
    usize low = 0;
    usize high = objects.size();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        const std::string_view candidate = objects[middle].LegacyId.View();
        if (candidate == legacyId)
        {
            return &objects[middle];
        }
        if (IsLess(candidate, legacyId) == true)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return nullptr;
}

IdentityRecordStatus MigrationManifest::Read(std::string_view json, IdentityRecordError& outError) noexcept
{
    usize peakBytes = 0;
    return Read(json, outError, peakBytes);
}

IdentityRecordStatus MigrationManifest::Read(std::string_view json,
                                             IdentityRecordError& outError,
                                             usize& outPeakBytes,
                                             usize maxDecodeBytes) noexcept
{
    outError = {};
    outPeakBytes = 0;
    const usize budget = maxDecodeBytes < MAX_MANIFEST_DECODE_BYTES ? maxDecodeBytes : MAX_MANIFEST_DECODE_BYTES;
    foundation::parsing::JsonLimits limits = internal::MakeLimits(MAX_MANIFEST_BYTES);
    limits.MaxWorkspaceBytes = budget;
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError parseError;
    const foundation::parsing::ParseStatus parsed = document.Read(json, parseError, limits);
    const usize workspaceBytes = document.WorkspaceBytes();
    outPeakBytes = workspaceBytes;
    if (parsed != foundation::parsing::ParseStatus::Ok)
    {
        return FailManifestParse(outError, parseError);
    }

    const JsonValue root = document.Root();
    if (root.Object() == false)
    {
        return FailManifest(outError, IdentityRecordStatus::Malformed, "expected a manifest object");
    }
    std::string_view format;
    if (root.Get("format").String(format) == false)
    {
        return FailManifest(outError,
                            IdentityRecordStatus::Malformed,
                            "expected the manifest format string",
                            {},
                            "format");
    }
    uint64 version = 0;
    if (root.Get("version").Integer(version) == false)
    {
        return FailManifest(outError,
                            IdentityRecordStatus::Malformed,
                            "expected the manifest version integer",
                            {},
                            "version");
    }
    const JsonValue documents = root.Get("documents");
    if (documents.Array() == false)
    {
        return FailManifest(outError, IdentityRecordStatus::Malformed, "expected a documents array", {}, "documents");
    }
    if (root.Fields({"format", "version", "documents"}) == false)
    {
        return FailManifest(outError, IdentityRecordStatus::Malformed, "unexpected manifest member");
    }
    if (format != MIGRATION_MANIFEST_FORMAT)
    {
        return FailManifest(outError,
                            IdentityRecordStatus::UnsupportedVersion,
                            "unsupported manifest format",
                            {},
                            "format");
    }
    if (version != MIGRATION_MANIFEST_VERSION)
    {
        return FailManifest(outError,
                            IdentityRecordStatus::UnsupportedVersion,
                            "unsupported manifest version",
                            {},
                            "version");
    }
    if (documents.Count() > MAX_MANIFEST_DOCUMENTS)
    {
        return FailManifest(outError, IdentityRecordStatus::Limit, "manifest has too many documents", {}, "documents");
    }

    // Count before reserving so every table has exactly one allocation and no growth overlap.
    usize objectCount = 0;
    for (usize i = 0; i < documents.Count(); ++i)
    {
        const JsonValue entry = documents.At(i);
        if (entry.Object() == false)
        {
            return FailManifest(outError, IdentityRecordStatus::Malformed, "expected a document object", {i});
        }
        const JsonValue objects = entry.Get("objects");
        if (objects.Array() == false)
        {
            return FailManifest(outError, IdentityRecordStatus::Malformed, "expected an objects array", {i}, "objects");
        }
        if (objects.Count() > MAX_DOCUMENT_OBJECTS)
        {
            return FailManifest(outError, IdentityRecordStatus::Limit, "document has too many objects", {i}, "objects");
        }
        objectCount += objects.Count();
    }
    foundation::Array<MigrationDocument> candidateDocuments;
    foundation::Array<MigrationObject> candidateObjects;
    ManifestDecodeMemory memory{budget, workspaceBytes, outPeakBytes};
    IdentityRecordStatus reserved = memory.Reserve(candidateDocuments, documents.Count(), outError);
    if (reserved != IdentityRecordStatus::Success)
    {
        return reserved;
    }
    reserved = memory.Reserve(candidateObjects, objectCount, outError);
    if (reserved != IdentityRecordStatus::Success)
    {
        return reserved;
    }

    for (usize i = 0; i < documents.Count(); ++i)
    {
        const JsonValue entry = documents.At(i);
        const JsonValue objects = entry.Get("objects");
        std::string_view path;
        MigrationDocument record;
        if (entry.Get("path").String(path) == false)
        {
            return FailManifest(outError,
                                IdentityRecordStatus::Malformed,
                                "expected a document path string",
                                {i},
                                "path");
        }
        if (IsValidDocumentPath(path) == false || record.Path.Set(path) == false)
        {
            return FailManifest(outError, IdentityRecordStatus::InvalidLegacyId, "invalid document path", {i}, "path");
        }
        if (i > 0 && IsLess(candidateDocuments[i - 1].Path.View(), path) == false)
        {
            return FailManifest(outError,
                                IdentityRecordStatus::InvalidLegacyId,
                                "documents must be sorted by unique path",
                                {i},
                                "path");
        }
        const JsonValue documentIdentity = entry.Get("document");
        if (documentIdentity.Valid() == false)
        {
            return FailManifest(outError, IdentityRecordStatus::Malformed, "missing document UUID", {i}, "document");
        }
        if (entry.Fields({"path", "document", "objects"}) == false)
        {
            return FailManifest(outError, IdentityRecordStatus::Malformed, "unexpected document member", {i});
        }
        if (internal::TryReadIdentity(documentIdentity, record.Document.Value) == false)
        {
            return FailManifest(outError,
                                IdentityRecordStatus::InvalidIdentity,
                                "expected a nonnil version-4 UUID",
                                {i},
                                "document");
        }
        record.FirstObject = candidateObjects.GetSize();
        record.ObjectCount = objects.Count();
        for (usize j = 0; j < objects.Count(); ++j)
        {
            const JsonValue item = objects.At(j);
            std::string_view legacyId;
            MigrationObject object;
            if (item.Object() == false)
            {
                return FailManifest(outError, IdentityRecordStatus::Malformed, "expected an object entry", {i, j});
            }
            if (item.Get("legacy_id").String(legacyId) == false)
            {
                return FailManifest(outError,
                                    IdentityRecordStatus::Malformed,
                                    "expected a legacy ID string",
                                    {i, j},
                                    "legacy_id");
            }
            if (IsLegacyId(legacyId) == false || object.LegacyId.Set(legacyId) == false ||
                (j > 0 && IsLess(candidateObjects[record.FirstObject + j - 1].LegacyId.View(), legacyId) == false))
            {
                return FailManifest(outError,
                                    IdentityRecordStatus::InvalidLegacyId,
                                    "legacy IDs must be valid, unique and sorted",
                                    {i, j},
                                    "legacy_id");
            }
            const JsonValue objectIdentity = item.Get("object");
            if (objectIdentity.Valid() == false)
            {
                return FailManifest(outError, IdentityRecordStatus::Malformed, "missing object UUID", {i, j}, "object");
            }
            if (item.Fields({"legacy_id", "object"}) == false)
            {
                return FailManifest(outError, IdentityRecordStatus::Malformed, "unexpected object member", {i, j});
            }
            if (internal::TryReadIdentity(objectIdentity, object.Object.Value) == false)
            {
                return FailManifest(outError,
                                    IdentityRecordStatus::InvalidIdentity,
                                    "expected a nonnil version-4 UUID",
                                    {i, j},
                                    "object");
            }
            [[maybe_unused]] const bool isAdded = candidateObjects.TryAdd(object);
            LUDUS_ASSERT(isAdded == true);
        }
        [[maybe_unused]] const bool isAdded = candidateDocuments.TryAdd(record);
        LUDUS_ASSERT(isAdded == true);
    }

    document.Release();
    memory.Live -= workspaceBytes;
    const IdentityRecordStatus unique = CheckUniqueIdentities(candidateDocuments, candidateObjects, memory, outError);
    if (unique != IdentityRecordStatus::Success)
    {
        return unique;
    }
    ManifestBuilder::Assign(*this, Move(candidateDocuments), Move(candidateObjects));
    return IdentityRecordStatus::Success;
}

IdentityRecordStatus MigrationManifest::Write(Bytes& outJson) const noexcept
{
    const usize capacity =
        ENTRY_OVERHEAD_BYTES +
        mDocuments.GetSize() * (MAX_RESOURCE_PATH_BYTES * ESCAPED_BYTES_PER_BYTE + ENTRY_OVERHEAD_BYTES) +
        mObjects.GetSize() * (MAX_LEGACY_ID_BYTES + ENTRY_OVERHEAD_BYTES);
    internal::IndentedWriter writer(capacity);
    writer.Open('{');
    writer.BeginMember("format");
    writer.WriteString(MIGRATION_MANIFEST_FORMAT);
    writer.BeginMember("version");
    writer.WriteInteger(MIGRATION_MANIFEST_VERSION);
    writer.BeginMember("documents");
    writer.Open('[');
    for (const MigrationDocument& document : mDocuments)
    {
        writer.BeginElement();
        writer.Open('{');
        writer.BeginMember("path");
        writer.WriteString(document.Path.View());
        writer.BeginMember("document");
        writer.WriteUuid(document.Document.Value);
        writer.BeginMember("objects");
        writer.Open('[');
        for (const MigrationObject& object : GetObjects(document))
        {
            writer.BeginElement();
            writer.Open('{');
            writer.BeginMember("legacy_id");
            writer.WriteString(object.LegacyId.View());
            writer.BeginMember("object");
            writer.WriteUuid(object.Object.Value);
            writer.Close('}');
        }
        writer.Close(']');
        writer.Close('}');
    }
    writer.Close(']');
    writer.Close('}');
    return writer.Finish(outJson);
}

IdentityRecordStatus GenerateMigrationManifest(std::span<const MigrationSource> sources,
                                               const foundation::identity::EntropySource& entropy,
                                               MigrationManifest& outManifest,
                                               IdentityRecordError& outError) noexcept
{
    outError = {};
    if (sources.size() > MAX_MANIFEST_DOCUMENTS)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "too many documents for one manifest");
    }
    foundation::Array<usize> order;
    if (order.TryEnsureCapacity(sources.size()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the document order");
    }
    for (usize i = 0; i < sources.size(); ++i)
    {
        if (IsValidDocumentPath(sources[i].Path) == false)
        {
            return Fail(outError, IdentityRecordStatus::InvalidLegacyId, "invalid document path", sources[i].Path);
        }
        [[maybe_unused]] const bool isAdded = order.TryAdd(i);
        LUDUS_ASSERT(isAdded == true);
    }
    foundation::Sort(order.GetData(), order.GetSize(), [sources](usize left, usize right) noexcept {
        return IsLess(sources[left].Path, sources[right].Path);
    });
    for (usize i = 1; i < order.GetSize(); ++i)
    {
        if (sources[order[i]].Path == sources[order[i - 1]].Path)
        {
            return Fail(outError,
                        IdentityRecordStatus::InvalidLegacyId,
                        "document path repeated",
                        sources[order[i]].Path);
        }
    }

    foundation::Array<MigrationDocument> documents;
    foundation::Array<MigrationObject> objects;
    if (documents.TryEnsureCapacity(sources.size()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the document table");
    }
    for (const usize index : order)
    {
        MigrationDocument document;
        [[maybe_unused]] const bool isPathSet = document.Path.Set(sources[index].Path);
        LUDUS_ASSERT(isPathSet == true);
        document.FirstObject = objects.GetSize();
        const IdentityRecordStatus status = ReadLegacyIds(sources[index], objects, outError);
        if (status != IdentityRecordStatus::Success)
        {
            return status;
        }
        document.ObjectCount = objects.GetSize() - document.FirstObject;
        [[maybe_unused]] const bool isAdded = documents.TryAdd(document);
        LUDUS_ASSERT(isAdded == true);
    }

    // Mint every identity before anything is published: document first, then its objects in legacy-ID order.
    foundation::Array<Uuid> minted;
    if (minted.TryEnsureCapacity(documents.GetSize() + objects.GetSize()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the identity index");
    }
    for (MigrationDocument& document : documents)
    {
        Uuid value;
        IdentityRecordStatus status = MintIdentity(entropy, minted, value, outError);
        if (status != IdentityRecordStatus::Success)
        {
            return status;
        }
        document.Document = AssetId{value};
        [[maybe_unused]] bool isAdded = minted.TryAdd(value);
        LUDUS_ASSERT(isAdded == true);
        for (usize i = 0; i < document.ObjectCount; ++i)
        {
            status = MintIdentity(entropy, minted, value, outError);
            if (status != IdentityRecordStatus::Success)
            {
                return status;
            }
            objects[document.FirstObject + i].Object = SourceObjectId{value};
            isAdded = minted.TryAdd(value);
            LUDUS_ASSERT(isAdded == true);
        }
    }
    ManifestBuilder::Assign(outManifest, Move(documents), Move(objects));
    return IdentityRecordStatus::Success;
}

IdentityRecordStatus PublishMigrationManifest(std::string_view root,
                                              std::string_view path,
                                              const MigrationManifest& manifest,
                                              IdentityRecordError& outError) noexcept
{
    outError = {};
    Bytes json;
    const IdentityRecordStatus written = manifest.Write(json);
    if (written != IdentityRecordStatus::Success)
    {
        return Fail(outError, written, "cannot encode the manifest");
    }
    // A null expected digest requires the destination to be absent at publication.
    switch (SaveFile(root, path, json.Data(), nullptr))
    {
        case Status::Ok:
            return IdentityRecordStatus::Success;
        case Status::Conflict:
            return Fail(outError,
                        IdentityRecordStatus::AlreadyExists,
                        "manifest already exists or another writer holds the directory lock; it was not replaced",
                        path);
        case Status::Limit:
            return Fail(outError, IdentityRecordStatus::Limit, "manifest is too large to publish", path);
        case Status::OutOfMemory:
            return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate while publishing", path);
        case Status::Unsupported:
            return Fail(outError,
                        IdentityRecordStatus::IoError,
                        "atomic publication is unsupported on this platform",
                        path);
        default:
            return Fail(outError, IdentityRecordStatus::IoError, "cannot publish the manifest", path);
    }
}
} // namespace ludus::content::identity_records
