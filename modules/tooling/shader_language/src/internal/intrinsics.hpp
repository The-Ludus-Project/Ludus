#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include "type_table.hpp"

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Signature shape shared by a group of built-in functions. "T" is a scalar or vector.
enum class IntrinsicFamily : foundation::uint8
{
    FloatUnary,     ///< (float T) -> T: sqrt, sin, floor, saturate, ...
    FloatVector,    ///< (float vector V) -> V: normalize.
    NumericUnary,   ///< (numeric T) -> T: abs.
    NumericBinary,  ///< (numeric T, T) -> T: min, max.
    FloatBinary,    ///< (float T, T) -> T: pow, atan2, step, fmod.
    NumericTernary, ///< (numeric T, T, T) -> T: clamp.
    FloatTernary,   ///< (float T, T, T) -> T: lerp, smoothstep.
    Dot,            ///< (numeric vector V, V) -> scalar.
    Length,         ///< (float vector V) -> scalar.
    Distance,       ///< (float vector V, V) -> scalar.
    Cross,          ///< (float 3-vector, same) -> same.
    Reflect,        ///< (float vector V, V) -> V.
    BoolReduce,     ///< (bool T) -> bool: any, all.
    BitcastToUint,  ///< (32-bit T) -> uint32 T: asuint.
    BitcastToInt,   ///< (32-bit T) -> int32 T: asint.
    BitcastToFloat, ///< (32-bit T) -> float32 T: asfloat.
    BitCount,       ///< (32-bit integer T) -> uint32 T: countbits, firstbithigh, firstbitlow, reversebits.
    Transpose,      ///< (matrix R x C) -> matrix C x R.
};

/// One built-in function; its Slang name is the same.
struct IntrinsicSpelling final
{
    std::string_view Name;
    IntrinsicFamily Family = IntrinsicFamily::FloatUnary;
};

/// Number of built-in functions.
inline constexpr foundation::usize INTRINSIC_COUNT = 46;

/// The built-in functions, lowered 1:1 to the Slang intrinsics of the same name.
inline constexpr foundation::StaticArray<IntrinsicSpelling, INTRINSIC_COUNT> INTRINSICS = {{
    {"sqrt", IntrinsicFamily::FloatUnary},
    {"rsqrt", IntrinsicFamily::FloatUnary},
    {"exp", IntrinsicFamily::FloatUnary},
    {"exp2", IntrinsicFamily::FloatUnary},
    {"log", IntrinsicFamily::FloatUnary},
    {"log2", IntrinsicFamily::FloatUnary},
    {"sin", IntrinsicFamily::FloatUnary},
    {"cos", IntrinsicFamily::FloatUnary},
    {"tan", IntrinsicFamily::FloatUnary},
    {"asin", IntrinsicFamily::FloatUnary},
    {"acos", IntrinsicFamily::FloatUnary},
    {"atan", IntrinsicFamily::FloatUnary},
    {"floor", IntrinsicFamily::FloatUnary},
    {"ceil", IntrinsicFamily::FloatUnary},
    {"round", IntrinsicFamily::FloatUnary},
    {"trunc", IntrinsicFamily::FloatUnary},
    {"frac", IntrinsicFamily::FloatUnary},
    {"saturate", IntrinsicFamily::FloatUnary},
    {"ddx", IntrinsicFamily::FloatUnary},
    {"ddy", IntrinsicFamily::FloatUnary},
    {"normalize", IntrinsicFamily::FloatVector},
    {"abs", IntrinsicFamily::NumericUnary},
    {"min", IntrinsicFamily::NumericBinary},
    {"max", IntrinsicFamily::NumericBinary},
    {"pow", IntrinsicFamily::FloatBinary},
    {"atan2", IntrinsicFamily::FloatBinary},
    {"step", IntrinsicFamily::FloatBinary},
    {"fmod", IntrinsicFamily::FloatBinary},
    {"clamp", IntrinsicFamily::NumericTernary},
    {"lerp", IntrinsicFamily::FloatTernary},
    {"smoothstep", IntrinsicFamily::FloatTernary},
    {"dot", IntrinsicFamily::Dot},
    {"length", IntrinsicFamily::Length},
    {"distance", IntrinsicFamily::Distance},
    {"cross", IntrinsicFamily::Cross},
    {"reflect", IntrinsicFamily::Reflect},
    {"any", IntrinsicFamily::BoolReduce},
    {"all", IntrinsicFamily::BoolReduce},
    {"asuint", IntrinsicFamily::BitcastToUint},
    {"asint", IntrinsicFamily::BitcastToInt},
    {"asfloat", IntrinsicFamily::BitcastToFloat},
    {"countbits", IntrinsicFamily::BitCount},
    {"firstbithigh", IntrinsicFamily::BitCount},
    {"firstbitlow", IntrinsicFamily::BitCount},
    {"reversebits", IntrinsicFamily::BitCount},
    {"transpose", IntrinsicFamily::Transpose},
}};

/// Number of arguments a family takes.
[[nodiscard]] foundation::uint32 GetIntrinsicArity(IntrinsicFamily family) noexcept;

/// @brief Resolve a built-in call from its argument types.
/// @param argumentTypes Checked argument types; untyped constants adapt to the other arguments.
/// @param outOperandType Receives the type every argument must convert to.
/// @param outResultType Receives the call's type.
/// @return False when the arguments do not fit the family; the caller reports it.
[[nodiscard]] bool ResolveIntrinsic(IntrinsicFamily family,
                                    const TypeTable& types,
                                    foundation::BorrowedView<const foundation::uint32> argumentTypes,
                                    foundation::uint32& outOperandType,
                                    foundation::uint32& outResultType) noexcept;
} // namespace ludus::tooling::shader_language::internal
