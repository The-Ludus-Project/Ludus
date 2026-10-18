#include <ludus/content/content.h>
#include <ludus/content/json.h>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include <new>

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
    if (value.empty() || value.size() > 128 || value.front() == '/' || value.back() == '/')
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
Catalog::Catalog(Catalog&& other) noexcept : mEntries(other.mEntries), mCount(other.mCount)
{
    other.mEntries = nullptr;
    other.mCount = 0;
}
Catalog& Catalog::operator=(Catalog&& other) noexcept
{
    if (this != &other)
    {
        delete[] mEntries;
        mEntries = other.mEntries;
        mCount = other.mCount;
        other.mEntries = nullptr;
        other.mCount = 0;
    }
    return *this;
}
Catalog::~Catalog() noexcept
{
    delete[] mEntries;
}
const Resource* Catalog::Find(std::string_view id) const noexcept
{
    for (usize i = 0; i < mCount; ++i)
    {
        if (mEntries[i].Id.View() == id)
        {
            return &mEntries[i];
        }
    }
    return nullptr;
}
Status Catalog::Read(std::string_view json, Diagnostic& diagnostic) noexcept
{
    JsonDocument document;
    auto status = document.Read(json, diagnostic);
    if (status != Status::Ok)
    {
        return status;
    }
    const auto root = document.Root();
    uint64 version = 0;
    if (root.Fields({"version", "resources"}) == false || root.Get("version").Integer(version) == false || version != 1)
    {
        return diagnostic.Result = Status::Invalid;
    }
    const auto entries = root.Get("resources");
    const usize count = entries.Count();
    if (entries.Array() == false || count > MAX_RESOURCES)
    {
        return diagnostic.Result = Status::Limit;
    }
    auto* next = new (std::nothrow) Resource[count];
    if (next == nullptr)
    {
        return diagnostic.Result = Status::OutOfMemory;
    }
    for (usize i = 0; i < count; ++i)
    {
        const auto item = entries.At(i);
        std::string_view id, path, kind;
        bool valid = item.Fields({"id", "kind", "path"}) && item.Get("id").String(id) && ValidId(id) &&
                     item.Get("path").String(path) && ValidPath(path) && item.Get("kind").String(kind);
        if (valid)
        {
            if (kind == "audio-source")
            {
                next[i].Type = Kind::AudioSource;
            }
            else if (kind == "sound")
            {
                next[i].Type = Kind::Sound;
            }
            else if (kind == "music")
            {
                next[i].Type = Kind::Music;
            }
            else if (kind == "mesh")
            {
                next[i].Type = Kind::Mesh;
            }
            else if (kind == "cooked-scene")
            {
                next[i].Type = Kind::CookedScene;
            }
            else
            {
                valid = false;
            }
        }
        for (usize j = 0; valid && j < i; ++j)
        {
            if (next[j].Id.View() == id)
            {
                valid = false;
            }
        }
        if (valid == false)
        {
            delete[] next;
            return diagnostic.Result = Status::Invalid;
        }
        (void)next[i].Id.Set(id);
        (void)next[i].Path.Set(path);
    }
    // Stable IDs, independent of source array ordering.
    for (usize i = 1; i < count; ++i)
    {
        const auto item = next[i];
        usize pos = i;
        while (pos > 0 && next[pos - 1].Id.View() > item.Id.View())
        {
            next[pos] = next[pos - 1];
            --pos;
        }
        next[pos] = item;
    }
    delete[] mEntries;
    mEntries = next;
    mCount = count;
    return Status::Ok;
}
Status Catalog::Put(const Resource& resource) noexcept
{
    if ((resource.Type != Kind::AudioSource && resource.Type != Kind::Sound && resource.Type != Kind::Music &&
         resource.Type != Kind::Mesh && resource.Type != Kind::CookedScene) ||
        ValidId(resource.Id.View()) == false || ValidPath(resource.Path.View()) == false)
    {
        return Status::Invalid;
    }
    const auto* existing = Find(resource.Id.View());
    if (existing != nullptr)
    {
        return existing->Path.View() == resource.Path.View() && existing->Type == resource.Type ? Status::Ok
                                                                                                : Status::Conflict;
    }
    if (mCount >= MAX_RESOURCES)
    {
        return Status::Limit;
    }
    auto* next = new (std::nothrow) Resource[mCount + 1];
    if (next == nullptr)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < mCount; ++i)
    {
        next[i] = mEntries[i];
    }
    next[mCount] = resource;
    usize pos = mCount;
    while (pos > 0 && next[pos - 1].Id.View() > resource.Id.View())
    {
        next[pos] = next[pos - 1];
        --pos;
    }
    next[pos] = resource;
    delete[] mEntries;
    mEntries = next;
    ++mCount;
    return Status::Ok;
}
Status Catalog::Write(Bytes& output) const noexcept
{
    Bytes buffer;
    if (buffer.Resize(MAX_DOCUMENT_BYTES) == false)
    {
        return Status::OutOfMemory;
    }
    JsonWriter writer(buffer.Data());
    writer.Raw("{\n  \"version\": 1,\n  \"resources\": [");
    for (usize i = 0; i < mCount; ++i)
    {
        const auto& r = mEntries[i];
        writer.Raw(i == 0 ? "\n    { \"id\": " : ",\n    { \"id\": ");
        writer.String(r.Id.View());
        writer.Raw(", \"kind\": ");
        writer.String(r.Type == Kind::Sound         ? "sound"
                      : r.Type == Kind::Music       ? "music"
                      : r.Type == Kind::Mesh        ? "mesh"
                      : r.Type == Kind::CookedScene ? "cooked-scene"
                                                    : "audio-source");
        writer.Raw(", \"path\": ");
        writer.String(r.Path.View());
        writer.Raw(" }");
    }
    writer.Raw("\n  ]\n}");
    return writer.Finish(output);
}
} // namespace ludus::content
