#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>

#if defined(LUDUS_FILESYSTEM_NAMESPACE_FAULT_TESTING)
#    include "internal/namespace_test_hooks.hpp"
#endif

#include <atomic>
#include <cstring>
#include <new>
#include <span>
#include <string_view>

// Thanks to Noel Llopis and Charles Nicholson, "Stay in the Game: Asset
// Hotloading for Fast Iteration", Game Programming Gems 6, sec. 1.10,
// pp. 109-114: prepare/publish/retire inspires immutable mount publication and
// retained readers here. Independently implemented; Content still owns resource
// rebinding. See docs/architecture/filesystem.md, "Contracts for F2".

namespace ludus::foundation::filesystem
{
namespace
{
template <typename T, typename... Args>
T* AllocateObject(Args&&... args) noexcept
{
#if defined(LUDUS_FILESYSTEM_NAMESPACE_FAULT_TESTING)
    if (test::NamespaceAllocationAllowed() == false)
    {
        return nullptr;
    }
#endif
    return new (std::nothrow) T(Forward<Args>(args)...);
}
template <typename T>
T* AllocateArray(usize count) noexcept
{
#if defined(LUDUS_FILESYSTEM_NAMESPACE_FAULT_TESTING)
    if (test::NamespaceAllocationAllowed() == false)
    {
        return nullptr;
    }
#endif
    return new (std::nothrow) T[count];
}
bool ValidRoot(Root root) noexcept
{
    return root == Root::Assets || root == Root::Project || root == Root::Cache || root == Root::User;
}
bool ValidAllocation(usize size) noexcept
{
    isize admitted = 0;
    return TryIntegerCast(size, admitted);
}
} // namespace
Result VirtualPath::Set(Root root, std::string_view relativePath) noexcept
{
    if (ValidRoot(root) == false || ValidPath(relativePath) == false)
    {
        return {Status::InvalidArgument};
    }
    // Set may borrow its own bytes, including an overlapping subpath.
    std::memmove(mBytes, relativePath.data(), relativePath.size());
    mLength = relativePath.size();
    mRoot = root;
    return {};
}
bool VirtualPath::operator==(const VirtualPath& other) const noexcept
{
    return mRoot == other.mRoot && Path() == other.Path();
}
bool VirtualPath::IsValid() const noexcept
{
    return mLength != 0;
}
Root VirtualPath::GetRoot() const noexcept
{
    return mRoot;
}
std::string_view VirtualPath::Path() const noexcept
{
    return {mBytes, mLength};
}
ProviderHandle::ProviderHandle(Provider* owned) noexcept : mProvider(owned) {}
ProviderHandle::~ProviderHandle() noexcept
{
    if (mProvider != nullptr && mProvider->mReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        delete mProvider;
    }
}
ProviderHandle::ProviderHandle(const ProviderHandle& other) noexcept : mProvider(other.mProvider)
{
    if (mProvider != nullptr)
    {
        mProvider->mReferences.fetch_add(1, std::memory_order_relaxed);
    }
}
ProviderHandle& ProviderHandle::operator=(const ProviderHandle& other) noexcept
{
    ProviderHandle next(other);
    std::swap(mProvider, next.mProvider);
    return *this;
}
ProviderHandle::ProviderHandle(ProviderHandle&& other) noexcept : mProvider(other.mProvider)
{
    other.mProvider = nullptr;
}
ProviderHandle& ProviderHandle::operator=(ProviderHandle&& other) noexcept
{
    if (this != &other)
    {
        ProviderHandle next(Move(other));
        std::swap(mProvider, next.mProvider);
    }
    return *this;
}
ProviderHandle ProviderHandle::Share(const Provider& provider) noexcept
{
    ProviderHandle next;
    next.mProvider = &provider;
    provider.mReferences.fetch_add(1, std::memory_order_relaxed);
    return next;
}
const Provider* ProviderHandle::Get() const noexcept
{
    return mProvider;
}
namespace
{
struct StoredEntry final
{
    char* Path = nullptr;
    usize PathSize = 0;
    uint8* Bytes = nullptr;
    usize Length = 0;
    ~StoredEntry() noexcept
    {
        delete[] Path;
        delete[] Bytes;
    }
    [[nodiscard]] std::string_view Key() const noexcept
    {
        return {Path, PathSize};
    }
};
class MemoryFile final : public ProviderFile
{
public:
    MemoryFile(const Provider& source, const StoredEntry& entry) noexcept
        : mSource(ProviderHandle::Share(source)), mEntry(entry)
    {
    }
    [[nodiscard]] uint64 Size() const noexcept override
    {
        return static_cast<uint64>(mEntry.Length);
    }
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> destination) const noexcept override
    {
        usize begin = 0;
        if (TryIntegerCast(offset, begin) == false || begin > mEntry.Length)
        {
            return {{Status::InvalidArgument}};
        }
        const usize count = (destination.size() < mEntry.Length - begin ? destination.size() : mEntry.Length - begin);
        if (count != 0)
        {
            std::memcpy(destination.data(), mEntry.Bytes + begin, count);
        }
        return {{}, count};
    }

private:
    ProviderHandle mSource;
    const StoredEntry& mEntry;
};
class MemoryProvider final : public Provider
{
public:
    ~MemoryProvider() noexcept override
    {
        delete[] Entries;
        delete[] Index;
    }
    StoredEntry* Entries = nullptr;
    const StoredEntry** Index = nullptr;
    usize Count = 0;
    [[nodiscard]] Result OpenRead(std::string_view path, ProviderFile*& output) const noexcept override
    {
        if (ValidPath(path) == false)
        {
            return {Status::InvalidArgument};
        }
        usize begin = 0, end = Count;
        while (begin < end)
        {
            const usize middle = begin + (end - begin) / 2;
            if (Index[middle]->Key() < path)
            {
                begin = middle + 1;
            }
            else
            {
                end = middle;
            }
        }
        if (begin == Count || Index[begin]->Key() != path)
        {
            return {Status::NotFound};
        }
        auto* next = AllocateObject<MemoryFile>(*this, *Index[begin]);
        if (next == nullptr)
        {
            return {Status::OutOfMemory};
        }
        output = next;
        return {};
    }
};
class NativeFile final : public ProviderFile
{
public:
    File Revision;
    [[nodiscard]] uint64 Size() const noexcept override
    {
        return Revision.Size();
    }
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> destination) const noexcept override
    {
        return Revision.ReadAt(offset, destination);
    }
};
class DirectoryProvider final : public Provider
{
public:
    Directory NativeRoot;
    [[nodiscard]] Result OpenRead(std::string_view path, ProviderFile*& output) const noexcept override
    {
        auto* next = AllocateObject<NativeFile>();
        if (next == nullptr)
        {
            return {Status::OutOfMemory};
        }
        const auto result = NativeRoot.OpenRead(path, next->Revision);
        if (result.Succeeded() == false)
        {
            delete next;
            return result;
        }
        output = next;
        return {};
    }
};
} // namespace
Result CreateMemoryProvider(std::span<const MemoryEntry> entries, ProviderHandle& output) noexcept
{
    if (entries.size() > MAX_MEMORY_FILES)
    {
        return {Status::InvalidArgument};
    }
    for (const auto& entry : entries)
    {
        if (ValidPath(entry.Path) == false || ValidAllocation(entry.Bytes.size()) == false)
        {
            return {Status::InvalidArgument};
        }
    }
    auto* next = AllocateObject<MemoryProvider>();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    ProviderHandle owner(next);
    if (entries.empty() == false)
    {
        next->Entries = AllocateArray<StoredEntry>(entries.size());
        next->Index = AllocateArray<const StoredEntry*>(entries.size());
        if (next->Entries == nullptr || next->Index == nullptr)
        {
            return {Status::OutOfMemory};
        }
    }
    next->Count = entries.size();
    for (usize i = 0; i < entries.size(); ++i)
    {
        auto& stored = next->Entries[i];
        const auto& entry = entries[i];
        stored.Path = AllocateArray<char>(entry.Path.size());
        stored.PathSize = entry.Path.size();
        stored.Length = entry.Bytes.size();
        if (stored.Length != 0)
        {
            stored.Bytes = AllocateArray<uint8>(stored.Length);
        }
        if (stored.Path == nullptr || (stored.Length != 0 && stored.Bytes == nullptr))
        {
            return {Status::OutOfMemory};
        }
        std::memcpy(stored.Path, entry.Path.data(), stored.PathSize);
        if (stored.Length != 0)
        {
            std::memcpy(stored.Bytes, entry.Bytes.data(), stored.Length);
        }
        next->Index[i] = &stored;
    }
    if (next->Count != 0)
    {
        foundation::Sort(next->Index, next->Count, [](const StoredEntry* a, const StoredEntry* b) noexcept {
            return a->Key() < b->Key();
        });
        for (usize i = 1; i < next->Count; ++i)
        {
            if (next->Index[i - 1]->Key() == next->Index[i]->Key())
            {
                return {Status::InvalidArgument};
            }
        }
    }
    output = Move(owner);
    return {};
}
Result CreateDirectoryProvider(std::string_view nativeRoot, ProviderHandle& output) noexcept
{
    auto* next = AllocateObject<DirectoryProvider>();
    if (next == nullptr)
    {
        return {Status::OutOfMemory};
    }
    ProviderHandle owner(next);
    const auto result = next->NativeRoot.Open(nativeRoot);
    if (result.Succeeded())
    {
        output = Move(owner);
    }
    return result;
}

namespace
{
struct StoredMount final
{
    Root Namespace = Root::Assets;
    char Prefix[MAX_PATH_BYTES]{};
    usize PrefixSize = 0;
    int32 Priority = 0;
    uint64 Id = 0;
    ProviderHandle Source;
    [[nodiscard]] std::string_view Key() const noexcept
    {
        return {Prefix, PrefixSize};
    }
};
} // namespace
struct MountSnapshot::Impl final
{
    std::atomic<uint32> References{1};
    uint64 Generation = 0;
    usize Count = 0;
    StoredMount* Mounts = nullptr;
    ~Impl() noexcept
    {
        delete[] Mounts;
    }
};
MountSnapshot::~MountSnapshot() noexcept
{
    if (mImpl != nullptr && mImpl->References.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        delete mImpl;
    }
}
MountSnapshot::MountSnapshot(const MountSnapshot& other) noexcept : mImpl(other.mImpl)
{
    if (mImpl != nullptr)
    {
        mImpl->References.fetch_add(1, std::memory_order_relaxed);
    }
}
MountSnapshot& MountSnapshot::operator=(const MountSnapshot& other) noexcept
{
    MountSnapshot next(other);
    std::swap(mImpl, next.mImpl);
    return *this;
}
MountSnapshot::MountSnapshot(MountSnapshot&& other) noexcept : mImpl(other.mImpl)
{
    other.mImpl = nullptr;
}
MountSnapshot& MountSnapshot::operator=(MountSnapshot&& other) noexcept
{
    if (this != &other)
    {
        MountSnapshot next(Move(other));
        std::swap(mImpl, next.mImpl);
    }
    return *this;
}
Result MountSnapshot::Create(std::span<const Mount> mounts, uint64 generation, MountSnapshot& output) noexcept
{
    if (generation == 0 || mounts.size() > MAX_MOUNTS)
    {
        return {Status::InvalidArgument};
    }
    for (usize i = 0; i < mounts.size(); ++i)
    {
        const auto& mount = mounts[i];
        if (ValidRoot(mount.Namespace) == false ||
            (mount.Prefix.empty() == false && ValidPath(mount.Prefix) == false) || mount.Id == 0 ||
            mount.Source.Get() == nullptr)
        {
            return {Status::InvalidArgument};
        }
        for (usize j = 0; j < i; ++j)
        {
            if (mount.Id == mounts[j].Id)
            {
                return {Status::InvalidArgument};
            }
        }
    }
    MountSnapshot next;
    next.mImpl = AllocateObject<Impl>();
    if (next.mImpl == nullptr)
    {
        return {Status::OutOfMemory};
    }
    next.mImpl->Generation = generation;
    next.mImpl->Count = mounts.size();
    if (mounts.empty() == false)
    {
        next.mImpl->Mounts = AllocateArray<StoredMount>(mounts.size());
        if (next.mImpl->Mounts == nullptr)
        {
            return {Status::OutOfMemory};
        }
        for (usize i = 0; i < mounts.size(); ++i)
        {
            auto& stored = next.mImpl->Mounts[i];
            const auto& input = mounts[i];
            stored.Namespace = input.Namespace;
            stored.PrefixSize = input.Prefix.size();
            if (stored.PrefixSize != 0)
            {
                std::memcpy(stored.Prefix, input.Prefix.data(), stored.PrefixSize);
            }
            stored.Priority = input.Priority;
            stored.Id = input.Id;
            stored.Source = input.Source;
        }
        foundation::Sort(next.mImpl->Mounts, mounts.size(), [](const StoredMount& a, const StoredMount& b) noexcept {
            return a.Priority != b.Priority ? a.Priority > b.Priority : a.Id < b.Id;
        });
    }
    output = Move(next);
    return {};
}
bool MountSnapshot::IsValid() const noexcept
{
    return mImpl != nullptr;
}
uint64 MountSnapshot::Generation() const noexcept
{
    return mImpl != nullptr ? mImpl->Generation : 0;
}
usize MountSnapshot::Count() const noexcept
{
    return mImpl != nullptr ? mImpl->Count : 0;
}
bool MountSnapshot::Describe(usize index, MountInfo& output) const noexcept
{
    if (index >= Count())
    {
        return false;
    }
    const auto& mount = mImpl->Mounts[index];
    output = {mount.Namespace, mount.Key(), mount.Priority, mount.Id};
    return true;
}
struct VirtualFile::Impl final
{
    std::atomic<uint32> References{1};
    ProviderFile* Revision = nullptr;
    MountSnapshot Snapshot;
    uint64 MountId = 0;
    ~Impl() noexcept
    {
        delete Revision;
    }
};
VirtualFile::~VirtualFile() noexcept
{
    Close();
}
VirtualFile::VirtualFile(VirtualFile&& other) noexcept : mImpl(other.mImpl)
{
    other.mImpl = nullptr;
}
VirtualFile& VirtualFile::operator=(VirtualFile&& other) noexcept
{
    if (this != &other)
    {
        Close();
        mImpl = other.mImpl;
        other.mImpl = nullptr;
    }
    return *this;
}
void VirtualFile::Close() noexcept
{
    if (mImpl != nullptr && mImpl->References.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        delete mImpl;
    }
    mImpl = nullptr;
}
bool VirtualFile::IsOpen() const noexcept
{
    return mImpl != nullptr;
}
uint64 VirtualFile::Size() const noexcept
{
    return mImpl != nullptr ? mImpl->Revision->Size() : 0;
}
uint64 VirtualFile::MountId() const noexcept
{
    return mImpl != nullptr ? mImpl->MountId : 0;
}
uint64 VirtualFile::Generation() const noexcept
{
    return mImpl != nullptr ? mImpl->Snapshot.Generation() : 0;
}
Result VirtualFile::Clone(VirtualFile& output) const noexcept
{
    if (mImpl == nullptr)
    {
        return {Status::InvalidArgument};
    }
    VirtualFile next;
    next.mImpl = mImpl;
    mImpl->References.fetch_add(1, std::memory_order_relaxed);
    output = Move(next);
    return {};
}
ReadResult VirtualFile::ReadAt(uint64 offset, std::span<uint8> destination) const noexcept
{
    if (mImpl == nullptr || offset > Size())
    {
        return {{Status::InvalidArgument}};
    }
    return mImpl->Revision->ReadAt(offset, destination);
}
LookupResult
MountSnapshot::OpenRead(const VirtualPath& path, VirtualFile& output, std::span<MountAttempt> attempts) const noexcept
{
    if (mImpl == nullptr || path.IsValid() == false)
    {
        return {{Status::InvalidArgument}};
    }
    usize count = 0;
    for (usize i = 0; i < mImpl->Count; ++i)
    {
        const auto& mount = mImpl->Mounts[i];
        auto key = path.Path();
        if (mount.Namespace != path.GetRoot())
        {
            continue;
        }
        if (mount.PrefixSize != 0)
        {
            if (key.size() <= mount.PrefixSize || key.starts_with(mount.Key()) == false || key[mount.PrefixSize] != '/')
            {
                continue;
            }
            key.remove_prefix(mount.PrefixSize + 1);
        }
        ProviderFile* revision = nullptr;
        auto result = mount.Source.Get()->OpenRead(key, revision);
        // A successful provider must transfer a revision. Treat a broken provider
        // contract as a visible fault rather than permitting silent fallback.
        if (result.Succeeded() && revision == nullptr)
        {
            result = {Status::IoError};
        }
        if (count < attempts.size())
        {
            attempts[count] = {mount.Id, result};
        }
        ++count;
        if (result.Succeeded() == false)
        {
            delete revision;
            if (result.Code == Status::NotFound)
            {
                continue;
            }
            return {result, count};
        }
        VirtualFile next;
        next.mImpl = AllocateObject<VirtualFile::Impl>();
        if (next.mImpl == nullptr)
        {
            delete revision;
            return {{Status::OutOfMemory}, count};
        }
        next.mImpl->Revision = revision;
        next.mImpl->Snapshot = *this;
        next.mImpl->MountId = mount.Id;
        output = Move(next);
        return {{}, count};
    }
    return {{Status::NotFound}, count};
}
} // namespace ludus::foundation::filesystem
