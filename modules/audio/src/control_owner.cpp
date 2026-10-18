#include <ludus/audio/audio_system.h>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>

#include "internal/decode.hpp"
#include "internal/impl.hpp"
#include "internal/log_categories.h"

#include <cmath>
#include <new>

// This translation unit completes AudioSystem::Impl (storage defined in
// internal/impl.hpp) with the boundary/render/service logic.
// Thanks to Guy Somberg, "Sound Engine State Machine", Game Audio Programming:
// Principles and Practices, chapter 2, pp. 13-30, for reversible virtualization
// versus terminal stopping; and Robert Gay, "Voice Management and Virtualization",
// Game Audio Programming 3: Principles and Practices, chapter 9, pp. 133-142,
// for distinguishing physical voice budgets from per-family limits. Ludus
// prepares before Play, uses fixed resident groups and charges fades globally;
// loading states, overlapping pools and automatic stream virtualization are
// outside this implementation. Reviewed scope: docs/architecture/audio-research.md.

namespace ludus::audio
{
namespace
{
[[nodiscard]] bool IsFiniteF(float32 v) noexcept
{
    return std::isfinite(v);
}
} // namespace

// ---------------------------------------------------------------------------
// Boundary processing
// ---------------------------------------------------------------------------
void AudioSystem::Impl::ConsumeAndApplyBatch() noexcept
{
    // Apply whole batches up to the CONTROL_BOUNDARY_BUDGET (64) records per
    // boundary. A batch is NEVER split: the leading record of each batch carries
    // its length; if applying it would exceed the remaining budget AND at least
    // one batch has already been applied this boundary, the whole batch is
    // deferred to the next boundary (eventual progress, no partial update). A
    // single batch is bounded by MAX_BATCH_RECORDS (32) < 64, so it always fits
    // when the boundary starts empty.
    auto& source = Control != nullptr ? Control->CommandRing : CommandRing;
    QueuedCommand cmd{};
    uint32 applied = 0;
    while (applied < CONTROL_BOUNDARY_BUDGET && source.Peek(cmd))
    {
        const uint32 batchLen = cmd.BatchLength == 0 ? 1 : cmd.BatchLength;
        if (applied != 0 && applied + batchLen > CONTROL_BOUNDARY_BUDGET)
        {
            break; // defer the whole batch; never split it across a boundary
        }
        for (uint32 bi = 0; bi < batchLen && source.Peek(cmd); ++bi)
        {
            source.ConsumeOne();
            ++applied;
            ++AcceptedCommands;
            ApplyOneCommand(cmd);
        }
    }
}

void AudioSystem::Impl::ApplyOneCommand(const QueuedCommand& cmd) noexcept
{
    {
        switch (cmd.Kind)
        {
            case CommandKind::Play: {
                VoiceSlot& v = Voices[cmd.VoiceSlotIndex];
                if (Control != nullptr)
                {
                    v.Generation.Value = cmd.VoiceGeneration;
                    v.InUse = true;
                    v.State = VoiceState::Pending;
                    v.ClipSlotIndex = cmd.ClipSlotIndex;
                    v.BusIndex = cmd.BusIndex;
                    v.GroupIndex = cmd.GroupIndex;
                    v.Priority = cmd.Priority;
                    v.Gain = cmd.Gain;
                    v.Rate = cmd.Rate;
                    v.Looping = cmd.Looping;
                    v.Policy = cmd.Policy;
                    v.Positional = cmd.Positional;
                    v.Origin = cmd.Origin;
                    v.MinDistance = cmd.MinDistance;
                    v.MaxDistance = cmd.MaxDistance;
                    v.ExplicitPan = cmd.ExplicitPan;
                    v.EmitterPosition = cmd.EmitterPosition;
                    v.PolicyTag = cmd.PolicyTag;
                    v.IsStream = cmd.IsStream;
                    v.StreamSlotIndex = cmd.StreamSlotIndex;
                    v.AdmissionEpoch = cmd.AdmissionEpoch;
                    v.PreparedPeakScore = cmd.Peak;
                    v.FadeGain = 0;
                    v.FadeTarget = 1;
                    v.PlayDrained = false;
                    v.PhysicalIndex = 0xFFFFFFFFU;
                    if (cmd.IsStream == false)
                    {
                        auto& c = Clips[v.ClipSlotIndex];
                        c.Pcm = const_cast<float32*>(cmd.Pcm);
                        c.Frames = cmd.Frames;
                        c.Channels = cmd.Channels;
                        c.LoopBegin = cmd.LoopBegin;
                        c.LoopEnd = cmd.LoopEnd;
                    }
                    if (cmd.IsStream)
                    {
                        Streams[cmd.StreamSlotIndex].Data = cmd.Stream;
                    }
                }
                // Reject a stale command targeting a reused slot.
                if (v.Generation.Value != cmd.VoiceGeneration || v.InUse == false)
                {
                    ++StaleCommands;
                    break;
                }
                // Cancellation precedence: an admission epoch older than the live
                // StopAll epoch is cancelled even though it reached the ring.
                if (cmd.AdmissionEpoch < StopAllEpoch)
                {
                    TerminateVoice(cmd.VoiceSlotIndex, TerminalReason::Cancelled, StateCause::StopAllEpoch);
                    break;
                }
                // An individual Stop stored before consumption cancels the pending
                // play (a Stop on Pending waits for this drain).
                const uint32 stopGen = (Control != nullptr ? Control->Voices[cmd.VoiceSlotIndex].Stop : v.Stop)
                                           .StopGeneration.load(std::memory_order_acquire);
                if (stopGen == v.Generation.Value)
                {
                    v.PlayDrained = true;
                    TerminateVoice(cmd.VoiceSlotIndex, TerminalReason::Cancelled, StateCause::Cancelled);
                    break;
                }
                v.PlayDrained = true;
                v.State = VoiceState::Scheduled;
                v.LastCause = StateCause::PlayConsumed;
                v.StartFrame = cmd.StartFrame == 0 ? RenderFrame : cmd.StartFrame;
                // A command applied after its desired frame starts at this
                // boundary and reports LateStart + lateness; never rewind or
                // silently skip the transient.
                if (cmd.StartFrame != 0 && cmd.StartFrame < RenderFrame)
                {
                    v.LateStart = true;
                    v.Lateness = RenderFrame - cmd.StartFrame;
                    v.StartFrame = RenderFrame;
                }
                break;
            }
            case CommandKind::UpdateVoice: {
                VoiceSlot& v = Voices[cmd.VoiceSlotIndex];
                if (v.Generation.Value != cmd.VoiceGeneration || v.InUse == false)
                {
                    ++StaleCommands;
                    break;
                }
                // A stopped generation cannot be revived by a later Update.
                if (v.State == VoiceState::Stopping || v.State == VoiceState::Terminal)
                {
                    ++StaleCommands;
                    break;
                }
                if (cmd.SetGain && IsFiniteF(cmd.Gain))
                {
                    v.Gain = cmd.Gain;
                }
                if (cmd.SetRate && IsFiniteF(cmd.Rate))
                {
                    v.Rate = cmd.Rate;
                }
                if (cmd.SetPriority)
                {
                    v.Priority = cmd.Priority;
                }
                if (cmd.SetPosition)
                {
                    v.EmitterPosition = cmd.EmitterPosition;
                }
                break;
            }
            case CommandKind::StopVoice: {
                VoiceSlot& v = Voices[cmd.VoiceSlotIndex];
                if (v.Generation.Value != cmd.VoiceGeneration || v.InUse == false)
                {
                    ++StaleCommands;
                    break;
                }
                if (v.State == VoiceState::Mixed || v.State == VoiceState::Virtualizing)
                {
                    v.State = VoiceState::Stopping;
                    v.LastCause = StateCause::StopRequested;
                    v.FadeTarget = 0.0F;
                }
                else if (v.State == VoiceState::Virtual || v.State == VoiceState::Scheduled)
                {
                    TerminateVoice(cmd.VoiceSlotIndex, TerminalReason::Stopped, StateCause::StopRequested);
                }
                break;
            }
            case CommandKind::SetListener:
                Listener = cmd.Listener;
                break;
            case CommandKind::SetBusBaseGain:
                if (cmd.BusIndex < BusCount && IsFiniteF(cmd.BusGain))
                {
                    Buses[cmd.BusIndex].BaseGain = cmd.BusGain;
                }
                break;
            case CommandKind::SetBusUserGain:
                if (cmd.BusIndex < BusCount && IsFiniteF(cmd.BusGain))
                {
                    Buses[cmd.BusIndex].UserGain = cmd.BusGain;
                }
                break;
            case CommandKind::AddModifier: {
                internal::ModifierState& m = Modifiers[cmd.ModifierSlotIndex];
                if (Control != nullptr && cmd.Kind == CommandKind::AddModifier)
                {
                    m.Generation.Value = cmd.ModifierGeneration;
                    m.InUse = true;
                    m.Removing = false;
                }
                if (m.Generation.Value != cmd.ModifierGeneration || m.InUse == false)
                {
                    ++StaleCommands;
                    break;
                }
                m.Weight = cmd.ModifierWeight;
                m.TargetWeight = cmd.ModifierWeight;
                for (uint32 i = 0; i < BUS_CAPACITY; ++i)
                {
                    m.HasBus[i] = false;
                }
                for (uint32 i = 0; i < cmd.ModifierValueCount; ++i)
                {
                    const uint32 bi = cmd.ModifierValues[i].BusIndex;
                    if (bi < BUS_CAPACITY)
                    {
                        m.Db[bi] = cmd.ModifierValues[i].TargetDb;
                        m.HasBus[bi] = true;
                    }
                }
                break;
            }
            case CommandKind::UpdateModifier: {
                internal::ModifierState& m = Modifiers[cmd.ModifierSlotIndex];
                if (m.Generation.Value != cmd.ModifierGeneration || m.InUse == false)
                {
                    ++StaleCommands;
                    break;
                }
                m.TargetWeight = cmd.ModifierWeight;
                m.Weight = cmd.ModifierWeight;
                break;
            }
            case CommandKind::RemoveModifier: {
                internal::ModifierState& m = Modifiers[cmd.ModifierSlotIndex];
                if (m.Generation.Value != cmd.ModifierGeneration || m.InUse == false)
                {
                    ++StaleCommands;
                    break;
                }
                // Fade weight to zero before releasing the slot; A3 adds the ramp.
                m.Removing = true;
                m.TargetWeight = 0.0F;
                m.Weight = 0.0F;
                m.InUse = false; // A1: immediate; A3 defers to acknowledged fade-out
                break;
            }
        }
    }
}

void AudioSystem::Impl::ResolveScheduledStarts() noexcept
{
    const uint64 quantumEnd = RenderFrame + CONTROL_QUANTUM_FRAMES;
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        VoiceSlot& v = Voices[i];
        if (v.InUse == false || v.State != VoiceState::Scheduled)
        {
            continue;
        }
        if (v.StartFrame < quantumEnd)
        {
            // Enter Mixed (A1: no physical-slot contention modeling yet; A2 adds
            // selection competition). Charge selection in SelectAndCharge.
            v.State = VoiceState::Mixed;
            v.LastCause = StateCause::StartFrameReached;
            v.FadeGain = 0.0F;
            v.FadeTarget = 1.0F;
            v.CursorFrame = 0;
            v.CursorFraction = 0.0;
        }
    }
}

void AudioSystem::Impl::SelectAndCharge() noexcept
{
    // Reset per-boundary group selection counters.
    for (uint32 g = 0; g < GroupCount; ++g)
    {
        Groups[g].Selected = 0;
        Groups[g].Fading = 0;
    }

    // Recompute the independent silence-cause flags from scratch each boundary
    // (design section 11: flags reflect the current boundary, not stale state;
    // multiple causes can coexist). Base causes that do not depend on selection
    // are set here; the selection loop below adds BelowThreshold/GroupQuota/
    // GlobalBudget for voices it does not select.
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        VoiceSlot& v = Voices[i];
        if (v.InUse == false || v.IsStream)
        {
            continue;
        }
        v.Silence = SilenceCause::None;
        if (v.State == VoiceState::Pending || v.State == VoiceState::Scheduled)
        {
            v.Silence |= SilenceCause::NotStarted;
        }
        if (v.State == VoiceState::Stopping)
        {
            v.Silence |= SilenceCause::Stopping;
        }
        // Distance-zero: a positional voice fully attenuated at/after MaxDistance.
        if (v.Positional && v.ScoreDistanceGain <= 0.0F)
        {
            v.Silence |= SilenceCause::DistanceZero;
        }
        // User-muted: the voice's bus chain carries a zero UserGain anywhere.
        uint32 b = v.BusIndex;
        uint32 guard = 0;
        while (b != kNoParent && guard <= BusCount)
        {
            if (Buses[b].UserGain == 0.0F)
            {
                v.Silence |= SilenceCause::UserMuted;
                break;
            }
            b = Buses[b].ParentIndex;
            ++guard;
        }
    }

    // Build a deterministic ordered candidate list of resident voices eligible
    // for steady-state selection (Mixed / Virtual / Virtualizing wanting to
    // reverse). Rank by priority (desc), then estimated audibility (desc), with
    // stable ties preferring incumbents then admission (slot) order. Selection
    // uses a bounded insertion into a fixed array (O(V*P)), no heap growth.
    uint32 order[LOGICAL_VOICE_CAPACITY];
    uint32 candidateCount = 0;
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        VoiceSlot& v = Voices[i];
        if (v.InUse == false || v.IsStream)
        {
            continue;
        }
        if (v.State == VoiceState::Stopping || v.State == VoiceState::Terminal || v.State == VoiceState::Pending ||
            v.State == VoiceState::Scheduled)
        {
            continue;
        }
        // Compute gains/score for this boundary (fills Score*/Pan).
        float32 gl = 0.0F;
        float32 gr = 0.0F;
        ComputeVoiceGains(v, gl, gr);
        v.TargetGainL = gl;
        v.TargetGainR = gr;

        // Insertion into the ordered list.
        const float32 estI = EstimateAudibility(v);
        uint32 pos = candidateCount;
        while (pos > 0)
        {
            const VoiceSlot& prev = Voices[order[pos - 1]];
            const float32 estP = EstimateAudibility(prev);
            bool iBefore = false;
            if (v.Priority != prev.Priority)
            {
                iBefore = v.Priority > prev.Priority;
            }
            else if (estI != estP)
            {
                iBefore = estI > estP;
            }
            else
            {
                // Stable: incumbents (currently Mixed) before non-incumbents;
                // then lower slot (earlier admission) first.
                const bool iInc = v.State == VoiceState::Mixed;
                const bool pInc = prev.State == VoiceState::Mixed;
                if (iInc != pInc)
                {
                    iBefore = iInc;
                }
                else
                {
                    iBefore = i < order[pos - 1];
                }
            }
            if (iBefore == false)
            {
                break;
            }
            order[pos] = order[pos - 1];
            --pos;
        }
        order[pos] = i;
        ++candidateCount;
    }

    // Select top candidates subject to each group's MaxSelected and the global
    // resident steady-state budget. Hysteresis: within equal priority an
    // incumbent needs the challenger to exceed it (already ordered); a voice
    // below the virtualize threshold is not selected.
    uint32 globalSelected = 0;
    const uint32 globalBudget = static_cast<uint32>(RESIDENT_STEADY_SELECT);

    for (uint32 k = 0; k < candidateCount; ++k)
    {
        VoiceSlot& v = Voices[order[k]];
        const uint32 g = v.GroupIndex;
        const float32 est = EstimateAudibility(v);

        // Audibility gate: below the virtualize threshold (-60 dB ~ 0.001) a
        // voice is not selected; it virtualizes or (reentry) stays virtual until
        // above the reentry threshold (-57 dB). Mixed incumbents use the lower
        // (virtualize) threshold; virtual challengers use the higher (reentry).
        const float32 virtualizeThresh = 0.001F; // ~ -60 dB
        const float32 reentryThresh = 0.001413F; // ~ -57 dB
        const bool incumbent = v.State == VoiceState::Mixed || v.State == VoiceState::Virtualizing;
        const float32 thresh = incumbent ? virtualizeThresh : reentryThresh;

        bool select = est >= thresh;
        if (select && Groups[g].Selected >= Groups[g].MaxSelected)
        {
            select = false; // group quota reached
        }
        if (select && globalSelected >= globalBudget)
        {
            select = false; // global steady-state budget reached
        }

        if (select)
        {
            ++Groups[g].Selected;
            ++globalSelected;
            // Base silence causes (DistanceZero/UserMuted) were already computed
            // this boundary and are preserved: a selected voice on a user-muted
            // bus is still silent for that reason. Selection only avoids adding
            // the not-selected causes below.
            // Transition toward Mixed. A Virtual or Virtualizing voice reverses
            // into Mixed (the Virtualizing fade reverses in place from its
            // current value); an incumbent Mixed voice simply retargets to full.
            if (v.State == VoiceState::Virtual || v.State == VoiceState::Virtualizing)
            {
                v.State = VoiceState::Mixed;
                v.LastCause = StateCause::ReselectedReversed;
            }
            v.FadeTarget = 1.0F;
        }
        else
        {
            // Not selected: follow policy. Mixed -> Virtualizing (advance) or
            // Stopping (kill). Virtualizing continues. Virtual stays virtual.
            if (est < thresh)
            {
                v.Silence |= SilenceCause::BelowThreshold;
            }
            if (Groups[g].Selected >= Groups[g].MaxSelected)
            {
                v.Silence |= SilenceCause::GroupQuota;
            }
            if (globalSelected >= globalBudget)
            {
                v.Silence |= SilenceCause::GlobalBudget;
            }
            if (v.State == VoiceState::Mixed)
            {
                if (v.Policy == VirtualPolicy::AdvanceWhenVirtual)
                {
                    v.State = VoiceState::Virtualizing;
                    v.LastCause = StateCause::LostSelectionGroupQuota;
                    v.FadeTarget = 0.0F;
                }
                else
                {
                    v.State = VoiceState::Stopping;
                    v.LastCause = StateCause::LostSelectionAudibility;
                    v.FadeTarget = 0.0F;
                }
            }
            // Virtualizing continues toward Virtual; Virtual stays.
        }
    }

    // Tally fading tails for diagnostics.
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        const VoiceSlot& v = Voices[i];
        if (v.InUse == false || v.IsStream)
        {
            continue;
        }
        if ((v.State == VoiceState::Virtualizing || v.State == VoiceState::Stopping) && v.GroupIndex < GroupCount)
        {
            ++Groups[v.GroupIndex].Fading;
        }
    }
}

void AudioSystem::Impl::AcknowledgeTerminals() noexcept
{
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        VoiceSlot& v = Voices[i];
        if (v.InUse == false)
        {
            continue;
        }
        // Durable terminal acknowledgment (design section 4): reclaim ONLY after
        // ACQUIRING the per-slot terminal mailbox and matching its generation to
        // the live slot generation. Reading v.State alone would be a data race
        // once the renderer runs on a separate device/worklet thread; the
        // acquire pairs with the renderer's release in TerminateVoice.
        const uint32 termGen = v.Terminal.TerminalGeneration.load(std::memory_order_acquire);
        if (termGen != 0 && termGen == v.Generation.Value)
        {
            if (v.IsStream == false && v.GroupIndex < GroupCount && Groups[v.GroupIndex].Admitted > 0)
            {
                --Groups[v.GroupIndex].Admitted;
            }
            ReleaseVoiceSlot(i);
        }
    }
}

void AudioSystem::Impl::TerminateVoice(uint32 slot, TerminalReason reason, StateCause cause) noexcept
{
    VoiceSlot& v = Voices[slot];
    v.State = VoiceState::Terminal;
    v.LastCause = cause;
    // A terminal voice is not "silent for a reason"; clear transient silence
    // causes so GetVoiceInfo on a terminal/stale slot does not report stale
    // flags. The terminal reason is the authoritative explanation.
    v.Silence = SilenceCause::None;
    v.Terminal.Reason = reason;
    v.Terminal.FinalFrame = RenderFrame;
    // Release-publish the terminal generation; the owner acquires it to reclaim.
    v.Terminal.TerminalGeneration.store(v.Generation.Value, std::memory_order_release);
    if (Control != nullptr)
    {
        auto& mailbox = Control->Voices[slot].Terminal;
        mailbox.Reason = reason;
        mailbox.FinalFrame = RenderFrame;
        mailbox.TerminalGeneration.store(v.Generation.Value, std::memory_order_release);
    }
}

void AudioSystem::Impl::ApplyStopMailboxes() noexcept
{
    // Acquire each live voice's stop-generation mailbox and the StopAll epoch at
    // the boundary (design section 4). A Stop is deliverable independently of the
    // normal command queue. Cancellation takes precedence over Scheduled/Update.
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        VoiceSlot& v = Voices[i];
        if (v.InUse == false || v.State == VoiceState::Terminal)
        {
            continue;
        }
        // StopAll epoch: a voice admitted before the live epoch is cancelled.
        if (v.AdmissionEpoch < StopAllEpoch)
        {
            if (v.State == VoiceState::Mixed || v.State == VoiceState::Virtualizing)
            {
                v.State = VoiceState::Stopping;
                v.LastCause = StateCause::StopAllEpoch;
                v.FadeTarget = 0.0F;
            }
            else
            {
                TerminateVoice(i, TerminalReason::Stopped, StateCause::StopAllEpoch);
            }
            continue;
        }
        // Individual stop mailbox.
        const uint32 stopGen =
            (Control != nullptr ? Control->Voices[i].Stop : v.Stop).StopGeneration.load(std::memory_order_acquire);
        if (stopGen == v.Generation.Value)
        {
            if (v.State == VoiceState::Mixed || v.State == VoiceState::Virtualizing)
            {
                v.State = VoiceState::Stopping;
                v.LastCause = StateCause::StopRequested;
                v.FadeTarget = 0.0F;
            }
            else if (v.State != VoiceState::Stopping)
            {
                TerminateVoice(i, TerminalReason::Stopped, StateCause::StopRequested);
            }
        }
    }
}

void AudioSystem::Impl::ProcessControlBoundary() noexcept
{
    if (Control != nullptr)
    {
        StopAllEpoch = Control->PublishedStopAll.load(std::memory_order_acquire);
    }
    ConsumeAndApplyBatch();
    ApplyStopMailboxes();
    ResolveScheduledStarts();
    RecomputeBusGains();
    SelectAndCharge();
}

// ---------------------------------------------------------------------------
// Render (design sections 5-8). Walks the request in 128-frame control quanta,
// applies a boundary at each multiple, selects/binds physical voices, runs the
// resident DSP kernel into a stereo accumulator, applies the bus clamp and
// metering, and writes interleaved/planar output. Bounded, allocation-free.
// ---------------------------------------------------------------------------
Status AudioSystem::Impl::RenderFrames(std::span<float32> output,
                                       ChannelLayout layout,
                                       BufferLayout bufferLayout,
                                       uint32 frames) noexcept
{
    const uint32 channels = ChannelCount(layout);
    if (channels != 2)
    {
        return Status::Unsupported; // v1 output is stereo
    }
    if (frames == 0)
    {
        return Status::Ok;
    }
    const uint64 required = static_cast<uint64>(frames) * channels;
    if (output.size() < required)
    {
        return Status::InvalidArgument;
    }

    // Preinitialize physical voice DSP (resamplers) once, outside the per-span
    // loop (still cold relative to steady rendering; first call only).
    if (EnsurePhysicalInitialized() == false)
    {
        for (uint64 i = 0; i < required; ++i)
        {
            output[i] = 0.0F;
        }
        ++Errors;
        return Status::OutOfMemory;
    }

    // Clear output.
    for (uint64 i = 0; i < required; ++i)
    {
        output[i] = 0.0F;
    }

    const uint32 rampFrames = (DEFAULT_RAMP_MS * SampleRate) / 1000U;

    uint32 produced = 0;
    while (produced < frames)
    {
        if (RenderFrame % CONTROL_QUANTUM_FRAMES == 0)
        {
            ProcessControlBoundary();
        }
        const uint32 toNextBoundary =
            CONTROL_QUANTUM_FRAMES - static_cast<uint32>(RenderFrame % CONTROL_QUANTUM_FRAMES);
        uint32 span = frames - produced;
        if (span > toNextBoundary)
        {
            span = toNextBoundary;
        }
        if (span > internal::VOICE_SCRATCH_FRAMES)
        {
            span = internal::VOICE_SCRATCH_FRAMES;
        }

        // Clear every bus accumulator for this span (child-before-parent fold
        // happens after all voices are routed).
        BeginBusSpan(span);

        // Render every mixing/fading resident voice through the DSP kernel into
        // a per-voice stereo accumulator, then route it to the voice's bus.
        for (uint32 i = 0; i < LogicalCapacity; ++i)
        {
            VoiceSlot& v = Voices[i];
            if (v.InUse == false)
            {
                continue;
            }
            if (v.IsStream)
            {
                if (v.State == VoiceState::Mixed || v.State == VoiceState::Stopping)
                {
                    auto* stream = Streams[v.StreamSlotIndex].Data;
                    for (uint32 f = 0; f < span * 2; ++f)
                    {
                        MixAccum[f] = 0;
                    }
                    const auto rendered = stream->Render(MixAccum, {span, v.Gain, v.State == VoiceState::Stopping});
                    const bool terminal = rendered.Terminal, error = rendered.Error;
                    v.CursorFrame += rendered.Frames;
                    v.FadeGain = stream->Fade;
                    if (rendered.Frames < span && terminal == false && v.State != VoiceState::Stopping)
                    {
                        ++StreamStarvations;
                    }
                    RouteVoiceToBus(v.BusIndex, MixAccum, span);
                    if (terminal)
                    {
                        const bool stopped = v.State == VoiceState::Stopping;
                        TerminateVoice(i,
                                       error     ? TerminalReason::StreamError
                                       : stopped ? TerminalReason::Stopped
                                                 : TerminalReason::Completed,
                                       error     ? StateCause::None
                                       : stopped ? StateCause::StopRequested
                                                 : StateCause::NaturalEof);
                    }
                }
                continue;
            }
            const bool audiblePhase =
                v.State == VoiceState::Mixed || v.State == VoiceState::Virtualizing || v.State == VoiceState::Stopping;
            if (audiblePhase)
            {
                // Bind a physical slot if needed.
                uint32 phys = v.PhysicalIndex;
                if (phys >= MixedCapacity || Physical[phys].Bound == false || Physical[phys].LogicalSlot != i)
                {
                    phys = FindPhysicalFor(i);
                    if (phys >= MixedCapacity)
                    {
                        phys = BindPhysical(i);
                    }
                    v.PhysicalIndex = phys;
                }
                if (phys >= MixedCapacity)
                {
                    // No physical slot free: wait virtually for this span.
                    v.Silence |= SilenceCause::WaitingForFadeSlot;
                    continue;
                }

                internal::PhysicalVoice& ph = Physical[phys];
                const internal::ClipSlot& c = Clips[v.ClipSlotIndex];

                // Clear this voice's accumulator (the kernel accumulates +=).
                for (uint32 f = 0; f < span * 2; ++f)
                {
                    MixAccum[f] = 0.0F;
                }

                // Fade target: 1 for Mixed, 0 for Virtualizing/Stopping. The
                // kernel ramps from the carried current toward the target using
                // a FIXED per-frame step so the fade completes in a bounded
                // duration (recompute the step only when the target changes).
                const float32 fade = v.FadeTarget;
                const float32 targetL = v.TargetGainL * fade;
                const float32 targetR = v.TargetGainR * fade;
                if (ph.HasTarget == false || targetL != ph.LastTargetL || targetR != ph.LastTargetR)
                {
                    const float32 denom = rampFrames == 0 ? 1.0F : static_cast<float32>(rampFrames);
                    float32 dl = targetL - ph.CurrentGainL;
                    float32 dr = targetR - ph.CurrentGainR;
                    if (dl < 0.0F)
                    {
                        dl = -dl;
                    }
                    if (dr < 0.0F)
                    {
                        dr = -dr;
                    }
                    ph.StepL = rampFrames == 0 ? 0.0F : dl / denom;
                    ph.StepR = rampFrames == 0 ? 0.0F : dr / denom;
                    ph.LastTargetL = targetL;
                    ph.LastTargetR = targetR;
                    ph.HasTarget = true;
                }
                internal::KernelInput in{};
                in.Clip = &c;
                in.Dsp = &ph.DspFor(c.Channels);
                in.SessionRate = SampleRate;
                in.Rate = v.Rate;
                in.Looping = v.Looping;
                in.LoopBegin = c.LoopBegin;
                in.LoopEnd = c.LoopEnd;
                in.TargetGainL = targetL;
                in.TargetGainR = targetR;
                in.CurrentGainL = &ph.CurrentGainL;
                in.CurrentGainR = &ph.CurrentGainR;
                in.StepL = ph.StepL;
                in.StepR = ph.StepR;

                // Sub-span start offset (design section 5): a voice whose exact
                // StartFrame falls inside this span emits zero before its start,
                // then samples from the start offset. MixAccum[0..startOffset)
                // stays zero (cleared above); the kernel writes from the offset.
                uint32 startOffset = 0;
                if (v.StartFrame > RenderFrame)
                {
                    const uint64 delta = v.StartFrame - RenderFrame;
                    startOffset = delta >= span ? span : static_cast<uint32>(delta);
                }
                const uint32 renderFrames = span - startOffset;
                if (renderFrames > 0)
                {
                    const uint32 producedFrames =
                        internal::RenderResidentVoice(in,
                                                      MixAccum + static_cast<usize>(startOffset) * 2,
                                                      renderFrames,
                                                      Scratch,
                                                      internal::VOICE_SCRATCH_FRAMES);
                    (void)producedFrames;
                }

                // Route this voice's output to its bus (input tap, before gain).
                RouteVoiceToBus(v.BusIndex, MixAccum, span);

                v.CursorFrame = in.Dsp->SourceCursor;
                // Informational fade level: the larger carried channel gain.
                v.FadeGain = ph.CurrentGainL > ph.CurrentGainR ? ph.CurrentGainL : ph.CurrentGainR;

                // Natural EOF of a finite nonloop voice.
                if (in.Dsp->AtEof && v.Looping == false)
                {
                    ++EofEvents;
                    UnbindPhysical(phys);
                    v.PhysicalIndex = 0xFFFFFFFFU;
                    TerminateVoice(i, TerminalReason::Completed, StateCause::NaturalEof);
                    continue;
                }
                // Fade completion for Virtualizing/Stopping (gain reached 0).
                if ((v.State == VoiceState::Virtualizing || v.State == VoiceState::Stopping) &&
                    ph.CurrentGainL <= 0.0001F && ph.CurrentGainR <= 0.0001F)
                {
                    UnbindPhysical(phys);
                    v.PhysicalIndex = 0xFFFFFFFFU;
                    if (v.State == VoiceState::Stopping)
                    {
                        TerminateVoice(i, TerminalReason::Stopped, StateCause::FadeComplete);
                    }
                    else
                    {
                        v.State = VoiceState::Virtual;
                        v.LastCause = StateCause::FadeComplete;
                        v.VirtualSinceFrame = RenderFrame;
                    }
                }
            }
            else if (v.State == VoiceState::Virtual)
            {
                // Advance the virtual cursor without DSP, carrying the fractional
                // remainder on the voice so the timeline does not drift. Finite
                // nonloop voices expire; this happens even while muted (advance
                // policy), and an unmute must not resurrect old transients.
                const internal::ClipSlot& c = Clips[v.ClipSlotIndex];
                const bool alive = internal::AdvanceVirtualVoice(v.CursorFrame,
                                                                 v.CursorFraction,
                                                                 c.Frames,
                                                                 v.Rate,
                                                                 v.Looping,
                                                                 c.LoopBegin,
                                                                 c.LoopEnd,
                                                                 span);
                if (alive == false)
                {
                    ++EofEvents;
                    TerminateVoice(i, TerminalReason::Expired, StateCause::Expired);
                }
            }
        }

        // Fold the bus tree child-before-parent (each bus gain applied once) and
        // read the root bus's post-gain stereo result. Count pre-clamp overs,
        // map nonfinite to zero, then clamp to [-1,1]. The root post-gain is the
        // pre-clamp output tap (design section 8).
        FoldBusesToRoot(span);
        const uint32 root = RootBusIndex();
        const float32* rootL = Buses[root].AccumL;
        const float32* rootR = Buses[root].AccumR;
        for (uint32 f = 0; f < span; ++f)
        {
            float32 l = rootL[f];
            float32 r = rootR[f];
            if (std::isfinite(l) == false)
            {
                l = 0.0F;
                ++NonFiniteFaults;
            }
            if (std::isfinite(r) == false)
            {
                r = 0.0F;
                ++NonFiniteFaults;
            }
            if (l > 1.0F || l < -1.0F || r > 1.0F || r < -1.0F)
            {
                ++PreClipFrames;
                ++Buses[root].ClippedFrames;
            }
            if (l > 1.0F)
            {
                l = 1.0F;
            }
            if (l < -1.0F)
            {
                l = -1.0F;
            }
            if (r > 1.0F)
            {
                r = 1.0F;
            }
            if (r < -1.0F)
            {
                r = -1.0F;
            }

            const uint32 outFrame = produced + f;
            if (bufferLayout == BufferLayout::Interleaved)
            {
                output[static_cast<uint64>(outFrame) * 2 + 0] = l;
                output[static_cast<uint64>(outFrame) * 2 + 1] = r;
            }
            else // Planar: [L...][R...]
            {
                output[outFrame] = l;
                output[static_cast<uint64>(frames) + outFrame] = r;
            }
        }

        RenderFrame += span;
        produced += span;
    }

    if (Control != nullptr)
    {
        PublishRender();
    }
    return Status::Ok;
}

void AudioSystem::Impl::FillSnapshot(SystemSnapshot& out) const noexcept
{
    SystemSnapshot& s = out;
    s = SystemSnapshot{};
    s.State = State;
    s.SystemMode = SystemMode;
    s.Session = Session;
    s.SampleRate = SampleRate;
    s.RenderFrame = RenderFrame;
    s.QueueDepth = CommandRing.Available();
    s.QueueHighWater = CommandRing.HighWater();
    s.AcceptedCommands = AcceptedCommands;
    s.RejectedCommands = RejectedCommands;
    s.StaleCommands = StaleCommands;

    uint32 pending = 0;
    uint32 mixed = 0;
    uint32 virt = 0;
    uint32 fading = 0;
    uint32 streams = 0;
    uint32 unreclaimed = 0;
    uint32 clips = 0;
    uint64 pcmBytes = 0;
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        const VoiceSlot& v = Voices[i];
        if (v.InUse == false)
        {
            continue;
        }
        switch (v.State)
        {
            case VoiceState::Pending:
            case VoiceState::Scheduled:
                ++pending;
                break;
            case VoiceState::Mixed:
                v.IsStream ? ++streams : ++mixed;
                break;
            case VoiceState::Virtual:
                ++virt;
                break;
            case VoiceState::Virtualizing:
            case VoiceState::Stopping:
                ++fading;
                break;
            case VoiceState::Terminal:
                ++unreclaimed;
                break;
        }
    }
    for (uint32 i = 0; i < LogicalCapacity; ++i)
    {
        const internal::ClipSlot& c = Clips[i];
        if (c.InUse)
        {
            ++clips;
            pcmBytes += c.SampleValues * sizeof(float32);
        }
    }
    s.PendingVoices = pending;
    s.MixedVoices = mixed;
    s.VirtualVoices = virt;
    s.FadingVoices = fading;
    s.StreamVoices = streams;
    s.LogicalHighWater = LogicalHighWater;
    s.UnreclaimedTerminals = unreclaimed;
    s.StreamStarvations = StreamStarvations;
    s.EofEvents = EofEvents;
    s.Errors = Errors;
    s.ResidentClips = clips;
    s.ResidentPcmBytes = pcmBytes;
    if (Control == nullptr)
    {
        for (const auto& stream : Streams)
        {
            if (stream.InUse)
            {
                s.StreamEncodedBytes += stream.Data->EncodedBytes;
                s.StreamRingBytes += static_cast<uint64>(sizeof(internal::StreamChunk)) * STREAM_CHUNK_COUNT;
            }
        }
    }
    s.SnapshotLosses = SnapshotLosses;
    s.PreClipFrames = PreClipFrames;
    s.NonFiniteFaults = NonFiniteFaults;
}

void AudioSystem::Impl::PublishSnapshot() noexcept
{
    internal::SnapshotRecord rec{};
    FillSnapshot(rec.System);
    std::span<const internal::SnapshotRecord> one(&rec, 1);
    if (SnapshotRing.TryPublishBatch(one) == false)
    {
        ++SnapshotLosses;
    }
}

} // namespace ludus::audio
