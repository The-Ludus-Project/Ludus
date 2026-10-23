#pragma once
#include <ludus/foundation/base/core.h>

#include <span>

namespace ludus::runtime::scripting
{
using namespace foundation;
enum class FieldKind : uint8
{
    Uint32 = 1,
    Boolean = 2
};
struct FieldValue
{
    uint32 Id = 0;
    FieldKind Kind = FieldKind::Uint32;
    uint32 Value = 0;
};
struct FieldSpec
{
    uint32 Id = 0;
    FieldKind Kind = FieldKind::Uint32;
    uint32 Min = 0;
    uint32 Max = 0;
    uint32 Default = 0;
};
/// Most fields one script state record holds.
inline constexpr uint32 MAX_STATE_FIELDS = 16;
struct StateRecord
{
    uint32 Schema = 0;
    uint32 Count = 0;
    FieldValue Fields[MAX_STATE_FIELDS];
};
// Versioned LE tagged records: no native layout, pointer, VM object or hidden
// upvalue. All functions preserve output on failure. Missing new fields use the
// target's validated default; changed kinds/ranges/removed IDs need explicit policy.
[[nodiscard]] bool EncodeState(const StateRecord& record, std::span<uint8> output, usize& written) noexcept;
[[nodiscard]] bool DecodeState(std::span<const uint8> bytes, StateRecord& output) noexcept;
[[nodiscard]] bool
MigrateState(const StateRecord& source, uint32 schema, std::span<const FieldSpec> fields, StateRecord& output) noexcept;
} // namespace ludus::runtime::scripting
