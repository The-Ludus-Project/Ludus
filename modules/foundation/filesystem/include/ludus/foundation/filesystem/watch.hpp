#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/filesystem/filesystem.hpp>

#include <string_view>

namespace ludus::foundation::filesystem
{
/// Fixed storage and debounce admission for a host-polled cooked-file watcher.
struct WatchConfig final
{
    usize MaxPaths = 128;                    ///< Registered path capacity, from 1 through 4096.
    usize HintCapacity = 128;                ///< Queued hint capacity, from 1 through 4096.
    uint64 DebounceNanoseconds = 50'000'000; ///< Stable observation interval; zero emits immediately.
};
/// Identifies one registration in one watcher lifetime; retired handles cannot remove new paths.
struct WatchHandle final
{
    uint64 Owner = 0;    ///< Watcher lifetime identity; zero is invalid.
    uint64 Sequence = 0; ///< Registration identity; zero is invalid.
    usize Slot = 0;      ///< Bounded storage slot, not a persistent catalog identifier.
    /// Compares registration identity.
    [[nodiscard]] bool operator==(const WatchHandle&) const noexcept = default;
};
/// Hint purpose; every hint requires explicit host revalidation before revision selection.
enum class WatchHintKind : uint8
{
    Changed,        ///< Debounced metadata difference, creation or disappearance.
    RescanRequired, ///< Queue overflow or observation error invalidated incremental hints.
    RescanEntry,    ///< One current registered path observed during a bounded rescan.
};
/// Owned hint value; it does not hold a file revision or reference watcher storage.
struct WatchHint final
{
    WatchHintKind Kind = WatchHintKind::Changed; ///< Notification or rescan purpose.
    WatchHandle Handle;                          ///< Registration; empty for a global RescanRequired hint.
    uint64 Tag = 0;                              ///< Host tag supplied at registration; zero for a global hint.
    Result Observation;                          ///< Ok for a regular file, NotFound for absence, or a typed error.
    FileStamp Stamp;                             ///< Metadata only when Observation is Ok.
    char Path[MAX_PATH_BYTES + 1]{};             ///< Validated relative UTF-8 path, terminated.
    usize PathBytes = 0;                         ///< Encoded path length, excluding the terminator.
    /// Returns the owned path view, valid for this hint's lifetime.
    [[nodiscard]] std::string_view PathView() const noexcept;
};
/// Poll operation status; a successful empty poll preserves the supplied hint.
struct WatchPollResult final
{
    Result Outcome;         ///< InvalidArgument for an uninitialized watcher; Conflict for wrong rescan phase.
    bool Available = false; ///< Whether output was replaced with a hint.
};
/// Bounded polling adapter for registered final cooked paths on Linux/macOS or Emscripten MEMFS.
/// Browser observations stay in session memory and do not watch host filesystem changes.
/// All methods and destruction require one host owner thread. No background jobs/callbacks.
/// Metadata polling can miss transient or metadata-preserving edits and discovers no new paths.
/// Producers should atomically publish cooked output; debounce does not validate completeness.
/// Watcher hints never modify opened readers, mounts or Content bindings.
class Watcher final
{
public:
    /// Creates an uninitialized adapter.
    Watcher() noexcept = default;
    /// Releases registration storage and the pinned root best-effort.
    ~Watcher() noexcept;
    /// Watchers have one stable owner identity and cannot be copied.
    Watcher(const Watcher&) = delete;
    /// Watchers have one stable owner identity and cannot be assigned.
    Watcher& operator=(const Watcher&) = delete;
    /// Pins a trusted native root and allocates fixed storage; failure preserves existing state.
    /// Unsupported native targets return Unsupported. Successful reinitialization retires handles.
    [[nodiscard]] Result Init(std::string_view nativeRoot, const WatchConfig& config = {}) noexcept;
    /// Registers a unique byte-spelled path and captures its initial regular-file/absence baseline.
    /// Failure preserves output. No allocation occurs. Refused during a rescan.
    [[nodiscard]] Result Add(std::string_view path, uint64 tag, WatchHandle& output) noexcept;
    /// Retires a matching handle and removes its pending hints; stale/foreign handles are invalid.
    /// Refused during a rescan. Does not access the filesystem or allocate.
    [[nodiscard]] Result Remove(WatchHandle handle) noexcept;
    /// Examines at most maxSteps slot positions, advancing a round-robin cursor without allocation.
    /// maxSteps must be 1..MaxPaths; nowNanoseconds is a host monotonic timestamp, allowed to repeat.
    /// Backward time is rejected. Overflow/errors require rescan and stop incremental observation.
    [[nodiscard]] Result Advance(uint64 nowNanoseconds, usize maxSteps) noexcept;
    /// Pops one changed hint or the sticky global rescan notice; no filesystem access/allocation.
    /// The global notice is delivered once until a failed rescan requests another attempt.
    [[nodiscard]] WatchPollResult Poll(WatchHint& output) noexcept;
    /// Clears incremental hints and starts a rescan of the fixed registration set.
    /// Add/Remove/Advance/Poll are refused until NextRescan completes the pass.
    [[nodiscard]] Result BeginRescan() noexcept;
    /// Observes at most one registered path per call and returns a RescanEntry, even unchanged.
    /// A successful empty result completes the pass. Any observation error other than NotFound
    /// keeps NeedsRescan set. No allocation occurs; scanning skips at most MaxPaths slots.
    /// A pass is not an atomic snapshot; hosts must open and validate selected revisions explicitly.
    [[nodiscard]] WatchPollResult NextRescan(WatchHint& output) noexcept;
    /// Returns whether incremental observations require a full registered-path rescan.
    [[nodiscard]] bool NeedsRescan() const noexcept;
    /// Returns whether a rescan pass is in progress.
    [[nodiscard]] bool IsRescanning() const noexcept;
    /// Returns a saturating count of incremental hints discarded by overflow or observation errors in this lifetime.
    [[nodiscard]] uint64 DroppedHints() const noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};
} // namespace ludus::foundation::filesystem
