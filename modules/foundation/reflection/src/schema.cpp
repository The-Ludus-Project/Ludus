#include <ludus/foundation/reflection/schema.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/parsing/parsing.hpp>

namespace ludus::foundation::reflection
{
namespace
{
Status Fail(Diagnostic& error, Status status, uint32 id = 0) noexcept
{
    error = { .Code = status, .FieldId = id };
    return status;
}
bool ValidText(std::string_view text) noexcept
{
    parsing::ParseError error;
    return text.find('\0') == std::string_view::npos && parsing::ValidateUtf8(text, error) == parsing::ParseStatus::Ok;
}
bool Representable(const Value& value) noexcept
{
    switch (value.Type)
    {
        case Kind::Bool:
            return true;
        case Kind::Int32: {
            int32 result = 0;
            return TryIntegerCast(value.Signed, result);
        }
        case Kind::Uint32: {
            uint32 result = 0;
            return TryIntegerCast(value.Unsigned, result);
        }
        case Kind::Int64:
        case Kind::Uint64:
            return true;
        case Kind::Float32:
            return math::IsFinite(value.Real) && value.Real >= -3.4028234663852886e38 &&
                   value.Real <= 3.4028234663852886e38 &&
                   static_cast<float64>(static_cast<float32>(value.Real)) == value.Real;
        case Kind::Float64:
            return math::IsFinite(value.Real);
    }
    return false;
}
bool Ordered(const Value& low, const Value& high) noexcept
{
    if (low.Type != high.Type || Representable(low) == false || Representable(high) == false)
    {
        return false;
    }
    switch (low.Type)
    {
        case Kind::Bool:
            return low.Boolean == false || high.Boolean;
        case Kind::Int32:
        case Kind::Int64:
            return low.Signed <= high.Signed;
        case Kind::Uint32:
        case Kind::Uint64:
            return low.Unsigned <= high.Unsigned;
        case Kind::Float32:
        case Kind::Float64:
            return low.Real <= high.Real;
    }
    return false;
}
} // namespace

Status ValidateValue(const Field& field, const Value& value) noexcept
{
    return value.Type == field.Default.Type && Ordered(field.Min, value) && Ordered(value, field.Max)
               ? Status::Ok
               : Status::InvalidValue;
}
Status ValidateSchema(const Schema& schema, Diagnostic& error) noexcept
{
    bool hasId = false;
    for (uint8 byte : schema.Id.Bytes)
    {
        hasId |= byte != 0;
    }
    if (hasId == false || schema.Version == 0 || schema.Name.empty() || schema.Name.size() > 256 ||
        ValidText(schema.Name) == false || schema.Fields.empty() || schema.Fields.size() > MAX_FIELDS)
    {
        return Fail(error, Status::InvalidSchema);
    }
    for (usize index = 0; index < schema.Fields.size(); ++index)
    {
        const auto& field = schema.Fields[index];
        if (field.Id == 0 || field.Key.empty() || field.Key.size() > 64 || field.Label.empty() ||
            field.Label.size() > 64 || ValidText(field.Key) == false || ValidText(field.Label) == false ||
            Ordered(field.Min, field.Max) == false || ValidateValue(field, field.Default) != Status::Ok)
        {
            return Fail(error, Status::InvalidSchema, field.Id);
        }
        for (usize prior = 0; prior < index; ++prior)
        {
            if (schema.Fields[prior].Id == field.Id || schema.Fields[prior].Key == field.Key)
            {
                return Fail(error, Status::InvalidSchema, field.Id);
            }
        }
    }
    error = {};
    return Status::Ok;
}
const Field* FindField(const Schema& schema, uint32 id) noexcept
{
    if (schema.Fields.size() > MAX_FIELDS)
    {
        return nullptr;
    }
    for (const auto& field : schema.Fields)
    {
        if (field.Id == id)
        {
            return &field;
        }
    }
    return nullptr;
}
Status ValidateValues(const Schema& schema, std::span<const Value> values, Diagnostic& error) noexcept
{
    if (const auto status = ValidateSchema(schema, error); status != Status::Ok)
    {
        return status;
    }
    if (values.size() != schema.Fields.size())
    {
        return Fail(error, Status::InvalidArgument);
    }
    for (usize index = 0; index < values.size(); ++index)
    {
        if (ValidateValue(schema.Fields[index], values[index]) != Status::Ok)
        {
            return Fail(error, Status::InvalidValue, schema.Fields[index].Id);
        }
    }
    error = {};
    return Status::Ok;
}
Status PrepareValues(const Schema& schema,
                     std::span<const Value> current,
                     std::span<const Edit> edits,
                     std::span<Value> output,
                     Diagnostic& error) noexcept
{
    if (const auto status = ValidateValues(schema, current, error); status != Status::Ok)
    {
        return status;
    }
    if (output.size() < current.size())
    {
        return Fail(error, Status::BufferTooSmall);
    }
    if (edits.empty() || edits.size() > MAX_FIELDS)
    {
        return Fail(error, Status::InvalidArgument);
    }
    Value staged[MAX_FIELDS]{};
    for (usize index = 0; index < current.size(); ++index)
    {
        staged[index] = current[index];
    }
    for (usize index = 0; index < edits.size(); ++index)
    {
        const auto& edit = edits[index];
        const auto* field = FindField(schema, edit.FieldId);
        if (field == nullptr)
        {
            return Fail(error, Status::UnknownField, edit.FieldId);
        }
        for (usize prior = 0; prior < index; ++prior)
        {
            if (edits[prior].FieldId == edit.FieldId)
            {
                return Fail(error, Status::DuplicateField, edit.FieldId);
            }
        }
        if (field->Editable == false)
        {
            return Fail(error, Status::ReadOnly, edit.FieldId);
        }
        if (ValidateValue(*field, edit.Data) != Status::Ok)
        {
            return Fail(error, Status::InvalidValue, edit.FieldId);
        }
        staged[static_cast<usize>(field - schema.Fields.data())] = edit.Data;
    }
    for (usize index = 0; index < current.size(); ++index)
    {
        output[index] = staged[index];
    }
    error = {};
    return Status::Ok;
}
} // namespace ludus::foundation::reflection
