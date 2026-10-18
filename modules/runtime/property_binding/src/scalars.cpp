#include <ludus/runtime/property_binding/scalars.hpp>

#include <ludus/foundation/base/checked_integer.hpp>

#include <cstring>

// Thanks to Charles Cafrelli, "A Property Class for Generic C++ Member Access",
// Game Programming Gems 2, section 1.7, pp. 46-50: generic tools consume native
// member descriptions. Thanks to Matthew Campbell and Curtiss Murphy, "Exposing
// Actor Properties Using Nonintrusive Proxies", Game Programming Gems 6,
// section 4.5, pp. 383-392: a projection keeps tool types out of native records.
// Our independent adapter copies bounded ABI records instead of retaining raw
// instance pointers and uses statuses instead of exceptions. Detailed review:
// docs/architecture/reflection-serialization-gems-review.md.
namespace ludus::runtime::property_binding
{
using namespace foundation;
using namespace foundation::reflection;
using game_api::PropertyKind;
namespace
{
Status Fail(Diagnostic& error, Status status, uint32 id = 0) noexcept
{
    error = { .Code = status, .FieldId = id };
    return status;
}
bool ProjectKind(Kind kind, uint32& output) noexcept
{
    switch (kind)
    {
        case Kind::Bool:
            output = static_cast<uint32>(PropertyKind::Bool);
            return true;
        case Kind::Int32:
            output = static_cast<uint32>(PropertyKind::Int32);
            return true;
        case Kind::Float32:
            output = static_cast<uint32>(PropertyKind::Float32);
            return true;
        default:
            return false;
    }
}
Status ValidateProjection(const Schema& schema, Diagnostic& error) noexcept
{
    if (const auto status = ValidateSchema(schema, error); status != Status::Ok)
    {
        return status;
    }
    for (const auto& field : schema.Fields)
    {
        uint32 kind = 0;
        if (ProjectKind(field.Default.Type, kind) == false)
        {
            return Fail(error, Status::UnsupportedKind, field.Id);
        }
    }
    return Status::Ok;
}
} // namespace
Status Describe(const Schema& schema,
                uint64 objectId,
                std::span<game_api::PropertyDescriptor> output,
                Diagnostic& error) noexcept
{
    if (const auto status = ValidateProjection(schema, error); status != Status::Ok)
    {
        return status;
    }
    if (objectId == 0)
    {
        return Fail(error, Status::InvalidArgument);
    }
    if (output.size() < schema.Fields.size())
    {
        return Fail(error, Status::BufferTooSmall);
    }
    game_api::PropertyDescriptor staged[MAX_FIELDS]{};
    for (usize index = 0; index < schema.Fields.size(); ++index)
    {
        const auto& field = schema.Fields[index];
        auto& descriptor = staged[index];
        descriptor.ObjectId = objectId;
        descriptor.PropertyId = field.Id;
        (void)ProjectKind(field.Default.Type, descriptor.Kind);
        descriptor.Scope = static_cast<uint32>(field.Persist    ? game_api::PropertyScope::Persistable
                                               : field.Editable ? game_api::PropertyScope::SessionOnly
                                                                : game_api::PropertyScope::ReadOnly);
        descriptor.Writable = field.Editable ? 1 : 0;
        descriptor.LabelLength = static_cast<uint8>(field.Label.size());
        std::memcpy(descriptor.Label, field.Label.data(), field.Label.size());
        if (field.Default.Type == Kind::Int32)
        {
            descriptor.MinInt = static_cast<int32>(field.Min.Signed);
            descriptor.MaxInt = static_cast<int32>(field.Max.Signed);
        }
        if (field.Default.Type == Kind::Float32)
        {
            descriptor.MinFloat = static_cast<float32>(field.Min.Real);
            descriptor.MaxFloat = static_cast<float32>(field.Max.Real);
        }
    }
    for (usize index = 0; index < schema.Fields.size(); ++index)
    {
        output[index] = staged[index];
    }
    error = {};
    return Status::Ok;
}
Status Snapshot(const Schema& schema,
                std::span<const Value> values,
                uint64 objectId,
                uint64 revision,
                std::span<game_api::PropertyValue> output,
                Diagnostic& error) noexcept
{
    if (const auto status = ValidateProjection(schema, error); status != Status::Ok)
    {
        return status;
    }
    if (const auto status = ValidateValues(schema, values, error); status != Status::Ok)
    {
        return status;
    }
    if (objectId == 0 || revision == 0)
    {
        return Fail(error, Status::InvalidArgument);
    }
    if (output.size() < values.size())
    {
        return Fail(error, Status::BufferTooSmall);
    }
    game_api::PropertyValue staged[MAX_FIELDS]{};
    for (usize index = 0; index < values.size(); ++index)
    {
        auto& snapshot = staged[index];
        const auto& value = values[index];
        snapshot.ObjectId = objectId;
        snapshot.PropertyId = schema.Fields[index].Id;
        snapshot.ObjectRevision = revision;
        (void)ProjectKind(value.Type, snapshot.Kind);
        if (value.Type == Kind::Bool)
        {
            snapshot.IntOrEnum = value.Boolean ? 1U : 0U;
        }
        else if (value.Type == Kind::Int32)
        {
            const auto integer = static_cast<int32>(value.Signed);
            std::memcpy(&snapshot.IntOrEnum, &integer, sizeof(integer));
        }
        else
        {
            snapshot.Float = static_cast<float32>(value.Real);
        }
    }
    for (usize index = 0; index < values.size(); ++index)
    {
        output[index] = staged[index];
    }
    error = {};
    return Status::Ok;
}
Status DecodeEdits(const Schema& schema,
                   std::span<const game_api::PropertyEdit> input,
                   uint64 objectId,
                   uint64 revision,
                   std::span<Edit> output,
                   Diagnostic& error) noexcept
{
    if (const auto status = ValidateProjection(schema, error); status != Status::Ok)
    {
        return status;
    }
    if (objectId == 0 || revision == 0 || input.empty() || input.size() > game_api::kMaxEditBatch)
    {
        return Fail(error, Status::InvalidArgument);
    }
    if (output.size() < input.size())
    {
        return Fail(error, Status::BufferTooSmall);
    }
    Edit staged[MAX_FIELDS]{};
    for (usize index = 0; index < input.size(); ++index)
    {
        const auto& edit = input[index];
        uint32 id = 0;
        if (edit.ObjectId != objectId || edit.ExpectedRevision != revision ||
            TryIntegerCast(edit.PropertyId, id) == false)
        {
            return Fail(error, Status::InvalidArgument);
        }
        const auto* field = FindField(schema, id);
        if (field == nullptr)
        {
            return Fail(error, Status::UnknownField, id);
        }
        if (field->Editable == false)
        {
            return Fail(error, Status::ReadOnly, id);
        }
        for (usize prior = 0; prior < index; ++prior)
        {
            if (input[prior].PropertyId == edit.PropertyId)
            {
                return Fail(error, Status::DuplicateField, id);
            }
        }
        uint32 kind = 0;
        (void)ProjectKind(field->Default.Type, kind);
        if (edit.Kind != kind || edit.StringLength != 0)
        {
            return Fail(error, Status::InvalidValue, id);
        }
        auto& value = staged[index].Data;
        staged[index].FieldId = id;
        value.Type = field->Default.Type;
        switch (value.Type)
        {
            case Kind::Bool:
                if (edit.IntOrEnum > 1)
                {
                    return Fail(error, Status::InvalidValue, id);
                }
                value.Boolean = edit.IntOrEnum != 0;
                break;
            case Kind::Int32: {
                int32 integer = 0;
                std::memcpy(&integer, &edit.IntOrEnum, sizeof(integer));
                value.Signed = integer;
                break;
            }
            case Kind::Float32:
                value.Real = static_cast<float64>(edit.Float);
                break;
            default:
                return Fail(error, Status::UnsupportedKind, id);
        }
        if (ValidateValue(*field, value) != Status::Ok)
        {
            return Fail(error, Status::InvalidValue, id);
        }
    }
    for (usize index = 0; index < input.size(); ++index)
    {
        output[index] = staged[index];
    }
    error = {};
    return Status::Ok;
}
} // namespace ludus::runtime::property_binding
