#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>

#include <ludus/audio/audio_app_adapter.h>

#include <span>
#include <string_view>

namespace ludus::audio::content
{
using namespace ludus::foundation;
using ludus::content::Bytes;
using ludus::content::Diagnostic;
using ludus::content::ResourceId;
using ludus::content::Text;
using ContentStatus = ludus::content::Status;
/// Longest retrigger cooldown a sound definition may declare (`Sound::CooldownMs`), in
/// milliseconds (ten minutes). Reading rejects larger values; editors use it as their input bound.
inline constexpr uint64 MAX_COOLDOWN_MILLISECONDS = 600000;
struct Loop final
{
    bool Enabled = false;
    uint64 Begin = 0;
    uint64 End = 0;
};
struct Sound final
{
    ResourceId Id;
    ResourceId Bus;
    ResourceId Group;
    uint8 Priority = 4;
    float32 Gain = 1.0F;
    float32 Rate = 1.0F;
    bool Positional = false;
    float32 MinDistance = 1.0F;
    float32 MaxDistance = 100.0F;
    Loop Region;
    uint32 CooldownMs = 0;
    bool SuppressWhileActive = false;
    ResourceId Variations[16];
    usize VariationCount = 0;
};
struct Music final
{
    ResourceId Id;
    ResourceId Source;
    ResourceId Bus;
    uint8 Priority = 6;
    float32 Gain = 1.0F;
    Loop Region;
};
struct Routing final
{
    std::span<const std::string_view> Buses;
    std::span<const std::string_view> Groups;
};
[[nodiscard]] ContentStatus ReadSound(std::string_view json, Sound& output, Diagnostic& diagnostic) noexcept;
[[nodiscard]] ContentStatus ReadMusic(std::string_view json, Music& output, Diagnostic& diagnostic) noexcept;
[[nodiscard]] ContentStatus WriteSound(const Sound& sound, Bytes& output) noexcept;
[[nodiscard]] ContentStatus WriteMusic(const Music& music, Bytes& output) noexcept;
[[nodiscard]] ContentStatus Resolve(const Sound& sound,
                                    const ludus::content::Catalog& catalog,
                                    const Routing& routing,
                                    app::EventDescriptor& output,
                                    Diagnostic& diagnostic) noexcept;
[[nodiscard]] ContentStatus ValidateMusic(const Music& music,
                                          const ludus::content::Catalog& catalog,
                                          const Routing& routing,
                                          uint32& bus,
                                          Diagnostic& diagnostic) noexcept;
} // namespace ludus::audio::content
