#include <ludus/content/identity_records/migration_manifest.h>

#include "internal/record_json.h"

#include <ludus/foundation/containers/sort.hpp>
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

/// Reject any UUID that appears twice among documents and objects.
[[nodiscard]] IdentityRecordStatus CheckUniqueIdentities(const foundation::Array<MigrationDocument>& documents,
                                                         const foundation::Array<MigrationObject>& objects,
                                                         IdentityRecordError& outError) noexcept
{
    foundation::Array<Uuid> values;
    if (values.TryEnsureCapacity(documents.GetSize() + objects.GetSize()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the identity index");
    }
    for (const MigrationDocument& document : documents)
    {
        // Capacity was reserved above, so these additions cannot fail.
        [[maybe_unused]] const bool isAdded = values.TryAdd(document.Document.Value);
        LUDUS_ASSERT(isAdded == true);
    }
    for (const MigrationObject& object : objects)
    {
        [[maybe_unused]] const bool isAdded = values.TryAdd(object.Object.Value);
        LUDUS_ASSERT(isAdded == true);
    }
    foundation::Sort(values.GetData(), values.GetSize(), IsUuidLess);
    for (usize i = 1; i < values.GetSize(); ++i)
    {
        if (values[i] == values[i - 1])
        {
            char text[foundation::identity::UUID_TEXT_BUFFER_BYTES];
            const bool isFormatted = foundation::identity::FormatUuid(values[i], text, sizeof(text)) ==
                                     foundation::identity::UuidStatus::Success;
            return Fail(outError,
                        IdentityRecordStatus::DuplicateIdentity,
                        "UUID repeated in the manifest",
                        isFormatted == true ? std::string_view(text, foundation::identity::UUID_TEXT_LENGTH)
                                            : std::string_view{});
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
    outError = {};
    if (json.size() > MAX_MANIFEST_BYTES)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "manifest is too large");
    }
    foundation::parsing::JsonDocument document;
    foundation::parsing::ParseError parseError;
    if (document.Read(json, parseError, internal::MakeLimits(MAX_MANIFEST_BYTES)) !=
        foundation::parsing::ParseStatus::Ok)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "manifest is not valid JSON");
    }
    const JsonValue root = document.Root();
    std::string_view format;
    uint64 version = 0;
    if (root.Object() == false || root.Fields({"format", "version", "documents"}) == false ||
        root.Get("format").String(format) == false || root.Get("version").Integer(version) == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "manifest root must have format, version and documents");
    }
    if (format != MIGRATION_MANIFEST_FORMAT || version != MIGRATION_MANIFEST_VERSION)
    {
        return Fail(outError, IdentityRecordStatus::UnsupportedVersion, "unsupported manifest format or version");
    }
    const JsonValue documents = root.Get("documents");
    if (documents.Array() == false)
    {
        return Fail(outError, IdentityRecordStatus::Malformed, "documents must be an array");
    }
    if (documents.Count() > MAX_MANIFEST_DOCUMENTS)
    {
        return Fail(outError, IdentityRecordStatus::Limit, "manifest has too many documents");
    }
    foundation::Array<MigrationDocument> candidateDocuments;
    foundation::Array<MigrationObject> candidateObjects;
    if (candidateDocuments.TryEnsureCapacity(documents.Count()) == false)
    {
        return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the document table");
    }
    for (usize i = 0; i < documents.Count(); ++i)
    {
        const JsonValue entry = documents.At(i);
        const JsonValue objects = entry.Get("objects");
        std::string_view path;
        MigrationDocument record;
        if (entry.Object() == false || entry.Fields({"path", "document", "objects"}) == false ||
            entry.Get("path").String(path) == false || objects.Array() == false)
        {
            return Fail(outError, IdentityRecordStatus::Malformed, "document must have path, document and objects");
        }
        if (IsValidDocumentPath(path) == false || record.Path.Set(path) == false)
        {
            return Fail(outError, IdentityRecordStatus::InvalidLegacyId, "invalid document path", path);
        }
        if (i > 0 && IsLess(candidateDocuments[i - 1].Path.View(), path) == false)
        {
            return Fail(outError,
                        IdentityRecordStatus::InvalidLegacyId,
                        "documents must be sorted by unique path",
                        path);
        }
        if (internal::TryReadIdentity(entry.Get("document"), record.Document.Value) == false)
        {
            return Fail(outError,
                        IdentityRecordStatus::InvalidIdentity,
                        "document UUID must be a nonnil version-4 UUID",
                        path);
        }
        if (objects.Count() > MAX_DOCUMENT_OBJECTS)
        {
            return Fail(outError, IdentityRecordStatus::Limit, "document has too many objects", path);
        }
        record.FirstObject = candidateObjects.GetSize();
        record.ObjectCount = objects.Count();
        if (candidateObjects.TryEnsureCapacity(record.FirstObject + record.ObjectCount) == false)
        {
            return Fail(outError, IdentityRecordStatus::OutOfMemory, "cannot allocate the object table");
        }
        for (usize j = 0; j < objects.Count(); ++j)
        {
            const JsonValue item = objects.At(j);
            std::string_view legacyId;
            MigrationObject object;
            if (item.Object() == false || item.Fields({"legacy_id", "object"}) == false ||
                item.Get("legacy_id").String(legacyId) == false)
            {
                return Fail(outError, IdentityRecordStatus::Malformed, "object must have legacy_id and object", path);
            }
            if (IsLegacyId(legacyId) == false || object.LegacyId.Set(legacyId) == false ||
                (j > 0 && IsLess(candidateObjects[record.FirstObject + j - 1].LegacyId.View(), legacyId) == false))
            {
                return Fail(outError,
                            IdentityRecordStatus::InvalidLegacyId,
                            "legacy IDs must be valid, unique and sorted",
                            legacyId);
            }
            if (internal::TryReadIdentity(item.Get("object"), object.Object.Value) == false)
            {
                return Fail(outError,
                            IdentityRecordStatus::InvalidIdentity,
                            "object UUID must be a nonnil version-4 UUID",
                            legacyId);
            }
            [[maybe_unused]] const bool isAdded = candidateObjects.TryAdd(object);
            LUDUS_ASSERT(isAdded == true);
        }
        [[maybe_unused]] const bool isAdded = candidateDocuments.TryAdd(record);
        LUDUS_ASSERT(isAdded == true);
    }
    const IdentityRecordStatus unique = CheckUniqueIdentities(candidateDocuments, candidateObjects, outError);
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
