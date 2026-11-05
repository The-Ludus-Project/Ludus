#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include <span>
#include <string_view>

namespace ludus::foundation::filesystem
{
/// Requested native sync operations; acknowledgments are not power-loss guarantees.
enum class SyncPolicy : uint8
{
    PublishOnly,      ///< Atomically publish without requesting sync.
    File,             ///< Sync temporary file before publication.
    FileAndDirectory, ///< Sync file before publication and parent directory afterward.
};
/// Publication admission checked under a cooperative, nonblocking parent-directory lock.
/// Uncooperative writers may race the final check and rename; metadata is not a digest.
enum class WriteCondition : uint8
{
    Any,           ///< Permit an absent or regular-file destination.
    Missing,       ///< Require the destination to be absent at both checks.
    MatchingStamp, ///< Require the regular destination to match Expected at both checks.
};
/// Bounded atomic replacement options; native directories must already exist.
struct WriteOptions final
{
    uint64 MaxBytes = uint64{32} * 1024 * 1024;     ///< Admission bound, in bytes; zero permits empty files.
    SyncPolicy Sync = SyncPolicy::FileAndDirectory; ///< Requested sync operations.
    WriteCondition Condition = WriteCondition::Any; ///< Destination admission rule.
    FileStamp Expected;                             ///< Required metadata when Condition is MatchingStamp.
};
/// Publication outcome. A post-publication sync error cannot roll back visible bytes.
struct PublicationResult final
{
    Result Outcome{};             ///< First operation failure, or Ok.
    bool Published = false;       ///< Atomic replacement succeeded even if Outcome is an error.
    bool FileSynced = false;      ///< Native file sync acknowledged before publication.
    bool DirectorySynced = false; ///< Native parent sync acknowledged after publication.
    Result Cleanup{};             ///< Temporary unlink or descriptor close failure, if any; never hides Outcome.
};
/// Explicit host-selected write capability on Windows, Linux and macOS.
/// Other targets return Unsupported.
/// Operates outside virtual mounts. Refuses child symlinks, Windows reparse points
/// and non-regular destinations.
/// Root selection is trusted; hostile renames, hard links and mounts are outside this boundary.
/// Concurrent Publish calls require a stable owner lifetime and disjoint source buffers.
class WriteDirectory final
{
public:
    /// Creates an empty capability.
    WriteDirectory() noexcept = default;
    /// Releases the root best-effort.
    ~WriteDirectory() noexcept;
    /// Write capability ownership is move-only.
    WriteDirectory(const WriteDirectory&) = delete;
    /// Write capability ownership is move-only.
    WriteDirectory& operator=(const WriteDirectory&) = delete;
    /// Transfers ownership, leaving other empty.
    WriteDirectory(WriteDirectory&& other) noexcept;
    /// Releases the old capability and transfers ownership; self-move preserves it.
    WriteDirectory& operator=(WriteDirectory&& other) noexcept;
    /// Pins a trusted nonempty native root; failure preserves the existing capability.
    [[nodiscard]] Result Open(std::string_view nativeRoot) noexcept;
    /// Returns whether a root is pinned.
    [[nodiscard]] bool IsOpen() const noexcept;
    /// Writes a complete bounded buffer into an exclusive same-directory temporary.
    /// Checks the condition before and after preparation, then atomically renames.
    /// Linux and macOS use a 0600 temporary and an advisory directory lock.
    /// Windows inherits the directory ACL and takes a nonblocking write share on
    /// the reserved .ludus-write-lock file. Leave that file in place.
    /// Pre-publication failure preserves the destination; owned temporary cleanup is reported.
    /// Open File readers retain previous revisions. Publish allocates no C++ storage.
    /// FileAndDirectory reports a failed parent flush with Published still true.
    /// Native sync acknowledgments depend on the OS, filesystem and storage device.
    [[nodiscard]] PublicationResult Publish(std::string_view relativePath,
                                            std::span<const uint8> bytes,
                                            const WriteOptions& options = {}) const noexcept;
    /// Releases ownership even on error; repeated calls succeed. Requires exclusive access.
    [[nodiscard]] Result Close() noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};
} // namespace ludus::foundation::filesystem
