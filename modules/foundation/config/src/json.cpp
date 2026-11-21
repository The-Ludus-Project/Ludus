#include <ludus/foundation/config/json.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <new>

// Thanks to Jason Gregory, Game Engine Architecture, 3rd ed., sec.6.5,
// pp.470-479: persist deliberate overrides so new defaults keep taking effect.
// Versioned strict records replace implicit INI coercion; see architecture doc.
namespace ludus::foundation::config
{
namespace
{
/// Radix of the decimal integer spelling in configuration documents.
constexpr uint64 DECIMAL_RADIX = 10;
/// Longest configuration document identity, in bytes.
constexpr usize MAX_IDENTITY_BYTES = 128;
/// Smallest int64, and its magnitude, which has no positive int64 spelling.
constexpr int64 MIN_INT64_VALUE = -9223372036854775807LL - 1;
constexpr uint64 MIN_INT64_MAGNITUDE = uint64{1} << 63;
Status Fail(Diagnostic& error, Status status, std::string_view key = {}, usize record = 0) noexcept
{
    error = {};
    error.Result = status;
    error.Record = record;
    (void)error.Key.TryAssign(key);
    return status;
}
bool ParseDecimal(std::string_view text, uint64& output) noexcept
{
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
    {
        return false;
    }
    uint64 value = 0;
    for (char character : text)
    {
        uint64 next{};
        if (character < '0' || character > '9' || TryMultiply(value, DECIMAL_RADIX, next) == false ||
            TryAdd(next, static_cast<uint64>(character - '0'), value) == false)
        {
            return false;
        }
    }
    output = value;
    return true;
}
void Decimal(parsing::JsonWriter& writer, uint64 magnitude, bool negative, bool quoted) noexcept
{
    char digits[21]{};
    usize position = sizeof(digits);
    do
    {
        digits[--position] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative)
    {
        digits[--position] = '-';
    }
    const std::string_view text{digits + position, sizeof(digits) - position};
    if (quoted)
    {
        writer.String(text);
    }
    else
    {
        writer.Raw(text);
    }
}
Status Finish(parsing::JsonWriter& writer, std::span<const uint8>& output) noexcept
{
    const auto status = writer.Finish(output);
    return status == parsing::ParseStatus::Ok              ? Status::Ok
           : status == parsing::ParseStatus::LimitExceeded ? Status::LimitExceeded
                                                           : Status::InvalidValue;
}
void Header(parsing::JsonWriter& writer, std::string_view identity) noexcept
{
    writer.Raw("{\"version\":1,\"schema\":");
    writer.String(identity);
}
bool ValidIdentity(std::string_view identity) noexcept
{
    return identity.empty() == false && identity.size() <= MAX_IDENTITY_BYTES &&
           ValidateCString(identity) == StringStatus::Ok;
}
void WriteDescriptorBounds(parsing::JsonWriter& inoutWriter, const Descriptor& descriptor) noexcept
{
    Value minimum;
    Value maximum;
    switch (descriptor.Default.Kind)
    {
        case Type::Int32:
        case Type::Int64:
            minimum = Value::FromInt64(descriptor.MinSigned);
            maximum = Value::FromInt64(descriptor.MaxSigned);
            break;
        case Type::Uint32:
        case Type::Uint64:
            minimum = Value::FromUint64(descriptor.MinUnsigned);
            maximum = Value::FromUint64(descriptor.MaxUnsigned);
            break;
        case Type::Float32:
        case Type::Float64:
            minimum = Value::FromFloat64(descriptor.MinReal);
            maximum = Value::FromFloat64(descriptor.MaxReal);
            break;
        case Type::Enum:
        case Type::String:
            inoutWriter.Raw(",\"max_text_bytes\":");
            inoutWriter.Integer(descriptor.MaxTextBytes);
            return;
        case Type::Bool:
            return;
    }
    inoutWriter.Raw(",\"min\":");
    EncodeValue(inoutWriter, minimum);
    inoutWriter.Raw(",\"max\":");
    EncodeValue(inoutWriter, maximum);
}
} // namespace
Status DecodeValue(parsing::JsonValue input, Type type, Value& output) noexcept
{
    Value result;
    result.Kind = type;
    bool ok = false;
    switch (type)
    {
        case Type::Bool:
            ok = input.Boolean(result.Boolean);
            break;
        case Type::Int32: {
            int32 narrowed{};
            ok = input.SignedInteger(result.Signed) && TryIntegerCast(result.Signed, narrowed);
            break;
        }
        case Type::Uint32: {
            uint32 narrowed{};
            ok = input.Integer(result.Unsigned) && TryIntegerCast(result.Unsigned, narrowed);
            break;
        }
        case Type::Int64:
        case Type::Uint64: {
            std::string_view text;
            uint64 magnitude{};
            if (input.String(text) == false)
            {
                break;
            }
            const bool isNegative = text.empty() == false && text.front() == '-';
            if (isNegative && type == Type::Uint64)
            {
                break;
            }
            if (ParseDecimal(isNegative ? text.substr(1) : text, magnitude) == false || (isNegative && magnitude == 0))
            {
                break;
            }
            if (type == Type::Uint64)
            {
                result.Unsigned = magnitude;
                ok = true;
            }
            else if (isNegative && magnitude == MIN_INT64_MAGNITUDE)
            {
                result.Signed = MIN_INT64_VALUE;
                ok = true;
            }
            else if (TryIntegerCast(magnitude, result.Signed))
            {
                if (isNegative)
                {
                    result.Signed = -result.Signed;
                }
                ok = true;
            }
            break;
        }
        case Type::Float32:
        case Type::Float64:
            ok = input.Number(result.Real);
            break;
        case Type::Enum:
        case Type::String: {
            std::string_view text;
            ok = input.String(text) && Value::FromText(type, text, result) == Status::Ok;
            break;
        }
        default:
            break;
    }
    if (ok == false)
    {
        return Status::InvalidValue;
    }
    output = result;
    return Status::Ok;
}
void EncodeValue(parsing::JsonWriter& writer, const Value& value) noexcept
{
    switch (value.Kind)
    {
        case Type::Bool:
            writer.Boolean(value.Boolean);
            break;
        case Type::Int32:
        case Type::Int64: {
            const uint64 bits = static_cast<uint64>(value.Signed);
            Decimal(writer, value.Signed < 0 ? uint64{0} - bits : bits, value.Signed < 0, value.Kind == Type::Int64);
            break;
        }
        case Type::Uint32:
            writer.Integer(value.Unsigned);
            break;
        case Type::Uint64:
            Decimal(writer, value.Unsigned, false, true);
            break;
        case Type::Float32:
        case Type::Float64:
            writer.Number(value.Real);
            break;
        case Type::Enum:
        case Type::String:
            writer.String(value.Text.GetView());
            break;
        default:
            writer.Raw("null");
            break;
    }
}
Status PrepareJson(Context& context,
                   std::string_view input,
                   Layer expectedLayer,
                   std::string_view schemaIdentity,
                   uint64 expectedRevision,
                   const AllocationDomain& domain,
                   Diagnostic& error) noexcept
{
    error = {};
    if (context.IsValid() == false || ValidIdentity(schemaIdentity) == false ||
        static_cast<usize>(expectedLayer) >= LAYER_COUNT)
    {
        return Fail(error, Status::InvalidState);
    }
    parsing::JsonDocument document(domain);
    parsing::ParseError parseError;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_BUNDLE_BYTES;
    limits.MaxDepth = 4;
    limits.MaxObjectMembers = 8;
    limits.MaxArrayElements = 4096;
    limits.MaxValues = 50000;
    limits.MaxStringBytes = 4096;
    const auto parsed = document.Read(input, parseError, limits);
    if (parsed != parsing::ParseStatus::Ok)
    {
        const auto status = parsed == parsing::ParseStatus::OutOfMemory     ? Status::OutOfMemory
                            : parsed == parsing::ParseStatus::LimitExceeded ? Status::LimitExceeded
                                                                            : Status::InvalidBundle;
        (void)Fail(error, status);
        error.ByteOffset = parseError.Offset;
        return status;
    }
    return PrepareJsonLayer(context, document.Root(), expectedLayer, schemaIdentity, expectedRevision, domain, error);
}
Status PrepareJsonLayer(Context& inoutContext,
                        parsing::JsonValue root,
                        Layer expectedLayer,
                        std::string_view schemaIdentity,
                        uint64 expectedRevision,
                        const AllocationDomain& domain,
                        Diagnostic& outDiagnostic) noexcept
{
    outDiagnostic = {};
    if (inoutContext.IsValid() == false || ValidIdentity(schemaIdentity) == false ||
        static_cast<usize>(expectedLayer) >= LAYER_COUNT)
    {
        return Fail(outDiagnostic, Status::InvalidState);
    }
    uint64 version{};
    std::string_view identity;
    std::string_view layer;
    const parsing::JsonValue values = root.Get("values");
    if (root.Fields({"version", "schema", "layer", "values"}) == false ||
        root.Get("version").Integer(version) == false || version != 1 || root.Get("schema").String(identity) == false ||
        identity != schemaIdentity || root.Get("layer").String(layer) == false || layer != LayerName(expectedLayer) ||
        values.Array() == false || values.Count() > inoutContext.Schema().size())
    {
        return Fail(outDiagnostic, Status::InvalidBundle);
    }
    const usize count = values.Count();
    if (count == 0)
    {
        return inoutContext.Prepare(expectedLayer, {}, true, expectedRevision, outDiagnostic);
    }
    usize bytes{};
    if (TryMultiply(count, sizeof(Assignment), bytes) == false)
    {
        return Fail(outDiagnostic, Status::LimitExceeded);
    }
    Assignment* assignmentsOrNull = static_cast<Assignment*>(domain.TryAllocate(bytes, alignof(Assignment)));
    if (assignmentsOrNull == nullptr)
    {
        return Fail(outDiagnostic, Status::OutOfMemory);
    }
    struct Cleanup final
    {
        const AllocationDomain& Domain;
        Assignment* Data;
        usize Count;
        ~Cleanup() noexcept
        {
            for (usize i = 0; i < Count; ++i)
            {
                Data[i].~Assignment();
            }
            Domain.Free(Data, Count * sizeof(Assignment), alignof(Assignment));
        }
    } cleanup{domain, assignmentsOrNull, count};
    for (usize i = 0; i < count; ++i)
    {
        new (assignmentsOrNull + i) Assignment{};
    }
    for (usize i = 0; i < count; ++i)
    {
        const parsing::JsonValue record = values.At(i);
        Assignment& assignment = assignmentsOrNull[i];
        std::string_view type;
        std::string_view source;
        if (record.Fields({"key", "type", "value", "source", "line"}) == false ||
            record.Get("key").String(assignment.Name) == false || record.Get("type").String(type) == false ||
            record.Get("source").String(source) == false || source.size() > MAX_ORIGIN_SOURCE_BYTES ||
            ValidateCString(source) != StringStatus::Ok || assignment.From.Source.TryAssign(source) != StringStatus::Ok)
        {
            return Fail(outDiagnostic, Status::InvalidBundle, assignment.Name, i);
        }
        Binding binding;
        const Status bound = inoutContext.Bind(assignment.Name, binding);
        if (bound != Status::Ok)
        {
            return Fail(outDiagnostic, bound, assignment.Name, i);
        }
        const Type kind = inoutContext.Schema()[binding.Index].Default.Kind;
        if (type != TypeName(kind))
        {
            return Fail(outDiagnostic, Status::InvalidValue, assignment.Name, i);
        }
        uint64 line{};
        if (record.Get("line").Integer(line) == false || TryIntegerCast(line, assignment.From.Line) == false)
        {
            return Fail(outDiagnostic, Status::InvalidBundle, assignment.Name, i);
        }
        const Status decoded = DecodeValue(record.Get("value"), kind, assignment.Data);
        if (decoded != Status::Ok)
        {
            return Fail(outDiagnostic, decoded, assignment.Name, i);
        }
    }
    return inoutContext.Prepare(expectedLayer, {assignmentsOrNull, count}, true, expectedRevision, outDiagnostic);
}
Status WriteLayer(const Context& context,
                  Layer layer,
                  std::string_view schemaIdentity,
                  std::span<uint8> buffer,
                  std::span<const uint8>& output) noexcept
{
    if (context.IsValid() == false || ValidIdentity(schemaIdentity) == false ||
        static_cast<usize>(layer) >= LAYER_COUNT)
    {
        return Status::InvalidState;
    }
    const usize capacity = buffer.size() < MAX_BUNDLE_BYTES ? buffer.size() : MAX_BUNDLE_BYTES;
    parsing::JsonWriter writer(buffer.first(capacity));
    Header(writer, schemaIdentity);
    writer.Raw(",\"layer\":");
    writer.String(LayerName(layer));
    writer.Raw(",\"values\":[");
    bool first = true;
    for (const auto& descriptor : context.Schema())
    {
        Binding binding;
        Value value;
        Origin origin;
        if (context.Bind(descriptor.Name, binding) != Status::Ok ||
            context.ReadLayer(binding, layer, value, origin) != Status::Ok)
        {
            continue;
        }
        if (first == false)
        {
            writer.Raw(",");
        }
        first = false;
        writer.Raw("{\"key\":");
        writer.String(descriptor.Name);
        writer.Raw(",\"type\":");
        writer.String(TypeName(value.Kind));
        writer.Raw(",\"value\":");
        EncodeValue(writer, value);
        writer.Raw(",\"source\":");
        writer.String(origin.Source.GetView());
        writer.Raw(",\"line\":");
        writer.Integer(origin.Line);
        writer.Raw("}");
    }
    writer.Raw("]}");
    return Finish(writer, output);
}
Status WriteSchema(const Context& context,
                   std::string_view schemaIdentity,
                   std::span<uint8> buffer,
                   std::span<const uint8>& outOutput) noexcept
{
    if (context.IsValid() == false || ValidIdentity(schemaIdentity) == false)
    {
        return Status::InvalidState;
    }
    parsing::JsonWriter writer(buffer);
    Header(writer, schemaIdentity);
    writer.Raw(",\"settings\":[");
    bool isFirst = true;
    for (const Descriptor& descriptor : context.Schema())
    {
        if (isFirst == false)
        {
            writer.Raw(",");
        }
        isFirst = false;
        writer.Raw("{\"key\":");
        writer.String(descriptor.Name);
        writer.Raw(",\"type\":");
        writer.String(TypeName(descriptor.Default.Kind));
        writer.Raw(",\"default\":");
        EncodeValue(writer, descriptor.Default);
        writer.Raw(",\"help\":");
        writer.String(descriptor.Help);
        writer.Raw(",\"units\":");
        writer.String(descriptor.Units);
        writer.Raw(",\"apply\":");
        writer.String(ApplyName(descriptor.Application));
        writer.Raw(",\"persistent\":");
        writer.Boolean(descriptor.Persistent);
        writer.Raw(",\"simulation\":");
        writer.Boolean(descriptor.Simulation);
        writer.Raw(",\"allowed_sources\":");
        writer.Integer(descriptor.AllowedSources);
        writer.Raw(",\"group\":");
        writer.Integer(descriptor.Group);
        WriteDescriptorBounds(writer, descriptor);
        writer.Raw(",\"choices\":[");
        bool isFirstChoice = true;
        for (std::string_view choice : descriptor.Choices)
        {
            if (isFirstChoice == false)
            {
                writer.Raw(",");
            }
            isFirstChoice = false;
            writer.String(choice);
        }
        writer.Raw("]}");
    }
    writer.Raw("]}");
    return Finish(writer, outOutput);
}
} // namespace ludus::foundation::config
