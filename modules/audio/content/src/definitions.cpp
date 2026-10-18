#include <ludus/audio/content/definitions.h>
#include <ludus/content/json.h>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::audio::content
{
namespace
{
using ludus::content::JsonValue;
ContentStatus Bad(Diagnostic& diagnostic, const char* field) noexcept
{
    diagnostic.Result = ContentStatus::Invalid;
    usize i = 0;
    while (field[i] != '\0' && i < 127)
    {
        diagnostic.Field[i] = field[i];
        ++i;
    }
    diagnostic.Field[i] = '\0';
    return diagnostic.Result;
}
bool Id(JsonValue root, const char* field, ResourceId& id) noexcept
{
    std::string_view value;
    return root.Get(field).String(value) && ludus::content::ValidId(value) && id.Set(value);
}
bool Number(JsonValue root, const char* field, float32& output, float64 min, float64 max) noexcept
{
    float64 value = 0;
    if (root.Get(field).Number(value) == false ||
        (ludus::foundation::math::IsFinite(value) == false || value < min || value > max))
    {
        return false;
    }
    output = static_cast<float32>(value);
    return true;
}
bool ReadLoop(JsonValue root, Loop& loop) noexcept
{
    return root.Fields({"enabled", "begin_frame", "end_frame"}) && root.Get("enabled").Boolean(loop.Enabled) &&
           root.Get("begin_frame").Integer(loop.Begin) && root.Get("end_frame").Integer(loop.End) &&
           (loop.Enabled ? loop.Begin < loop.End : loop.Begin == 0 && loop.End == 0);
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters): named definition scalar fields.
bool Common(JsonValue root,
            ResourceId& id,
            ResourceId& bus,
            uint8& priority,
            float32& gain,
            Loop& loop,
            Diagnostic& diagnostic) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    uint64 version = 0, p = 0;
    if (root.Get("version").Integer(version) == false || version != 1)
    {
        (void)Bad(diagnostic, "version");
        return false;
    }
    if (Id(root, "id", id) == false)
    {
        (void)Bad(diagnostic, "id");
        return false;
    }
    if (Id(root, "bus", bus) == false)
    {
        (void)Bad(diagnostic, "bus");
        return false;
    }
    if (root.Get("priority").Integer(p) == false || p > 7)
    {
        (void)Bad(diagnostic, "priority");
        return false;
    }
    if (Number(root, "gain", gain, 0, 1) == false)
    {
        (void)Bad(diagnostic, "gain");
        return false;
    }
    if (ReadLoop(root.Get("loop"), loop) == false)
    {
        (void)Bad(diagnostic, "loop");
        return false;
    }
    priority = static_cast<uint8>(p);
    return true;
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters): named definition scalar fields.
void WriteCommon(ludus::content::JsonWriter& w,
                 const ResourceId& id,
                 const ResourceId& bus,
                 uint8 priority,
                 float32 gain) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    w.Raw("{\n  \"version\": 1,\n  \"id\": ");
    w.String(id.View());
    w.Raw(",\n  \"bus\": ");
    w.String(bus.View());
    w.Raw(",\n  \"priority\": ");
    w.Integer(priority);
    w.Raw(",\n  \"gain\": ");
    w.Number(static_cast<float64>(gain));
}
void WriteLoop(ludus::content::JsonWriter& w, const Loop& loop) noexcept
{
    w.Raw(",\n  \"loop\": { \"enabled\": ");
    w.Boolean(loop.Enabled);
    w.Raw(", \"begin_frame\": ");
    w.Integer(loop.Begin);
    w.Raw(", \"end_frame\": ");
    w.Integer(loop.End);
    w.Raw(" }");
}
uint32 Index(std::span<const std::string_view> names, std::string_view name) noexcept
{
    for (usize i = 0; i < names.size(); ++i)
    {
        if (names[i] == name)
        {
            return static_cast<uint32>(i);
        }
    }
    return static_cast<uint32>(names.size());
}
} // namespace
ContentStatus ReadSound(std::string_view json, Sound& output, Diagnostic& diagnostic) noexcept
{
    ludus::content::JsonDocument doc;
    const auto status = doc.Read(json, diagnostic);
    if (status != ContentStatus::Ok)
    {
        return status;
    }
    const auto root = doc.Root();
    Sound next;
    if (root.Fields(
            {"version", "id", "bus", "group", "priority", "gain", "rate", "spatial", "loop", "policy", "variations"}) ==
        false)
    {
        return Bad(diagnostic, "sound.fields");
    }
    if (Common(root, next.Id, next.Bus, next.Priority, next.Gain, next.Region, diagnostic) == false)
    {
        return diagnostic.Result;
    }
    if (Id(root, "group", next.Group) == false || Number(root, "rate", next.Rate, 0.5, 2.0) == false)
    {
        return Bad(diagnostic, "sound.rate/group");
    }
    const auto spatial = root.Get("spatial");
    std::string_view mode;
    if (spatial.Fields({"mode", "min_distance", "max_distance"}) == false ||
        spatial.Get("mode").String(mode) == false || (mode != "point" && mode != "none") ||
        Number(spatial, "min_distance", next.MinDistance, 0, 1e30) == false ||
        Number(spatial, "max_distance", next.MaxDistance, 0, 1e30) == false || next.MaxDistance <= next.MinDistance)
    {
        return Bad(diagnostic, "sound.spatial");
    }
    next.Positional = mode == "point";
    const auto policy = root.Get("policy");
    uint64 cooldown = 0;
    if (policy.Fields({"cooldown_ms", "suppress_while_active"}) == false ||
        policy.Get("cooldown_ms").Integer(cooldown) == false || cooldown > 600000 ||
        policy.Get("suppress_while_active").Boolean(next.SuppressWhileActive) == false)
    {
        return Bad(diagnostic, "sound.policy");
    }
    next.CooldownMs = static_cast<uint32>(cooldown);
    const auto variations = root.Get("variations");
    if (variations.Array() == false || variations.Count() == 0 || variations.Count() > 16)
    {
        return Bad(diagnostic, "sound.variations");
    }
    next.VariationCount = variations.Count();
    for (usize i = 0; i < next.VariationCount; ++i)
    {
        std::string_view id;
        if (variations.At(i).String(id) == false || ludus::content::ValidId(id) == false ||
            next.Variations[i].Set(id) == false)
        {
            return Bad(diagnostic, "sound.variations.id");
        }
        for (usize j = 0; j < i; ++j)
        {
            if (next.Variations[j].View() == id)
            {
                return Bad(diagnostic, "sound.variations.duplicate");
            }
        }
    }
    output = next;
    return ContentStatus::Ok;
}
ContentStatus ReadMusic(std::string_view json, Music& output, Diagnostic& diagnostic) noexcept
{
    ludus::content::JsonDocument doc;
    const auto status = doc.Read(json, diagnostic);
    if (status != ContentStatus::Ok)
    {
        return status;
    }
    const auto root = doc.Root();
    Music next;
    if (root.Fields({"version", "id", "source", "bus", "priority", "gain", "loop"}) == false)
    {
        return Bad(diagnostic, "music.fields");
    }
    if (Common(root, next.Id, next.Bus, next.Priority, next.Gain, next.Region, diagnostic) == false)
    {
        return diagnostic.Result;
    }
    if (Id(root, "source", next.Source) == false)
    {
        return Bad(diagnostic, "source");
    }
    output = next;
    return ContentStatus::Ok;
}
ContentStatus WriteSound(const Sound& sound, Bytes& output) noexcept
{
    Bytes scratch;
    if (scratch.Resize(16384) == false)
    {
        return ContentStatus::OutOfMemory;
    }
    ludus::content::JsonWriter w(scratch.Data());
    WriteCommon(w, sound.Id, sound.Bus, sound.Priority, sound.Gain);
    w.Raw(",\n  \"group\": ");
    w.String(sound.Group.View());
    w.Raw(",\n  \"rate\": ");
    w.Number(static_cast<float64>(sound.Rate));
    w.Raw(",\n  \"spatial\": { \"mode\": ");
    w.String(sound.Positional ? "point" : "none");
    w.Raw(", \"min_distance\": ");
    w.Number(static_cast<float64>(sound.MinDistance));
    w.Raw(", \"max_distance\": ");
    w.Number(static_cast<float64>(sound.MaxDistance));
    w.Raw(" }");
    WriteLoop(w, sound.Region);
    w.Raw(",\n  \"policy\": { \"cooldown_ms\": ");
    w.Integer(sound.CooldownMs);
    w.Raw(", \"suppress_while_active\": ");
    w.Boolean(sound.SuppressWhileActive);
    w.Raw(" },\n  \"variations\": [");
    if (sound.VariationCount > 16)
    {
        return ContentStatus::Limit;
    }
    for (usize i = 0; i < sound.VariationCount; ++i)
    {
        if (i != 0)
        {
            w.Raw(", ");
        }
        w.String(sound.Variations[i].View());
    }
    w.Raw("]\n}");
    Bytes candidate;
    auto status = w.Finish(candidate);
    if (status != ContentStatus::Ok)
    {
        return status;
    }
    Sound checked;
    Diagnostic d;
    status = ReadSound(candidate.String(), checked, d);
    if (status == ContentStatus::Ok)
    {
        output = Move(candidate);
    }
    return status;
}
ContentStatus WriteMusic(const Music& music, Bytes& output) noexcept
{
    Bytes scratch;
    if (scratch.Resize(4096) == false)
    {
        return ContentStatus::OutOfMemory;
    }
    ludus::content::JsonWriter w(scratch.Data());
    WriteCommon(w, music.Id, music.Bus, music.Priority, music.Gain);
    w.Raw(",\n  \"source\": ");
    w.String(music.Source.View());
    WriteLoop(w, music.Region);
    w.Raw("\n}");
    Bytes candidate;
    auto status = w.Finish(candidate);
    if (status != ContentStatus::Ok)
    {
        return status;
    }
    Music checked;
    Diagnostic d;
    status = ReadMusic(candidate.String(), checked, d);
    if (status == ContentStatus::Ok)
    {
        output = Move(candidate);
    }
    return status;
}
ContentStatus Resolve(const Sound& sound,
                      const ludus::content::Catalog& catalog,
                      const Routing& routing,
                      app::EventDescriptor& output,
                      Diagnostic& diagnostic) noexcept
{
    if (sound.VariationCount == 0 || sound.VariationCount > 16)
    {
        return Bad(diagnostic, "variations.count");
    }
    app::EventDescriptor next;
    next.BusIndex = Index(routing.Buses, sound.Bus.View());
    next.GroupIndex = Index(routing.Groups, sound.Group.View());
    if (next.BusIndex >= routing.Buses.size() || next.GroupIndex >= routing.Groups.size())
    {
        return Bad(diagnostic, "routing");
    }
    const auto* entry = catalog.Find(sound.Id.View());
    if (entry == nullptr || entry->Type != ludus::content::Kind::Sound)
    {
        return Bad(diagnostic, "id");
    }
    for (usize i = 0; i < sound.VariationCount; ++i)
    {
        const auto* source = catalog.Find(sound.Variations[i].View());
        if (source == nullptr || source->Type != ludus::content::Kind::AudioSource)
        {
            return Bad(diagnostic, "variations.reference");
        }
    }
    next.Priority = sound.Priority;
    next.DefaultGain = sound.Gain;
    next.Rate = sound.Rate;
    next.Positional = sound.Positional;
    next.MinDistance = sound.MinDistance;
    next.MaxDistance = sound.MaxDistance;
    next.Looping = sound.Region.Enabled;
    output = next;
    return ContentStatus::Ok;
}
ContentStatus ValidateMusic(const Music& music,
                            const ludus::content::Catalog& catalog,
                            const Routing& routing,
                            uint32& bus,
                            Diagnostic& diagnostic) noexcept
{
    const auto* entry = catalog.Find(music.Id.View());
    const auto* source = catalog.Find(music.Source.View());
    const auto index = Index(routing.Buses, music.Bus.View());
    if (entry == nullptr || entry->Type != ludus::content::Kind::Music || source == nullptr ||
        source->Type != ludus::content::Kind::AudioSource || index >= routing.Buses.size())
    {
        return Bad(diagnostic, "music.reference/routing");
    }
    bus = index;
    return ContentStatus::Ok;
}
} // namespace ludus::audio::content
