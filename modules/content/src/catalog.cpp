#include <ludus/content/content.h>
#include <ludus/content/json.h>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/parsing/parsing.hpp>

#include <new>
#include <span>
#include <string_view>
#include <type_traits>

namespace ludus::content
{
Bytes::~Bytes() noexcept
{
    delete[] mData;
}
Bytes::Bytes(Bytes&& other) noexcept : mData(other.mData), mSize(other.mSize)
{
    other.mData = nullptr;
    other.mSize = 0;
}
Bytes& Bytes::operator=(Bytes&& other) noexcept
{
    if (this != &other)
    {
        delete[] mData;
        mData = other.mData;
        mSize = other.mSize;
        other.mData = nullptr;
        other.mSize = 0;
    }
    return *this;
}
bool Bytes::Resize(usize size) noexcept
{
    auto* next = new (std::nothrow) uint8[size];
    if (next == nullptr)
    {
        return false;
    }
    delete[] mData;
    mData = next;
    mSize = size;
    return true;
}
bool ValidId(std::string_view value) noexcept
{
    if (value.empty() || value.size() > MAX_RESOURCE_ID_BYTES || value.front() == '/' || value.back() == '/')
    {
        return false;
    }
    char previous = '\0';
    for (char ch : value)
    {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-' && ch != '/')
        {
            return false;
        }
        if (ch == '/' && previous == '/')
        {
            return false;
        }
        previous = ch;
    }
    return true;
}
bool ValidPath(std::string_view value) noexcept
{
    return foundation::filesystem::ValidPath(value);
}
namespace
{
constexpr char RESOURCE_ARRAY_FIELD[] = "resources";
constexpr usize DECIMAL_RADIX = 10;
constexpr usize MAX_INDEX_DIGITS = 20; // Maximum decimal width of a uint64.
static_assert(sizeof(usize) <= sizeof(uint64));
static_assert(sizeof(RESOURCE_ARRAY_FIELD) - 1 + MAX_INDEX_DIGITS + sizeof("[].path") <= sizeof(Diagnostic::Field));

char* CopyDiagnosticField(char* destination, std::string_view field) noexcept
{
    for (const char character : field)
    {
        *destination++ = character;
    }
    *destination = '\0';
    return destination;
}

Status FailCatalog(Status result, std::string_view field, Diagnostic& outDiagnostic) noexcept
{
    outDiagnostic.Result = result;
    outDiagnostic.Offset = foundation::parsing::UNKNOWN_BYTE_OFFSET;
    CopyDiagnosticField(outDiagnostic.Field, field);
    return result;
}

Status FailResource(usize index, std::string_view field, Diagnostic& outDiagnostic) noexcept
{
    outDiagnostic.Result = Status::Invalid;
    outDiagnostic.Offset = foundation::parsing::UNKNOWN_BYTE_OFFSET;
    char* destination = CopyDiagnosticField(outDiagnostic.Field, RESOURCE_ARRAY_FIELD);
    *destination++ = '[';
    usize divisor = 1;
    while (index / divisor >= DECIMAL_RADIX)
    {
        divisor *= DECIMAL_RADIX;
    }
    for (; divisor != 0; divisor /= DECIMAL_RADIX)
    {
        *destination++ = static_cast<char>('0' + index / divisor);
        index %= divisor;
    }
    *destination++ = ']';
    if (field.empty() == false)
    {
        *destination++ = '.';
    }
    CopyDiagnosticField(destination, field);
    return Status::Invalid;
}

bool ParseResourceKind(std::string_view name, uint64 version, Kind& outKind) noexcept
{
    if (name == "audio-source")
    {
        outKind = Kind::AudioSource;
    }
    else if (name == "sound")
    {
        outKind = Kind::Sound;
    }
    else if (name == "music")
    {
        outKind = Kind::Music;
    }
    else if (name == "mesh")
    {
        outKind = Kind::Mesh;
    }
    else if (name == "cooked-scene")
    {
        outKind = Kind::CookedScene;
    }
    else if (name == "cooked-geometry" && version >= 2)
    {
        outKind = Kind::CookedGeometry;
    }
    else
    {
        return false;
    }
    return true;
}
} // namespace

Catalog::Catalog() noexcept : Catalog(GetSystemAllocationDomain()) {}
Catalog::Catalog(const AllocationDomain& domain) noexcept : mDomain(&domain) {}
Catalog::Catalog(Catalog&& other) noexcept = default;
Catalog& Catalog::operator=(Catalog&& other) noexcept = default;
Catalog::~Catalog() noexcept = default;

void Catalog::EntryDeleter::operator()(Resource* entries) const noexcept
{
    static_assert(std::is_trivially_destructible_v<Resource>);
    LUDUS_ASSERT(DomainOrNull != nullptr);
    DomainOrNull->Free(entries, Count * sizeof(Resource), alignof(Resource));
}

Catalog::EntryOwner Catalog::allocateEntries(usize count) const noexcept
{
    static_assert(MAX_RESOURCES <= (~usize{0}) / sizeof(Resource));
    LUDUS_ASSERT(count <= MAX_RESOURCES);
    void* storageOrNull = mDomain->TryAllocate(count * sizeof(Resource), alignof(Resource));
    if (storageOrNull == nullptr)
    {
        return {};
    }
    Resource* entries = ::new (storageOrNull) Resource[count]{};
    return EntryOwner(entries, { .DomainOrNull = mDomain, .Count = count });
}

std::span<const Resource> Catalog::Entries() const noexcept
{
    const Resource* entriesOrNull = mEntries.Get();
    return {entriesOrNull, entriesOrNull == nullptr ? 0 : mEntries.GetDeleter().Count};
}

const Resource* Catalog::Find(std::string_view id) const noexcept
{
    for (const Resource& entry : Entries())
    {
        if (entry.Id.View() == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

Status Catalog::Read(std::string_view json, Diagnostic& outDiagnostic) noexcept
{
    JsonDocument document(*mDomain);
    const Status status = document.Read(json, outDiagnostic);
    if (status != Status::Ok)
    {
        return status;
    }

    const JsonValue root = document.Root();
    if (root.Object() == false)
    {
        return FailCatalog(Status::Invalid, "$", outDiagnostic);
    }
    uint64 version = 0;
    if (root.Get("version").Integer(version) == false || (version != 1 && version != 2))
    {
        return FailCatalog(Status::Invalid, "version", outDiagnostic);
    }
    const JsonValue resources = root.Get(RESOURCE_ARRAY_FIELD);
    if (resources.Value == nullptr)
    {
        return FailCatalog(Status::Invalid, RESOURCE_ARRAY_FIELD, outDiagnostic);
    }
    if (root.Fields({"version", RESOURCE_ARRAY_FIELD}) == false)
    {
        return FailCatalog(Status::Invalid, "$", outDiagnostic);
    }
    const usize count = resources.Count();
    if (resources.Array() == false || count > MAX_RESOURCES)
    {
        return FailCatalog(Status::Limit, RESOURCE_ARRAY_FIELD, outDiagnostic);
    }

    EntryOwner candidate = allocateEntries(count);
    Resource* entriesOrNull = candidate.Get();
    if (count != 0 && entriesOrNull == nullptr)
    {
        return FailCatalog(Status::OutOfMemory, RESOURCE_ARRAY_FIELD, outDiagnostic);
    }
    for (usize index = 0; index < count; ++index)
    {
        const JsonValue item = resources.At(index);
        if (item.Object() == false)
        {
            return FailResource(index, {}, outDiagnostic);
        }
        Resource& entry = entriesOrNull[index];
        std::string_view id;
        if (item.Get("id").String(id) == false || ValidId(id) == false || entry.Id.Set(id) == false)
        {
            return FailResource(index, "id", outDiagnostic);
        }
        std::string_view path;
        if (item.Get("path").String(path) == false || ValidPath(path) == false || entry.Path.Set(path) == false)
        {
            return FailResource(index, "path", outDiagnostic);
        }
        std::string_view kind;
        if (item.Get("kind").String(kind) == false || ParseResourceKind(kind, version, entry.Type) == false)
        {
            return FailResource(index, "kind", outDiagnostic);
        }
        if (item.Fields({"id", "kind", "path"}) == false)
        {
            return FailResource(index, {}, outDiagnostic);
        }
        for (usize previous = 0; previous < index; ++previous)
        {
            if (entriesOrNull[previous].Id.View() == id)
            {
                return FailResource(index, "id", outDiagnostic);
            }
        }
    }
    // Stable IDs, independent of source array ordering.
    for (usize index = 1; index < count; ++index)
    {
        const Resource item = entriesOrNull[index];
        usize position = index;
        while (position > 0 && entriesOrNull[position - 1].Id.View() > item.Id.View())
        {
            entriesOrNull[position] = entriesOrNull[position - 1];
            --position;
        }
        entriesOrNull[position] = item;
    }
    mEntries = Move(candidate);
    return Status::Ok;
}

Status Catalog::Put(const Resource& resource) noexcept
{
    if ((resource.Type != Kind::AudioSource && resource.Type != Kind::Sound && resource.Type != Kind::Music &&
         resource.Type != Kind::Mesh && resource.Type != Kind::CookedScene && resource.Type != Kind::CookedGeometry) ||
        ValidId(resource.Id.View()) == false || ValidPath(resource.Path.View()) == false)
    {
        return Status::Invalid;
    }
    const Resource* existingOrNull = Find(resource.Id.View());
    if (existingOrNull != nullptr)
    {
        return existingOrNull->Path.View() == resource.Path.View() && existingOrNull->Type == resource.Type
                   ? Status::Ok
                   : Status::Conflict;
    }

    const std::span<const Resource> entries = Entries();
    if (entries.size() >= MAX_RESOURCES)
    {
        return Status::Limit;
    }
    EntryOwner candidate = allocateEntries(entries.size() + 1);
    Resource* replacementOrNull = candidate.Get();
    if (replacementOrNull == nullptr)
    {
        return Status::OutOfMemory;
    }
    usize position = entries.size();
    while (position > 0 && entries[position - 1].Id.View() > resource.Id.View())
    {
        replacementOrNull[position] = entries[position - 1];
        --position;
    }
    replacementOrNull[position] = resource;
    for (usize index = 0; index < position; ++index)
    {
        replacementOrNull[index] = entries[index];
    }
    mEntries = Move(candidate);
    return Status::Ok;
}
Status Catalog::Write(Bytes& outBytes) const noexcept
{
    Bytes buffer;
    if (buffer.Resize(MAX_DOCUMENT_BYTES) == false)
    {
        return Status::OutOfMemory;
    }
    JsonWriter writer(buffer.Data());
    // Emit schema version 2 only when a cooked-geometry entry is present, so a
    // catalog without geometry keeps round-tripping as version 1.
    bool hasGeometry = false;
    const std::span<const Resource> entries = Entries();
    for (const Resource& entry : entries)
    {
        if (entry.Type == Kind::CookedGeometry)
        {
            hasGeometry = true;
        }
    }
    writer.Raw(hasGeometry == true ? "{\n  \"version\": 2,\n  \"resources\": ["
                                   : "{\n  \"version\": 1,\n  \"resources\": [");
    for (usize index = 0; index < entries.size(); ++index)
    {
        const Resource& entry = entries[index];
        writer.Raw(index == 0 ? "\n    { \"id\": " : ",\n    { \"id\": ");
        writer.String(entry.Id.View());
        writer.Raw(", \"kind\": ");
        writer.String(entry.Type == Kind::Sound            ? "sound"
                      : entry.Type == Kind::Music          ? "music"
                      : entry.Type == Kind::Mesh           ? "mesh"
                      : entry.Type == Kind::CookedScene    ? "cooked-scene"
                      : entry.Type == Kind::CookedGeometry ? "cooked-geometry"
                                                           : "audio-source");
        writer.Raw(", \"path\": ");
        writer.String(entry.Path.View());
        writer.Raw(" }");
    }
    writer.Raw("\n  ]\n}");
    return writer.Finish(outBytes);
}
} // namespace ludus::content
