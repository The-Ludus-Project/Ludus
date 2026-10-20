#pragma once

#include <ludus/foundation/base/core.h>

#include <span>
#include <string_view>

namespace ludus::foundation::filesystem
{
/// Maximum encoded length of a validated relative path, excluding its terminator.
inline constexpr usize MAX_PATH_BYTES = 1024;
/// Maximum encoded length of a trusted native root, excluding its terminator.
inline constexpr usize MAX_ROOT_BYTES = 4096;

/// Explicit outcome of a filesystem operation; native failures retain their error number.
enum class Status : uint8
{
    Ok,              ///< Operation completed successfully, including reads at EOF.
    InvalidArgument, ///< Invalid path, closed owner, invalid offset or native path component.
    NotFound,        ///< A requested native path component does not exist.
    AccessDenied,    ///< Native access was denied or a child symlink was refused.
    NotRegularFile,  ///< The opened object is not a readable regular file.
    OutOfMemory,     ///< Opening or cloning could not allocate its private owner.
    IoError,         ///< A native operation failed for another reason.
    Changed,         ///< Size or modification time no longer matches the captured revision.
    Unsupported,     ///< Native I/O or a requested pack format/feature is unavailable.
    CorruptData,     ///< A pack index, payload or integrity check is malformed.
    LimitExceeded,   ///< Configured storage/index/decoded resource admission was exceeded.
    Conflict,        ///< A publication precondition or cooperative writer lock was refused.
};

/// Status and optional native error number; success has no native error.
struct Result final
{
    Status Code = Status::Ok; ///< Portable outcome.
    int32 NativeCode = 0;     ///< Native errno on syscall failure; zero otherwise.
    /// Returns whether the operation succeeded.
    [[nodiscard]] bool Succeeded() const noexcept
    {
        return Code == Status::Ok;
    }
};

/// Native metadata observation, not a content digest or a globally unique revision.
/// Equality can miss edits that preserve metadata; never substitutes for content validation.
struct FileStamp final
{
    uint64 Device = 0;             ///< Native filesystem device identifier.
    uint64 Inode = 0;              ///< Native file identifier, which may eventually be reused.
    uint64 Size = 0;               ///< Observed byte length.
    int64 ModifiedSeconds = 0;     ///< Native modification timestamp seconds.
    int64 ModifiedNanoseconds = 0; ///< Modification timestamp fractional nanoseconds.
    int64 ChangedSeconds = 0;      ///< Native metadata change timestamp seconds.
    int64 ChangedNanoseconds = 0;  ///< Metadata change timestamp fractional nanoseconds.
    /// Compares all observed fields; equality does not prove identical bytes.
    [[nodiscard]] bool operator==(const FileStamp&) const noexcept = default;
};

/// Read outcome and valid progress. On Changed, discard the potentially touched buffer.
struct ReadResult final
{
    Result Outcome;      ///< Status of the read or revision check.
    usize BytesRead = 0; ///< Valid bytes transferred; zero on Changed, partial on other errors.
};

/// Validates a bounded UTF-8 relative path without normalization or case folding.
/// Rejects absolute paths, empty/dot components, NUL, backslash, colon and control bytes.
[[nodiscard]] bool ValidPath(std::string_view path) noexcept;

// Thanks to Noel Llopis and Charles Nicholson, "Stay in the Game: Asset
// Hotloading for Fast Iteration", Game Programming Gems 6, sec. 1.10,
// pp. 109-114: their prepare/rebind boundary informs revision ownership here.
// Active file readers retain old bytes; Content owns asset rebinding. Design
// inspiration only, no article code copied. See docs/architecture/filesystem.md.
/// Owns an opened regular-file revision on Linux, macOS or Emscripten MEMFS.
/// Other targets return Unsupported. Browser reads address session memory, not host files.
/// Concurrent const operations require disjoint destinations and stable owner lifetime.
/// Closing, moving and replacing an owner require exclusive access.
class File final
{
public:
    /// Creates an empty owner.
    File() noexcept = default;
    /// Closes best-effort without logging or propagating errors.
    ~File() noexcept;
    /// Ownership is move-only; use Clone to duplicate an opened revision.
    File(const File&) = delete;
    /// Ownership is move-only; use Clone to duplicate an opened revision.
    File& operator=(const File&) = delete;
    /// Transfers ownership, leaving other empty.
    File(File&& other) noexcept;
    /// Closes the previous revision and transfers ownership; self-move preserves it.
    File& operator=(File&& other) noexcept;
    /// Returns whether this owner contains an opened revision.
    [[nodiscard]] bool IsOpen() const noexcept;
    /// Returns the captured byte length, or zero for an empty owner.
    [[nodiscard]] uint64 Size() const noexcept;
    /// Duplicates the opened revision without pathname lookup; failure preserves output.
    /// A closed source returns InvalidArgument. Output may alias this owner.
    [[nodiscard]] Result Clone(File& output) const noexcept;
    /// Fills destination up to captured EOF; offsets above Size() return InvalidArgument.
    /// Handles interrupted/short reads. Size/mtime checks run before and after even empty reads.
    /// Changed means BytesRead is zero and the touched buffer must be discarded; other
    /// failures report partial progress. Metadata detection provides no snapshot guarantee.
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> destination) const noexcept;
    /// Releases ownership even on native error; repeated calls succeed. Never retries close.
    [[nodiscard]] Result Close() noexcept;

private:
    friend class Directory;
    struct Impl;
    Impl* mImpl = nullptr;
};

/// Pins a host-selected trusted root on Linux, macOS or Emscripten MEMFS.
/// Other targets return Unsupported. Browser roots belong to the session filesystem.
/// Child symlinks are rejected at every component. This is not a sandbox against
/// hostile renames, hard links or mount changes. Const operations require a stable lifetime.
class Directory final
{
public:
    /// Creates an empty root owner.
    Directory() noexcept = default;
    /// Releases the root best-effort without logging.
    ~Directory() noexcept;
    /// Root ownership is move-only.
    Directory(const Directory&) = delete;
    /// Root ownership is move-only.
    Directory& operator=(const Directory&) = delete;
    /// Transfers the pinned root, leaving other empty.
    Directory(Directory&& other) noexcept;
    /// Releases the previous root and transfers ownership; self-move preserves it.
    Directory& operator=(Directory&& other) noexcept;
    /// Returns whether a native root is pinned.
    [[nodiscard]] bool IsOpen() const noexcept;
    /// Opens a nonempty native root of at most MAX_ROOT_BYTES with no embedded NUL.
    /// Failure preserves the existing root. Trusted roots may have symlink ancestry.
    [[nodiscard]] Result Open(std::string_view nativeRoot) noexcept;
    /// Opens a validated relative path as a regular-file revision; failure preserves output.
    /// Native filename equivalence follows the host filesystem, including macOS case/Unicode rules.
    [[nodiscard]] Result OpenRead(std::string_view relativePath, File& output) const noexcept;
    /// Observes a validated path without allocating or opening the leaf for reading.
    /// Rejects child symlinks and non-regular leaves; failure preserves output.
    /// NotFound means an ancestor or leaf was absent at observation time.
    [[nodiscard]] Result Observe(std::string_view relativePath, FileStamp& output) const noexcept;
    /// Releases the root even on error; open files retain their independent lifetimes.
    [[nodiscard]] Result Close() noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};
} // namespace ludus::foundation::filesystem
