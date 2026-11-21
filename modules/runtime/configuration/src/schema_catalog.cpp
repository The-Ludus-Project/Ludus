#include <ludus/runtime/configuration/schema_catalog.hpp>

#include <ludus/audio/configuration/audio_configuration.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/runtime/configuration/host_options.hpp>

namespace ludus::runtime::configuration
{
namespace
{
/// GameHost startup and audio.
constexpr foundation::usize SCHEMA_COUNT = 2;
} // namespace

std::span<const SchemaEntry> GetSchemaCatalog() noexcept
{
    static const foundation::StaticArray<SchemaEntry, SCHEMA_COUNT> CATALOG{
        SchemaEntry{ .Identity = HOST_SCHEMA, .Title = "GameHost", .Settings = HostSchema() },
        SchemaEntry
        {
            .Identity = audio::configuration::AUDIO_SCHEMA,
            .Title = "Audio",
            .Settings = audio::configuration::AudioSchema(),
        },
    };
    return {CATALOG.GetData(), CATALOG.GetSize()};
}

const SchemaEntry* FindSchemaOrNull(std::string_view identity) noexcept
{
    for (const SchemaEntry& entry : GetSchemaCatalog())
    {
        if (entry.Identity == identity)
        {
            return &entry;
        }
    }
    return nullptr;
}
} // namespace ludus::runtime::configuration
