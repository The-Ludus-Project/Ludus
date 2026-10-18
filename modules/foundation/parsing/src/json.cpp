#include <ludus/foundation/parsing/json.hpp>

#include <ludus/foundation/base/checked_integer.hpp>

#include <span>
#include <string_view>
#include <utility>

#include <yyjson.h>

// Thanks to Tim Bray (editor) and the IETF, "The JavaScript Object Notation
// (JSON) Data Interchange Format", RFC 8259 (2017), sections 2-9 (decoded-key comparison in 8.3):
// https://www.rfc-editor.org/rfc/rfc8259 . Adopt JSON syntax and decoded-key
// comparison; duplicate-key rejection and bounded admission are Ludus policies.
// Thanks to YaoYuan (ibireme) and yyjson contributors, "yyjson 0.10.0",
// yyjson.h reader/allocator/DOM API: https://github.com/ibireme/yyjson/tree/0.10.0 .
// This adapter uses the bundled backend with owned pool storage and additional
// validation; retain its MIT notice in third_party/yyjson/LICENSE.
// Source review and limits: docs/architecture/parsing-gems-review.md.
namespace ludus::foundation::parsing
{
namespace
{
constexpr usize POOL_ALIGNMENT = alignof(uint64);

yyjson_val* Val(JsonValue value) noexcept
{
    return const_cast<yyjson_val*>(static_cast<const yyjson_val*>(value.Value));
}
ParseStatus Fail(ParseError& error, ParseStatus status, usize offset = UNKNOWN_BYTE_OFFSET) noexcept
{
    error = {};
    error.Status = status;
    error.Offset = offset;
    return status;
}
struct LimitUsage final
{
    usize Budget;
    usize Observed;
};
ParseStatus Limit(ParseError& error, ParseLimit limit, LimitUsage usage, usize offset = UNKNOWN_BYTE_OFFSET) noexcept
{
    (void)Fail(error, ParseStatus::LimitExceeded, offset);
    error.Limit = limit;
    error.Budget = usage.Budget;
    error.Observed = usage.Observed;
    return error.Status;
}

// Admission scan only, not a second JSON parser. Escaped quotes/brackets are
// ignored. Full grammar, scalar depth, and Unicode escapes remain codec checks.
ParseStatus CheckNesting(std::string_view input, usize maxDepth, ParseError& error) noexcept
{
    usize depth = 0;
    bool string = false;
    bool escaped = false;
    for (usize offset = 0; offset < input.size(); ++offset)
    {
        const char ch = input[offset];
        if (string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                string = false;
            }
        }
        else if (ch == '"')
        {
            string = true;
        }
        else if (ch == '[' || ch == '{')
        {
            if (depth > maxDepth)
            {
                return Limit(error, ParseLimit::NestingDepth, { .Budget = maxDepth, .Observed = depth }, offset);
            }
            ++depth;
        }
        else if ((ch == ']' || ch == '}') && depth != 0)
        {
            --depth;
        }
    }
    return ParseStatus::Ok;
}
ParseStatus CheckString(yyjson_val* value, const JsonLimits& limits, ParseError& error) noexcept
{
    const usize bytes = yyjson_get_len(value);
    if (bytes > limits.MaxStringBytes)
    {
        return Limit(error, ParseLimit::StringBytes, { .Budget = limits.MaxStringBytes, .Observed = bytes });
    }
    return ParseStatus::Ok;
}
// Recursion and quadratic duplicate comparison have hard safety ceilings.
// Decoded bytes, including embedded NUL, decide identity, never a hash alone.
ParseStatus Check(yyjson_val* value, usize depth, usize& values, const JsonLimits& limits, ParseError& error) noexcept
{
    if (depth > limits.MaxDepth)
    {
        return Limit(error, ParseLimit::NestingDepth, { .Budget = limits.MaxDepth, .Observed = depth });
    }
    if (values >= limits.MaxValues)
    {
        return Limit(error, ParseLimit::Values, { .Budget = limits.MaxValues, .Observed = values + 1 });
    }
    ++values;
    if (yyjson_is_str(value))
    {
        return CheckString(value, limits, error);
    }
    if (yyjson_is_obj(value))
    {
        const usize count = yyjson_obj_size(value);
        if (count > limits.MaxObjectMembers)
        {
            return Limit(error, ParseLimit::ObjectMembers, { .Budget = limits.MaxObjectMembers, .Observed = count });
        }
        usize index = 0, length = 0;
        yyjson_val* key = nullptr;
        yyjson_val* child = nullptr;
        yyjson_obj_foreach(value, index, length, key, child)
        {
            if (CheckString(key, limits, error) != ParseStatus::Ok)
            {
                return error.Status;
            }
            const std::string_view name{yyjson_get_str(key), yyjson_get_len(key)};
            yyjson_obj_iter previous = yyjson_obj_iter_with(value);
            for (usize i = 0; i < index; ++i)
            {
                yyjson_val* other = yyjson_obj_iter_next(&previous);
                if (name == std::string_view{yyjson_get_str(other), yyjson_get_len(other)})
                {
                    return Fail(error, ParseStatus::DuplicateKey);
                }
            }
            if (Check(child, depth + 1, values, limits, error) != ParseStatus::Ok)
            {
                return error.Status;
            }
        }
    }
    else if (yyjson_is_arr(value))
    {
        const usize count = yyjson_arr_size(value);
        if (count > limits.MaxArrayElements)
        {
            return Limit(error, ParseLimit::ArrayElements, { .Budget = limits.MaxArrayElements, .Observed = count });
        }
        usize index = 0, length = 0;
        yyjson_val* child = nullptr;
        yyjson_arr_foreach(value, index, length, child)
        {
            if (Check(child, depth + 1, values, limits, error) != ParseStatus::Ok)
            {
                return error.Status;
            }
        }
    }
    return ParseStatus::Ok;
}
} // namespace

JsonValue JsonValue::Get(std::string_view key) const noexcept
{
    return {yyjson_obj_getn(Val(*this), key.empty() ? "" : key.data(), key.size())};
}
JsonValue JsonValue::Get(const char* key) const noexcept
{
    return key == nullptr ? JsonValue{} : Get(std::string_view{key});
}
JsonValue JsonValue::At(usize index) const noexcept
{
    return {yyjson_arr_get(Val(*this), index)};
}
usize JsonValue::Count() const noexcept
{
    return yyjson_arr_size(Val(*this));
}
bool JsonValue::Array() const noexcept
{
    return yyjson_is_arr(Val(*this));
}
bool JsonValue::Object() const noexcept
{
    return yyjson_is_obj(Val(*this));
}
usize JsonValue::MemberCount() const noexcept
{
    return yyjson_obj_size(Val(*this));
}
bool JsonValue::MemberAt(usize index, std::string_view& key, JsonValue& value) const noexcept
{
    if (Object() == false || index >= MemberCount())
    {
        return false;
    }
    auto iterator = yyjson_obj_iter_with(Val(*this));
    yyjson_val* entry = nullptr;
    for (usize current = 0; current <= index; ++current)
    {
        entry = yyjson_obj_iter_next(&iterator);
    }
    key = {yyjson_get_str(entry), yyjson_get_len(entry)};
    value = { .Value = yyjson_obj_iter_get_val(entry) };
    return true;
}
bool JsonValue::Null() const noexcept
{
    return yyjson_is_null(Val(*this));
}
bool JsonValue::String(std::string_view& output) const noexcept
{
    if (yyjson_is_str(Val(*this)) == false)
    {
        return false;
    }
    output = {yyjson_get_str(Val(*this)), yyjson_get_len(Val(*this))};
    return true;
}
bool JsonValue::Integer(uint64& output) const noexcept
{
    if (yyjson_is_uint(Val(*this)) == false)
    {
        return false;
    }
    output = yyjson_get_uint(Val(*this));
    return true;
}
bool JsonValue::SignedInteger(int64& output) const noexcept
{
    if (yyjson_is_sint(Val(*this)))
    {
        output = yyjson_get_sint(Val(*this));
        return true;
    }
    return yyjson_is_uint(Val(*this)) && TryIntegerCast(yyjson_get_uint(Val(*this)), output);
}
bool JsonValue::Number(float64& output) const noexcept
{
    if (yyjson_is_num(Val(*this)) == false)
    {
        return false;
    }
    output = yyjson_get_num(Val(*this));
    return true;
}
bool JsonValue::Boolean(bool& output) const noexcept
{
    if (yyjson_is_bool(Val(*this)) == false)
    {
        return false;
    }
    output = yyjson_get_bool(Val(*this));
    return true;
}
bool JsonValue::Fields(std::initializer_list<const char*> required) const noexcept
{
    if (Object() == false || yyjson_obj_size(Val(*this)) != required.size())
    {
        return false;
    }
    for (const char* key : required)
    {
        if (Get(key).Valid() == false)
        {
            return false;
        }
    }
    return true;
}

JsonDocument::JsonDocument() noexcept : JsonDocument(GetSystemAllocationDomain()) {}
JsonDocument::JsonDocument(const AllocationDomain& domain) noexcept : mDomain(&domain) {}
JsonDocument::~JsonDocument() noexcept
{
    Release();
}
JsonDocument::JsonDocument(JsonDocument&& other) noexcept
    : mDomain(other.mDomain), mDocument(std::exchange(other.mDocument, nullptr)),
      mPool(std::exchange(other.mPool, nullptr)), mCapacity(std::exchange(other.mCapacity, 0))
{
}
JsonDocument& JsonDocument::operator=(JsonDocument&& other) noexcept
{
    if (this != &other)
    {
        Release();
        mDomain = other.mDomain;
        mDocument = std::exchange(other.mDocument, nullptr);
        mPool = std::exchange(other.mPool, nullptr);
        mCapacity = std::exchange(other.mCapacity, 0);
    }
    return *this;
}
void JsonDocument::Reset() noexcept
{
    if (mDocument != nullptr)
    {
        yyjson_doc_free(static_cast<yyjson_doc*>(mDocument));
        mDocument = nullptr;
    }
}
void JsonDocument::Release() noexcept
{
    Reset();
    if (mPool != nullptr)
    {
        mDomain->Free(mPool, mCapacity, POOL_ALIGNMENT);
        mPool = nullptr;
        mCapacity = 0;
    }
}
ParseStatus JsonDocument::Read(std::string_view input, ParseError& error, const JsonLimits& limits) noexcept
{
    Reset();
    error = {};
    if (mCapacity > limits.MaxWorkspaceBytes)
    {
        Release();
    }
    if (input.size() > limits.MaxInputBytes)
    {
        return Limit(error, ParseLimit::InputBytes, { .Budget = limits.MaxInputBytes, .Observed = input.size() });
    }
    if (limits.MaxDepth > MAX_JSON_DEPTH)
    {
        return Limit(error, ParseLimit::NestingDepth, { .Budget = MAX_JSON_DEPTH, .Observed = limits.MaxDepth });
    }
    if (limits.MaxObjectMembers > MAX_JSON_OBJECT_MEMBERS)
    {
        return Limit(error,
                     ParseLimit::ObjectMembers,
                     { .Budget = MAX_JSON_OBJECT_MEMBERS, .Observed = limits.MaxObjectMembers });
    }
    if (input.empty())
    {
        return Fail(error, ParseStatus::InvalidSyntax, 0);
    }
    if (ValidateUtf8(input, error) != ParseStatus::Ok || CheckNesting(input, limits.MaxDepth, error) != ParseStatus::Ok)
    {
        return error.Status;
    }
    const usize size = yyjson_read_max_memory_usage(input.size(), 0);
    if (size == 0 || size > limits.MaxWorkspaceBytes)
    {
        return Limit(error,
                     ParseLimit::WorkspaceBytes,
                     { .Budget = limits.MaxWorkspaceBytes, .Observed = size == 0 ? UNKNOWN_BYTE_OFFSET : size });
    }
    if (mCapacity < size)
    {
        // Old views are already invalid. Free first to keep peak owned storage
        // within the workspace budget even when growing a retained pool.
        Release();
        void* pool = mDomain->TryAllocate(size, POOL_ALIGNMENT);
        if (pool == nullptr)
        {
            return Fail(error, ParseStatus::OutOfMemory);
        }
        mPool = pool;
        mCapacity = size;
    }
    yyjson_alc allocator{};
    if (yyjson_alc_pool_init(&allocator, mPool, mCapacity) == false)
    {
        return Fail(error, ParseStatus::OutOfMemory);
    }
    yyjson_read_err codecError{};
    mDocument = yyjson_read_opts(const_cast<char*>(input.data()), input.size(), 0, &allocator, &codecError);
    if (mDocument == nullptr)
    {
        return Fail(error,
                    codecError.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION ? ParseStatus::OutOfMemory
                                                                           : ParseStatus::InvalidSyntax,
                    codecError.pos);
    }
    usize values = 0;
    if (Check(yyjson_doc_get_root(static_cast<yyjson_doc*>(mDocument)), 0, values, limits, error) != ParseStatus::Ok)
    {
        Reset();
        return error.Status;
    }
    return ParseStatus::Ok;
}
JsonValue JsonDocument::Root() const noexcept
{
    return {mDocument == nullptr ? nullptr : yyjson_doc_get_root(static_cast<yyjson_doc*>(mDocument))};
}

void JsonWriter::Raw(std::string_view value) noexcept
{
    if (mStatus != ParseStatus::Ok)
    {
        return;
    }
    if (mFinished)
    {
        mStatus = ParseStatus::InvalidState;
        return;
    }
    if (value.size() > mOutput.size() - mSize)
    {
        mStatus = ParseStatus::LimitExceeded;
        return;
    }
    for (char ch : value)
    {
        mOutput[mSize++] = static_cast<uint8>(ch);
    }
}
void JsonWriter::String(std::string_view value) noexcept
{
    if (mStatus != ParseStatus::Ok)
    {
        return;
    }
    if (mFinished)
    {
        Raw({});
        return;
    }
    ParseError error;
    if (ValidateUtf8(value, error) != ParseStatus::Ok)
    {
        mStatus = error.Status;
        return;
    }
    Raw("\"");
    usize start = 0;
    for (usize i = 0; i < value.size(); ++i)
    {
        if (mStatus != ParseStatus::Ok)
        {
            return;
        }
        const char ch = value[i];
        if (ch == '"' || ch == '\\' || static_cast<uint8>(ch) < 32)
        {
            Raw(value.substr(start, i - start));
            if (ch == '"' || ch == '\\')
            {
                const char escaped[] = {'\\', ch};
                Raw({escaped, 2});
            }
            else
            {
                constexpr char hex[] = "0123456789abcdef";
                const char escaped[] =
                    {'\\', 'u', '0', '0', hex[static_cast<uint8>(ch) >> 4], hex[static_cast<uint8>(ch) & 15]};
                Raw({escaped, 6});
            }
            start = i + 1;
        }
    }
    Raw(value.substr(start));
    Raw("\"");
}
void JsonWriter::Integer(uint64 value) noexcept
{
    char data[20];
    usize pos = sizeof(data);
    do
    {
        data[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    Raw({data + pos, sizeof(data) - pos});
}
void JsonWriter::Number(float64 value) noexcept
{
    if (mStatus != ParseStatus::Ok)
    {
        return;
    }
    if (mFinished)
    {
        Raw({});
        return;
    }
    yyjson_mut_val number{};
    yyjson_mut_set_real(&number, value);
    usize length = 0;
    yyjson_write_err error{};
    alignas(uint64) uint8 pool[1024]{};
    yyjson_alc allocator{};
    (void)yyjson_alc_pool_init(&allocator, pool, sizeof(pool));
    char* text = yyjson_mut_val_write_opts(&number, 0, &allocator, &length, &error);
    if (text == nullptr)
    {
        mStatus =
            error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION ? ParseStatus::OutOfMemory : ParseStatus::InvalidSyntax;
    }
    else
    {
        Raw({text, length});
    }
}
void JsonWriter::Boolean(bool value) noexcept
{
    Raw(value ? "true" : "false");
}
ParseStatus JsonWriter::Finish(std::span<const uint8>& output) noexcept
{
    if (mFinished == false)
    {
        Raw("\n");
        mFinished = true;
    }
    if (mStatus != ParseStatus::Ok)
    {
        return mStatus;
    }
    output = mOutput.first(mSize);
    return ParseStatus::Ok;
}
} // namespace ludus::foundation::parsing
