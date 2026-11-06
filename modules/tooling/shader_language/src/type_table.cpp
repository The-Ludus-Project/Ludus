#include "internal/type_table.hpp"

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::uint32;
using foundation::usize;

/// First scalar index; Error, Void and the two untyped types come before.
constexpr uint32 FIRST_SCALAR = 4;
/// Vector sizes per scalar kind: 2, 3 and 4.
constexpr uint32 VECTOR_SIZES = MAX_COMPONENTS - 1;
constexpr uint32 FIRST_VECTOR = FIRST_SCALAR + SCALAR_KIND_COUNT;
/// Matrix shapes per scalar kind: rows and columns of 2 to 4.
constexpr uint32 MATRIX_SHAPES = VECTOR_SIZES * VECTOR_SIZES;
constexpr uint32 FIRST_MATRIX = FIRST_VECTOR + SCALAR_KIND_COUNT * VECTOR_SIZES;
constexpr uint32 BUILTIN_TYPE_COUNT = FIRST_MATRIX + SCALAR_KIND_COUNT * MATRIX_SHAPES;
/// Smallest vector size and matrix dimension.
constexpr uint32 MIN_COMPONENTS = 2;
/// Bytes of a fixed-width matrix suffix such as `x4x4`, and the offset of its column digit.
constexpr usize FIXED_MATRIX_SUFFIX_BYTES = 4;
constexpr usize FIXED_MATRIX_COLUMNS_OFFSET = 3;
/// Bytes of an alias matrix suffix such as `4x4`.
constexpr usize ALIAS_MATRIX_SUFFIX_BYTES = 3;

struct ScalarSpelling final
{
    std::string_view Name;
    ScalarKind Kind = ScalarKind::Bool;
};

constexpr foundation::StaticArray<ScalarSpelling, SCALAR_KIND_COUNT> FIXED_WIDTH_NAMES = {{
    {"bool", ScalarKind::Bool},
    {"int16", ScalarKind::Int16},
    {"uint16", ScalarKind::Uint16},
    {"int32", ScalarKind::Int32},
    {"uint32", ScalarKind::Uint32},
    {"int64", ScalarKind::Int64},
    {"uint64", ScalarKind::Uint64},
    {"float16", ScalarKind::Float16},
    {"float32", ScalarKind::Float32},
    {"float64", ScalarKind::Float64},
}};

/// Number of Slang/HLSL-style alias spellings.
constexpr usize ALIAS_COUNT = 6;

constexpr foundation::StaticArray<ScalarSpelling, ALIAS_COUNT> ALIAS_NAMES = {{
    {"bool", ScalarKind::Bool},
    {"int", ScalarKind::Int32},
    {"uint", ScalarKind::Uint32},
    {"half", ScalarKind::Float16},
    {"float", ScalarKind::Float32},
    {"double", ScalarKind::Float64},
}};

/// Value of a single dimension digit 2 to 4, or zero.
[[nodiscard]] uint32 ReadDimension(char digit) noexcept
{
    return digit >= '2' && digit <= '4' ? static_cast<uint32>(digit - '0') : 0;
}
} // namespace

std::string_view GetScalarName(ScalarKind kind) noexcept
{
    return FIXED_WIDTH_NAMES[static_cast<usize>(kind)].Name;
}

bool TypeTable::Initialize() noexcept
{
    if (mTypes.TryEnsureCapacity(BUILTIN_TYPE_COUNT) == false)
    {
        return false;
    }
    SemanticType error;
    error.Kind = TypeKind::Error;
    mTypes.Add(error);
    SemanticType voidType;
    voidType.Kind = TypeKind::Void;
    mTypes.Add(voidType);
    SemanticType untypedInteger;
    untypedInteger.Kind = TypeKind::UntypedInteger;
    mTypes.Add(untypedInteger);
    SemanticType untypedFloat;
    untypedFloat.Kind = TypeKind::UntypedFloat;
    mTypes.Add(untypedFloat);
    for (uint32 kind = 0; kind < SCALAR_KIND_COUNT; ++kind)
    {
        SemanticType scalar;
        scalar.Kind = TypeKind::Scalar;
        scalar.Scalar = static_cast<ScalarKind>(kind);
        mTypes.Add(scalar);
    }
    for (uint32 kind = 0; kind < SCALAR_KIND_COUNT; ++kind)
    {
        for (uint32 size = MIN_COMPONENTS; size <= MAX_COMPONENTS; ++size)
        {
            SemanticType vector;
            vector.Kind = TypeKind::Vector;
            vector.Scalar = static_cast<ScalarKind>(kind);
            vector.Rows = static_cast<foundation::uint8>(size);
            mTypes.Add(vector);
        }
    }
    for (uint32 kind = 0; kind < SCALAR_KIND_COUNT; ++kind)
    {
        for (uint32 rows = MIN_COMPONENTS; rows <= MAX_COMPONENTS; ++rows)
        {
            for (uint32 columns = MIN_COMPONENTS; columns <= MAX_COMPONENTS; ++columns)
            {
                SemanticType matrix;
                matrix.Kind = TypeKind::Matrix;
                matrix.Scalar = static_cast<ScalarKind>(kind);
                matrix.Rows = static_cast<foundation::uint8>(rows);
                matrix.Columns = static_cast<foundation::uint8>(columns);
                mTypes.Add(matrix);
            }
        }
    }
    return true;
}

uint32 TypeTable::GetScalar(ScalarKind kind) const noexcept
{
    return FIRST_SCALAR + static_cast<uint32>(kind);
}

uint32 TypeTable::GetVector(ScalarKind kind, uint32 size) const noexcept
{
    if (size <= 1)
    {
        return GetScalar(kind);
    }
    return FIRST_VECTOR + static_cast<uint32>(kind) * VECTOR_SIZES + (size - MIN_COMPONENTS);
}

uint32 TypeTable::GetMatrix(ScalarKind kind, uint32 rows, uint32 columns) const noexcept
{
    return FIRST_MATRIX + static_cast<uint32>(kind) * MATRIX_SHAPES + (rows - MIN_COMPONENTS) * VECTOR_SIZES +
           (columns - MIN_COMPONENTS);
}

uint32 TypeTable::WithScalar(uint32 type, ScalarKind kind) const noexcept
{
    const SemanticType& shape = mTypes[type];
    switch (shape.Kind)
    {
        case TypeKind::Scalar:
            return GetScalar(kind);
        case TypeKind::Vector:
            return GetVector(kind, shape.Rows);
        case TypeKind::Matrix:
            return GetMatrix(kind, shape.Rows, shape.Columns);
        default:
            return NO_TYPE;
    }
}

TypeOutcome TypeTable::TryGetArray(uint32 element, uint32 count, uint32& outType) noexcept
{
    for (uint32 i = BUILTIN_TYPE_COUNT; i < mTypes.GetSize(); ++i)
    {
        const SemanticType& existing = mTypes[i];
        if (existing.Kind == TypeKind::Array && existing.Element == element && existing.Count == count)
        {
            outType = i;
            return TypeOutcome::Ok;
        }
    }
    if (mTypes.GetSize() >= MAX_SEMANTIC_TYPES)
    {
        return TypeOutcome::Limit;
    }
    SemanticType array;
    array.Kind = TypeKind::Array;
    array.Element = element;
    array.Count = count;
    if (mTypes.TryAdd(array) == false)
    {
        return TypeOutcome::OutOfMemory;
    }
    outType = static_cast<uint32>(mTypes.GetSize() - 1);
    return TypeOutcome::Ok;
}

TypeOutcome TypeTable::TryGetResource(ResourceKind kind, uint32 element, uint32& outType) noexcept
{
    for (uint32 i = BUILTIN_TYPE_COUNT; i < mTypes.GetSize(); ++i)
    {
        const SemanticType& existing = mTypes[i];
        if (existing.Kind == TypeKind::Resource && existing.Resource == kind && existing.Element == element)
        {
            outType = i;
            return TypeOutcome::Ok;
        }
    }
    if (mTypes.GetSize() >= MAX_SEMANTIC_TYPES)
    {
        return TypeOutcome::Limit;
    }
    SemanticType resource;
    resource.Kind = TypeKind::Resource;
    resource.Resource = kind;
    resource.Element = element;
    if (mTypes.TryAdd(resource) == false)
    {
        return TypeOutcome::OutOfMemory;
    }
    outType = static_cast<uint32>(mTypes.GetSize() - 1);
    return TypeOutcome::Ok;
}

const ResourceSpelling* FindResourceSpellingOrNull(std::string_view name) noexcept
{
    for (const ResourceSpelling& spelling : RESOURCE_SPELLINGS)
    {
        if (spelling.Name == name)
        {
            return &spelling;
        }
    }
    return nullptr;
}

std::string_view GetResourceName(ResourceKind kind) noexcept
{
    for (const ResourceSpelling& spelling : RESOURCE_SPELLINGS)
    {
        if (spelling.Kind == kind)
        {
            return spelling.Name;
        }
    }
    return {};
}

TypeOutcome TypeTable::TryGetNamed(TypeKind kind, ScalarKind underlying, uint32 symbol, uint32& outType) noexcept
{
    for (uint32 i = BUILTIN_TYPE_COUNT; i < mTypes.GetSize(); ++i)
    {
        if (mTypes[i].Kind == kind && mTypes[i].Symbol == symbol)
        {
            outType = i;
            return TypeOutcome::Ok;
        }
    }
    if (mTypes.GetSize() >= MAX_SEMANTIC_TYPES)
    {
        return TypeOutcome::Limit;
    }
    SemanticType named;
    named.Kind = kind;
    named.Scalar = underlying;
    named.Symbol = symbol;
    if (mTypes.TryAdd(named) == false)
    {
        return TypeOutcome::OutOfMemory;
    }
    outType = static_cast<uint32>(mTypes.GetSize() - 1);
    return TypeOutcome::Ok;
}

uint32 TypeTable::FindBuiltin(std::string_view name) const noexcept
{
    // Fixed-width spellings: `float32`, `float32x3`, `float32x4x4`.
    for (const ScalarSpelling& spelling : FIXED_WIDTH_NAMES)
    {
        if (name.starts_with(spelling.Name) == false)
        {
            continue;
        }
        const std::string_view rest = name.substr(spelling.Name.size());
        if (rest.empty() == true)
        {
            return GetScalar(spelling.Kind);
        }
        if (rest.size() == 2 && rest[0] == 'x' && ReadDimension(rest[1]) != 0)
        {
            return GetVector(spelling.Kind, ReadDimension(rest[1]));
        }
        // `x<rows>x<columns>`.
        if (rest.size() == FIXED_MATRIX_SUFFIX_BYTES && rest[0] == 'x' && rest[2] == 'x' &&
            ReadDimension(rest[1]) != 0 && ReadDimension(rest[FIXED_MATRIX_COLUMNS_OFFSET]) != 0)
        {
            return GetMatrix(spelling.Kind, ReadDimension(rest[1]), ReadDimension(rest[FIXED_MATRIX_COLUMNS_OFFSET]));
        }
    }
    // Slang/HLSL aliases: `float`, `float3`, `float4x4`.
    for (const ScalarSpelling& spelling : ALIAS_NAMES)
    {
        if (name.starts_with(spelling.Name) == false)
        {
            continue;
        }
        const std::string_view rest = name.substr(spelling.Name.size());
        if (rest.empty() == true)
        {
            return GetScalar(spelling.Kind);
        }
        if (rest.size() == 1 && ReadDimension(rest[0]) != 0)
        {
            return GetVector(spelling.Kind, ReadDimension(rest[0]));
        }
        // `<rows>x<columns>`.
        if (rest.size() == ALIAS_MATRIX_SUFFIX_BYTES && rest[1] == 'x' && ReadDimension(rest[0]) != 0 &&
            ReadDimension(rest[2]) != 0)
        {
            return GetMatrix(spelling.Kind, ReadDimension(rest[0]), ReadDimension(rest[2]));
        }
    }
    return NO_TYPE;
}
} // namespace ludus::tooling::shader_language::internal
