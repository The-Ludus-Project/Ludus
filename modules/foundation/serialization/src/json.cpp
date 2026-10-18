#include <ludus/foundation/serialization/json.hpp>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/parsing/json.hpp>

// Thanks to Jason Beardsley, "Template-Based Object Serialization", Game
// Programming Gems 3, section 5.5, pp. 534-545: explicit scalar representations
// and owner extension points. This authored JSON codec preserves exact integers,
// validates in shipping, and stages values before publication. No chapter code
// is copied. Review: docs/architecture/reflection-serialization-gems-review.md.
namespace ludus::foundation::serialization
{
using namespace reflection;
namespace
{
Status Fail(Diagnostic& error, Status status, uint32 id = 0) noexcept
{
    error = { .Code = status, .FieldId = id };
    return status;
}
void FormatId(const SchemaId& id, char (&text)[36]) noexcept
{
    constexpr std::string_view hex = "0123456789abcdef";
    usize cursor = 0;
    for (usize index = 0; index < 16; ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
        {
            text[cursor++] = '-';
        }
        text[cursor++] = hex[id.Bytes[index] >> 4U];
        text[cursor++] = hex[id.Bytes[index] & 15U];
    }
}
Status ParseFailure(parsing::ParseStatus status) noexcept
{
    switch (status)
    {
        case parsing::ParseStatus::Ok:
            return Status::Ok;
        case parsing::ParseStatus::OutOfMemory:
            return Status::OutOfMemory;
        case parsing::ParseStatus::LimitExceeded:
            return Status::LimitExceeded;
        default:
            return Status::InvalidDocument;
    }
}
bool Decode(parsing::JsonValue node, const Field& field, Value& result) noexcept
{
    const auto kind = field.Default.Type;
    Value value{ .Type = kind };
    bool valid = false;
    switch (kind)
    {
        case Kind::Bool:
            valid = node.Boolean(value.Boolean);
            break;
        case Kind::Int32:
        case Kind::Int64:
            valid = node.SignedInteger(value.Signed);
            break;
        case Kind::Uint32:
        case Kind::Uint64:
            valid = node.Integer(value.Unsigned);
            break;
        case Kind::Float32:
            valid = node.Number(value.Real) && math::IsFinite(value.Real) && value.Real >= field.Min.Real &&
                    value.Real <= field.Max.Real;
            if (valid)
            {
                value.Real = static_cast<float64>(static_cast<float32>(value.Real));
            }
            break;
        case Kind::Float64:
            valid = node.Number(value.Real);
            break;
    }
    if (valid)
    {
        result = value;
    }
    return valid;
}
void Encode(parsing::JsonWriter& writer, const Value& value) noexcept
{
    switch (value.Type)
    {
        case Kind::Bool:
            writer.Boolean(value.Boolean);
            break;
        case Kind::Int32:
        case Kind::Int64:
            if (value.Signed < 0)
            {
                writer.Raw("-");
                writer.Integer(static_cast<uint64>(-(value.Signed + 1)) + 1U);
            }
            else
            {
                writer.Integer(static_cast<uint64>(value.Signed));
            }
            break;
        case Kind::Uint32:
        case Kind::Uint64:
            writer.Integer(value.Unsigned);
            break;
        case Kind::Float32:
        case Kind::Float64:
            writer.Number(value.Real);
            break;
    }
}
} // namespace

Status ReadJson(const Schema& schema,
                std::string_view input,
                std::span<Value> output,
                const AllocationDomain& domain,
                Diagnostic& error) noexcept
{
    if (const auto status = ValidateSchema(schema, error); status != Status::Ok)
    {
        return status;
    }
    if (output.size() < schema.Fields.size())
    {
        return Fail(error, Status::BufferTooSmall);
    }
    parsing::JsonDocument document(domain);
    parsing::ParseError parseError;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_DOCUMENT_BYTES;
    limits.MaxWorkspaceBytes = MAX_DOCUMENT_BYTES * 16;
    limits.MaxDepth = 2;
    limits.MaxObjectMembers = MAX_FIELDS;
    limits.MaxArrayElements = 0;
    limits.MaxValues = MAX_FIELDS + 4;
    limits.MaxStringBytes = 64;
    if (const auto status = document.Read(input, parseError, limits); status != parsing::ParseStatus::Ok)
    {
        error = { .Code = ParseFailure(status), .ByteOffset = parseError.Offset };
        return error.Code;
    }
    const auto root = document.Root();
    if (root.Fields({"schema", "version", "values"}) == false || root.Get("values").Object() == false)
    {
        return Fail(error, Status::InvalidDocument);
    }
    char expectedId[36]{};
    FormatId(schema.Id, expectedId);
    std::string_view actualId;
    uint64 version = 0;
    if (root.Get("schema").String(actualId) == false || root.Get("version").Integer(version) == false)
    {
        return Fail(error, Status::InvalidDocument);
    }
    if (actualId != std::string_view{expectedId, sizeof(expectedId)} || version != schema.Version)
    {
        return Fail(error, Status::SchemaChanged);
    }
    Value staged[MAX_FIELDS]{};
    for (usize index = 0; index < schema.Fields.size(); ++index)
    {
        staged[index] = schema.Fields[index].Default;
    }
    const auto values = root.Get("values");
    for (usize index = 0; index < values.MemberCount(); ++index)
    {
        std::string_view key;
        parsing::JsonValue node;
        (void)values.MemberAt(index, key, node);
        usize slot = 0;
        while (slot < schema.Fields.size() && schema.Fields[slot].Key != key)
        {
            ++slot;
        }
        if (slot == schema.Fields.size())
        {
            return Fail(error, Status::UnknownField);
        }
        const auto& field = schema.Fields[slot];
        if (field.Persist == false)
        {
            return Fail(error, Status::ReadOnly, field.Id);
        }
        if (Decode(node, field, staged[slot]) == false || ValidateValue(field, staged[slot]) != Status::Ok)
        {
            return Fail(error, Status::InvalidValue, field.Id);
        }
    }
    for (usize index = 0; index < schema.Fields.size(); ++index)
    {
        output[index] = staged[index];
    }
    error = {};
    return Status::Ok;
}
Status WriteJson(const Schema& schema,
                 std::span<const Value> values,
                 std::span<uint8> storage,
                 std::span<const uint8>& output,
                 Diagnostic& error) noexcept
{
    if (const auto status = ValidateValues(schema, values, error); status != Status::Ok)
    {
        return status;
    }
    if (storage.size() > MAX_DOCUMENT_BYTES)
    {
        storage = storage.first(MAX_DOCUMENT_BYTES);
    }
    parsing::JsonWriter writer(storage);
    char id[36]{};
    FormatId(schema.Id, id);
    writer.Raw("{\"schema\":");
    writer.String({id, sizeof(id)});
    writer.Raw(",\"version\":");
    writer.Integer(schema.Version);
    writer.Raw(",\"values\":{");
    bool first = true;
    for (usize index = 0; index < schema.Fields.size(); ++index)
    {
        const auto& field = schema.Fields[index];
        if (field.Persist == false)
        {
            continue;
        }
        if (first == false)
        {
            writer.Raw(",");
        }
        first = false;
        writer.String(field.Key);
        writer.Raw(":");
        Encode(writer, values[index]);
    }
    writer.Raw("}}");
    const auto status = ParseFailure(writer.Finish(output));
    if (status != Status::Ok)
    {
        return Fail(error, status);
    }
    error = {};
    return Status::Ok;
}
} // namespace ludus::foundation::serialization
