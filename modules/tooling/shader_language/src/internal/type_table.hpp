#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/limits.hpp>
#include <ludus/tooling/shader_language/semantic.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Number of ScalarKind values.
inline constexpr foundation::uint32 SCALAR_KIND_COUNT = 10;
/// Largest vector size and matrix dimension.
inline constexpr foundation::uint32 MAX_COMPONENTS = 4;

/// Result of interning a type.
enum class TypeOutcome : foundation::uint8
{
    Ok,          ///< The type index was written.
    OutOfMemory, ///< Allocation failed.
    Limit,       ///< MAX_SEMANTIC_TYPES types already exist.
};

/// @brief The interned types of one model, with built-ins at fixed indices.
///
/// Error, Void, the untyped constant types, every scalar, vector and matrix exist from Initialize
/// on, so looking them up never allocates. Arrays, structs and enums are interned on demand.
class TypeTable final
{
public:
    static constexpr foundation::uint32 ERROR_TYPE = 0;
    static constexpr foundation::uint32 VOID_TYPE = 1;
    static constexpr foundation::uint32 UNTYPED_INTEGER_TYPE = 2;
    static constexpr foundation::uint32 UNTYPED_FLOAT_TYPE = 3;

    /// Borrow the model's type array, which must outlive the table.
    explicit TypeTable(foundation::Array<SemanticType>& inoutTypes) noexcept : mTypes(inoutTypes) {}

    /// Create the built-in types; must succeed before any other call.
    [[nodiscard]] bool Initialize() noexcept;

    [[nodiscard]] const SemanticType& Get(foundation::uint32 type) const noexcept
    {
        return mTypes[type];
    }

    /// Index of the scalar of `kind`.
    [[nodiscard]] foundation::uint32 GetScalar(ScalarKind kind) const noexcept;
    /// Index of the vector of `size` components; size one gives the scalar.
    [[nodiscard]] foundation::uint32 GetVector(ScalarKind kind, foundation::uint32 size) const noexcept;
    /// Index of the `rows` by `columns` matrix; rows and columns are 2 to 4.
    [[nodiscard]] foundation::uint32
    GetMatrix(ScalarKind kind, foundation::uint32 rows, foundation::uint32 columns) const noexcept;
    /// The same shape with another element type; NO_TYPE for a non-numeric shape.
    [[nodiscard]] foundation::uint32 WithScalar(foundation::uint32 type, ScalarKind kind) const noexcept;

    /// Intern an array of `count` `element` values.
    [[nodiscard]] TypeOutcome
    TryGetArray(foundation::uint32 element, foundation::uint32 count, foundation::uint32& outType) noexcept;
    /// Intern the resource type of `kind` with `element` (NO_TYPE for a sampler).
    [[nodiscard]] TypeOutcome
    TryGetResource(ResourceKind kind, foundation::uint32 element, foundation::uint32& outType) noexcept;
    /// Intern the struct or enum type that `symbol` declares.
    [[nodiscard]] TypeOutcome
    TryGetNamed(TypeKind kind, ScalarKind underlying, foundation::uint32 symbol, foundation::uint32& outType) noexcept;

    /// @brief Resolve a built-in type name, fixed-width (`float32x3`, `boolx2`, `float32x4x4`) or
    /// alias (`float3`, `bool2`, `float4x4`, `int`, `half`).
    /// @return The type index, or NO_TYPE when `name` is not a built-in type.
    [[nodiscard]] foundation::uint32 FindBuiltin(std::string_view name) const noexcept;

private:
    foundation::Array<SemanticType>& mTypes;
};

[[nodiscard]] constexpr bool IsIntegerScalar(ScalarKind kind) noexcept
{
    return kind != ScalarKind::Bool && kind != ScalarKind::Float16 && kind != ScalarKind::Float32 &&
           kind != ScalarKind::Float64;
}

[[nodiscard]] constexpr bool IsFloatScalar(ScalarKind kind) noexcept
{
    return kind == ScalarKind::Float16 || kind == ScalarKind::Float32 || kind == ScalarKind::Float64;
}

[[nodiscard]] constexpr bool IsSignedScalar(ScalarKind kind) noexcept
{
    return kind == ScalarKind::Int16 || kind == ScalarKind::Int32 || kind == ScalarKind::Int64 ||
           IsFloatScalar(kind) == true;
}

/// @brief A resource type name, as LUSL spells it.
struct ResourceSpelling final
{
    std::string_view Name;
    ResourceKind Kind = ResourceKind::Texture2D;
    /// True when the name takes one type argument.
    bool HasElement = true;
};

/// Every resource type name.
inline constexpr ResourceSpelling RESOURCE_SPELLINGS[] = {
    { .Name = "Texture2D", .Kind = ResourceKind::Texture2D, .HasElement = true },
    { .Name = "RWTexture2D", .Kind = ResourceKind::RWTexture2D, .HasElement = true },
    { .Name = "SamplerState", .Kind = ResourceKind::Sampler, .HasElement = false },
    { .Name = "ConstantBuffer", .Kind = ResourceKind::ConstantBuffer, .HasElement = true },
    { .Name = "StructuredBuffer", .Kind = ResourceKind::StructuredBuffer, .HasElement = true },
    { .Name = "RWStructuredBuffer", .Kind = ResourceKind::RWStructuredBuffer, .HasElement = true },
};

/// The spelling of `name` when it names a resource type, or nullptr.
[[nodiscard]] const ResourceSpelling* FindResourceSpellingOrNull(std::string_view name) noexcept;

/// The LUSL (and Slang) name of a resource kind, such as `Texture2D`.
[[nodiscard]] std::string_view GetResourceName(ResourceKind kind) noexcept;

/// True for resources that shaders write: RWTexture2D and RWStructuredBuffer.
[[nodiscard]] constexpr bool IsWritableResource(ResourceKind kind) noexcept
{
    return kind == ResourceKind::RWTexture2D || kind == ResourceKind::RWStructuredBuffer;
}

/// Fixed-width spelling of a scalar, such as `uint32`.
[[nodiscard]] std::string_view GetScalarName(ScalarKind kind) noexcept;
} // namespace ludus::tooling::shader_language::internal
