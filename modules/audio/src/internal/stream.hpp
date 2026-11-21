#pragma once
#include <ludus/audio/audio_source.h>
#include <ludus/audio/audio_types.h>

#include <atomic>
#include <climits>
#include <ludus/foundation/containers/static_array.hpp>
#include <span>

namespace ludus::audio::internal
{
struct StreamChunk final
{
    float32 Samples[STREAM_CHUNK_FRAMES * 2]{};
    uint32 Frames = 0;
    bool Eof = false;
    bool Error = false;
};
struct StreamRequest final
{
    uint32 Frames = 0;
    float32 Gain = 1.0F;
    bool Stop = false;
};
struct StreamResult final
{
    uint32 Frames = 0;
    bool Terminal = false;
    bool Error = false;
};
struct StreamData final
{
    // Owner initializes the predecessor before publishing Play. The renderer atomically
    // seals the requested frame. Zero in an editable plan means cancellation.
    static constexpr uint64 SEALED = uint64{1} << (sizeof(uint64) * CHAR_BIT - 1);
    VoiceHandle Outgoing;
    std::atomic<uint64> TransitionFrame{0};
    std::atomic<uint64> HeldFrames{0};
    uint64 EndRenderFrame = 0; // renderer only
    ludus::foundation::StaticArray<StreamData*, STREAM_CAPACITY> Members{};
    ludus::foundation::StaticArray<float32, STREAM_CAPACITY> Gains{};
    ludus::foundation::StaticArray<float32, STREAM_CAPACITY> PendingGains{};
    ludus::foundation::StaticArray<float32, STREAM_CAPACITY> TargetGains{};
    uint64 GainFrame = 0;
    uint32 PendingRamp = 0;
    uint32 RemainingRamp = 0;
    bool HasGainEdit = false;
    usize MemberCount = 0;
    uint64 SourceFrames = 0;
    uint32 SourceRate = 0;
    uint32 CrossfadeFrames = 0;
    uint64 FadeOutBegin = 0;
    float32 GroupLeft = 0;
    float32 GroupRight = 0;
    float32 AvailabilityGain = 1;
    [[nodiscard]] StreamResult RenderGroup(float32* output, const StreamRequest& request, uint64 firstFrame) noexcept;
    StreamChunk Chunks[STREAM_CHUNK_COUNT];
    std::atomic<uint32> Written{0};
    std::atomic<uint32> Read{0};
    std::atomic<bool> Cancel{false};
    std::atomic<bool> Done{false};
    void* Decoder = nullptr;
    uint8* Encoded = nullptr;
    uint64 EncodedBytes = 0;
    StreamInput* Input = nullptr;
    uint32 Rate = 0;
    uint64 Begin = 0;
    uint64 End = 0;
    uint64 DecodeCursor = 0;
    bool Looping = false;
    bool WorkerEof = false;
    // Renderer-exclusive fields.
    uint32 Offset = 0;
    float32 Fade = 0.0F;
    float32 LastL = 0.0F;
    float32 LastR = 0.0F;
    ~StreamData() noexcept;
    [[nodiscard]] Status Open(std::span<const uint8> bytes, const StreamDescriptor& descriptor, uint32 rate) noexcept;
    [[nodiscard]] Status Open(StreamInput* input, const StreamDescriptor& descriptor, uint32 rate) noexcept;
    [[nodiscard]] Status Configure(const StreamDescriptor& descriptor, uint32 rate) noexcept;
    void Refill() noexcept;
    [[nodiscard]] StreamResult Render(float32* output, const StreamRequest& request) noexcept;
};
struct StreamSlot final
{
    uint32 Generation = 0;
    bool InUse = false;
    bool Played = false;
    bool Retiring = false;
    StreamData* Data = nullptr;
};
} // namespace ludus::audio::internal
