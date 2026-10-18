// Thanks to Keith Weiner, "Interactive Processing Pipeline for Digital Audio",
// Game Programming Gems 2 (2001), section 6.4, pp. 529-538, for explicit input
// consumption/output production and consumer-requested frame counts. Ludus uses
// preallocated scratch and bounded chunks, rather than the chapter's general
// retry/backfill pipeline. Reviewed scope: docs/architecture/audio-research.md.
#include "internal/voice_kernel.hpp"

#include <cmath>

namespace ludus::audio::internal
{
namespace
{
// Effective loop end: whole clip when LoopEnd == 0.
[[nodiscard]] uint64 EffectiveLoopEnd(uint64 loopEnd, uint64 clipFrames) noexcept
{
    return loopEnd == 0 ? clipFrames : loopEnd;
}
} // namespace

uint32 RenderResidentVoice(const KernelInput& input,
                           float32* mixLR,
                           uint32 frames,
                           float32* scratch,
                           uint32 scratchFrames) noexcept
{
    const ClipSlot* clip = input.Clip;
    VoiceDsp* dsp = input.Dsp;
    if (clip == nullptr || dsp == nullptr || clip->Pcm == nullptr || frames == 0)
    {
        return 0;
    }
    if (dsp->Rs.IsReady() == false)
    {
        return 0;
    }

    const uint32 srcChannels = clip->Channels;
    const uint64 clipFrames = clip->Frames;
    const uint64 loopEnd = EffectiveLoopEnd(input.LoopEnd, clipFrames);
    const uint64 loopBegin = input.LoopBegin;

    // Configure the resampler ratio from rates (never buffer lengths). rateOut =
    // sessionRate / Rate so it consumes ~Rate source frames per output frame.
    const uint32 rateOut =
        static_cast<uint32>(std::lround(static_cast<double>(input.SessionRate) / static_cast<double>(input.Rate)));
    if (rateOut != dsp->ConfiguredRateOut)
    {
        if (dsp->Rs.SetRate(input.SessionRate, rateOut == 0 ? input.SessionRate : rateOut))
        {
            dsp->ConfiguredRateOut = rateOut;
        }
    }

    // Ramp setup. The caller supplies a fixed absolute per-frame step (sign
    // toward the target). A zero step means jump straight to the target.
    float32 curL = input.CurrentGainL != nullptr ? *input.CurrentGainL : input.TargetGainL;
    float32 curR = input.CurrentGainR != nullptr ? *input.CurrentGainR : input.TargetGainR;
    if (input.StepL == 0.0F)
    {
        curL = input.TargetGainL;
    }
    if (input.StepR == 0.0F)
    {
        curR = input.TargetGainR;
    }
    const float32 stepL = curL < input.TargetGainL ? input.StepL : -input.StepL;
    const float32 stepR = curR < input.TargetGainR ? input.StepR : -input.StepR;

    uint32 producedTotal = 0;
    bool endReached = false;

    // Produce output in bounded chunks, assembling wrap-aware source scratch for
    // each chunk and running it through the resampler. The resampler holds the
    // fractional phase/history across chunks and spans.
    while (producedTotal < frames && endReached == false)
    {
        uint32 outWant = frames - producedTotal;
        if (outWant > scratchFrames)
        {
            outWant = scratchFrames;
        }

        // How many source frames does this many output frames need at the ratio?
        uint64 needIn = dsp->Rs.RequiredInputFrames(outWant);
        if (needIn == 0)
        {
            needIn = outWant; // conservative fallback
        }
        if (needIn > scratchFrames)
        {
            needIn = scratchFrames;
        }

        // Assemble `needIn` source frames into scratch, honoring loop/EOF. The
        // scratch is interleaved at the clip's channel count.
        uint64 assembled = 0;
        uint64 cursor = dsp->SourceCursor;
        while (assembled < needIn)
        {
            if (cursor >= clipFrames)
            {
                if (input.Looping && loopEnd > loopBegin)
                {
                    cursor = loopBegin;
                }
                else
                {
                    endReached = true;
                    break;
                }
            }
            // When looping, wrap at loopEnd (half-open), not clip end.
            uint64 regionEnd = clipFrames;
            if (input.Looping && loopEnd > loopBegin)
            {
                regionEnd = loopEnd;
                if (cursor >= loopEnd)
                {
                    cursor = loopBegin;
                }
            }
            uint64 avail = regionEnd - cursor;
            uint64 take = needIn - assembled;
            if (take > avail)
            {
                take = avail;
            }
            const float32* src = clip->Pcm + cursor * srcChannels;
            float32* dst = scratch + assembled * srcChannels;
            for (uint64 i = 0; i < take * srcChannels; ++i)
            {
                dst[i] = src[i];
            }
            assembled += take;
            cursor += take;
        }

        if (assembled == 0)
        {
            endReached = true;
            break;
        }

        // Run the resampler: consume up to `assembled` input, produce up to
        // `outWant` output. The resampler reports exact consumed/produced.
        uint64 inCount = assembled;
        uint64 outCount = outWant;
        // Temporary stereo-or-mono output into the tail of scratch is avoided;
        // produce directly into a small stereo staging area reused per chunk.
        // We resample into the same scratch region shifted past the input.
        float32* outStage = scratch + static_cast<uint64>(scratchFrames) * srcChannels;
        // outStage must hold outWant * srcChannels; the scratch buffer the caller
        // supplies is sized for both (2x). Guard conservatively.
        if (dsp->Rs.Process(scratch, &inCount, outStage, &outCount) == false)
        {
            break;
        }

        // Advance the integer source cursor by the frames the resampler consumed.
        dsp->SourceCursor += inCount;
        // Normalize cursor against loop region for the next chunk.
        if (input.Looping && loopEnd > loopBegin)
        {
            while (dsp->SourceCursor >= loopEnd)
            {
                dsp->SourceCursor = loopBegin + (dsp->SourceCursor - loopEnd);
            }
        }

        if (outCount == 0)
        {
            // No forward progress possible (ran out of input at EOF).
            if (endReached)
            {
                break;
            }
            // Avoid an infinite loop: treat as end.
            endReached = true;
            break;
        }

        // Mix the produced output into the stereo accumulator with the ramp.
        for (uint64 f = 0; f < outCount; ++f)
        {
            float32 l = 0.0F;
            float32 r = 0.0F;
            if (srcChannels == 1)
            {
                const float32 s = outStage[f];
                l = s;
                r = s;
            }
            else
            {
                l = outStage[f * 2 + 0];
                r = outStage[f * 2 + 1];
            }
            const uint32 outFrame = producedTotal + static_cast<uint32>(f);
            mixLR[outFrame * 2 + 0] += l * curL;
            mixLR[outFrame * 2 + 1] += r * curR;
            curL += stepL;
            curR += stepR;
            // Clamp ramp overshoot at the target.
            if ((stepL > 0.0F && curL > input.TargetGainL) || (stepL < 0.0F && curL < input.TargetGainL))
            {
                curL = input.TargetGainL;
            }
            if ((stepR > 0.0F && curR > input.TargetGainR) || (stepR < 0.0F && curR < input.TargetGainR))
            {
                curR = input.TargetGainR;
            }
        }
        producedTotal += static_cast<uint32>(outCount);
    }

    if (input.CurrentGainL != nullptr)
    {
        *input.CurrentGainL = curL;
    }
    if (input.CurrentGainR != nullptr)
    {
        *input.CurrentGainR = curR;
    }
    if (endReached && input.Looping == false)
    {
        dsp->AtEof = true;
    }
    return producedTotal;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): named transport args.
bool AdvanceVirtualVoice(uint64& cursor,
                         double& fraction,
                         uint64 clipFrames,
                         float32 rate,
                         bool looping,
                         uint64 loopBegin,
                         uint64 loopEnd,
                         uint32 frames) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const uint64 end = EffectiveLoopEnd(loopEnd, clipFrames);
    // Advance the cursor by rate * frames source frames, carrying the fractional
    // remainder across calls so the virtual timeline does not drift relative to
    // the mixed (resampled) timeline. Virtual advance does not read PCM, so the
    // subsample phase only needs to be accounted, not interpolated; reentry
    // rebuilds resampler history around the integer cursor.
    const double advance = static_cast<double>(rate) * static_cast<double>(frames) + fraction;
    const uint64 whole = static_cast<uint64>(advance);
    fraction = advance - static_cast<double>(whole);
    cursor += whole;

    if (looping && end > loopBegin)
    {
        while (cursor >= end)
        {
            cursor = loopBegin + (cursor - end);
        }
        return true; // looping voices never expire from advance
    }
    if (cursor >= clipFrames)
    {
        cursor = clipFrames;
        return false; // finite nonloop voice expired
    }
    return true;
}

} // namespace ludus::audio::internal
