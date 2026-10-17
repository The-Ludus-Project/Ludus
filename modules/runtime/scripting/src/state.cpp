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
    const usize size = 16 + static_cast<usize>(record.Count) * 12;
    if (!Valid(record) || output.size() < size)
    {
        return false;
    }
    (void)TryWriteLittleEndian(MAGIC, output);
    (void)TryWriteLittleEndian(uint32{1}, output.subspan(4));
    (void)TryWriteLittleEndian(record.Schema, output.subspan(8));
    (void)TryWriteLittleEndian(record.Count, output.subspan(12));
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
    if (bytes.size() < 16 || !TryReadLittleEndian(bytes, magic) || magic != MAGIC ||
        !TryReadLittleEndian(bytes.subspan(4), version) || version != 1 ||
        !TryReadLittleEndian(bytes.subspan(8), result.Schema) ||
        !TryReadLittleEndian(bytes.subspan(12), result.Count) || result.Count > 16 ||
        bytes.size() != 16 + static_cast<usize>(result.Count) * 12)
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
    if (!Valid(result))
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
    if (!Valid(source) || schema == 0 || fields.empty() || fields.size() > 16)
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
        if (!found)
        {
            return false;
        }
    }
    output = result;
    return true;
}
} // namespace ludus::runtime::scripting
