#pragma once

// The instance-owned Ludus audio control owner, per .kiro/specs/audio/design.md
// section 3 and requirements AU02-AU16.
//
// AudioSystem is a plain instance with explicit, caller-owned lifetime: no
// global service locator, no implicit thread-safe facade, no MPSC queue. Exactly
// one owner thread makes all public calls; jobs/game threads forward requests to
// that owner. Real-time submission/query methods are noexcept and allocate/free
// nothing. Loading and native device setup are explicit cold operations that may
// block the control owner; browser setup/shutdown are asynchronous.
//
// Construction is fallible: check IsValid() (or the Initialize status). Setup
// failure is reported explicitly, never through an exception (engine code is
// -fno-exceptions). No miniaudio / Web Audio type appears in this header.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp> // ludus::foundation::core::UniquePtr

#include <ludus/audio/audio_source.h>
#include <ludus/audio/audio_types.h>

#include <span>

namespace ludus::audio
{
class AudioDebugSnapshotSink; // opt-in trace, declared in audio_debug.h

/// Timing of a stream-group gain change or crossfade on the audio session clock.
struct StreamRamp final
{
    uint64 StartFrame = 0;     ///< Absolute session frame at which the ramp begins.
    uint32 DurationFrames = 0; ///< Session frames from start to target gain. Zero applies it immediately.
};

/// @brief Result of submitting a batch: the aggregate acceptance status plus the voice handles assigned to each
/// Play record in submission order.
///
/// A rejected batch yields no handles and mutates no accounting.
struct BatchResult final
{
    Status Result = Status::InvalidArgument;
    /// @brief Filled only on Status::Ok: one handle per Play command, in the order the Play commands appeared
    /// in the batch.
    ///
    /// Non-Play commands contribute no entry.
    std::span<const VoiceHandle> Voices;
};

/// Browser async teardown progress (design section 10).
enum class ShutdownState : uint8
{
    NotStarted,
    ClosingAdmission,
    QuiescingRenderer,
    QuiescingWorker,
    ClosingContext,
    Complete,
    Failed,
};

/// @brief Instance-owned audio control surface with one caller-owned control thread.
///
/// Public warm operations allocate nothing; preparation and native device setup
/// are explicit cold operations. Forward requests from other threads to the owner.
/// Construction is fallible: check IsValid or Initialize. Browser lifecycle is
/// asynchronous; call Service to advance it. See docs/wiki/guides/audio.md.
class AudioSystem final
{
public:
    AudioSystem() noexcept;
    ~AudioSystem() noexcept;

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    // --- Lifecycle (design section 3) -----------------------------------
    /// Allocate fixed runtime storage and validate the bus tree, groups and capacities.
    /// @param config Session mode, sample rate and borrowed bus/group configuration.
    /// @return Ok when ready; InvalidArgument for invalid configuration or repeated
    /// initialization; OutOfMemory on allocation failure; DeviceError when native
    /// context/device startup fails. Failed initialization permits an explicit retry.
    /// @note Call on the sole control owner. Device mode uses CoreAudio on macOS
    /// and PulseAudio/ALSA on Linux, with float32 stereo at the requested session
    /// rate. The period is a hint; miniaudio may convert the hardware format/rate.
    /// No null-device or Disabled fallback is selected implicitly.
    [[nodiscard]] Status Initialize(const SystemConfig& config) noexcept;

    [[nodiscard]] SystemState GetState() const noexcept;
    [[nodiscard]] Mode GetMode() const noexcept;
    [[nodiscard]] uint32 GetSession() const noexcept;
    [[nodiscard]] uint32 GetSampleRate() const noexcept;

    // --- Preparation (cold; design sections 3, 9) -----------------------
    /// @brief Decode/convert encoded bytes into owned finite PCM at the session rate.
    ///
    /// The caller's bytes are borrowed only for the duration of this call.
    [[nodiscard]] Status
    PrepareClip(std::span<const uint8> encoded, const ClipDescriptor& descriptor, ClipHandle& outClip) noexcept;

    /// Copy encoded bytes, open a seekable decoder and prefill bounded PCM chunks.
    /// @param encoded Bytes borrowed only during this cold owner-side call.
    /// @param descriptor Codec and optional source-frame loop bounds.
    /// @param outStream Receives a session handle on success; invalidated on failure.
    /// @return Ok, a decode/argument/capacity/allocation status, or IoError if the
    /// native worker cannot start. Native workers support Linux and macOS.
    [[nodiscard]] Status
    PrepareStream(std::span<const uint8> encoded, const StreamDescriptor& descriptor, StreamHandle& outStream) noexcept;

    /// @brief Close new admission for an asset; storage is retained for existing reservations/voices/worker
    /// jobs and reclaimed by Service after terminal acknowledgment.
    ///
    /// No forced stop.
    [[nodiscard]] Status RetireClip(ClipHandle clip) noexcept;
    /// Close stream admission without stopping existing voices; Service reclaims after quiescence.
    [[nodiscard]] Status RetireStream(StreamHandle stream) noexcept;

    /// @brief True when `clip` names a currently-prepared, non-retiring resident clip in this session.
    ///
    /// Owner-side; lets application descriptor validators check a variation handle without attempting playback.
    [[nodiscard]] bool IsClipReady(ClipHandle clip) const noexcept;

    /// Prepare a stream with exclusive input ownership, including on failure.
    /// @param input Seekable reader used during preparation and by the native
    /// Linux/macOS decode worker. File inputs bound encoded memory independently
    /// of track duration; renderer callbacks never read or seek this input.
    /// @param descriptor Codec and optional source-frame loop bounds.
    /// @param outStream Receives a session handle on success; invalidated on failure.
    /// @return Ok or an explicit decode/argument/capacity/allocation/worker status.
    /// @note Call on the sole control owner. Retired input remains owned until
    /// voice and worker references quiesce; shutdown joins before reclaiming it.
    [[nodiscard]] Status PrepareStream(foundation::core::UniquePtr<StreamInput> input,
                                       const StreamDescriptor& descriptor,
                                       StreamHandle& outStream) noexcept;

    [[nodiscard]] Status InstallClip(PreparedClip& prepared, ClipHandle& outClip) noexcept;

    // --- Submission (warm; design section 4) ----------------------------
    /// @brief Validate the whole batch (<= MAX_BATCH_RECORDS), copy all records, reserve slots and asset pins
    /// transactionally, then publish once.
    ///
    /// The returned Voices span is owned by the system and remains valid until the next submission call.
    /// noexcept, no allocation, no lock, no wait.
    [[nodiscard]] BatchResult TrySubmitBatch(std::span<const Command> commands) noexcept;

    /// Convenience single-command Play using the batch path.
    [[nodiscard]] Status PlayClip(const PlayParams& params, VoiceHandle& outVoice) noexcept;

    /// @brief Stream playback: a prepared StreamHandle is admitted once.
    ///
    /// A second play on the same handle returns NotReady.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): accepted public shape.
    [[nodiscard]] Status
    PlayStream(StreamHandle stream, uint32 busIndex, uint8 priority, float32 gain, VoiceHandle& outVoice) noexcept;

    /// Start aligned looping streams as one voice. Gains are per-layer linear amplitudes.
    /// All handles must be distinct, unplayed and compatible. The group advances only when
    /// every member has data. Failure preserves outVoice. Call on the control owner and retire each stream after use.
    [[nodiscard]] Status PlayStreamGroup(std::span<const StreamHandle> streams,
                                         std::span<const float32> gains,
                                         uint32 busIndex,
                                         uint8 priority,
                                         VoiceHandle& outVoice) noexcept;

    /// Queue new linear gains for a stream group at an absolute session frame. Later
    /// queued edits supersede an unapplied edit. A ramp advances in consumed group frames.
    /// Call on the control owner. Queue rejection preserves the pending edit.
    [[nodiscard]] Status
    SetStreamGroupGains(VoiceHandle voice, std::span<const float32> gains, StreamRamp ramp) noexcept;

    /// Schedule an entire prepared group as the replacement for outgoing, on its bus and
    /// priority. The ramp duration sets the linear crossfade length in session frames. A zero
    /// duration cuts at the accepted frame. Failure preserves outVoice and leaves all incoming streams available for
    /// retry.
    [[nodiscard]] Status ScheduleStreamGroup(std::span<const StreamHandle> streams,
                                             std::span<const float32> gains,
                                             VoiceHandle outgoing,
                                             StreamRamp ramp,
                                             VoiceHandle& outVoice) noexcept;

    /// Admit a prepared incoming stream for a hard cut from a live outgoing stream on its
    /// bus, with its priority and gain. The sole control owner supplies an absolute session
    /// frame. One incoming plan may claim an outgoing voice. The renderer seals the plan at
    /// the beginning of the control quantum containing the transition. Query the incoming
    /// voice for the applied start frame. Capacity or validation failure preserves playback.
    [[nodiscard]] Status
    ScheduleStreamTransition(StreamHandle incoming, VoiceHandle outgoing, uint64 frame, VoiceHandle& outVoice) noexcept;

    /// Edit an admitted transition on the control owner. Zero cancels it before sealing.
    /// TooLate means the renderer has sealed it. A successfully cancelled
    /// incoming stream retires through the usual voice/stream lifecycle.
    [[nodiscard]] Status ReviseStreamTransition(VoiceHandle incoming, uint64 frame) noexcept;

    // --- Cancellation (always available; design section 4) --------------
    /// @brief Stop uses a per-slot generation mailbox independent of normal queue room.
    ///
    /// Idempotent for a terminal handle still known to the owner.
    [[nodiscard]] Status Stop(VoiceHandle voice) noexcept;

    /// @brief Advance the cancellation epoch; affects all voices admitted before this call, including queued
    /// plays.
    ///
    /// Never needs queue space. Mix settings remain.
    void StopAll() noexcept;

    // --- Service (owner/control thread; design section 3) ---------------
    /// @brief Drain diagnostics, acknowledge durable terminals, free retired storage, and advance
    /// setup/worker/backend lifecycle.
    ///
    /// Not a mixer tick. Output continuity does not depend on calling this at the game frame rate.
    void Service() noexcept;

    // --- Queries (design sections 3, 11) --------------------------------
    [[nodiscard]] Status GetVoiceInfo(VoiceHandle voice, VoiceInfo& out) const noexcept;
    [[nodiscard]] Status GetGroupInfo(uint32 groupIndex, GroupInfo& out) const noexcept;
    [[nodiscard]] Status GetBusMeter(uint32 busIndex, BusMeter& out) const noexcept;
    void GetSystemSnapshot(SystemSnapshot& out) const noexcept;

    // --- Offline rendering (Offline mode only; design section 5) --------
    /// @brief Run the production render entry serially on the caller thread.
    ///
    /// Rejects concurrent device rendering. `output` is validated against layout/frames.
    [[nodiscard]] Status
    RenderOffline(std::span<float32> output, ChannelLayout layout, BufferLayout bufferLayout, uint32 frames) noexcept;

    // --- Browser gesture / shutdown (design section 10) -----------------
    /// @brief Must be called inside a genuine DOM activation handler on the browser.
    ///
    /// Schedules resume; returns Pending/result.
    [[nodiscard]] Status ResumeFromUserGesture() noexcept;

    /// Close admission, quiesce native callbacks and join the decode worker before
    /// reclaiming assets. Linux/macOS teardown is synchronous on the control owner.
    /// @return Ok after native teardown (also on repeated completed shutdown),
    /// or NotReady before initialization. Native completion leaves Disabled state.
    [[nodiscard]] Status BeginShutdown() noexcept;
    [[nodiscard]] ShutdownState GetShutdownState() const noexcept;

    // --- Diagnostics opt-in ---------------------------------------------
    /// @brief Attach/detach an owner-side snapshot/trace sink.
    ///
    /// Does not transfer ownership; the sink must outlive the attachment. Null detaches.
    void SetDebugSink(AudioDebugSnapshotSink* sink) noexcept;

    // --- Attenuation modifiers (design section 8) -----------------------
    /// @brief Acquire a bounded attenuation-modifier instance with per-bus dB targets and a weight in [0,1].
    ///
    /// Returns a generation-checked ModifierHandle.
    /// Overlapping owners get distinct instances. AssetCapacity-style failure is
    /// reported as Status (no handle). The modifier ramps in on acquisition.
    [[nodiscard]] Status
    AcquireModifier(std::span<const ModifierValue> values, float32 weight, ModifierHandle& outModifier) noexcept;

    /// Update a modifier's weight (ramps). Generation-checked.
    [[nodiscard]] Status UpdateModifier(ModifierHandle modifier, float32 weight) noexcept;

    /// @brief Release a modifier: fades its weight to zero, then retires its slot after the renderer
    /// acknowledges.
    ///
    /// Removing one instance never removes another.
    [[nodiscard]] Status ReleaseModifier(ModifierHandle modifier) noexcept;

    // --- Owner-side user-gain helper (design section 8) -----------------
    /// @brief Map a normalized slider u in [0,1] through an explicit dB range to a
    /// UserGain amplitude: 0 at u==0 (exact mute), else 10^((u-1)*range/20).
    [[nodiscard]] static float32 UserGainFromSlider(float32 u, float32 dbRange) noexcept;

    /// True when construction + Initialize produced usable fixed storage.
    [[nodiscard]] bool IsValid() const noexcept
    {
        return mImpl.Get() != nullptr;
    }

private:
    struct Impl;
    ludus::foundation::core::UniquePtr<Impl> mImpl;
};

} // namespace ludus::audio
