#include <ludus/audio/audio_app_adapter.h>

namespace ludus::audio::app
{
EventAdapter::EventAdapter(AudioSystem& system) noexcept : mSystem(system) {}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): policy table key.
uint32 EventAdapter::FindPolicy(uint32 eventId, uint32 ownerTag) const noexcept
{
    for (uint32 i = 0; i < POLICY_TABLE_CAPACITY; ++i)
    {
        if (mPolicy[i].InUse && mPolicy[i].EventId == eventId && mPolicy[i].OwnerTag == ownerTag)
        {
            return i;
        }
    }
    return POLICY_TABLE_CAPACITY;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): policy table key.
uint32 EventAdapter::AllocPolicy(uint32 eventId, uint32 ownerTag) noexcept
{
    for (uint32 i = 0; i < POLICY_TABLE_CAPACITY; ++i)
    {
        if (mPolicy[i].InUse == false)
        {
            mPolicy[i] = PolicyEntry{};
            mPolicy[i].InUse = true;
            mPolicy[i].EventId = eventId;
            mPolicy[i].OwnerTag = ownerTag;
            return i;
        }
    }
    return POLICY_TABLE_CAPACITY;
}

bool EventAdapter::VoiceStillActive(VoiceHandle v) const noexcept
{
    if (v.IsValid() == false)
    {
        return false;
    }
    VoiceInfo info{};
    const Status s = mSystem.GetVoiceInfo(v, info);
    if (IsOk(s) == false)
    {
        // Reclaimed/stale generation of a previously accepted handle proves the
        // owner no longer owns a live slot (design section 4).
        return false;
    }
    return info.State != VoiceState::Terminal;
}

void EventAdapter::TrackLoop(uint32 ownerTag, VoiceHandle v) noexcept
{
    for (auto& loop : mLoops)
    {
        if (loop.InUse == false)
        {
            loop.InUse = true;
            loop.OwnerTag = ownerTag;
            loop.Voice = v;
            return;
        }
    }
    // Table full: the loop still plays but is untracked for teardown. This is an
    // explicit bounded limit, surfaced via the sink.
    EmitOwner(OwnerEventKind::PolicySuppressed, Status::Ok, PolicyCause::TableFull, 0, ownerTag, 0);
}

void EventAdapter::EmitOwner(OwnerEventKind kind,
                             Status status,
                             PolicyCause cause,
                             uint32 tag, // NOLINT(bugprone-easily-swappable-parameters)
                             uint32 owner,
                             uint64 clock) noexcept
{
    if (mSink == nullptr)
    {
        return;
    }
    OwnerEvent ev{};
    ev.Kind = kind;
    ev.Result = status;
    ev.Policy = cause;
    ev.PolicyTag = tag;
    ev.OwnerKey = owner;
    ev.ApplicationClock = clock;
    mSink->OnOwnerEvent(ev);
}

TriggerResult EventAdapter::Trigger(const TriggerRequest& req, uint64 nowTicks) noexcept
{
    TriggerResult out{};

    // --- Policy evaluation BEFORE any reservation ---
    uint32 slot = FindPolicy(req.EventId, req.OwnerTag);
    if (slot < POLICY_TABLE_CAPACITY)
    {
        PolicyEntry& e = mPolicy[slot];
        // AlreadyActive: an accepted (incl. Pending/virtual) instance still live.
        if (req.SuppressWhileActive && e.HasActive && VoiceStillActive(e.Active))
        {
            out.Suppressed = SuppressReason::AlreadyActive;
            EmitOwner(OwnerEventKind::PolicySuppressed,
                      Status::Ok,
                      PolicyCause::AlreadyActive,
                      req.EventId,
                      req.OwnerTag,
                      nowTicks);
            return out;
        }
        // Cooldown: too soon since the last successful admission.
        if (e.CooldownTicks > 0 && nowTicks < e.LastAdmitTick + e.CooldownTicks)
        {
            out.Suppressed = SuppressReason::Cooldown;
            EmitOwner(OwnerEventKind::PolicySuppressed,
                      Status::Ok,
                      PolicyCause::Cooldown,
                      req.EventId,
                      req.OwnerTag,
                      nowTicks);
            return out;
        }
    }
    else
    {
        // New key: ensure there is table room before attempting admission.
        bool hasRoom = false;
        for (const auto& e : mPolicy)
        {
            if (e.InUse == false)
            {
                hasRoom = true;
                break;
            }
        }
        if (hasRoom == false)
        {
            out.Suppressed = SuppressReason::TableFull;
            EmitOwner(OwnerEventKind::PolicySuppressed,
                      Status::Ok,
                      PolicyCause::TableFull,
                      req.EventId,
                      req.OwnerTag,
                      nowTicks);
            return out;
        }
    }

    // --- Attempt admission ---
    VoiceHandle v{};
    const Status s = mSystem.PlayClip(req.Play, v);
    out.EngineStatus = s;
    if (IsOk(s) == false)
    {
        // Engine failure consumes no cooldown and leaves no active/duck state.
        EmitOwner(OwnerEventKind::VoiceCapacityRejected, s, PolicyCause::None, req.EventId, req.OwnerTag, nowTicks);
        return out;
    }

    // --- Only successful admission updates policy state ---
    if (slot >= POLICY_TABLE_CAPACITY)
    {
        slot = AllocPolicy(req.EventId, req.OwnerTag);
    }
    if (slot < POLICY_TABLE_CAPACITY)
    {
        PolicyEntry& e = mPolicy[slot];
        e.CooldownTicks = req.CooldownTicks;
        e.LastAdmitTick = nowTicks;
        e.HasActive = true;
        e.Active = v;
    }
    if (req.IsOwnedLoop)
    {
        TrackLoop(req.OwnerTag, v);
    }

    out.Admitted = true;
    out.Voice = v;
    return out;
}

DescriptorValidation EventAdapter::ValidateDescriptor(const EventDescriptor& desc) const noexcept
{
    DescriptorValidation out{};
    if (desc.VariationCount == 0 || desc.VariationCount > MAX_VARIATIONS)
    {
        out.Field = DescriptorField::VariationCount;
        return out;
    }
    for (uint32 i = 0; i < desc.VariationCount; ++i)
    {
        if (mSystem.IsClipReady(desc.Variations[i]) == false)
        {
            out.Field = DescriptorField::VariationHandle;
            out.BadVariationIndex = i;
            return out;
        }
    }
    if (desc.Priority > PRIORITY_MAX)
    {
        out.Field = DescriptorField::Priority;
        return out;
    }
    if ((desc.DefaultGain >= 0.0F) == false || desc.DefaultGain > 1.0F)
    {
        out.Field = DescriptorField::Gain; // also catches NaN (fails >= 0)
        return out;
    }
    if ((desc.Rate >= RATE_MIN) == false || desc.Rate > RATE_MAX)
    {
        out.Field = DescriptorField::Rate;
        return out;
    }
    if (desc.Positional && (desc.MinDistance < 0.0F || desc.MaxDistance <= desc.MinDistance))
    {
        out.Field = DescriptorField::Distance;
        return out;
    }
    // Bus/group IDs are validated by the engine at admission; a descriptor with
    // an out-of-range id is rejected there. Report range here best-effort via
    // the engine's own acceptance (left to admission to avoid duplicating bus
    // count knowledge in the adapter).
    out.Valid = true;
    out.Field = DescriptorField::None;
    return out;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): named descriptor args.
TriggerResult EventAdapter::TriggerDescriptor(const EventDescriptor& desc,
                                              uint32 ownerTag,
                                              uint64 cooldownTicks,
                                              uint32 seed,
                                              uint64 nowTicks,
                                              bool suppressWhileActive) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    TriggerResult out{};
    const DescriptorValidation v = ValidateDescriptor(desc);
    if (v.Valid == false)
    {
        // Owner-side preparation/validation failure: no voice, no phantom.
        EmitOwner(OwnerEventKind::PreparationFailed,
                  Status::InvalidArgument,
                  PolicyCause::None,
                  desc.EventId,
                  ownerTag,
                  nowTicks);
        return out;
    }

    // Deterministic variation selection from the supplied seed. The chosen
    // variant is resolved ONCE here and carried by the resulting voice; reentry
    // never re-rolls it (the renderer stores the clip slot at admission).
    const uint32 variant = desc.VariationCount == 1 ? 0 : (seed % desc.VariationCount);

    TriggerRequest req{};
    req.EventId = desc.EventId;
    req.OwnerTag = ownerTag;
    req.CooldownTicks = cooldownTicks;
    req.SuppressWhileActive = suppressWhileActive;
    req.IsOwnedLoop = desc.Looping;
    req.Play.Clip = desc.Variations[variant];
    req.Play.BusIndex = desc.BusIndex;
    req.Play.GroupIndex = desc.GroupIndex;
    req.Play.Priority = desc.Priority;
    req.Play.Gain = desc.DefaultGain;
    req.Play.Rate = desc.Rate;
    req.Play.Looping = desc.Looping;
    req.Play.Policy = desc.Policy;
    req.Play.Positional = desc.Positional;
    req.Play.MinDistance = desc.MinDistance;
    req.Play.MaxDistance = desc.MaxDistance;
    req.Play.PolicyTag = desc.EventId; // numeric tag for diagnostic correlation
    return Trigger(req, nowTicks);
}

void EventAdapter::Update(uint64 nowTicks) noexcept
{
    // Release active membership for durably-terminated voices, and reclaim
    // policy entries whose cooldown has elapsed and have no live instance.
    for (auto& e : mPolicy)
    {
        if (e.InUse == false)
        {
            continue;
        }
        if (e.HasActive && VoiceStillActive(e.Active) == false)
        {
            e.HasActive = false;
            e.Active = VoiceHandle{};
        }
        const bool cooldownElapsed = e.CooldownTicks == 0 || nowTicks >= e.LastAdmitTick + e.CooldownTicks;
        if (e.HasActive == false && cooldownElapsed)
        {
            e.InUse = false; // reclaim; owner-key reuse gets a fresh entry
        }
    }
    // Reap finished owned loops.
    for (auto& loop : mLoops)
    {
        if (loop.InUse && VoiceStillActive(loop.Voice) == false)
        {
            loop.InUse = false;
        }
    }
    // Release the shared dialogue duck after the last member terminates.
    if (mDuckActive)
    {
        bool any = false;
        for (auto& d : mDialogue)
        {
            if (d.InUse)
            {
                if (VoiceStillActive(d.Voice))
                {
                    any = true;
                }
                else
                {
                    d.InUse = false; // member durably terminated
                }
            }
        }
        if (any == false)
        {
            // Remove the owned duck modifier (never restore bus gain to 1).
            (void)mSystem.ReleaseModifier(mDuck);
            mDuck = ModifierHandle{};
            mDuckActive = false;
        }
    }
}

void EventAdapter::StopOwnedLoops(uint32 ownerTag) noexcept
{
    for (auto& loop : mLoops)
    {
        if (loop.InUse && loop.OwnerTag == ownerTag)
        {
            (void)mSystem.Stop(loop.Voice);
            loop.InUse = false;
        }
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named duck config.
void EventAdapter::ConfigureDialogueDuck(uint32 duckBusIndex, float32 duckDb) noexcept
{
    mDuckBus = duckBusIndex;
    mDuckDb = duckDb;
    mDuckConfigured = true;
}

TriggerResult EventAdapter::TriggerDialogue(const TriggerRequest& req, uint64 nowTicks) noexcept
{
    // Admit the dialogue line through the normal policed path first. A failed
    // admission must create no member and no duck.
    TriggerResult out = Trigger(req, nowTicks);
    if (out.Admitted == false || mDuckConfigured == false)
    {
        return out;
    }

    // On the first accepted member, acquire the shared duck modifier.
    if (mDuckActive == false)
    {
        ModifierValue mv{};
        mv.BusIndex = mDuckBus;
        mv.TargetDb = mDuckDb;
        ModifierHandle h{};
        if (IsOk(mSystem.AcquireModifier(std::span<const ModifierValue>(&mv, 1), 1.0F, h)))
        {
            mDuck = h;
            mDuckActive = true;
        }
    }
    // Record membership.
    for (auto& d : mDialogue)
    {
        if (d.InUse == false)
        {
            d.InUse = true;
            d.Voice = out.Voice;
            break;
        }
    }
    return out;
}

bool EventAdapter::DialogueDuckActive() const noexcept
{
    return mDuckActive;
}

uint32 EventAdapter::DialogueMemberCount() const noexcept
{
    uint32 n = 0;
    for (const auto& d : mDialogue)
    {
        if (d.InUse)
        {
            ++n;
        }
    }
    return n;
}

} // namespace ludus::audio::app
