// Thanks to the Ludus GameApi checkpoint protocol (project-live-reload, §8),
// and Celes/de Figueiredo/Ierusalimschy, "Binding C/C++ Objects to Lua",
// Game Programming Gems 6, §4.2, pp. 341–355: persistent host values are separate from VM wrappers/lifetimes.
// This original bounded codec uses stable field IDs and explicit LE widths;
// see docs/architecture/luau-s2.md. No language object graph is serialized.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>

#include <span>

#include "internal/state.h"

namespace ludus::runtime::scripting
{
namespace
{
constexpr uint32 MAGIC = 0x3253544c;
/// Encoded state header: magic, version, schema and field count, then 12-byte field records.
constexpr usize STATE_VERSION_OFFSET = 4;
constexpr usize STATE_SCHEMA_OFFSET = 8;
constexpr usize STATE_COUNT_OFFSET = 12;
constexpr usize STATE_HEADER_BYTES = 16;
constexpr usize FIELD_RECORD_BYTES = 12;
bool Valid(const StateRecord& record) noexcept
{
    if (record.Schema == 0 || record.Count > 16)
    {
        return false;
    }
    for (uint32 i = 0; i < record.Count; ++i)
    {
        const auto field = record.Fields[i];
        if (field.Id == 0 || (field.Kind != FieldKind::Uint32 && field.Kind != FieldKind::Boolean) ||
            (field.Kind == FieldKind::Boolean && field.Value > 1))
        {
            return false;
        }
        for (uint32 j = 0; j < i; ++j)
        {
            if (record.Fields[j].Id == field.Id)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

bool EncodeState(const StateRecord& record, std::span<uint8> output, usize& written) noexcept
{
    const usize size = STATE_HEADER_BYTES + static_cast<usize>(record.Count) * FIELD_RECORD_BYTES;
    if (Valid(record) == false || output.size() < size)
    {
        return false;
    }
    (void)TryWriteLittleEndian(MAGIC, output);
    (void)TryWriteLittleEndian(uint32{1}, output.subspan(STATE_VERSION_OFFSET));
    (void)TryWriteLittleEndian(record.Schema, output.subspan(STATE_SCHEMA_OFFSET));
    (void)TryWriteLittleEndian(record.Count, output.subspan(STATE_COUNT_OFFSET));
    for (uint32 i = 0; i < record.Count; ++i)
    {
        auto bytes = output.subspan(16 + static_cast<usize>(i) * 12, 12);
        (void)TryWriteLittleEndian(record.Fields[i].Id, bytes);
        (void)TryWriteLittleEndian(static_cast<uint32>(record.Fields[i].Kind), bytes.subspan(4));
        (void)TryWriteLittleEndian(record.Fields[i].Value, bytes.subspan(8));
    }
    written = size;
    return true;
}

bool DecodeState(std::span<const uint8> bytes, StateRecord& output) noexcept
{
    uint32 magic = 0;
    uint32 version = 0;
    StateRecord result;
    if (bytes.size() < STATE_HEADER_BYTES || TryReadLittleEndian(bytes, magic) == false || magic != MAGIC ||
        TryReadLittleEndian(bytes.subspan(STATE_VERSION_OFFSET), version) == false || version != 1 ||
        TryReadLittleEndian(bytes.subspan(STATE_SCHEMA_OFFSET), result.Schema) == false ||
        TryReadLittleEndian(bytes.subspan(STATE_COUNT_OFFSET), result.Count) == false ||
        result.Count > MAX_STATE_FIELDS ||
        bytes.size() != STATE_HEADER_BYTES + static_cast<usize>(result.Count) * FIELD_RECORD_BYTES)
    {
        return false;
    }
    for (uint32 i = 0; i < result.Count; ++i)
    {
        const auto field = bytes.subspan(16 + static_cast<usize>(i) * 12, 12);
        uint32 kind = 0;
        (void)TryReadLittleEndian(field, result.Fields[i].Id);
        (void)TryReadLittleEndian(field.subspan(4), kind);
        if (kind != 1 && kind != 2)
        {
            return false;
        }
        result.Fields[i].Kind = static_cast<FieldKind>(kind);
        (void)TryReadLittleEndian(field.subspan(8), result.Fields[i].Value);
    }
    if (Valid(result) == false)
    {
        return false;
    }
    output = result;
    return true;
}

bool MigrateState(const StateRecord& source,
                  uint32 schema,
                  std::span<const FieldSpec> fields,
                  StateRecord& output) noexcept
{
    if (Valid(source) == false || schema == 0 || fields.empty() || fields.size() > MAX_STATE_FIELDS)
    {
        return false;
    }
    StateRecord result;
    result.Schema = schema;
    result.Count = static_cast<uint32>(fields.size());
    for (usize i = 0; i < fields.size(); ++i)
    {
        const auto spec = fields[i];
        if (spec.Id == 0 || spec.Min > spec.Max || spec.Default < spec.Min || spec.Default > spec.Max ||
            (spec.Kind != FieldKind::Uint32 && spec.Kind != FieldKind::Boolean) ||
            (spec.Kind == FieldKind::Boolean && spec.Max > 1))
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (fields[j].Id == spec.Id)
            {
                return false;
            }
        }
        FieldValue value{ .Id = spec.Id, .Kind = spec.Kind, .Value = spec.Default };
        for (uint32 j = 0; j < source.Count; ++j)
        {
            if (source.Fields[j].Id == spec.Id)
            {
                value = source.Fields[j];
                if (value.Kind != spec.Kind || value.Value < spec.Min || value.Value > spec.Max)
                {
                    return false;
                }
            }
        }
        result.Fields[i] = value;
    }
    for (uint32 i = 0; i < source.Count; ++i)
    {
        bool found = false;
        for (const auto spec : fields)
        {
            found |= spec.Id == source.Fields[i].Id;
        }
        if (found == false)
        {
            return false;
        }
    }
    output = result;
    return true;
}
} // namespace ludus::runtime::scripting
