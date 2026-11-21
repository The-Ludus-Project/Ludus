#pragma once
#include <ludus/audio/content/loader.h>

namespace ludus::audio::content
{
/// Gameplay prediction expressed on the current audio session clock.
struct MusicCueTime final
{
    uint64 RequestedFrame = 0; ///< Earliest eligible sample frame for this action.
};

/// Control-owner view of the current section and single pending transition.
struct MusicTransport final
{
    ResourceId ActiveSection;  ///< Authored name of the playing section.
    ResourceId PendingSection; ///< Destination while HasPlan is true.
    uint64 SectionFrame = 0;   ///< Frames consumed in the section, including muted layers.
    uint64 HeldFrames = 0;     ///< Session frames held while waiting for a stem.
    VoiceHandle Active;        ///< Voice whose cursor counts consumed section frames, including muted frames.
    VoiceHandle Incoming;      ///< Renderer-admitted candidate, invalid while outside the submission horizon.
    uint64 RequestedFrame = 0; ///< Absolute gameplay cue time supplied by the caller.
    uint64 BoundaryFrame = 0;  ///< Quantized session frame requested of the renderer.
    uint64 AppliedFrame = 0;   ///< Most recent section start observed from the renderer.
    bool HasPlan = false;      ///< A section replacement is pending.
};

/// Bounded native music controller. Use one control thread for all methods. AudioSystem and
/// Loader must outlive the director. Keep the score lease live until destruction. Creating
/// a section can perform cold decoder setup. Service publishes prepared plans within the horizon.
class MusicDirector final
{
public:
    /// Bind the sole audio control owner and its loader.
    MusicDirector(AudioSystem& inoutAudio, Loader& inoutLoader) noexcept;
    /// Stop retained voices and retire any unsubmitted stream reservations.
    ~MusicDirector() noexcept;
    /// Playback ownership is unique.
    MusicDirector(const MusicDirector&) = delete;
    /// Playback ownership cannot be assigned.
    MusicDirector& operator=(const MusicDirector&) = delete;
    /// Start the score's initial section. intensity is normalized to [0,1].
    [[nodiscard]] Status Start(Lease score, float32 intensity) noexcept;
    /// Select an authored state for an absolute gameplay frame. Repeating the pending
    /// destination edits the boundary until sealed. A different pending destination returns NotReady.
    /// Editing a sealed plan returns TooLate. Queue and capacity failures preserve current playback.
    [[nodiscard]] Status SelectState(std::string_view state, MusicCueTime cue) noexcept;
    /// Cancel the pending transition before it seals. A sealed transition returns TooLate.
    [[nodiscard]] Status CancelTransition() noexcept;
    /// Quantize intensity on the current section's beat grid and ramp over the authored duration.
    [[nodiscard]] Status SetIntensity(float32 intensity, MusicCueTime cue) noexcept;
    /// Start the declared resident accent on its boundary using an already acquired sound lease.
    /// Reject excess simultaneous stingers before playback. variation selects an authored clip.
    [[nodiscard]] Status PlayStinger(std::string_view id, Lease sound, uint32 variation, MusicCueTime cue) noexcept;
    /// Observe acknowledged transitions and publish a prepared plan as it enters the horizon.
    /// Retry QueueFull on a later service tick. NotReady reports an ended or cancelled candidate.
    [[nodiscard]] Status Service() noexcept;
    /// Return copied control-owner diagnostics as of the last call.
    [[nodiscard]] MusicTransport GetTransport() const noexcept;

private:
    AudioSystem& mAudio;
    Loader& mLoader;
    MusicScore mScore;
    Lease mLease;
    MusicTransport mTransport;
    ludus::foundation::StaticArray<StreamHandle, STREAM_CAPACITY> mPrepared{};
    ludus::foundation::StaticArray<VoiceHandle, MAX_MUSIC_STINGERS> mAccents{};
    usize mPreparedCount = 0;
    usize mSection = 0;
    usize mCandidate = 0;
    usize mEdge = 0;
    uint32 mBus = 0;
    float32 mIntensity = 0;
    uint64 mOrigin = 0;
    uint64 mPlanOrigin = 0;
    VoiceHandle mFading;
    bool mHasOrigin = false;
    [[nodiscard]] Status prepare(usize section) noexcept;
    void retirePrepared() noexcept;
    [[nodiscard]] Status submit() noexcept;
    [[nodiscard]] bool quantize(MusicBoundary boundary, uint64 requested, uint64& outFrame) noexcept;
};
} // namespace ludus::audio::content
