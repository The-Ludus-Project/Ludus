#include <ludus/audio/audio_debug.h>
// Thanks to Scott Patterson, "Game Audio Design Patterns", Game Programming
// Gems 2 (2001), section 6.1, pp. 514-520, for separating sound identity from
// playback handles behind a small command facade. Ludus uses typed session/
// generation checks and control-thread polling rather than render callbacks.
// Reviewed design influences: docs/architecture/audio-research.md.
#include <ludus/audio/audio_system.h>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include "internal/impl.hpp"
#include "internal/log_categories.h"
#include "internal/prepared.hpp"

#include <cmath>
#include <new>

// The public AudioSystem façade: lifecycle, preparation, submission,
// cancellation, service, queries, offline render and the owner-side user-gain
// helper. One owner thread makes all public calls (design section 3). Hot
// submission/query paths allocate/free nothing and take no lock.

namespace ludus::audio
{
using internal::QueuedCommand;

// ===========================================================================
// Construction / lifecycle
// ===========================================================================
AudioSystem::AudioSystem() noexcept = default;

AudioSystem::~AudioSystem() noexcept = default;

Status AudioSystem::Initialize(const SystemConfig& config) noexcept
{
    if (mImpl)
    {
        return Status::InvalidArgument; // already initialized
    }

    // Allocate the single fixed storage block. The only allocation that is not a
    // cold asset preparation. Report failure explicitly; never throw.
    Impl* impl = new (std::nothrow) Impl();
    if (impl == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_AUDIO, "Failed to allocate audio runtime storage");
        return Status::OutOfMemory;
    }

    const Status cfgStatus = impl->ValidateConfig(config);
    if (IsOk(cfgStatus) == false)
    {
        delete impl;
        return cfgStatus;
    }

    const uint32 session = internal::SessionSource::Next();
    if (session == 0)
    {
        delete impl;
        return Status::SequenceExhausted;
    }

    impl->Config = config;
    impl->Session = session;
    impl->SystemMode = config.SystemMode;
    impl->SampleRate = config.SampleRate;
    impl->LogicalCapacity =
        config.LogicalVoiceCapacity == 0 ? static_cast<uint32>(LOGICAL_VOICE_CAPACITY) : config.LogicalVoiceCapacity;
    impl->MixedCapacity =
        config.MixedVoiceCapacity == 0 ? static_cast<uint32>(MIXED_VOICE_CAPACITY) : config.MixedVoiceCapacity;
    impl->StreamInstances =
        config.StreamInstanceCapacity == 0 ? static_cast<uint32>(STREAM_CAPACITY) : config.StreamInstanceCapacity;

    // Install the bus tree (empty => a single implicit root).
    if (config.Buses.empty())
    {
        impl->BusCount = 1;
        impl->Buses[0] = internal::BusState{};
        impl->Buses[0].Id = 0;
        impl->Buses[0].ParentIndex = kNoParent;
    }
    else
    {
        impl->BusCount = static_cast<uint32>(config.Buses.size());
        for (uint32 i = 0; i < impl->BusCount; ++i)
        {
            const BusConfig& b = config.Buses[i];
            internal::BusState& bus = impl->Buses[i];
            bus = internal::BusState{};
            bus.Id = b.Id;
            bus.ParentIndex = b.ParentIndex;
            bus.UserGain = b.UserGain;
            bus.BaseGain = b.BaseGain;
            bus.TargetBaseGain = b.BaseGain;
        }
        // Precompute depth (parent appears earlier, validated).
        for (uint32 i = 0; i < impl->BusCount; ++i)
        {
            internal::BusState& bus = impl->Buses[i];
            bus.Depth = bus.ParentIndex == kNoParent ? 0 : impl->Buses[bus.ParentIndex].Depth + 1;
        }
    }

    // Install groups (empty => one default group 256/56).
    if (config.Groups.empty())
    {
        impl->GroupCount = 1;
        impl->Groups[0] = internal::GroupState{};
    }
    else
    {
        impl->GroupCount = static_cast<uint32>(config.Groups.size());
        for (uint32 i = 0; i < impl->GroupCount; ++i)
        {
            impl->Groups[i] = internal::GroupState{};
            impl->Groups[i].MaxAdmitted = config.Groups[i].MaxAdmitted;
            impl->Groups[i].MaxSelected = config.Groups[i].MaxSelected;
        }
    }

    impl->RecomputeBusGains();

    switch (config.SystemMode)
    {
        case Mode::Offline:
            impl->State = SystemState::Ready;
            break;
        case Mode::Disabled:
            impl->State = SystemState::Disabled;
            break;
        case Mode::Device: {
            const auto status = impl->StartDevice();
            if (status != Status::Ok)
            {
                delete impl;
                return status;
            }
            impl->State = SystemState::Ready;
            break;
        }
    }

    mImpl = ludus::foundation::core::UniquePtr<Impl>(impl);
    return Status::Ok;
}

SystemState AudioSystem::GetState() const noexcept
{
    return mImpl ? mImpl->State : SystemState::Disabled;
}

Mode AudioSystem::GetMode() const noexcept
{
    return mImpl ? mImpl->SystemMode : Mode::Disabled;
}

uint32 AudioSystem::GetSession() const noexcept
{
    return mImpl ? mImpl->Session : 0;
}

uint32 AudioSystem::GetSampleRate() const noexcept
{
    return mImpl ? mImpl->SampleRate : 0;
}

// ===========================================================================
// Preparation (cold)
// ===========================================================================
Status
AudioSystem::PrepareClip(std::span<const uint8> encoded, const ClipDescriptor& descriptor, ClipHandle& outClip) noexcept
{
    outClip = {};
    if (mImpl.Get() == nullptr || mImpl->State != SystemState::Ready)
    {
        return Status::NotReady;
    }
    PreparedClip prepared;
    const auto status = prepared.Decode(encoded, descriptor, mImpl->SampleRate);
    return status == Status::Ok ? InstallClip(prepared, outClip) : status;
}
Status AudioSystem::InstallClip(PreparedClip& prepared, ClipHandle& outClip) noexcept
{
    outClip = {};
    if (mImpl.Get() == nullptr || mImpl->State != SystemState::Ready)
    {
        return Status::NotReady;
    }
    if (prepared.mImpl == nullptr || prepared.mImpl->Rate != mImpl->SampleRate)
    {
        return Status::InvalidArgument;
    }
    auto& data = *prepared.mImpl;
    auto& decoded = data.Decoded;
    // Find a free clip slot.
    uint32 slot = mImpl->LogicalCapacity;
    for (uint32 i = 0; i < mImpl->LogicalCapacity; ++i)
    {
        if (mImpl->Clips[i].InUse == false && mImpl->Clips[i].Generation.IsExhausted() == false)
        {
            slot = i;
            break;
        }
    }
    if (slot == mImpl->LogicalCapacity)
    {
        return Status::AssetCapacity;
    }

    uint64 residentBytes = decoded.Frames * decoded.Channels * sizeof(float32);
    for (uint32 i = 0; i < mImpl->LogicalCapacity; ++i)
    {
        residentBytes += mImpl->Clips[i].SampleValues * sizeof(float32);
    }
    if (residentBytes > RESIDENT_PCM_CAP_BYTES)
    {

        return Status::AssetCapacity;
    }
    internal::ClipSlot& c = mImpl->Clips[slot];
    if (c.Generation.Advance() == false)
    {

        return Status::AssetCapacity;
    }
    c.InUse = true;
    c.Retiring = false;
    c.Layout = decoded.Layout;
    c.Channels = decoded.Channels;
    c.Frames = decoded.Frames;
    c.SampleValues = decoded.Frames * decoded.Channels;
    c.Pcm = decoded.Pcm;
    c.PreparedPeak = decoded.PreparedPeak;
    c.AssetHash = data.AssetHash;
    c.Pins = 0;

    c.LoopBegin = data.LoopBegin;
    c.LoopEnd = data.LoopEnd;

    decoded.Pcm = nullptr;
    delete prepared.mImpl;
    prepared.mImpl = nullptr;
    outClip = mImpl->MakeClipHandle(slot);
    return Status::Ok;
}

Status AudioSystem::PrepareStream(std::span<const uint8> encoded,
                                  const StreamDescriptor& descriptor,
                                  StreamHandle& outStream) noexcept
{
    outStream = {};
    if (mImpl.Get() == nullptr || mImpl->State != SystemState::Ready)
    {
        return Status::NotReady;
    }
    auto& state = *mImpl;
    uint64 encodedTotal = encoded.size();
    for (const auto& existing : state.Streams)
    {
        if (existing.InUse)
        {
            encodedTotal += existing.Data->EncodedBytes;
        }
    }
    if (encodedTotal > BROWSER_ENCODED_CAP_BYTES)
    {
        return Status::AssetCapacity;
    }
    uint32 index = state.StreamInstances;
    for (uint32 i = 0; i < state.StreamInstances; ++i)
    {
        if (state.Streams[i].InUse == false && state.Streams[i].Generation < 0xFFFFFFFEU)
        {
            index = i;
            break;
        }
    }
    if (index == state.StreamInstances)
    {
        return Status::StreamCapacity;
    }
    auto* data = new (std::nothrow) internal::StreamData();
    if (data == nullptr)
    {
        return Status::OutOfMemory;
    }
    auto status = data->Open(encoded, descriptor, state.SampleRate);
    if (status != Status::Ok)
    {
        delete data;
        return status;
    }
    status = state.StartWorker();
    if (status != Status::Ok)
    {
        delete data;
        return status;
    }
    auto& slot = state.Streams[index];
    ++slot.Generation;
    slot.InUse = true;
    slot.Played = false;
    slot.Retiring = false;
    slot.Data = data;
    state.WorkerStreams[index].store(data, std::memory_order_release);
    outStream = {state.Session, index, slot.Generation};
    return Status::Ok;
}

Status AudioSystem::PrepareStream(foundation::core::UniquePtr<StreamInput> input,
                                  const StreamDescriptor& descriptor,
                                  StreamHandle& outStream) noexcept
{
    outStream = {};
    if (mImpl.Get() == nullptr || mImpl->State != SystemState::Ready)
    {
        return Status::NotReady;
    }
    auto& state = *mImpl;
    uint32 index = state.StreamInstances;
    for (uint32 i = 0; i < state.StreamInstances; ++i)
    {
        if (state.Streams[i].InUse == false && state.Streams[i].Generation < 0xFFFFFFFEU)
        {
            index = i;
            break;
        }
    }
    if (index == state.StreamInstances)
    {
        return Status::StreamCapacity;
    }
    auto* data = new (std::nothrow) internal::StreamData();
    if (data == nullptr)
    {
        return Status::OutOfMemory;
    }
    auto status = data->Open(input.Release(), descriptor, state.SampleRate);
    if (status != Status::Ok)
    {
        delete data;
        return status;
    }
    status = state.StartWorker();
    if (status != Status::Ok)
    {
        delete data;
        return status;
    }
    auto& slot = state.Streams[index];
    ++slot.Generation;
    slot.InUse = true;
    slot.Played = false;
    slot.Retiring = false;
    slot.Data = data;
    state.WorkerStreams[index].store(data, std::memory_order_release);
    outStream = {state.Session, index, slot.Generation};
    return Status::Ok;
}

bool AudioSystem::IsClipReady(ClipHandle clip) const noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return false;
    }
    return mImpl->FindClipSlot(clip) < mImpl->LogicalCapacity;
}

Status AudioSystem::RetireClip(ClipHandle clip) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const uint32 slot = mImpl->FindClipSlot(clip);
    if (slot >= mImpl->LogicalCapacity)
    {
        return Status::InvalidHandle;
    }
    internal::ClipSlot& c = mImpl->Clips[slot];
    c.Retiring = true; // close new admission; Service reclaims after pins drain
    return Status::Ok;
}

Status AudioSystem::RetireStream(StreamHandle stream) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const auto slot = mImpl->FindStream(stream);
    if (slot >= mImpl->StreamInstances)
    {
        return Status::InvalidHandle;
    }
    auto& instance = mImpl->Streams[slot];
    instance.Retiring = true;
    // Active playback retains the ring; cancellation follows durable terminal.
    if (instance.Played == false)
    {
        instance.Data->Cancel.store(true, std::memory_order_release);
    }
    return Status::Ok;
}

// ===========================================================================
// Submission (warm)
// ===========================================================================
BatchResult AudioSystem::TrySubmitBatch(std::span<const Command> commands) noexcept
{
    BatchResult result{};
    if (mImpl.Get() == nullptr)
    {
        result.Result = Status::NotReady;
        return result;
    }
    Impl& s = *mImpl;
    s.LastBatchVoiceCount = 0;
    if (s.State != SystemState::Ready && s.SystemMode != Mode::Disabled)
    {
        result.Result = Status::NotReady;
        return result;
    }

    if (commands.size() > MAX_BATCH_RECORDS)
    {
        result.Result = Status::InvalidArgument;
        ++s.RejectedCommands;
        return result;
    }
    if (commands.empty())
    {
        result.Result = Status::Ok;
        return result;
    }
    if (s.SystemMode == Mode::Disabled)
    {
        result.Result = Status::Disabled;
        return result;
    }

    s.CollectRender();
    // --- Phase 1: validate the whole batch, no mutation --------------------
    // Validate every record and batch-local references before any reservation.
    for (usize i = 0; i < commands.size(); ++i)
    {
        const Command& cmd = commands[i];
        switch (cmd.Kind)
        {
            case CommandKind::Play:
                if (s.ValidatePlay(cmd.Play) == false)
                {
                    result.Result = Status::InvalidArgument;
                    ++s.RejectedCommands;
                    return result;
                }
                if (s.FindClipSlot(cmd.Play.Clip) >= s.LogicalCapacity)
                {
                    result.Result = Status::InvalidHandle;
                    ++s.RejectedCommands;
                    return result;
                }
                break;
            case CommandKind::UpdateVoice: {
                const int32 local = cmd.Update.BatchLocalPlayIndex;
                if (local >= 0)
                {
                    if (static_cast<usize>(local) >= i || commands[local].Kind != CommandKind::Play)
                    {
                        result.Result = Status::InvalidArgument;
                        ++s.RejectedCommands;
                        return result;
                    }
                }
                else if (s.FindVoiceSlot(cmd.Update.Voice) >= s.LogicalCapacity)
                {
                    result.Result = Status::InvalidHandle;
                    ++s.RejectedCommands;
                    return result;
                }
                if (local < 0)
                {
                    const auto slot = s.FindVoiceSlot(cmd.Update.Voice);
                    if (s.Voices[slot].IsStream && (cmd.Update.SetRate || cmd.Update.SetPosition))
                    {
                        result.Result = Status::InvalidArgument;
                        ++s.RejectedCommands;
                        return result;
                    }
                }
                break;
            }
            case CommandKind::StopVoice: {
                const int32 local = cmd.StopBatchLocalPlayIndex;
                if (local >= 0)
                {
                    if (static_cast<usize>(local) >= i || commands[local].Kind != CommandKind::Play)
                    {
                        result.Result = Status::InvalidArgument;
                        ++s.RejectedCommands;
                        return result;
                    }
                }
                else if (s.FindVoiceSlot(cmd.StopTarget) >= s.LogicalCapacity)
                {
                    result.Result = Status::InvalidHandle;
                    ++s.RejectedCommands;
                    return result;
                }
                break;
            }
            case CommandKind::SetBusBaseGain:
            case CommandKind::SetBusUserGain:
                if (cmd.BusIndex >= s.BusCount || std::isfinite(cmd.BusGain) == false || cmd.BusGain < 0.0F ||
                    cmd.BusGain > 1.0F)
                {
                    result.Result = Status::InvalidArgument;
                    ++s.RejectedCommands;
                    return result;
                }
                break;
            case CommandKind::SetListener:
                // Orientation validated lazily; reject NaN/Inf and degenerate basis.
                {
                    const ListenerPose& l = cmd.Listener;
                    if (std::isfinite(l.Forward.X) == false || std::isfinite(l.Forward.Y) == false ||
                        std::isfinite(l.Forward.Z) == false || std::isfinite(l.Up.X) == false ||
                        std::isfinite(l.Up.Y) == false || std::isfinite(l.Up.Z) == false)
                    {
                        result.Result = Status::InvalidArgument;
                        ++s.RejectedCommands;
                        return result;
                    }
                }
                break;
            case CommandKind::AddModifier:
            case CommandKind::UpdateModifier:
            case CommandKind::RemoveModifier:
                if (cmd.ModifierValues.size() > BUS_CAPACITY)
                {
                    result.Result = Status::InvalidArgument;
                    ++s.RejectedCommands;
                    return result;
                }
                break;
        }
    }

    // --- Phase 2: transactional reservation --------------------------------
    // Reserve voice slots, asset pins and group MaxAdmitted charges for every
    // Play. On any failure, roll everything back and publish nothing.
    uint32 reservedSlots[MAX_BATCH_RECORDS]{};
    uint32 reservedCount = 0;
    // Per-group tentative charge, so a batch with several Plays in one group is
    // validated against MaxAdmitted as a whole.
    uint32 groupCharge[GROUP_CAPACITY]{};

    auto rollback = [&]() noexcept {
        for (uint32 r = 0; r < reservedCount; ++r)
        {
            const uint32 slot = reservedSlots[r];
            VoiceSlot& v = s.Voices[slot];
            if (v.IsStream == false)
            {
                internal::ClipSlot& c = s.Clips[v.ClipSlotIndex];
                if (c.Pins > 0)
                {
                    --c.Pins;
                }
            }
            v.InUse = false;
            v.State = VoiceState::Terminal;
        }
        for (uint32 g = 0; g < s.GroupCount; ++g)
        {
            // tentative charges were not yet applied to Groups[].Admitted
            (void)groupCharge[g];
        }
    };

    // Map each command index to its reserved voice slot (for local references).
    uint32 slotForCommand[MAX_BATCH_RECORDS]{};
    for (uint32 i = 0; i < MAX_BATCH_RECORDS; ++i)
    {
        slotForCommand[i] = s.LogicalCapacity;
    }

    uint32 scratchCount = 0;
    for (usize i = 0; i < commands.size(); ++i)
    {
        const Command& cmd = commands[i];
        if (cmd.Kind != CommandKind::Play)
        {
            continue;
        }
        const PlayParams& p = cmd.Play;

        // Group MaxAdmitted check (tentative + already-admitted).
        const uint32 g = p.GroupIndex;
        if (s.Groups[g].Admitted + groupCharge[g] + 1 > s.Groups[g].MaxAdmitted)
        {
            rollback();
            result.Result = Status::GroupCapacity;
            ++s.RejectedCommands;
            return result;
        }

        const uint32 slot = s.ReserveVoiceSlot();
        if (slot >= s.LogicalCapacity)
        {
            rollback();
            result.Result = Status::VoiceCapacity;
            ++s.RejectedCommands;
            return result;
        }
        reservedSlots[reservedCount++] = slot;
        slotForCommand[i] = slot;
        ++groupCharge[g];

        const uint32 clipSlot = s.FindClipSlot(p.Clip);
        internal::ClipSlot& c = s.Clips[clipSlot];
        ++c.Pins; // pin the asset for the pending play

        VoiceSlot& v = s.Voices[slot];
        v.State = VoiceState::Pending;
        v.LastCause = StateCause::None;
        v.ClipSlotIndex = clipSlot;
        v.BusIndex = p.BusIndex;
        v.GroupIndex = p.GroupIndex;
        v.Priority = p.Priority;
        v.Gain = p.Gain;
        v.Rate = p.Rate;
        v.Looping = p.Looping;
        v.Policy = p.Policy;
        v.Positional = p.Positional;
        v.Origin = p.Origin;
        v.MinDistance = p.MinDistance;
        v.MaxDistance = p.MaxDistance;
        v.ExplicitPan = p.ExplicitPan;
        v.EmitterPosition = p.EmitterPosition;
        v.PolicyTag = p.PolicyTag;
        v.IsStream = false;
        v.StartFrame = p.StartFrame;
        v.LateStart = false;
        v.Lateness = 0;
        v.CursorFrame = 0;
        v.CursorFraction = 0.0;
        v.AdmissionEpoch = s.StopAllEpoch;
        v.FadeGain = 0.0F;
        v.FadeTarget = 1.0F;
        v.PlayDrained = false;
        v.PreparedPeakScore = c.PreparedPeak;
        v.Stop.StopGeneration.store(0, std::memory_order_relaxed);
        v.Terminal.TerminalGeneration.store(0, std::memory_order_relaxed);

        // Record the returned handle in submission order.
        s.LastBatchVoices[s.LastBatchVoiceCount++] = s.MakeVoiceHandle(slot);
    }

    // Build the queued records in ORIGINAL command order (never Plays-first):
    // reordering a mixed batch would break Play/Stop ordering (design section 4).
    // Batch-local Update/Stop references resolve to the slot reserved above.
    for (usize i = 0; i < commands.size(); ++i)
    {
        const Command& cmd = commands[i];
        QueuedCommand& q = s.BatchScratch[scratchCount++];
        q = QueuedCommand{};
        q.Kind = cmd.Kind;
        switch (cmd.Kind)
        {
            case CommandKind::Play: {
                const PlayParams& p = cmd.Play;
                const uint32 slot = slotForCommand[i];
                const VoiceSlot& v = s.Voices[slot];
                q.VoiceSlotIndex = slot;
                q.VoiceGeneration = v.Generation.Value;
                q.ClipSlotIndex = v.ClipSlotIndex;
                q.BusIndex = p.BusIndex;
                q.GroupIndex = p.GroupIndex;
                q.Priority = p.Priority;
                q.Gain = p.Gain;
                q.Rate = p.Rate;
                q.Looping = p.Looping;
                q.Policy = p.Policy;
                q.StartFrame = p.StartFrame;
                q.Positional = p.Positional;
                q.EmitterPosition = p.EmitterPosition;
                q.Origin = p.Origin;
                q.MinDistance = p.MinDistance;
                q.MaxDistance = p.MaxDistance;
                q.ExplicitPan = p.ExplicitPan;
                q.PolicyTag = p.PolicyTag;
                const auto& clip = s.Clips[v.ClipSlotIndex];
                q.Pcm = clip.Pcm;
                q.Frames = clip.Frames;
                q.Channels = clip.Channels;
                q.LoopBegin = clip.LoopBegin;
                q.LoopEnd = clip.LoopEnd;
                q.Peak = clip.PreparedPeak;
                q.IsStream = false;
                q.AdmissionEpoch = s.StopAllEpoch;
                break;
            }
            case CommandKind::UpdateVoice: {
                uint32 slot = s.LogicalCapacity;
                if (cmd.Update.BatchLocalPlayIndex >= 0)
                {
                    slot = slotForCommand[cmd.Update.BatchLocalPlayIndex];
                }
                else
                {
                    slot = s.FindVoiceSlot(cmd.Update.Voice);
                }
                q.VoiceSlotIndex = slot;
                q.VoiceGeneration = slot < s.LogicalCapacity ? s.Voices[slot].Generation.Value : 0;
                q.SetGain = cmd.Update.SetGain;
                q.Gain = cmd.Update.Gain;
                q.SetRate = cmd.Update.SetRate;
                q.Rate = cmd.Update.Rate;
                q.SetPriority = cmd.Update.SetPriority;
                q.Priority = cmd.Update.Priority;
                q.SetPosition = cmd.Update.SetPosition;
                q.EmitterPosition = cmd.Update.EmitterPosition;
                break;
            }
            case CommandKind::StopVoice: {
                uint32 slot = s.LogicalCapacity;
                if (cmd.StopBatchLocalPlayIndex >= 0)
                {
                    slot = slotForCommand[cmd.StopBatchLocalPlayIndex];
                }
                else
                {
                    slot = s.FindVoiceSlot(cmd.StopTarget);
                }
                q.VoiceSlotIndex = slot;
                q.VoiceGeneration = slot < s.LogicalCapacity ? s.Voices[slot].Generation.Value : 0;
                break;
            }
            case CommandKind::SetListener:
                q.Listener = cmd.Listener;
                break;
            case CommandKind::SetBusBaseGain:
            case CommandKind::SetBusUserGain:
                q.BusIndex = cmd.BusIndex;
                q.BusGain = cmd.BusGain;
                break;
            case CommandKind::AddModifier:
            case CommandKind::UpdateModifier:
            case CommandKind::RemoveModifier: {
                const uint32 mslot = s.FindModifierSlot(cmd.Modifier);
                q.ModifierSlotIndex = mslot < MODIFIER_CAPACITY ? mslot : 0;
                q.ModifierGeneration = mslot < MODIFIER_CAPACITY ? s.Modifiers[mslot].Generation.Value : 0;
                q.ModifierWeight = cmd.ModifierWeight;
                q.ModifierValueCount = static_cast<uint32>(cmd.ModifierValues.size());
                for (uint32 k = 0; k < q.ModifierValueCount; ++k)
                {
                    q.ModifierValues[k] = cmd.ModifierValues[k];
                }
                break;
            }
        }
    }

    // Stamp the batch length on the leading record so the consumer can defer the
    // whole batch if it would exceed the remaining boundary budget (no split).
    if (scratchCount > 0)
    {
        s.BatchScratch[0].BatchLength = scratchCount;
    }

    // --- Phase 3: publish once ---------------------------------------------
    std::span<const QueuedCommand> batch(s.BatchScratch, scratchCount);
    if (s.CommandRing.TryPublishBatch(batch) == false)
    {
        rollback();
        s.LastBatchVoiceCount = 0;
        result.Result = Status::QueueFull;
        ++s.RejectedCommands;
        return result;
    }

    // Commit group admission charges now that publication succeeded.
    for (uint32 g = 0; g < s.GroupCount; ++g)
    {
        s.Groups[g].Admitted += groupCharge[g];
    }

    // Track logical high-water.
    uint32 live = 0;
    for (uint32 i = 0; i < s.LogicalCapacity; ++i)
    {
        if (s.Voices[i].InUse)
        {
            ++live;
        }
    }
    if (live > s.LogicalHighWater)
    {
        s.LogicalHighWater = live;
    }

    result.Result = Status::Ok;
    result.Voices = std::span<const VoiceHandle>(s.LastBatchVoices, s.LastBatchVoiceCount);
    return result;
}

Status AudioSystem::PlayClip(const PlayParams& params, VoiceHandle& outVoice) noexcept
{
    outVoice = VoiceHandle{};
    Command cmd{};
    cmd.Kind = CommandKind::Play;
    cmd.Play = params;
    const BatchResult r = TrySubmitBatch(std::span<const Command>(&cmd, 1));
    if (IsOk(r.Result) && r.Voices.empty() == false)
    {
        outVoice = r.Voices[0];
    }
    return r.Result;
}

// Fixed public stream-play signature (design section 3); callers pass named
// fields, not positional ints. The convertible busIndex/priority/gain triple is
// an accepted public shape here.
Status AudioSystem::PlayStream(StreamHandle stream,
                               uint32 busIndex, // NOLINT(bugprone-easily-swappable-parameters)
                               uint8 priority,
                               float32 gain,
                               VoiceHandle& outVoice) noexcept
{
    outVoice = {};
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    auto& s = *mImpl;
    s.CollectRender();
    if (s.State != SystemState::Ready)
    {
        return Status::NotReady;
    }
    const auto index = s.FindStream(stream);
    if (index >= s.StreamInstances)
    {
        return Status::InvalidHandle;
    }
    auto& instance = s.Streams[index];
    if (instance.Played)
    {
        return Status::NotReady;
    }
    if (busIndex >= s.BusCount || priority > 7 || std::isfinite(gain) == false || gain < 0 || gain > 1)
    {
        return Status::InvalidArgument;
    }
    if (s.CommandRing.FreeSlots() == 0)
    {
        return Status::QueueFull;
    }
    const auto slot = s.ReserveVoiceSlot();
    if (slot >= s.LogicalCapacity)
    {
        return Status::VoiceCapacity;
    }
    auto& v = s.Voices[slot];
    v.IsStream = true;
    v.StreamSlotIndex = index;
    v.State = VoiceState::Pending;
    v.BusIndex = busIndex;
    v.GroupIndex = 0;
    v.Priority = priority;
    v.Gain = gain;
    v.Rate = 1;
    v.Looping = instance.Data->Looping;
    v.AdmissionEpoch = s.StopAllEpoch;
    v.PlayDrained = false;
    v.FadeGain = 0;
    v.FadeTarget = 1;
    v.Terminal.TerminalGeneration.store(0, std::memory_order_relaxed);
    v.Stop.StopGeneration.store(0, std::memory_order_relaxed);
    QueuedCommand q{};
    q.Kind = CommandKind::Play;
    q.BatchLength = 1;
    q.VoiceSlotIndex = slot;
    q.VoiceGeneration = v.Generation.Value;
    q.BusIndex = busIndex;
    q.Priority = priority;
    q.Gain = gain;
    q.Rate = 1;
    q.IsStream = true;
    q.StreamSlotIndex = index;
    q.Stream = instance.Data;
    q.AdmissionEpoch = s.StopAllEpoch;
    q.Looping = v.Looping;
    if (s.CommandRing.TryPublishBatch({&q, 1}) == false)
    {
        v.InUse = false;
        return Status::QueueFull;
    }
    instance.Played = true;
    outVoice = s.MakeVoiceHandle(slot);
    return Status::Ok;
}

// ===========================================================================
// Cancellation
// ===========================================================================
Status AudioSystem::Stop(VoiceHandle voice) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const uint32 slot = mImpl->FindVoiceSlot(voice);
    if (slot >= mImpl->LogicalCapacity)
    {
        // Idempotent for a terminal handle the owner no longer tracks.
        return Status::InvalidHandle;
    }
    VoiceSlot& v = mImpl->Voices[slot];
    // Store the stop generation with release ordering, independent of the normal
    // command queue (always deliverable even when the ring is full).
    v.Stop.StopGeneration.store(v.Generation.Value, std::memory_order_release);
    return Status::Ok;
}

void AudioSystem::StopAll() noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return;
    }
    // Advance the cancellation epoch. Voices admitted before this epoch are
    // cancelled at the next boundary, including still-queued plays. Never needs
    // queue space. Epoch space is identity: reject wrap by exhaustion.
    if (mImpl->StopAllEpoch == 0xFFFFFFFFU)
    {
        mImpl->State = SystemState::Failed; // request a fresh session (design 4)
        return;
    }
    ++mImpl->StopAllEpoch;
    mImpl->PublishedStopAll.store(mImpl->StopAllEpoch, std::memory_order_release);
}

// ===========================================================================
// Service
// ===========================================================================
void AudioSystem::Service() noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return;
    }
    Impl& s = *mImpl;
    s.CollectRender();
    if (s.State == SystemState::Ready && s.DeviceStopped.load(std::memory_order_acquire))
    {
        s.State = SystemState::Failed;
        ++s.Errors;
    }

    // Acknowledge durable terminals and reclaim retired assets. In Offline/A1
    // the renderer runs on this same thread; acknowledgment is still routed
    // through the durable mailbox so the contract matches the device path.
    s.AcknowledgeTerminals();

    // Reclaim retiring clips whose pins have drained.
    for (uint32 i = 0; i < s.LogicalCapacity; ++i)
    {
        internal::ClipSlot& c = s.Clips[i];
        if (c.InUse && c.Retiring && c.Pins == 0)
        {
            c.ReleasePcm();
            c.InUse = false;
            c.Retiring = false;
            c.Frames = 0;
            c.LoopBegin = 0;
            c.LoopEnd = 0;
            c.PreparedPeak = 0.0F;
        }
    }

    for (uint32 i = 0; i < s.StreamInstances; ++i)
    {
        auto& slot = s.Streams[i];
        if (slot.InUse && slot.Retiring && slot.Data->Cancel.load(std::memory_order_acquire) &&
            s.WorkerStreams[i].load(std::memory_order_acquire) == nullptr)
        {
            delete slot.Data;
            slot.Data = nullptr;
            slot.InUse = false;
        }
    }
    // Publish a fresh diagnostic snapshot and drain it to the sink.
    s.PublishSnapshot();
    if (s.Sink != nullptr)
    {
        internal::SnapshotRecord rec{};
        if (s.SnapshotRing.Peek(rec))
        {
            s.SnapshotRing.ConsumeOne();
            s.Sink->OnSystemSnapshot(rec.System);
        }
    }
}

// ===========================================================================
// Queries
// ===========================================================================
Status AudioSystem::GetVoiceInfo(VoiceHandle voice, VoiceInfo& out) const noexcept
{
    out = VoiceInfo{};
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const uint32 slot = mImpl->FindVoiceSlot(voice);
    if (slot >= mImpl->LogicalCapacity)
    {
        return Status::InvalidHandle;
    }
    const VoiceSlot& v = mImpl->Voices[slot];
    out.Voice = voice;
    out.State = v.State;
    out.LastCause = v.LastCause;
    // The renderer writes terminal details before release-publishing generation.
    // Reading them while a voice is still active races with that publication.
    if (v.Terminal.TerminalGeneration.load(std::memory_order_acquire) == v.Generation.Value)
    {
        out.Terminal = v.Terminal.Reason;
        out.State = VoiceState::Terminal;
    }
    out.Silence = v.Silence;
    out.Stale = false;
    out.SnapshotFrame = mImpl->RenderFrame;
    out.CursorFrame = v.CursorFrame;
    out.StartFrame = v.StartFrame;
    out.EffectiveGain = v.FadeGain; // carried per-channel gain (incl. pan/attn)
    out.Priority = v.Priority;
    out.GroupIndex = v.GroupIndex;
    out.BusIndex = v.BusIndex;
    out.PolicyTag = v.PolicyTag;
    out.VirtualAgeFrames = v.State == VoiceState::Virtual ? mImpl->RenderFrame - v.VirtualSinceFrame : 0;
    out.ScoreVoiceGain = v.ScoreVoiceGain;
    out.ScorePreparedPeak = v.ScorePreparedPeak;
    out.ScoreDistanceGain = v.ScoreDistanceGain;
    out.ScoreBusChainGain = v.ScoreBusChainGain;
    out.EmitterPosition = v.EmitterPosition;
    out.PanningPosition = mImpl->Listener.PanningPosition;
    out.AttenuationPosition = mImpl->Listener.AttenuationPosition;
    out.Origin = v.Origin;
    out.Pan = v.Pan;
    return Status::Ok;
}

Status AudioSystem::GetGroupInfo(uint32 groupIndex, GroupInfo& out) const noexcept
{
    out = GroupInfo{};
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (groupIndex >= mImpl->GroupCount)
    {
        return Status::InvalidArgument;
    }
    const internal::GroupState& g = mImpl->Groups[groupIndex];
    out.MaxAdmitted = g.MaxAdmitted;
    out.MaxSelected = g.MaxSelected;
    out.Admitted = g.Admitted;
    out.Selected = g.Selected;
    out.Fading = g.Fading;
    return Status::Ok;
}

Status AudioSystem::GetBusMeter(uint32 busIndex, BusMeter& out) const noexcept
{
    out = BusMeter{};
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (busIndex >= mImpl->BusCount)
    {
        return Status::InvalidArgument;
    }
    mImpl->CollectRender();
    const internal::BusState& b = mImpl->Buses[busIndex];
    out.InputPeakL = b.InputMeter.OutPeakL;
    out.InputPeakR = b.InputMeter.OutPeakR;
    out.InputRmsL = b.InputMeter.OutRmsL;
    out.InputRmsR = b.InputMeter.OutRmsR;
    out.PostPeakL = b.PostMeter.OutPeakL;
    out.PostPeakR = b.PostMeter.OutPeakR;
    out.PostRmsL = b.PostMeter.OutRmsL;
    out.PostRmsR = b.PostMeter.OutRmsR;
    out.ClippedFrames = b.ClippedFrames;
    out.CurrentGain = b.CurrentEffective;
    out.TargetGain = b.TargetEffective;
    return Status::Ok;
}

void AudioSystem::GetSystemSnapshot(SystemSnapshot& out) const noexcept
{
    out = SystemSnapshot{};
    if (mImpl.Get() == nullptr)
    {
        return;
    }
    // Compute a fresh live view directly on the owner thread. This is the
    // owner-authoritative accounting; the SPSC snapshot ring is for a separate
    // async sink consumer (design section 11) and uses FIFO semantics, so it is
    // not read here.
    mImpl->CollectRender();
    mImpl->FillSnapshot(out);
}

// ===========================================================================
// Offline rendering
// ===========================================================================
Status AudioSystem::RenderOffline(std::span<float32> output,
                                  ChannelLayout layout,
                                  BufferLayout bufferLayout,
                                  uint32 frames) noexcept
{
    if (mImpl.Get() == nullptr || mImpl->State != SystemState::Ready)
    {
        return Status::NotReady;
    }
    if (mImpl->SystemMode != Mode::Offline)
    {
        return Status::InvalidArgument; // explicit Offline mode only
    }
    return mImpl->RenderFrames(output, layout, bufferLayout, frames);
}

// ===========================================================================
// Browser gesture / shutdown
// ===========================================================================
Status AudioSystem::ResumeFromUserGesture() noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    // No browser adapter in A1; the real gesture resume lands in A6.
    return Status::Unsupported;
}

Status AudioSystem::BeginShutdown() noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (mImpl->Shutdown == ShutdownState::Complete)
    {
        return Status::Ok;
    }
    mImpl->State = SystemState::Stopping;
    mImpl->CloseDevice();
    mImpl->CloseWorker();
    // Joining both consumers proves queued payloads are no longer borrowed.
    for (uint32 i = 0; i < mImpl->LogicalCapacity; ++i)
    {
        if (mImpl->Voices[i].InUse)
        {
            mImpl->TerminateVoice(i, TerminalReason::Stopped, StateCause::StopRequested);
        }
    }
    mImpl->AcknowledgeTerminals();
    for (uint32 i = 0; i < mImpl->StreamInstances; ++i)
    {
        mImpl->WorkerStreams[i].store(nullptr, std::memory_order_relaxed);
        delete mImpl->Streams[i].Data;
        mImpl->Streams[i].Data = nullptr;
        mImpl->Streams[i].InUse = false;
    }
    mImpl->ReleaseStorage();
    for (auto& clip : mImpl->Clips)
    {
        clip.InUse = false;
    }
    // Close admission after native renderer and worker quiescence.
    mImpl->State = SystemState::Stopping;
    mImpl->Shutdown = ShutdownState::Complete;
    mImpl->State = SystemState::Disabled;
    return Status::Ok;
}

ShutdownState AudioSystem::GetShutdownState() const noexcept
{
    return mImpl ? mImpl->Shutdown : ShutdownState::NotStarted;
}

// ===========================================================================
// Diagnostics / helpers
// ===========================================================================
void AudioSystem::SetDebugSink(AudioDebugSnapshotSink* sink) noexcept
{
    if (mImpl)
    {
        mImpl->Sink = sink;
    }
}

Status AudioSystem::AcquireModifier(std::span<const ModifierValue> values,
                                    float32 weight,
                                    ModifierHandle& outModifier) noexcept
{
    outModifier = ModifierHandle{};
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (values.size() > BUS_CAPACITY || std::isfinite(weight) == false || weight < 0.0F || weight > 1.0F)
    {
        return Status::InvalidArgument;
    }
    Impl& s = *mImpl;
    // Validate bus indices and dB ranges before reserving.
    for (const ModifierValue& mv : values)
    {
        if (mv.BusIndex >= s.BusCount || std::isfinite(mv.TargetDb) == false || mv.TargetDb < MODIFIER_DB_MIN ||
            mv.TargetDb > MODIFIER_DB_MAX)
        {
            return Status::InvalidArgument;
        }
    }
    // Reserve a free modifier slot (fading-out instances still count).
    uint32 slot = MODIFIER_CAPACITY;
    for (uint32 i = 0; i < MODIFIER_CAPACITY; ++i)
    {
        if (s.Modifiers[i].InUse == false && s.Modifiers[i].Generation.IsExhausted() == false)
        {
            slot = i;
            break;
        }
    }
    if (slot == MODIFIER_CAPACITY)
    {
        return Status::AssetCapacity;
    }
    internal::ModifierState& m = s.Modifiers[slot];
    if (m.Generation.Advance() == false)
    {
        return Status::AssetCapacity;
    }
    m.InUse = true;
    m.Removing = false;
    m.Weight = weight;
    m.TargetWeight = weight;
    for (uint32 i = 0; i < BUS_CAPACITY; ++i)
    {
        m.HasBus[i] = false;
        m.Db[i] = 0.0F;
    }
    for (const ModifierValue& mv : values)
    {
        m.Db[mv.BusIndex] = mv.TargetDb;
        m.HasBus[mv.BusIndex] = true;
    }
    if (s.SystemMode == Mode::Device)
    {
        QueuedCommand q{};
        q.Kind = CommandKind::AddModifier;
        q.BatchLength = 1;
        q.ModifierSlotIndex = slot;
        q.ModifierGeneration = m.Generation.Value;
        q.ModifierWeight = weight;
        q.ModifierValueCount = static_cast<uint32>(values.size());
        for (usize i = 0; i < values.size(); ++i)
        {
            q.ModifierValues[i] = values[i];
        }
        if (s.CommandRing.TryPublishBatch({&q, 1}) == false)
        {
            m.InUse = false;
            return Status::QueueFull;
        }
    }
    outModifier = ModifierHandle{s.Session, slot, m.Generation.Value};
    return Status::Ok;
}

Status AudioSystem::UpdateModifier(ModifierHandle modifier, float32 weight) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (std::isfinite(weight) == false || weight < 0.0F || weight > 1.0F)
    {
        return Status::InvalidArgument;
    }
    const uint32 slot = mImpl->FindModifierSlot(modifier);
    if (slot >= MODIFIER_CAPACITY)
    {
        return Status::InvalidHandle;
    }
    internal::ModifierState& m = mImpl->Modifiers[slot];
    if (mImpl->SystemMode == Mode::Device)
    {
        QueuedCommand q{};
        q.Kind = CommandKind::UpdateModifier;
        q.BatchLength = 1;
        q.ModifierSlotIndex = slot;
        q.ModifierGeneration = m.Generation.Value;
        q.ModifierWeight = weight;
        if (mImpl->CommandRing.TryPublishBatch({&q, 1}) == false)
        {
            return Status::QueueFull;
        }
    }
    m.Weight = weight;
    m.TargetWeight = weight;
    return Status::Ok;
}

Status AudioSystem::ReleaseModifier(ModifierHandle modifier) noexcept
{
    if (mImpl.Get() == nullptr)
    {
        return Status::NotReady;
    }
    const uint32 slot = mImpl->FindModifierSlot(modifier);
    if (slot >= MODIFIER_CAPACITY)
    {
        return Status::InvalidHandle;
    }
    internal::ModifierState& m = mImpl->Modifiers[slot];
    if (mImpl->SystemMode == Mode::Device)
    {
        QueuedCommand q{};
        q.Kind = CommandKind::RemoveModifier;
        q.BatchLength = 1;
        q.ModifierSlotIndex = slot;
        q.ModifierGeneration = m.Generation.Value;
        if (mImpl->CommandRing.TryPublishBatch({&q, 1}) == false)
        {
            return Status::QueueFull;
        }
    }
    // Fade the weight to zero and retire the slot. Removing one instance never
    // removes another (each has its own slot/generation). A1/A3 apply the final
    // state immediately on the serialized owner thread; the slot is freed so a
    // later acquire gets a new generation.
    m.Weight = 0.0F;
    m.TargetWeight = 0.0F;
    m.Removing = true;
    m.InUse = false;
    return Status::Ok;
}

float32 AudioSystem::UserGainFromSlider(float32 u, float32 dbRange) noexcept
{
    if (std::isfinite(u) == false || std::isfinite(dbRange) == false)
    {
        return 0.0F;
    }
    if (u <= 0.0F)
    {
        return 0.0F; // exact mute
    }
    if (u > 1.0F)
    {
        u = 1.0F;
    }
    return std::pow(10.0F, (u - 1.0F) * dbRange / 20.0F);
}

} // namespace ludus::audio
