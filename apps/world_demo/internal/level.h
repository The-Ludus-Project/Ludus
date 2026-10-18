#pragma once
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/identity/uuid.hpp>

#include <string_view>
namespace ludus::world_demo
{
using namespace foundation;
struct Vec2 final
{
    float32 X = 0;
    float32 Y = 0;
};
struct Name final
{
    char Text[32] = {};
    [[nodiscard]] std::string_view View() const noexcept
    {
        usize size = 0;
        while (size < sizeof(Text) && Text[size] != 0)
        {
            ++size;
        }
        return {Text, size};
    }
    [[nodiscard]] bool operator==(const Name& other) const noexcept
    {
        return View() == other.View();
    }
};
struct Box final
{
    Vec2 Center{};
    Vec2 HalfExtent{};
    Name Id{};
    /// Durable identity from a version-2 World record; nil for version-1 and compiled content.
    foundation::identity::SourceObjectId Object{};
};
enum class Kind : uint8
{
    Player,
    Enemy,
    Exit
};
struct Recipe final
{
    Name Id{};
    Kind Type = Kind::Player;
    Vec2 Position{};
    float32 Rotation = 0;
    Vec2 Scale{1, 1};
    float32 Speed = 0;
    uint32 Health = 0;
    Name Sprite{};
    Name Target{};
    Vec2 HalfExtent{0.4F, 0.4F};
    Name NextLevel{};
    /// Durable identity from a version-2 World record; nil for version-1 and compiled content.
    foundation::identity::SourceObjectId Object{};
};
/// Most scatter layers one room authors; the kernel's own limit is larger.
inline constexpr usize MAX_ROOM_SCATTER_LAYERS = 4;
/// Most variants one authored layer lists.
inline constexpr usize MAX_ROOM_SCATTER_VARIANTS = 4;
/// Most candidates per cell a layer may author; equals the kernel's limit (asserted where both are visible).
inline constexpr usize MAX_ROOM_SCATTER_CANDIDATES_PER_CELL = 64;
/// Most candidates one room's layers may enumerate; bounds the scratch storage prepared with the room.
inline constexpr usize MAX_ROOM_SCATTER_CANDIDATES = 4096;
/// Most accepted placements one room keeps; the render frame reserves this many draws for them.
inline constexpr usize MAX_ROOM_PLACEMENTS = 128;
/// Most lattice units per metre an author may choose.
inline constexpr uint32 MAX_SCATTER_UNITS_PER_METRE = 1000;
/// Largest lattice magnitude a scatter coordinate may reach (100000 metres at the finest lattice).
inline constexpr int32 MAX_SCATTER_LATTICE_MAGNITUDE = 100000000;
/// Scale unit of authored and generated scatter scales: a scale of 1000 is the variant's authored size.
inline constexpr uint16 SCATTER_SCALE_UNIT = 1000;
/// Largest keep-out margin in metres.
inline constexpr float32 MAX_SCATTER_KEEP_OUT_MARGIN = 10.0F;
/// Default lattice resolution: one centimetre.
inline constexpr uint32 DEFAULT_SCATTER_UNITS_PER_METRE = 100;
/// Default drawn half extent of a variant in metres.
inline constexpr float32 DEFAULT_SCATTER_HALF_EXTENT = 0.25F;
/// Colour channels an authored variant carries (red, green, blue).
inline constexpr usize SCATTER_COLOR_CHANNELS = 3;
/// Largest stable layer ID; layer IDs are stored in 8 bits of the placement identity.
inline constexpr uint64 MAX_SCATTER_LAYER_ID = 255;
/// Largest authored cell size, exclusion radius or scale: the 16-bit kernel fields.
inline constexpr uint64 MAX_SCATTER_UINT16_FIELD = 65535;
/// One drawable choice of a scatter layer (`scatter_variant` component).
struct ScatterVariantRecipe final
{
    Name Id{};
    /// Exclusion radius in lattice units, at most the layer's cell size.
    uint32 ExclusionRadius = 0;
    /// Drawn half extent in metres at scale SCATTER_SCALE_UNIT.
    Vec2 HalfExtent{DEFAULT_SCATTER_HALF_EXTENT, DEFAULT_SCATTER_HALF_EXTENT};
    /// Base colour; extraction applies a cosmetic tint.
    float32 Color[SCATTER_COLOR_CHANNELS] = {};
    /// Durable identity from a version-2 World record; nil for version-1 content.
    foundation::identity::SourceObjectId Object{};
};
/// One independently thinned scatter layer (`scatter_layer` component).
struct ScatterLayerRecipe final
{
    Name Id{};
    /// Stable layer ID, unique in the room; part of every placement identity.
    uint8 Layer = 0;
    /// Cell edge in lattice units.
    uint16 CellSize = 1;
    /// Candidates per cell.
    uint16 Candidates = 1;
    /// Smallest scale in SCATTER_SCALE_UNIT thousandths.
    uint16 MinimumScale = SCATTER_SCALE_UNIT;
    /// Largest scale, at least MinimumScale.
    uint16 MaximumScale = SCATTER_SCALE_UNIT;
    ScatterVariantRecipe Variants[MAX_ROOM_SCATTER_VARIANTS] = {};
    usize VariantCount = 0;
    /// Durable identity from a version-2 World record; nil for version-1 content.
    foundation::identity::SourceObjectId Object{};
};
/// Room-scoped scatter recipe (`scatter` component plus its layers and variants). Lattice units are integers;
/// UnitsPerMetre converts authored metres to the kernel's lattice and generated positions back.
struct RoomScatter final
{
    Name Id{};
    /// Lattice units per metre, 1 through MAX_SCATTER_UNITS_PER_METRE.
    uint32 UnitsPerMetre = DEFAULT_SCATTER_UNITS_PER_METRE;
    /// Region corners in metres, inside the level bounds.
    Vec2 Minimum{};
    Vec2 Maximum{};
    /// Metres added around every spawn point, exit and collision box to form the keep-outs.
    float32 KeepOutMargin = 0;
    ScatterLayerRecipe Layers[MAX_ROOM_SCATTER_LAYERS] = {};
    usize LayerCount = 0;
    /// Durable identity from a version-2 World record; nil for version-1 content.
    foundation::identity::SourceObjectId Object{};
};
// Bounded owned content, independent of the input buffer and runtime handles.
struct Level final
{
    Name Id{};
    /// Document identity from a version-2 World record; nil for version-1 and compiled content.
    foundation::identity::AssetId Document{};
    /// Durable identity of the version-2 settings entity; nil otherwise.
    foundation::identity::SourceObjectId SettingsObject{};
    uint64 Seed = 0;
    Vec2 Minimum{};
    Vec2 Maximum{};
    Vec2 Camera{};
    float32 VerticalExtent = 0;
    Box Collision[16] = {};
    usize CollisionCount = 0;
    Recipe Entities[32] = {};
    usize EntityCount = 0;
    /// Whether the World authored a `scatter` component; Scatter is meaningful only then.
    bool HasScatter = false;
    RoomScatter Scatter{};
};
enum class LevelError : uint8
{
    None,
    Syntax,
    UnknownField,
    DuplicateField,
    MissingField,
    InvalidValue,
    Capacity,
    InvalidReference,
    MissingAsset
};
struct LevelResult final
{
    LevelError Error = LevelError::None;
    usize Offset = 0;
};
// Read the bounded World adapter (envelope version 1, or version 2 with UUID records); failure
// preserves output. Version 1 never mints identities. No project code is executed.
[[nodiscard]] LevelResult ReadWorldLevel(std::string_view source, Level& output) noexcept;
[[nodiscard]] LevelResult ReadLevel(std::string_view source, Level& output) noexcept;
[[nodiscard]] LevelError ValidateLevel(const Level& level) noexcept;
/// @brief Convert authored metres to the scatter lattice by rounding to the nearest unit (ties away from zero).
/// @param metres A finite authored length or coordinate.
/// @param unitsPerMetre Lattice units per metre, 1 through MAX_SCATTER_UNITS_PER_METRE.
/// @param outLattice Written on success; unchanged on failure.
/// @return false when the inputs are out of range or the result exceeds MAX_SCATTER_LATTICE_MAGNITUDE.
[[nodiscard]] bool TryScaleToLattice(float32 metres, uint32 unitsPerMetre, int32& outLattice) noexcept;
[[nodiscard]] bool WriteLevel(const Level& level, foundation::core::Array<char>& output) noexcept;
[[nodiscard]] std::string_view ExampleLevel() noexcept;
[[nodiscard]] std::string_view BuiltinLevel(std::string_view id) noexcept;
} // namespace ludus::world_demo
