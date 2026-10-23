#pragma once

// Thanks to Wessam Bahnassi, "Game Tuning Infrastructure", Game Engine Gems 2,
// ch.16, pp.263-277, and Lasse Staff Jensen, "A Generic Tweaker", Game Programming
// Gems 2, sec.1.18, pp.118-126: expose typed metadata without changing ordinary
// option use. Original implementation; no direct member pointers or reflection.
// Review and lifetime contracts: docs/architecture/engine-configuration.md.
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/strings/static_string.hpp>

#include <span>
#include <string_view>

namespace ludus::foundation::config
{
enum class Status : uint8
{
    Ok,
    PendingRestart,
    InvalidState,
    InvalidSchema,
    InvalidValue,
    UnknownSetting,
    DuplicateAssignment,
    InvalidSource,
    ConstraintFailed,
    Conflict,
    MixedApplyGroups,
    Frozen,
    OutOfMemory,
    LimitExceeded,
    GenerationExhausted,
    InvalidBundle,
};
enum class Type : uint8
{
    Bool,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Float32,
    Float64,
    Enum,
    String,
};
/// Declared precedence, independent of mutation time. Defaults are implicit.
enum class Layer : uint8
{
    Engine,
    Project,
    Profile,
    Device,
    Quality,
    Preference,
    Developer,
    Launch,
    Session,
};
inline constexpr usize LAYER_COUNT = 9;
inline constexpr uint32 ALL_SOURCES = (uint32{1} << LAYER_COUNT) - 1;
[[nodiscard]] constexpr uint32 SourceMask(Layer layer) noexcept
{
    return static_cast<usize>(layer) < LAYER_COUNT ? uint32{1} << static_cast<uint8>(layer) : 0;
}
enum class Apply : uint8
{
    Live,
    RestartSession,
    RestartEngine,
    NextLaunch,
};

/// Capacity of a text setting value, in bytes.
inline constexpr usize MAX_TEXT_VALUE_BYTES = 256;
/// Capacity of an assignment's source name, in bytes.
inline constexpr usize MAX_ORIGIN_SOURCE_BYTES = 128;

/// @brief Owned bounded text.
///
/// Only the field selected by Kind has semantic meaning.
/// Copy on the control boundary; hot consumers materialize ordinary options.
struct Value final
{
    Type Kind = Type::Bool;
    bool Boolean = false;
    int64 Signed = 0;
    uint64 Unsigned = 0;
    float64 Real = 0;
    StaticString<MAX_TEXT_VALUE_BYTES> Text{};
    [[nodiscard]] static Value FromBool(bool value) noexcept;
    [[nodiscard]] static Value FromInt32(int32 value) noexcept;
    [[nodiscard]] static Value FromUint32(uint32 value) noexcept;
    [[nodiscard]] static Value FromInt64(int64 value) noexcept;
    [[nodiscard]] static Value FromUint64(uint64 value) noexcept;
    [[nodiscard]] static Value FromFloat32(float32 value) noexcept;
    [[nodiscard]] static Value FromFloat64(float64 value) noexcept;
    [[nodiscard]] static Status FromText(Type kind, std::string_view text, Value& output) noexcept;
    friend bool operator==(const Value& left, const Value& right) noexcept;
};

/// @brief Immutable schema storage (including names, help and enum spans) must outlive the Context.
///
/// No schema pointers may survive unloading their owning module.
struct Descriptor final
{
    std::string_view Name;
    Value Default{};
    std::string_view Help;
    std::string_view Units;
    std::span<const std::string_view> Choices;
    int64 MinSigned = -9223372036854775807LL - 1;
    int64 MaxSigned = 9223372036854775807LL;
    uint64 MinUnsigned = 0;
    uint64 MaxUnsigned = ~uint64{0};
    float64 MinReal = -1.7976931348623157e308;
    float64 MaxReal = 1.7976931348623157e308;
    uint32 AllowedSources = ALL_SOURCES;
    uint32 Group = 0;
    Apply Application = Apply::Live;
    uint32 MaxTextBytes = 256;
    bool Persistent = false;
    bool Simulation = false;
};
inline constexpr usize UNKNOWN_BYTE_OFFSET = ~usize{0};
struct Diagnostic final
{
    Status Result = Status::Ok;
    StaticString<128> Key{};
    usize Record = 0; ///< Semantic record index, never a parser byte offset.
    usize ByteOffset = UNKNOWN_BYTE_OFFSET;
};
struct Origin final
{
    StaticString<MAX_ORIGIN_SOURCE_BYTES> Source{};
    uint32 Line = 0; ///< Zero means unavailable; never fabricate a location.
};
struct Assignment final
{
    std::string_view Name;
    Value Data{};
    Origin From{};
    bool Remove = false;
};
struct Binding final
{
    uint64 Context = 0;
    uint32 Index = 0;
};
struct Explanation final
{
    Value Requested{};
    Value Active{};
    Origin From{};
    Layer Winner = Layer::Engine;
    bool IsDefault = true;
    bool Pending = false;
    uint32 Overridden = 0;
    uint64 GroupGeneration = 0;
};
struct Limits final
{
    usize MaxSettings = 256; ///< Hard ceiling: 4096.
    usize MaxStorageBytes = usize{64} * 1024 * 1024;
    usize MaxEdits = 64; ///< Hard ceiling: 4096 for startup/bundle replacement.
};
using ValidateFunction = Status (*)(void*, std::span<const Value>, Diagnostic&) noexcept;

/// @brief One control owner.
///
/// Read/prepare/commit are not a concurrent facade. Consumers copy their typed options at a safe point. Prepare is
/// side-effect-free; commit allocates nothing and calls no user code. No runtime I/O or global service.
class Context final
{
public:
    Context() noexcept = default;
    ~Context() noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    [[nodiscard]] Status Initialize(std::span<const Descriptor> schema,
                                    const AllocationDomain& domain,
                                    Diagnostic& error,
                                    const Limits& limits = {},
                                    ValidateFunction validate = nullptr,
                                    void* validationContext = nullptr) noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] uint64 Revision() const noexcept;
    [[nodiscard]] std::span<const Descriptor> Schema() const noexcept;
    [[nodiscard]] Status Bind(std::string_view name, Binding& output) const noexcept;
    [[nodiscard]] Status Explain(Binding binding, Explanation& output) const noexcept;
    [[nodiscard]] Status ReadLayer(Binding binding, Layer layer, Value& output, Origin& origin) const noexcept;
    /// @brief Replace clears that entire layer in the candidate; patch changes only the specified keys.
    ///
    /// All assignments, including masked values, are checked.
    [[nodiscard]] Status Prepare(Layer layer,
                                 std::span<const Assignment> edits,
                                 bool replace,
                                 uint64 expectedRevision,
                                 Diagnostic& error) noexcept;
    [[nodiscard]] Status ReadPrepared(Binding binding, Value& output) const noexcept;
    [[nodiscard]] Status Commit(uint64 expectedRevision, Diagnostic& error) noexcept;
    void Discard() noexcept;
    /// @brief Subsequent restart-only changes become requested/pending; simulation settings freeze.
    ///
    /// Called after initial options reach their native owners.
    [[nodiscard]] Status SealStartup() noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};

[[nodiscard]] bool ValidName(std::string_view name) noexcept;
[[nodiscard]] std::string_view StatusName(Status status) noexcept;
[[nodiscard]] std::string_view TypeName(Type type) noexcept;
[[nodiscard]] std::string_view LayerName(Layer layer) noexcept;
[[nodiscard]] std::string_view ApplyName(Apply apply) noexcept;
} // namespace ludus::foundation::config
