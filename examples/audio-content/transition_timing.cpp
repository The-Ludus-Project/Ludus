// Installed-SDK fixture. Left carries the score and right carries the incoming
// section's single transient. Capture both through the production stereo mixer.
#include <ludus/audio/audio_system.h>
#include <ludus/audio/content/music_director.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace
{
using namespace ludus::audio;
using namespace ludus::foundation;
constexpr timespec REFILL_INTERVAL{0, 1000000}; // One millisecond for the native decode worker.
constexpr uint32 BEAT_FRAMES = DEFAULT_SAMPLE_RATE / 2;
constexpr uint32 MARKER_OFFSET = 384;
constexpr uint32 INITIAL_FRAME = BEAT_FRAMES;
constexpr uint32 REPLACEMENT_FRAME = BEAT_FRAMES * 2;
constexpr uint32 LAST_FRAME = REPLACEMENT_FRAME + BEAT_FRAMES;
constexpr uint32 DISTANT_FRAME = BEAT_FRAMES * 5;
constexpr usize CHANNEL_COUNT = 2;
constexpr uint32 REGULAR_BLOCK = 512;
constexpr uint32 IRREGULAR_BLOCK = 173;
constexpr float32 MARKER_THRESHOLD = 0.1F;

enum class Operation : uint8
{
    Reference,
    Replace,
    Late,
    Cancel,
    Distant,
    Accent
};

bool Capture(std::string_view root, Operation operation, uint32 block, Array<float32>& outSamples)
{
    AudioSystem system;
    if (system.Initialize({}) != Status::Ok)
    {
        return false;
    }
    content::Loader loader(system);
    content::Lease lease;
    content::Diagnostic diagnostic;
    const std::string_view buses[] = {"music"};
    const std::string_view groups[] = {"accents"};
    const content::Routing routing{buses, groups};
    content::MusicScore score;
    uint32 bus = 0;
    const auto acquire = [&](std::string_view id, content::Lease& outLease) {
        if (loader.Begin(root, id) != content::ContentStatus::Pending)
        {
            return false;
        }
        constexpr uint32 MAX_POLLS = 5000;
        for (uint32 i = 0; i < MAX_POLLS; ++i)
        {
            const content::ContentStatus status = loader.Poll(routing, outLease, diagnostic);
            if (status != content::ContentStatus::Pending)
            {
                return status == content::ContentStatus::Ok;
            }
            nanosleep(&REFILL_INTERVAL, nullptr);
        }
        return false;
    };
    content::MusicDirector director(system, loader);
    uint64 firstBoundary = 0;
    uint64 changedBoundary = 0;
    if (acquire("music/journey", lease) == false ||
        loader.GetMusicScore(lease, score, bus) != content::ContentStatus::Ok || score.SectionCount != 2 ||
        score.TransitionCount != 1 || score.InitialState.View() != "exploring" ||
        content::QuantizeMusicFrame(score,
                                    system.GetSampleRate(),
                                    score.Transitions[0].Quantize,
                                    0,
                                    INITIAL_FRAME - 1,
                                    firstBoundary) == false ||
        content::QuantizeMusicFrame(score,
                                    system.GetSampleRate(),
                                    score.Transitions[0].Quantize,
                                    0,
                                    INITIAL_FRAME + 1,
                                    changedBoundary) == false ||
        firstBoundary != INITIAL_FRAME || changedBoundary != REPLACEMENT_FRAME ||
        director.Start(lease, 0) != Status::Ok)
    {
        std::fprintf(stderr, "Score acquisition or initial transport failed at %s\n", diagnostic.Field);
        return false;
    }
    const uint32 last = operation == Operation::Distant ? DISTANT_FRAME + BEAT_FRAMES : LAST_FRAME;
    if (outSamples.TryResize(last * CHANNEL_COUNT) == false)
    {
        return false;
    }
    uint32 position = 0;
    const auto renderThrough = [&](uint32 end) {
        while (position < end)
        {
            const uint32 count = ludus::foundation::math::Min(block, end - position);
            if (system.RenderOffline(std::span(outSamples).subspan(position * CHANNEL_COUNT, count * CHANNEL_COUNT),
                                     ChannelLayout::Stereo,
                                     BufferLayout::Interleaved,
                                     count) != Status::Ok)
            {
                return false;
            }
            position += count;
            const Status service = director.Service();
            if (service != Status::Ok && service != Status::Pending)
            {
                std::fprintf(stderr, "Service at %u: %s\n", position, ToString(service));
                return false;
            }
            // Allow the real decode worker to refill while rendering faster than realtime.
            nanosleep(&REFILL_INTERVAL, nullptr);
        }
        return true;
    };
    if (renderThrough(CONTROL_QUANTUM_FRAMES) == false)
    {
        return false;
    }
    if (operation == Operation::Distant)
    {
        if (director.SelectState("arrived", { .RequestedFrame = DISTANT_FRAME - 1 }) != Status::Pending ||
            director.GetTransport().Incoming.IsValid() == true || renderThrough(last) == false ||
            director.GetTransport().AppliedFrame != DISTANT_FRAME)
        {
            return false;
        }
        uint32 markers = 0;
        for (uint32 frame = 0; frame < last; ++frame)
        {
            if (outSamples[frame * CHANNEL_COUNT + 1] > MARKER_THRESHOLD)
            {
                if (frame != DISTANT_FRAME + MARKER_OFFSET)
                {
                    return false;
                }
                ++markers;
            }
        }
        return markers == 1;
    }
    if (operation == Operation::Accent)
    {
        content::Lease accent;
        if (acquire("sound/accent", accent) == false)
        {
            std::fprintf(stderr, "Accent acquisition failed at %s\n", diagnostic.Field);
            return false;
        }
        const Status intensity = director.SetIntensity(1, { .RequestedFrame = INITIAL_FRAME - 1 });
        const Status first = director.PlayStinger("accent", accent, 0, { .RequestedFrame = REPLACEMENT_FRAME - 1 });
        const Status repeated = director.PlayStinger("accent", accent, 0, { .RequestedFrame = REPLACEMENT_FRAME - 1 });
        if (intensity != Status::Ok || first != Status::Ok || repeated != Status::VoiceCapacity)
        {
            std::fprintf(stderr,
                         "Intensity %s, stinger %s, repeated %s\n",
                         ToString(intensity),
                         ToString(first),
                         ToString(repeated));
            return false;
        }
        if (renderThrough(LAST_FRAME) == false)
        {
            return false;
        }
        uint32 markers = 0;
        for (uint32 frame = 0; frame < LAST_FRAME; ++frame)
        {
            if (outSamples[frame * CHANNEL_COUNT + 1] > MARKER_THRESHOLD)
            {
                // The resident voice passes through the unity-rate linear resampler.
                constexpr uint32 RESIDENT_FILTER_DELAY_FRAMES = 1;
                if (frame != REPLACEMENT_FRAME + MARKER_OFFSET + RESIDENT_FILTER_DELAY_FRAMES)
                {
                    std::fprintf(stderr,
                                 "Unexpected accent sample at %u value=%f\n",
                                 frame,
                                 static_cast<double>(outSamples[frame * CHANNEL_COUNT + 1]));
                    return false;
                }
                ++markers;
            }
        }
        std::printf("stinger markers=%u\n", markers);
        return markers == 1;
    }
    if (operation == Operation::Reference)
    {
        return renderThrough(LAST_FRAME);
    }
    if (director.SelectState("arrived", { .RequestedFrame = INITIAL_FRAME - 1 }) != Status::Ok)
    {
        return false;
    }
    const VoiceHandle incoming = director.GetTransport().Incoming;
    const uint32 cutoff = INITIAL_FRAME - INITIAL_FRAME % CONTROL_QUANTUM_FRAMES;
    if (renderThrough(operation == Operation::Late ? cutoff + 1 : cutoff - 1) == false)
    {
        return false;
    }
    const uint32 revisionFrame = operation == Operation::Cancel ? 0 : static_cast<uint32>(changedBoundary);
    const Status result = operation == Operation::Cancel
                              ? director.CancelTransition()
                              : director.SelectState("arrived", { .RequestedFrame = INITIAL_FRAME + 1 });
    const content::MusicTransport acknowledgement = director.GetTransport();
    if (operation != Operation::Cancel &&
        acknowledgement.BoundaryFrame != (operation == Operation::Late ? firstBoundary : changedBoundary))
    {
        return false;
    }
    if (result != (operation == Operation::Late ? Status::TooLate : Status::Ok) || renderThrough(LAST_FRAME) == false)
    {
        return false;
    }
    VoiceInfo info;
    if (system.GetVoiceInfo(incoming, info) != Status::Ok)
    {
        return false;
    }
    uint32 marker = 0;
    uint32 markers = 0;
    for (uint32 frame = 0; frame < LAST_FRAME; ++frame)
    {
        if (outSamples[frame * CHANNEL_COUNT + 1] > MARKER_THRESHOLD)
        {
            ++markers;
            marker = frame;
        }
    }
    std::printf("block=%u initial_request=%u initial_boundary=%u revision_request=%u revision_boundary=%u result=%s "
                "edit_at=%u acknowledged=%llu applied=%llu markers=%u marker=%u\n",
                block,
                INITIAL_FRAME - 1,
                INITIAL_FRAME,
                operation == Operation::Cancel ? 0 : INITIAL_FRAME + 1,
                revisionFrame,
                ToString(result),
                operation == Operation::Late ? cutoff + 1 : cutoff - 1,
                static_cast<unsigned long long>(acknowledgement.HasPlan == true ? acknowledgement.BoundaryFrame : 0),
                static_cast<unsigned long long>(info.StartFrame),
                markers,
                marker);
    if (operation == Operation::Cancel)
    {
        return markers == 0 && info.Terminal == TerminalReason::Cancelled;
    }
    const uint32 expected = operation == Operation::Late ? INITIAL_FRAME : REPLACEMENT_FRAME;
    return markers == 1 && marker == expected + MARKER_OFFSET && info.StartFrame == expected;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    constexpr int WITH_CAPTURE = 3;
    if (argumentCount != 2 && argumentCount != WITH_CAPTURE)
    {
        return 1;
    }
    for (const uint32 block : {REGULAR_BLOCK, IRREGULAR_BLOCK})
    {
        Array<float32> reference;
        if (Capture(arguments[1], Operation::Reference, block, reference) == false)
        {
            return 1;
        }
        Array<float32> accent;
        if (Capture(arguments[1], Operation::Accent, block, accent) == false)
        {
            return 1;
        }
        // The second stem duplicates the first and ramps from silent to half gain.
        // Check its phase and gain throughout the ramp before the resident accent enters.
        constexpr uint32 LAYER_RAMP_FRAMES = 240;
        constexpr float32 GAIN_TOLERANCE = 0.00001F;
        for (uint32 frame = 0; frame < REPLACEMENT_FRAME; ++frame)
        {
            const uint32 elapsed = frame < INITIAL_FRAME ? 0 : frame - INITIAL_FRAME + 1;
            const float32 fraction = static_cast<float32>(math::Min(elapsed, LAYER_RAMP_FRAMES)) / LAYER_RAMP_FRAMES;
            const float32 expected = reference[frame * CHANNEL_COUNT] * (1 + 0.5F * fraction);
            if (math::Abs(accent[frame * CHANNEL_COUNT] - expected) > GAIN_TOLERANCE)
            {
                std::fprintf(stderr, "Layer ramp differs at frame %u\n", frame);
                return 1;
            }
        }
        for (const Operation operation : {Operation::Replace, Operation::Late, Operation::Cancel})
        {
            Array<float32> samples;
            const uint32 boundary = operation == Operation::Cancel ? LAST_FRAME
                                    : operation == Operation::Late ? INITIAL_FRAME
                                                                   : REPLACEMENT_FRAME;
            if (Capture(arguments[1], operation, block, samples) == false ||
                std::memcmp(reference.GetData(), samples.GetData(), sizeof(float32) * boundary * CHANNEL_COUNT) != 0)
            {
                return 1;
            }
            if (argumentCount == WITH_CAPTURE && block == REGULAR_BLOCK && operation == Operation::Replace)
            {
                std::FILE* fileOrNull = std::fopen(arguments[2], "wb");
                if (fileOrNull == nullptr)
                {
                    return 1;
                }
                const usize written = std::fwrite(samples.GetData(), sizeof(float32), samples.GetSize(), fileOrNull);
                if (std::fclose(fileOrNull) != 0 || written != samples.GetSize())
                {
                    return 1;
                }
            }
        }
    }
    Array<float32> distant;
    return Capture(arguments[1], Operation::Distant, IRREGULAR_BLOCK, distant) == true ? 0 : 1;
}
