#include <ludus/audio/audio_system.h>

#include "internal/impl.hpp"

#include <cmath>

// Spatial math and audibility estimation, per .kiro/specs/audio/design.md
// section 6 and 7.2. Mono emitters use meter coordinates; distance attenuation
// is a finite tunable envelope (1-t)^2; panning is equal-power from the
// displacement onto the listener Right vector. Separate panning/attenuation
// positions with a per-voice attenuation-origin choice. The true emitter
// position is never rewritten. Selection envelopes are excluded from the score.
// Thanks to Guy Somberg, "Listeners for Third-Person Cameras", Game Audio
// Programming: Principles and Practices, chapter 11, pp. 197-208, for separating
// attenuation origin from panning pose. Ludus evaluates both displacements
// directly and preserves the emitter position; this is a stereo policy, not
// fabricated middleware positions or an acoustics implementation.
// Reviewed design influences: docs/architecture/audio-research.md.

namespace ludus::audio
{
namespace
{
struct Vec3 final
{
    float32 X = 0.0F;
    float32 Y = 0.0F;
    float32 Z = 0.0F;
};

[[nodiscard]] Vec3 Sub(const AudioVec3& a, const AudioVec3& b) noexcept
{
    return Vec3{a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}

[[nodiscard]] float32 Length(const Vec3& v) noexcept
{
    return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
}

[[nodiscard]] float32 Dot(const Vec3& a, const Vec3& b) noexcept
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

[[nodiscard]] Vec3 Cross(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

[[nodiscard]] Vec3 Normalize(const Vec3& v) noexcept
{
    const float32 len = Length(v);
    if (len <= 0.0F || std::isfinite(len) == false)
    {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    return Vec3{v.X / len, v.Y / len, v.Z / len};
}

[[nodiscard]] float32 Clamp(float32 v, float32 lo, float32 hi) noexcept
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}
} // namespace

void AudioSystem::Impl::ComputeVoiceGains(VoiceSlot& v, float32& outGainL, float32& outGainR) noexcept
{
    const float32 busChain = BusChainGain(v.BusIndex);
    v.ScoreBusChainGain = busChain;
    v.ScoreVoiceGain = v.Gain;
    v.ScorePreparedPeak = v.PreparedPeakScore;

    // Output gains exclude the bus-chain gain: the bus stage applies each bus
    // gain exactly once at its edge (design section 8). The bus-chain gain is
    // retained only as a selection-score contribution.
    if (v.Positional == false)
    {
        v.ScoreDistanceGain = 1.0F;
        const bool stereoBed = Clips[v.ClipSlotIndex].Channels == 2;
        if (stereoBed)
        {
            // Stereo bed: preserve L/R with a scalar gain; do not collapse it
            // through equal-power panning (design section 6).
            v.Pan = 0.0F;
            outGainL = v.Gain;
            outGainR = v.Gain;
        }
        else
        {
            // Mono nonspatial: explicit equal-power pan.
            const float32 p = Clamp(v.ExplicitPan, -1.0F, 1.0F);
            v.Pan = p;
            outGainL = v.Gain * std::sqrt((1.0F - p) * 0.5F);
            outGainR = v.Gain * std::sqrt((1.0F + p) * 0.5F);
        }
        return;
    }

    // Positional. Attenuation uses the chosen origin; panning uses the panning
    // position. Both displacements are from the true emitter position.
    const AudioVec3 attnOrigin =
        v.Origin == AttenuationOrigin::PanningPosition ? Listener.PanningPosition : Listener.AttenuationPosition;
    const Vec3 attnDisp = Sub(v.EmitterPosition, attnOrigin);
    const float32 dist = Length(attnDisp);

    float32 t = 0.0F;
    if (v.MaxDistance > v.MinDistance)
    {
        t = Clamp((dist - v.MinDistance) / (v.MaxDistance - v.MinDistance), 0.0F, 1.0F);
    }
    const float32 attn = (1.0F - t) * (1.0F - t); // (1-t)^2
    v.ScoreDistanceGain = attn;

    // Pan: project the (normalized) displacement-from-panning-position onto the
    // listener Right vector. Right = normalize(cross(Forward, Up)) with the
    // documented handedness (+X right, +Y up, Forward = -Z).
    const Vec3 fwd = Normalize(Vec3{Listener.Forward.X, Listener.Forward.Y, Listener.Forward.Z});
    const Vec3 up = Normalize(Vec3{Listener.Up.X, Listener.Up.Y, Listener.Up.Z});
    const Vec3 right = Normalize(Cross(fwd, up));
    const Vec3 panDisp = Sub(v.EmitterPosition, Listener.PanningPosition);
    const Vec3 panDir = Normalize(panDisp);
    float32 p = 0.0F;
    if (Length(panDisp) > 0.0F)
    {
        p = Clamp(Dot(panDir, right), -1.0F, 1.0F);
    }
    v.Pan = p;
    const float32 gl = std::sqrt((1.0F - p) * 0.5F);
    const float32 gr = std::sqrt((1.0F + p) * 0.5F);

    const float32 base = v.Gain * attn; // bus gain applied by the bus stage
    outGainL = base * gl;
    outGainR = base * gr;
}

float32 AudioSystem::Impl::EstimateAudibility(const VoiceSlot& v) const noexcept
{
    // Audibility estimate from voice gain, prepared clip peak, distance gain and
    // effective bus-chain gain. Excludes the selection/start/stop fade envelope
    // (design 7.2) so a fading voice is not driven permanently virtual.
    const float32 distGain = v.ScoreDistanceGain <= 0.0F ? 0.0F : v.ScoreDistanceGain;
    const float32 est = v.Gain * v.PreparedPeakScore * distGain * BusChainGain(v.BusIndex);
    return est;
}

uint32 AudioSystem::Impl::BindPhysical(uint32 logicalSlot) noexcept
{
    for (uint32 i = 0; i < MixedCapacity; ++i)
    {
        internal::PhysicalVoice& ph = Physical[i];
        if (ph.Bound == false)
        {
            ph.Bound = true;
            ph.LogicalSlot = logicalSlot;
            const internal::ClipSlot& c = Clips[Voices[logicalSlot].ClipSlotIndex];
            ph.ResetFor(c.Channels);
            // Resume from the authoritative cursor (devirtualization) rather than
            // zero: set the DSP cursor to the logical voice's advanced cursor.
            internal::VoiceDsp& d = ph.DspFor(c.Channels);
            d.SourceCursor = Voices[logicalSlot].CursorFrame;
            d.AtEof = false;
            return i;
        }
    }
    return MixedCapacity;
}

void AudioSystem::Impl::UnbindPhysical(uint32 physicalIndex) noexcept
{
    if (physicalIndex < MixedCapacity)
    {
        Physical[physicalIndex].Bound = false;
    }
}

uint32 AudioSystem::Impl::FindPhysicalFor(uint32 logicalSlot) const noexcept
{
    for (uint32 i = 0; i < MixedCapacity; ++i)
    {
        if (Physical[i].Bound && Physical[i].LogicalSlot == logicalSlot)
        {
            return i;
        }
    }
    return MixedCapacity;
}

bool AudioSystem::Impl::EnsurePhysicalInitialized() noexcept
{
    if (PhysicalInitialized)
    {
        return true;
    }
    for (uint32 i = 0; i < MixedCapacity; ++i)
    {
        if (Physical[i].Init(SampleRate, LpfOrder) == false)
        {
            return false;
        }
    }
    PhysicalInitialized = true;
    return true;
}

} // namespace ludus::audio
