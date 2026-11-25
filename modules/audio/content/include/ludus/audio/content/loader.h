#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/audio/audio_system.h>
#include <ludus/audio/content/music_score.h>

#include <string_view>

namespace ludus::audio::content
{
/// Exact revision ownership token issued by one Loader; copying does not acquire
/// another lease. Release once on that loader's control owner. Not serialized.
struct Lease final
{
    /// Revision-table slot; meaningful only with Generation and Owner.
    uint32 Slot = 0;
    /// Nonreused incarnation of the slot; zero is invalid.
    uint32 Generation = 0;
    /// Nonreused process-local loader identity; zero is invalid.
    uint64 Owner = 0;
    /// Compare token identity; equality does not establish validity.
    [[nodiscard]] bool operator==(const Lease&) const noexcept = default;
};
/// Acquire sound/music revisions on a Linux/macOS background worker.
/// Exactly one control owner calls all methods; at most one acquisition is pending.
/// Background work owns inputs/results, and Poll installs a whole revision.
/// @note The referenced AudioSystem must outlive this loader. Destroy the loader
/// on its control owner; destruction joins pending acquisition before reclaiming it.
class Loader final
{
public:
    /// Bind to an AudioSystem without acquiring any content.
    explicit Loader(AudioSystem& audio) noexcept;
    /// Blocking cold shutdown: cancel/join acquisition and release revisions.
    ~Loader() noexcept;
    /// Loader ownership cannot be copied.
    Loader(const Loader&) = delete;
    /// Loader ownership cannot be assigned.
    Loader& operator=(const Loader&) = delete;
    /// Begin cold file acquisition and decoding without blocking the control owner.
    /// @param root Native content root copied into the pending job.
    /// @param id Catalog resource identifier copied into the pending job.
    /// @param draft Optional sound draft copied and validated before worker launch.
    /// @param musicDraft Optional music draft; mutually exclusive with draft.
    /// @param snapshot Optional immutable version-1 catalog copied before launch;
    /// worker lookup uses this snapshot instead of reopening catalog.json. Source
    /// files remain loose-file revisions; this does not freeze their path contents.
    /// @return Pending after launch or while an acquisition is already pending;
    /// Invalid for invalid input/uninitialized audio; OutOfMemory for allocation
    /// failure; Limit if loader identity space is exhausted; IoError for worker
    /// creation failure. Other platforms are Unsupported.
    [[nodiscard]] ContentStatus Begin(std::string_view root,
                                      std::string_view id,
                                      const Sound* draft = nullptr,
                                      const Music* musicDraft = nullptr,
                                      const ludus::content::Catalog* snapshot = nullptr) noexcept;
    /// Install completed acquisition on the control owner after routing checks.
    /// @param routing Bus/group names borrowed during this call.
    /// @param output Receives the new lease on Ok; preserved on failure/Pending.
    /// @param diagnostic Receives completed acquisition/validation diagnostics.
    /// @return Pending while the worker runs; Ok on installation; NotFound with
    /// no pending job; Cancelled after the audio session changes or stops; otherwise
    /// the acquisition/validation status. Cancelled also follows RequestCancel.
    /// Completed work is detached and reclaimed without waiting for thread exit;
    /// detach failure returns IoError and retains the job for blocking Cancel.
    /// @note Never joins or waits for file/decode work. Installation and candidate
    /// cleanup still perform bounded cold work; this is not an audio-callback API.
    /// While cancellation drains, Begin remains Pending and cannot replace inputs.
    [[nodiscard]] ContentStatus Poll(const Routing& routing, Lease& output, Diagnostic& diagnostic) noexcept;
    /// Signal cancellation without joining, releasing inputs or installing results.
    /// Idempotent; no pending acquisition is a no-op. Call Poll until Cancelled
    /// (or an earlier terminal error) to drain ownership, or use blocking Cancel.
    /// Already-issued leases remain valid. Call only on the control owner.
    void RequestCancel() noexcept;
    /// Cancel and join pending acquisition on the control owner before freeing
    /// its inputs. May block until current file/decode work returns; no pending
    /// acquisition is a no-op. Existing revision leases remain valid.
    void Cancel() noexcept;
    /// Copy a sound definition and resolved descriptor on Ok; preserve outputs
    /// otherwise. Reject foreign/stale/released or wrong-kind leases as Invalid;
    /// unavailable installed clips return Cancelled. Descriptor borrows clip
    /// identity; retain the lease until playback has been submitted.
    [[nodiscard]] ContentStatus
    GetSound(Lease lease, Sound& definition, app::EventDescriptor& descriptor) const noexcept;
    /// Copy a music definition on Ok; preserve output for invalid/foreign/stale
    /// or wrong-kind leases. The source descriptor remains owned by the lease.
    [[nodiscard]] ContentStatus GetMusic(Lease lease, Music& definition) const noexcept;
    /// Copy the version-two definition and resolved bus from a live lease. Failure preserves both outputs.
    [[nodiscard]] ContentStatus GetMusicScore(Lease lease, MusicScore& outScore, uint32& outBus) const noexcept;
    /// Cold control-owner operation. Prepare an independent loop from the exact source pinned by a score lease.
    /// The caller owns a successful stream and must retire it. Failure preserves outStream.
    [[nodiscard]] Status
    PrepareMusicLayer(Lease lease, usize sectionIndex, usize layerIndex, StreamHandle& outStream) noexcept;
    /// Start an independent stream from a valid music lease. Playback retains its
    /// own source reader and survives lease release; voice is assigned on success.
    /// Stream preparation is cold work. Invalid leases return InvalidHandle.
    [[nodiscard]] Status PlayMusic(Lease lease, VoiceHandle& voice) noexcept;
    /// Release one owned revision, including after its audio session ends.
    /// Foreign/stale/already-released tokens return Invalid without mutation.
    /// Last shared-clip release requests AudioSystem retirement; active playback
    /// frees only through AudioSystem's acknowledgments. Control owner only.
    [[nodiscard]] ContentStatus Release(Lease lease) noexcept;
    /// Service AudioSystem and request retirement of terminal music instances.
    /// Does not poll or join acquisition. Call on the control owner.
    void Service() noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};
} // namespace ludus::audio::content
