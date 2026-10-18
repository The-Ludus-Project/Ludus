#include <ludus/audio/audio_system.h>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include "internal/decode.hpp"
#include "internal/impl.hpp"
#include "internal/log_categories.h"

#include <cmath>
#include <new>

namespace ludus::audio
{
namespace
{
[[nodiscard]] bool IsFinite(float32 v) noexcept
{
    return std::isfinite(v);
}

[[nodiscard]] bool IsFinite(const AudioVec3& v) noexcept
{
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}
} // namespace

// ===========================================================================
// Validation
// ===========================================================================
Status AudioSystem::Impl::ValidateConfig(const SystemConfig& cfg) const noexcept
{
    if (cfg.SampleRate == 0)
    {
        return Status::InvalidArgument;
    }
    const uint32 logical =
        cfg.LogicalVoiceCapacity == 0 ? static_cast<uint32>(LOGICAL_VOICE_CAPACITY) : cfg.LogicalVoiceCapacity;
    const uint32 mixed =
        cfg.MixedVoiceCapacity == 0 ? static_cast<uint32>(MIXED_VOICE_CAPACITY) : cfg.MixedVoiceCapacity;
    const uint32 streams =
        cfg.StreamInstanceCapacity == 0 ? static_cast<uint32>(STREAM_CAPACITY) : cfg.StreamInstanceCapacity;
    if (logical > LOGICAL_VOICE_CAPACITY || logical == 0)
    {
        return Status::InvalidArgument;
    }
    if (mixed > MIXED_VOICE_CAPACITY || mixed == 0)
    {
        return Status::InvalidArgument;
    }
    if (streams > STREAM_CAPACITY)
    {
        return Status::InvalidArgument;
    }
    if (cfg.Buses.size() > BUS_CAPACITY)
    {
        return Status::InvalidArgument;
    }
    if (cfg.Groups.size() > GROUP_CAPACITY)
    {
        return Status::InvalidArgument;
    }

    // Static bus tree: one root, each nonroot parent appears earlier (acyclic,
    // child-before-parent precompute), gains finite in [0,1].
    uint32 roots = 0;
    for (usize i = 0; i < cfg.Buses.size(); ++i)
    {
        const BusConfig& b = cfg.Buses[i];
        if (b.ParentIndex == kNoParent)
        {
            ++roots;
        }
        else
        {
            if (b.ParentIndex >= cfg.Buses.size() || b.ParentIndex >= i)
            {
                return Status::InvalidArgument;
            }
        }
        if (IsFinite(b.BaseGain) == false || IsFinite(b.UserGain) == false || b.BaseGain < 0.0F || b.BaseGain > 1.0F ||
            b.UserGain < 0.0F || b.UserGain > 1.0F)
        {
            return Status::InvalidArgument;
        }
    }
    if (cfg.Buses.empty() == false && roots != 1)
    {
        return Status::InvalidArgument;
    }

    for (usize i = 0; i < cfg.Groups.size(); ++i)
    {
        const GroupConfig& g = cfg.Groups[i];
        if (g.MaxAdmitted == 0 || g.MaxAdmitted > GROUP_MAX_ADMITTED_LIMIT)
        {
            return Status::InvalidArgument;
        }
        if (g.MaxSelected == 0 || g.MaxSelected > GROUP_MAX_SELECTED_LIMIT)
        {
            return Status::InvalidArgument;
        }
    }
    return Status::Ok;
}

bool AudioSystem::Impl::ValidatePlay(const PlayParams& p) const noexcept
{
    if (p.BusIndex >= BusCount || p.GroupIndex >= GroupCount)
    {
        return false;
    }
    if (p.Priority > PRIORITY_MAX)
    {
        return false;
    }
    if (IsFinite(p.Gain) == false || p.Gain < 0.0F || p.Gain > 1.0F)
    {
        return false;
    }
    if (IsFinite(p.Rate) == false || p.Rate < RATE_MIN || p.Rate > RATE_MAX)
    {
        return false;
    }
    if (p.StartFrame != 0)
    {
        const uint64 horizon = RenderFrame + static_cast<uint64>(MAX_SCHEDULE_HORIZON_SECONDS) * SampleRate;
        if (p.StartFrame > horizon)
        {
            return false;
        }
    }
    if (p.Positional)
    {
        if (IsFinite(p.EmitterPosition) == false || IsFinite(p.MinDistance) == false ||
            IsFinite(p.MaxDistance) == false)
        {
            return false;
        }
        if (p.MinDistance < 0.0F || p.MaxDistance <= p.MinDistance)
        {
            return false;
        }
    }
    else if (IsFinite(p.ExplicitPan) == false || p.ExplicitPan < -1.0F || p.ExplicitPan > 1.0F)
    {
        return false;
    }
    return true;
}

// ===========================================================================
// Slot bookkeeping
// ===========================================================================
uint32 AudioSystem::Impl::ReserveVoiceSlot() noexcept
{
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        VoiceSlot& v = Voices[i];
        if (v.InUse == false && v.Generation.IsExhausted() == false)
        {
            if (v.Generation.Advance() == false)
            {
                continue;
            }
            v.InUse = true;
            return i;
        }
    }
    return LogicalCapacity;
}

void AudioSystem::Impl::ReleaseVoiceSlot(uint32 index) noexcept
{
    VoiceSlot& v = Voices[index];
    if (v.IsStream)
    {
        Streams[v.StreamSlotIndex].Data->Cancel.store(true, std::memory_order_release);
    }
    if (v.IsStream == false)
    {
        internal::ClipSlot& c = Clips[v.ClipSlotIndex];
        if (c.Pins > 0)
        {
            --c.Pins;
        }
    }
    v.InUse = false;
    v.State = VoiceState::Terminal;
}

uint32 AudioSystem::Impl::FindClipSlot(ClipHandle h) const noexcept
{
    if (h.IsValid() == false || h.Session != Session || h.Slot >= LogicalCapacity)
    {
        return LogicalCapacity;
    }
    const internal::ClipSlot& c = Clips[h.Slot];
    if (c.InUse == false || c.Generation.Value != h.Generation || c.Retiring)
    {
        // A retiring clip accepts no new admission; existing voices keep their
        // pin (held on the voice slot, not re-looked-up) so their bytes stay
        // valid until the last terminal.
        return LogicalCapacity;
    }
    return h.Slot;
}

uint32 AudioSystem::Impl::FindVoiceSlot(VoiceHandle h) const noexcept
{
    if (h.IsValid() == false || h.Session != Session || h.Slot >= LogicalCapacity)
    {
        return LogicalCapacity;
    }
    const VoiceSlot& v = Voices[h.Slot];
    if (v.Generation.Value != h.Generation)
    {
        return LogicalCapacity;
    }
    return h.Slot;
}

uint32 AudioSystem::Impl::FindModifierSlot(ModifierHandle h) const noexcept
{
    if (h.IsValid() == false || h.Session != Session || h.Slot >= MODIFIER_CAPACITY)
    {
        return MODIFIER_CAPACITY;
    }
    const internal::ModifierState& m = Modifiers[h.Slot];
    if (m.InUse == false || m.Generation.Value != h.Generation)
    {
        return MODIFIER_CAPACITY;
    }
    return h.Slot;
}

VoiceHandle AudioSystem::Impl::MakeVoiceHandle(uint32 slot) const noexcept
{
    return VoiceHandle{Session, slot, Voices[slot].Generation.Value};
}

ClipHandle AudioSystem::Impl::MakeClipHandle(uint32 slot) const noexcept
{
    return ClipHandle{Session, slot, Clips[slot].Generation.Value};
}

// ===========================================================================
// Bus gains (design section 8)
// ===========================================================================
void AudioSystem::Impl::RecomputeBusGains() noexcept
{
    for (uint32 b = 0; b < BusCount; ++b)
    {
        internal::BusState& bus = Buses[b];
        float32 db = 0.0F;
        for (uint32 m = 0; m < MODIFIER_CAPACITY; ++m)
        {
            const internal::ModifierState& mod = Modifiers[m];
            if (mod.InUse == false || mod.HasBus[b] == false)
            {
                continue;
            }
            db += mod.Weight * mod.Db[b];
        }
        if (db < MODIFIER_DB_MIN)
        {
            db = MODIFIER_DB_MIN;
        }
        if (db > MODIFIER_DB_MAX)
        {
            db = MODIFIER_DB_MAX;
        }
        const float32 modGain = std::pow(10.0F, db / 20.0F);
        const float32 user = bus.UserGain;
        const float32 target = user == 0.0F ? 0.0F : user * bus.BaseGain * modGain;
        bus.TargetEffective = target;
        bus.CurrentEffective = target; // A1 boundary-granular; A3 adds ramps
    }
}

float32 AudioSystem::Impl::BusChainGain(uint32 busIndex) const noexcept
{
    float32 g = 1.0F;
    uint32 b = busIndex;
    uint32 guard = 0;
    while (b != kNoParent && guard <= BusCount)
    {
        g *= Buses[b].CurrentEffective;
        b = Buses[b].ParentIndex;
        ++guard;
    }
    return g;
}

} // namespace ludus::audio
