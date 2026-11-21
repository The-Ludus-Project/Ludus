#pragma once
#include <ludus/foundation/containers/static_array.hpp>

// The control owner's private runtime state and renderer, per
// .kiro/specs/audio/design.md sections 3-11. This aggregates the SPSC command
// ring, the resident clip store, voice/group/bus state, session bookkeeping,
// durable terminal/stop mailboxes, and the bounded render entry. All fixed
// storage is allocated once at Initialize; the warm paths allocate/free nothing.

#include <ludus/audio/audio_debug.h>
#include <ludus/audio/audio_system.h>
#include <ludus/audio/audio_types.h>
#include <ludus/foundation/base/types.h>

#include "internal/clip_store.hpp"
#include "internal/handles.hpp"
#include "internal/mixer.hpp"
#include "internal/spsc_ring.hpp"
#include "internal/stream.hpp"

#include <atomic>
#include <span>

namespace ludus::audio::internal
{
using ludus::foundation::float32;
using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

// A command record as published into the ring. It mirrors the public Command
// but with batch-local references resolved to concrete slot indices and asset
// pins already reserved on the owner. The published payload is immutable until
// consumption. ModifierValues are copied into a bounded inline array.
struct QueuedCommand final
{
    CommandKind Kind = CommandKind::Play;

    // Batch metadata (design section 4): the first record of a published batch
    // carries the whole batch's record count in BatchLength (other records set
    // 0). The consumer reads BatchLength on the leading record and defers the
    // ENTIRE batch if it would exceed the remaining 64-record boundary budget,
    // so a batch is never split across a control boundary.
    uint32 BatchLength = 0;

    // Resolved voice slot (for Play this is the reserved slot; for Update/Stop
    // the targeted slot). SlotGeneration disambiguates reuse.
    uint32 VoiceSlotIndex = 0;
    uint32 VoiceGeneration = 0;

    // Play payload (resolved).
    uint32 ClipSlotIndex = 0;
    uint32 BusIndex = 0;
    uint32 GroupIndex = 0;
    uint8 Priority = 0;
    float32 Gain = 1.0F;
    float32 Rate = 1.0F;
    bool Looping = false;
    VirtualPolicy Policy = VirtualPolicy::KillWhenInaudible;
    uint64 StartFrame = 0;
    bool Positional = false;
    AudioVec3 EmitterPosition{};
    AttenuationOrigin Origin = AttenuationOrigin::Listener;
    float32 MinDistance = 1.0F;
    float32 MaxDistance = 100.0F;
    float32 ExplicitPan = 0.0F;
    uint32 PolicyTag = 0;
    bool IsStream = false;
    uint32 StreamSlotIndex = 0;
    uint32 AdmissionEpoch = 0;

    const float32* Pcm = nullptr;
    uint64 Frames = 0, LoopBegin = 0, LoopEnd = 0;
    uint32 Channels = 0;
    float32 Peak = 0.0F;
    StreamData* Stream = nullptr;
    // Update payload.
    ludus::foundation::StaticArray<float32, STREAM_CAPACITY> LayerGains{};
    uint32 LayerRamp = 0;
    bool HasLayerGains = false;
    bool SetGain = false;
    bool SetRate = false;
    bool SetPriority = false;
    bool SetPosition = false;

    // Listener payload.
    ListenerPose Listener{};

    // Bus/modifier payload.
    float32 BusGain = 1.0F;
    uint32 ModifierSlotIndex = 0;
    uint32 ModifierGeneration = 0;
    float32 ModifierWeight = 1.0F;
    uint32 ModifierValueCount = 0;
    ModifierValue ModifierValues[BUS_CAPACITY]{};
};

// Debug snapshot record published through the small snapshot ring.
struct SnapshotRecord final
{
    SystemSnapshot System{};
};

// Active attenuation modifier instance (design section 8).
struct ModifierState final
{
    SlotGeneration Generation{};
    bool InUse = false;
    bool Removing = false;
    float32 Weight = 0.0F;
    float32 TargetWeight = 0.0F;
    float32 Db[BUS_CAPACITY] = {};
    bool HasBus[BUS_CAPACITY] = {};
    TerminalMailbox Terminal{};
};

} // namespace ludus::audio::internal
