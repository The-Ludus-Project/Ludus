#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/content/limits.h>

#include <span>
#include <string_view>

namespace ludus::foundation
{
class AllocationDomain;
}

namespace ludus::content
{
using namespace ludus::foundation;

enum class Status : uint8
{
    Ok,
    Pending,
    Invalid,
    Limit,
    OutOfMemory,
    IoError,
    Conflict,
    NotFound,
    Unsupported,
    Cancelled,
};

struct Diagnostic final
{
    Status Result = Status::Ok;
    char Field[128]{};
    usize Offset = 0;
};

template <usize N>
struct Text final
{
    char Data[N + 1]{};
    usize Length = 0;
    [[nodiscard]] bool Set(std::string_view value) noexcept
    {
        if (value.size() > N)
        {
            return false;
        }
        for (usize i = 0; i < value.size(); ++i)
        {
            Data[i] = value[i];
        }
        Length = value.size();
        Data[Length] = '\0';
        return true;
    }
    [[nodiscard]] std::string_view View() const noexcept
    {
        return Length <= N ? std::string_view(Data, Length) : std::string_view{};
    }
};
/// Capacity of a ResourceId, excluding the terminator.
inline constexpr usize MAX_RESOURCE_ID_BYTES = 128;
/// Capacity of a ResourcePath, excluding the terminator.
inline constexpr usize MAX_RESOURCE_PATH_BYTES = 1024;
/// Capacity of a MessageText, excluding the terminator.
inline constexpr usize MAX_MESSAGE_BYTES = 255;
using ResourceId = Text<MAX_RESOURCE_ID_BYTES>;
using ResourcePath = Text<MAX_RESOURCE_PATH_BYTES>;
/// Bounded human-readable error or diagnostic message. Reuse this alias rather
/// than spelling a new Text<N> so every message shares one instantiation.
using MessageText = Text<MAX_MESSAGE_BYTES>;

class Bytes final
{
public:
    Bytes() noexcept = default;
    ~Bytes() noexcept;
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    Bytes(Bytes&& other) noexcept;
    Bytes& operator=(Bytes&& other) noexcept;
    [[nodiscard]] bool Resize(usize size) noexcept;
    [[nodiscard]] std::span<uint8> Data() noexcept
    {
        return {mData, mSize};
    }
    [[nodiscard]] std::span<const uint8> Data() const noexcept
    {
        return {mData, mSize};
    }
    [[nodiscard]] std::string_view String() const noexcept
    {
        return {reinterpret_cast<const char*>(mData), mSize};
    }

private:
    uint8* mData = nullptr;
    usize mSize = 0;
};

struct Digest final
{
    uint8 Data[32]{};
    [[nodiscard]] bool operator==(const Digest&) const noexcept = default;
};
class Hasher final
{
public:
    [[nodiscard]] bool Add(std::span<const uint8> data) noexcept;
    [[nodiscard]] Digest Finish() const noexcept;

private:
    uint32 mState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8 mTail[64]{};
    usize mCount = 0;
    uint64 mBytes = 0;
};
[[nodiscard]] Digest Hash(std::span<const uint8> data) noexcept;
[[nodiscard]] bool ValidId(std::string_view value) noexcept;
[[nodiscard]] bool ValidPath(std::string_view value) noexcept;

/// Read a regular file below a trusted native root into owned bytes.
/// @param root Nonempty filesystem root (up to 4096 bytes); a root symlink is allowed.
/// @param path Valid relative UTF-8 path; symlinks below the root are rejected.
/// @param cap Maximum admitted file size in bytes; empty files are valid.
/// @param outBytes Replaced only on success; unchanged on every failure.
/// @return Ok, Invalid, NotFound, Limit, OutOfMemory, IoError, Conflict if the
/// opened revision changes during reading, or Unsupported on other platforms.
/// @note Synchronous Windows, Linux, macOS or Emscripten session-filesystem I/O; call
/// outside render/audio critical paths. Browser reads do not access host files.
[[nodiscard]] Status ReadFile(std::string_view root, std::string_view path, usize cap, Bytes& outBytes) noexcept;
/// Atomically create or replace one content file on Windows, Linux, macOS or browser MEMFS.
/// @param root Trusted existing native or session directory (up to 4096 bytes); a root
/// symlink is allowed. Already-open directory mutation is outside this boundary.
/// @param path Valid relative UTF-8 path; parent directories must already exist.
/// Symlinks below the root and non-regular destination files are rejected.
/// @param data Bytes to publish, at most 32 MiB; empty data creates an empty file.
/// @param expected Borrowed digest of the last-read destination, or nullptr to
/// require absence. Only needed for this call. On Windows, validate the digest
/// against the observed revision, then check its native stamp under the
/// FoundationFilesystem writer lock before staging and before replacement.
/// Linux and macOS check the digest under their native parent-directory lock.
/// @return Ok means atomic pathname publication succeeded. Conflict means a busy
/// lock, absent expected file, or digest/absence mismatch; Invalid, Limit,
/// OutOfMemory and IoError report admission or prepublication failure.
/// Unsupported is returned on other platforms. Failure preserves the destination;
/// owned temporaries are removed where possible, and foreign temporaries retained.
/// @note Synchronous; writers must cooperate with the directory lock for strict
/// compare-and-swap. Uncooperative writers can race the final check and rename.
/// @note On Windows the publisher inherits the directory ACL and requests file sync.
/// On Linux and macOS, uses an exclusive 0600 same-directory temporary named with a
/// .ludus-save suffix (subject to native filename limits). Successful replacement
/// creates a new inode; existing readers retain the old revision. File fsync and
/// close must succeed before publication; directory sync is best-effort afterward.
/// Ok does not promise drive-cache flush or power-loss durability.
/// @note On Emscripten, call only from the session filesystem's single owner
/// thread. The checks and rename do not yield; no native lock is taken. Ok means
/// publication in volatile session memory, not browser persistence or download.
[[nodiscard]] Status
SaveFile(std::string_view root, std::string_view path, std::span<const uint8> data, const Digest* expected) noexcept;

/// @brief A descriptor-bound regular file.
///
/// Atomic replacement leaves an open revision intact; in-place size/timestamp changes fail subsequent reads. Clone
/// creates independent seek state, never an unvalidated pathname reopen.
class FileReader final
{
public:
    FileReader() noexcept = default;
    ~FileReader() noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    [[nodiscard]] Status Open(std::string_view root, std::string_view path) noexcept;
    [[nodiscard]] Status Clone(FileReader& output) const noexcept;
    [[nodiscard]] Status Read(std::span<uint8> bytes, usize& count) noexcept;
    [[nodiscard]] Status Seek(int64 offset, bool relative) noexcept;
    [[nodiscard]] uint64 Size() const noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};
/// Resource category recorded in the authored catalog.
enum class Kind : uint8
{
    AudioSource,    ///< Encoded audio source bytes.
    Sound,          ///< Immutable sound definition referencing audio sources.
    Music,          ///< Immutable music definition; playback state has a separate owner.
    Mesh,           ///< Immutable CPU triangle geometry; renderer installation is separate.
    CookedScene,    ///< Fully resident predecoded scene binding; project assets retain separate identities.
    CookedGeometry, ///< Cooked curves/surfaces geometry (LUGEO; Ludus::GeometryFormat). Catalog schema version 2+.
};
struct Resource final
{
    ResourceId Id;
    ResourcePath Path;
    Kind Type = Kind::AudioSource;
};
/// Move-only owner of catalog entries; Read publishes a replacement transactionally.
class Catalog final
{
public:
    /// Construct an empty catalog using the system allocation domain.
    Catalog() noexcept;
    /// Construct an empty catalog using a caller-supplied allocation domain.
    /// @param domain Backs parser workspace and entry storage. The domain and its callback context
    /// must outlive this catalog and any catalog to which its state is moved.
    explicit Catalog(const AllocationDomain& domain) noexcept;
    /// Release the entries through their original allocation domain.
    ~Catalog() noexcept;
    /// Transfer the entries and allocation domain. The source becomes empty and can reuse its domain.
    Catalog(Catalog&& other) noexcept;
    /// Release the current entries and transfer the source's entries and domain. Self-move preserves the catalog.
    Catalog& operator=(Catalog&& other) noexcept;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    /// Decode a strict catalog and replace the entries only after full validation.
    /// @param json Borrowed UTF-8 JSON bytes, needed only for this call.
    /// @param outDiagnostic Cleared on success. Domain failures carry a NUL-terminated schema path
    /// (for example `resources[3].path`) and `parsing::UNKNOWN_BYTE_OFFSET`. Syntax errors retain
    /// their parser byte offsets when available.
    /// @return Ok, Invalid for malformed input, Limit for an admission failure, or OutOfMemory.
    /// Existing entries and borrows stay valid on failure. Success replaces all entries and
    /// invalidates their borrows. Synchronize access to a catalog externally.
    [[nodiscard]] Status Read(std::string_view json, Diagnostic& outDiagnostic) noexcept;
    /// Write canonical JSON in resource ID order. Failure preserves the output bytes.
    [[nodiscard]] Status Write(Bytes& outBytes) const noexcept;
    /// Insert an owned copy in resource ID order. An identical existing entry succeeds without mutation.
    /// A conflicting ID, invalid entry, resource count limit, or allocation failure preserves the catalog.
    [[nodiscard]] Status Put(const Resource& resource) noexcept;
    /// Borrow the entry with the exact ID, or return nullptr if it is absent.
    /// The borrow has the same lifetime as the view from Entries().
    [[nodiscard]] const Resource* Find(std::string_view id) const noexcept;
    /// Borrow the entries in resource ID order until successful replacement, insertion, or destruction.
    [[nodiscard]] std::span<const Resource> Entries() const noexcept;

private:
    /// @cond INTERNAL
    struct EntryDeleter final
    {
        const AllocationDomain* DomainOrNull;
        usize Count;

        void operator()(Resource* entries) const noexcept;
    };
    using EntryOwner = UniquePtr<Resource, EntryDeleter>;
    [[nodiscard]] EntryOwner allocateEntries(usize count) const noexcept;

    const AllocationDomain* mDomain;
    EntryOwner mEntries;
    /// @endcond
};
} // namespace ludus::content
