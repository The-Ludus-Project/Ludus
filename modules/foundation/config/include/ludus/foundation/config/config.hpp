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
/// Outcome of a configuration operation.
enum class Status : uint8
{
    Ok,                  ///< Operation succeeded.
    PendingRestart,      ///< Commit accepted requested values for a future restart.
    InvalidState,        ///< The context, binding or transaction is in the wrong state.
    InvalidSchema,       ///< A descriptor, default or schema contract is invalid.
    InvalidValue,        ///< A value failed type, bounds, text or choice validation.
    UnknownSetting,      ///< The setting or explicit layer value was not found.
    DuplicateAssignment, ///< The same key appears more than once in the edit.
    InvalidSource,       ///< The specified layer cannot assign or remove the setting.
    ConstraintFailed,    ///< A cross-field constraint rejected the candidate.
    Conflict,            ///< The expected revision differs from the current revision.
    MixedApplyGroups,    ///< A sealed transaction spans incompatible groups or policies.
    Frozen,              ///< A sealed simulation setting would change.
    OutOfMemory,         ///< The allocation domain could not supply required storage.
    LimitExceeded,       ///< An input, edit, storage or output size exceeded its limit.
    GenerationExhausted, ///< A revision, generation or context identity cannot advance.
    InvalidBundle,       ///< A cooked layer has invalid syntax, shape or identity.
};
/// Exact value kinds accepted by a descriptor.
enum class Type : uint8
{
    Bool,    ///< Boolean.
    Int32,   ///< Signed 32-bit integer.
    Uint32,  ///< Unsigned 32-bit integer.
    Int64,   ///< Signed 64-bit integer.
    Uint64,  ///< Unsigned 64-bit integer.
    Float32, ///< Finite 32-bit floating point.
    Float64, ///< Finite 64-bit floating point.
    Enum,    ///< Text restricted to the descriptor choices.
    String,  ///< Bounded UTF-8 text.
};
/// Declared precedence, independent of mutation time. Defaults are implicit.
enum class Layer : uint8
{
    Engine,     ///< Engine baseline.
    Project,    ///< Project-authored overrides.
    Profile,    ///< Selected profile overrides.
    Device,     ///< Selected device overrides.
    Quality,    ///< Selected quality overrides.
    Preference, ///< User preferences.
    Developer,  ///< Developer overrides.
    Launch,     ///< Per-launch overrides.
    Session,    ///< Overrides for the current session.
};
/// Number of explicit layers. Defaults have no layer bit.
inline constexpr usize LAYER_COUNT = 9;
/// Mask containing every explicit layer bit.
inline constexpr uint32 ALL_SOURCES = (uint32{1} << LAYER_COUNT) - 1;
/// Return the bit for the layer, or zero for an invalid layer.
[[nodiscard]] constexpr uint32 SourceMask(Layer layer) noexcept
{
    return static_cast<usize>(layer) < LAYER_COUNT ? uint32{1} << static_cast<uint8>(layer) : 0;
}
/// When requested values can become active after startup is sealed.
enum class Apply : uint8
{
    Live,           ///< Publish on commit.
    RestartSession, ///< Requires a session restart.
    RestartEngine,  ///< Requires an engine restart.
    NextLaunch,     ///< Requires a subsequent launch.
};

/// Capacity of a text setting value, in bytes.
inline constexpr usize MAX_TEXT_VALUE_BYTES = 256;
/// Capacity of an assignment's source name, in bytes.
inline constexpr usize MAX_ORIGIN_SOURCE_BYTES = 128;

/// @brief An owned, exactly tagged setting value with bounded text storage.
///
/// Only the field selected by Kind has semantic meaning.
/// Copy on the control boundary; hot consumers materialize ordinary options.
struct Value final
{
    /// Discriminator selecting the meaningful payload.
    Type Kind = Type::Bool;
    /// Payload for Bool.
    bool Boolean = false;
    /// Payload for Int32 and Int64.
    int64 Signed = 0;
    /// Payload for Uint32 and Uint64.
    uint64 Unsigned = 0;
    /// Payload for Float32 and Float64, stored as float64.
    float64 Real = 0;
    /// Owned UTF-8 payload for Enum or String.
    StaticString<MAX_TEXT_VALUE_BYTES> Text{};
    /// Create a boolean value for subsequent descriptor validation.
    [[nodiscard]] static Value FromBool(bool value) noexcept;
    /// Create a signed 32-bit integer value for subsequent descriptor validation.
    [[nodiscard]] static Value FromInt32(int32 value) noexcept;
    /// Create an unsigned 32-bit integer value for subsequent descriptor validation.
    [[nodiscard]] static Value FromUint32(uint32 value) noexcept;
    /// Create a signed 64-bit integer value for subsequent descriptor validation.
    [[nodiscard]] static Value FromInt64(int64 value) noexcept;
    /// Create an unsigned 64-bit integer value for subsequent descriptor validation.
    [[nodiscard]] static Value FromUint64(uint64 value) noexcept;
    /// Create a 32-bit floating-point value for subsequent descriptor validation.
    [[nodiscard]] static Value FromFloat32(float32 value) noexcept;
    /// Create a 64-bit floating-point value for subsequent descriptor validation.
    [[nodiscard]] static Value FromFloat64(float64 value) noexcept;
    /// Copy bounded UTF-8 Enum or String text with no embedded NUL.
    /// Failure preserves the output. Per-setting length and choice checks occur during native validation.
    [[nodiscard]] static Status FromText(Type kind, std::string_view text, Value& output) noexcept;
    /// Compare exact kinds and their selected payloads.
    friend bool operator==(const Value& left, const Value& right) noexcept;
};

/// @brief Immutable per-setting validation and authoring metadata.
///
/// Descriptor storage and referenced strings and choices must outlive the Context.
///
/// No schema pointers may survive unloading their owning module.
struct Descriptor final
{
    /// Unique dotted lowercase key, at most 128 bytes.
    std::string_view Name;
    /// Implicit value and exact kind of this setting.
    Value Default{};
    /// UTF-8 help text for authoring and inspection.
    std::string_view Help;
    /// UTF-8 display label for units, or empty if unitless.
    std::string_view Units;
    /// Borrowed enum spellings in authoring order. Empty for other kinds.
    std::span<const std::string_view> Choices;
    /// Inclusive descriptor lower bound for signed integers.
    int64 MinSigned = -9223372036854775807LL - 1;
    /// Inclusive descriptor upper bound for signed integers.
    int64 MaxSigned = 9223372036854775807LL;
    /// Inclusive descriptor lower bound for unsigned integers.
    uint64 MinUnsigned = 0;
    /// Inclusive descriptor upper bound for unsigned integers.
    uint64 MaxUnsigned = ~uint64{0};
    /// Inclusive descriptor lower bound before and after any Float32 rounding.
    float64 MinReal = -1.7976931348623157e308;
    /// Inclusive descriptor upper bound before and after any Float32 rounding.
    float64 MaxReal = 1.7976931348623157e308;
    /// Nonzero combination of SourceMask bits for layers that may edit this setting.
    uint32 AllowedSources = ALL_SOURCES;
    /// Schema-local application group ID from 0 to 31.
    uint32 Group = 0;
    /// Activation policy after startup is sealed.
    Apply Application = Apply::Live;
    /// Inclusive UTF-8 byte limit for Enum and String, excluding a terminator.
    uint32 MaxTextBytes = 256;
    /// Allow Preference layer edits and serialization. Requires the Preference source bit.
    bool Persistent = false;
    /// Freeze the requested value after startup is sealed.
    bool Simulation = false;
};
/// Sentinel for a diagnostic without a parser byte offset.
inline constexpr usize UNKNOWN_BYTE_OFFSET = ~usize{0};
/// Details of a rejected operation.
struct Diagnostic final
{
    /// Status of the operation.
    Status Result = Status::Ok;
    /// Affected setting key when available.
    StaticString<128> Key{};
    usize Record = 0; ///< Semantic record index, never a parser byte offset.
    /// Parser position in input bytes, or UNKNOWN_BYTE_OFFSET when unavailable.
    usize ByteOffset = UNKNOWN_BYTE_OFFSET;
};
/// Copied provenance accompanying an explicit assignment.
struct Origin final
{
    /// Optional UTF-8 authoring source label.
    StaticString<MAX_ORIGIN_SOURCE_BYTES> Source{};
    /// Zero means unavailable; never fabricate a location.
    uint32 Line = 0; ///< Zero means unavailable;
};
/// An edit read during Prepare. Borrowed names need only survive the call.
struct Assignment final
{
    /// Exact setting key.
    std::string_view Name;
    /// New value, ignored for removal.
    Value Data{};
    /// Provenance copied with an accepted value.
    Origin From{};
    /// Remove the explicit layer entry instead of assigning Data.
    bool Remove = false;
};
/// Context-local handle returned by Context::Bind. Treat its fields as opaque.
struct Binding final
{
    /// Identity of the issuing context.
    uint64 Context = 0;
    /// Descriptor index within that context.
    uint32 Index = 0;
};
/// Snapshot of the committed resolution and application state for one setting.
struct Explanation final
{
    /// Value resolved by layer precedence.
    Value Requested{};
    /// Value available to native consumers.
    Value Active{};
    /// Origin of the winning assignment, empty for a default.
    Origin From{};
    /// Winning layer when IsDefault is false.
    Layer Winner = Layer::Engine;
    /// Whether no layer explicitly assigns the setting.
    bool IsDefault = true;
    /// Whether requested and active values differ.
    bool Pending = false;
    /// Number of lower-precedence explicit assignments.
    uint32 Overridden = 0;
    /// Generation of the active application group.
    uint64 GroupGeneration = 0;
};
/// Caller-selected ceilings checked before allocation and while preparing edits.
struct Limits final
{
    usize MaxSettings = 256; ///< Hard ceiling: 4096.
    /// Maximum context storage in bytes, excluding borrowed schema data.
    usize MaxStorageBytes = usize{64} * 1024 * 1024;
    usize MaxEdits = 64; ///< Hard ceiling: 4096 for startup/bundle replacement.
};
/// Pure validator receiving borrowed caller state and candidate values in descriptor order.
/// Return Ok to accept or a failure with an optional diagnostic. The view is valid only during the call.
/// Do not mutate engine state or reenter the context. May run more than once per prepare.
using ValidateFunction = Status (*)(void*, std::span<const Value>, Diagnostic&) noexcept;

/// @brief One control owner.
///
/// Serialize access on the control owner. Consumers copy typed options at a safe point.
/// Prepare leaves committed values intact. Commit allocates nothing and calls no user code.
class Context final
{
public:
    /// Create an uninitialized context.
    Context() noexcept = default;
    /// Release owned storage to the allocation domain supplied at initialization.
    ~Context() noexcept;
    /// Context identity and owned storage cannot be copied.
    Context(const Context&) = delete;
    /// Context identity and owned storage cannot be copied.
    Context& operator=(const Context&) = delete;
    /// Validate the immutable schema and allocate storage for one control owner.
    /// The schema, its referenced data, allocation domain and optional validator state are borrowed and
    /// must outlive the context. Failure leaves an uninitialized context reusable and fills the diagnostic.
    /// Calling on an initialized context returns InvalidState without replacing it.
    [[nodiscard]] Status Initialize(std::span<const Descriptor> schema,
                                    const AllocationDomain& domain,
                                    Diagnostic& error,
                                    const Limits& limits = {},
                                    ValidateFunction validate = nullptr,
                                    void* validationContext = nullptr) noexcept;
    /// Return whether initialization succeeded.
    [[nodiscard]] bool IsValid() const noexcept;
    /// Return the committed revision, or zero before initialization.
    [[nodiscard]] uint64 Revision() const noexcept;
    /// Borrow immutable descriptors in declaration order, or an empty view before initialization.
    [[nodiscard]] std::span<const Descriptor> Schema() const noexcept;
    /// Resolve an exact setting key to a context-local binding. Preserves the output on failure.
    [[nodiscard]] Status Bind(std::string_view name, Binding& output) const noexcept;
    /// Copy committed requested and active values, provenance and group generation.
    /// Requires a binding from this context and preserves the output on failure.
    [[nodiscard]] Status Explain(Binding binding, Explanation& output) const noexcept;
    /// Copy an explicit committed value and its origin from one layer.
    /// Returns UnknownSetting for an absent assignment. Preserves both outputs on failure.
    [[nodiscard]] Status ReadLayer(Binding binding, Layer layer, Value& output, Origin& origin) const noexcept;
    /// @brief Replace clears that entire layer in the candidate; patch changes only the specified keys.
    ///
    /// Checks every assignment, including masked values, against the current schema and revision.
    /// Success leaves one candidate to commit or discard. Failure preserves committed state.
    [[nodiscard]] Status Prepare(Layer layer,
                                 std::span<const Assignment> edits,
                                 bool replace,
                                 uint64 expectedRevision,
                                 Diagnostic& error) noexcept;
    /// Copy the resolved candidate value for a binding from this context.
    /// Requires an outstanding successful Prepare. Preserves the output on failure.
    [[nodiscard]] Status ReadPrepared(Binding binding, Value& output) const noexcept;
    /// Publish the prepared edit for the current expected revision.
    /// Ok and PendingRestart both commit and advance the revision. PendingRestart leaves applicable active
    /// values in place. Failure preserves committed state and leaves any prepared candidate available for discard.
    [[nodiscard]] Status Commit(uint64 expectedRevision, Diagnostic& error) noexcept;
    /// Drop any prepared candidate without changing committed values or revision.
    void Discard() noexcept;
    /// @brief Subsequent restart-only changes become requested/pending; simulation settings freeze.
    ///
    /// Called after initial options reach their native owners.
    [[nodiscard]] Status SealStartup() noexcept;

private:
    struct Impl;
    Impl* mImpl = nullptr;
};

/// Check the dotted lowercase key grammar and byte limit used by native schemas.
[[nodiscard]] bool ValidName(std::string_view name) noexcept;
/// Return a static diagnostic spelling. Unknown status values return "Unknown".
[[nodiscard]] std::string_view StatusName(Status status) noexcept;
/// Return the static JSON spelling for a kind. Unknown kinds return "unknown".
[[nodiscard]] std::string_view TypeName(Type type) noexcept;
/// Return the static JSON spelling for a layer. Unknown layers return "unknown".
[[nodiscard]] std::string_view LayerName(Layer layer) noexcept;
/// Return the static metadata spelling for an apply policy. Unknown policies return "unknown".
[[nodiscard]] std::string_view ApplyName(Apply apply) noexcept;
} // namespace ludus::foundation::config
