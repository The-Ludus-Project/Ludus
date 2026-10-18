#pragma once
#include <ludus/foundation/base/core.h>

#include <ludus/audio/content/loader.h>

// Game-owned copied outbox data. Engine handles never enter level/save data.
namespace sample
{
using namespace ludus::foundation;
namespace audio = ludus::audio;
struct Owner final
{
    uint64 World = 0;
    uint32 Slot = 0;
    uint32 Generation = 0;
    [[nodiscard]] bool operator==(const Owner&) const noexcept = default;
};
struct Request final
{
    uint64 Sequence = 0;
    Owner Entity;
    uint32 Seed = 0;
    bool Remove = false;
    bool Required = false;
    bool SingleStep = false;
    ludus::content::ResourceId SoundId;
};
class Presentation final
{
public:
    explicit Presentation(audio::AudioSystem& system) noexcept : System(system), Events(system) {}
    // Called after a committed simulation tick; render frequency is irrelevant.
    // Required admission failures remain retryable. Optional failures are dropped
    // explicitly, and a repeated/stale sequence never triggers another voice.
    [[nodiscard]] audio::Status Deliver(const Request& request,
                                        uint64 tick,
                                        const audio::content::Sound& sound,
                                        const audio::app::EventDescriptor& event) noexcept
    {
        if (request.Sequence == 0)
        {
            return audio::Status::InvalidArgument;
        }
        if (Paused && request.SingleStep == false)
        {
            return audio::Status::NotReady;
        }
        if (request.Sequence <= LastSequence)
        {
            return audio::Status::Ok;
        }
        if (request.Sequence != LastSequence + 1)
        {
            return audio::Status::InvalidArgument;
        }
        if (request.Entity.World != World || request.Entity.Generation == 0)
        {
            LastSequence = request.Sequence;
            return audio::Status::Ok;
        }
        if (request.Entity.Slot >= 64)
        {
            if (request.Required == false)
            {
                LastSequence = request.Sequence;
            }
            return audio::Status::VoiceCapacity;
        }
        auto* mapping = &Owners[request.Entity.Slot];
        if (request.Entity.Generation < mapping->Entity.Generation ||
            (request.Entity.Generation == mapping->Entity.Generation && mapping->InUse == false))
        {
            LastSequence = request.Sequence;
            return audio::Status::Ok;
        }
        if (request.Remove)
        {
            if (mapping->InUse)
            {
                Events.StopOwnedLoops(mapping->Tag);
            }
            mapping->Entity = request.Entity;
            mapping->InUse = false;
            LastSequence = request.Sequence;
            return audio::Status::Ok;
        }
        if (request.SoundId.View() != sound.Id.View())
        {
            return audio::Status::InvalidArgument;
        }
        if (request.Entity.Generation > mapping->Entity.Generation)
        {
            if (NextTag == 0)
            {
                if (request.Required == false)
                {
                    LastSequence = request.Sequence;
                }
                return audio::Status::VoiceCapacity;
            }
            if (mapping->InUse)
            {
                Events.StopOwnedLoops(mapping->Tag);
            }
            mapping->InUse = true;
            mapping->Entity = request.Entity;
            mapping->Tag = NextTag++;
        }
        const auto result = Events.TriggerDescriptor(event,
                                                     mapping->Tag,
                                                     (static_cast<uint64>(sound.CooldownMs) * 60 + 999) / 1000,
                                                     request.Seed,
                                                     tick,
                                                     sound.SuppressWhileActive);
        const auto status = result.Suppressed == audio::app::SuppressReason::TableFull ? audio::Status::VoiceCapacity
                                                                                       : result.EngineStatus;
        if (status == audio::Status::Ok || request.Required == false)
        {
            LastSequence = request.Sequence;
        }
        return status;
    }
    [[nodiscard]] audio::Status SetPaused(bool paused) noexcept
    {
        if (paused == Paused)
        {
            return audio::Status::Ok;
        }
        audio::Status status = audio::Status::Ok;
        if (paused)
        {
            const audio::ModifierValue values[] = {{1, -60.0F}, {2, -60.0F}};
            status = System.AcquireModifier(values, 1.0F, PauseModifier);
        }
        else
        {
            status = System.ReleaseModifier(PauseModifier);
        }
        if (status == audio::Status::Ok)
        {
            Paused = paused;
        }
        return status;
    }
    void ChangeWorld(uint64 world) noexcept
    {
        for (auto& entry : Owners)
        {
            if (entry.InUse)
            {
                Events.StopOwnedLoops(entry.Tag);
            }
            entry = {};
        }
        World = world;
    }
    void Service(uint64 tick) noexcept
    {
        System.Service();
        Events.Update(tick);
    }
    [[nodiscard]] uint64 DeliveredSequence() const noexcept
    {
        return LastSequence;
    }

private:
    struct Mapping final
    {
        bool InUse = false;
        Owner Entity;
        uint32 Tag = 0;
    };
    audio::AudioSystem& System;
    audio::app::EventAdapter Events;
    Mapping Owners[64];
    uint64 World = 0;
    uint64 LastSequence = 0;
    uint32 NextTag = 1;
    bool Paused = false;
    audio::ModifierHandle PauseModifier;
};
} // namespace sample
