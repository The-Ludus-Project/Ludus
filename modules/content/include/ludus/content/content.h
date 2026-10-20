#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/limits.h>

#include <span>
#include <string_view>

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
/// @param output Replaced only on success; unchanged on every failure.
/// @return Ok, Invalid, NotFound, Limit, OutOfMemory, IoError, Conflict if the
/// opened revision changes during reading, or Unsupported on other platforms.
/// @note Synchronous Linux/macOS or Emscripten session-filesystem I/O; call
/// outside render/audio critical paths. Browser reads do not access host files.
[[nodiscard]] Status ReadFile(std::string_view root, std::string_view path, usize cap, Bytes& output) noexcept;
/// Atomically create or replace one content file on Linux, macOS or browser MEMFS.
/// @param root Trusted existing native or session directory (up to 4096 bytes); a root
/// symlink is allowed. Already-open directory mutation is outside this boundary.
/// @param path Valid relative UTF-8 path; parent directories must already exist.
/// Symlinks below the root and non-regular destination files are rejected.
/// @param data Bytes to publish, at most 32 MiB; empty data creates an empty file.
/// @param expected Borrowed digest of the last-read destination, or nullptr to
/// require absence. Checked under a nonblocking cooperative parent-directory lock
/// before writing and again before publication. Only needed for this call.
/// @return Ok means atomic pathname publication succeeded. Conflict means a busy
/// lock, absent expected file, or digest/absence mismatch; Invalid, Limit,
/// OutOfMemory and IoError report admission or prepublication failure.
/// Unsupported is returned on other platforms. Failure preserves the destination;
/// owned temporaries are removed where possible, and foreign temporaries retained.
/// @note Synchronous; writers must cooperate with the directory lock for strict
/// compare-and-swap. Uncooperative writers can race the final check and rename.
/// @note Uses an exclusive 0600 same-directory temporary named with a
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
    AudioSource, ///< Encoded audio source bytes.
    Sound,       ///< Immutable sound definition referencing audio sources.
    Music,       ///< Immutable music definition; playback state has a separate owner.
    Mesh,        ///< Immutable CPU triangle geometry; renderer installation is separate.
    CookedScene, ///< Fully resident predecoded scene binding; project assets retain separate identities.
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
    Catalog() noexcept = default;
    ~Catalog() noexcept;
    Catalog(Catalog&& other) noexcept;
    Catalog& operator=(Catalog&& other) noexcept;
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    [[nodiscard]] Status Read(std::string_view json, Diagnostic& diagnostic) noexcept;
    [[nodiscard]] Status Write(Bytes& output) const noexcept;
    [[nodiscard]] Status Put(const Resource& resource) noexcept;
    [[nodiscard]] const Resource* Find(std::string_view id) const noexcept;
    [[nodiscard]] std::span<const Resource> Entries() const noexcept
    {
        return {mEntries, mCount};
    }

private:
    Resource* mEntries = nullptr;
    usize mCount = 0;
};
} // namespace ludus::content
