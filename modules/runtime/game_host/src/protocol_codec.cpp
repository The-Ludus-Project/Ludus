#include "internal/protocol_codec.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

#include <cstring>

namespace ludus::runtime::game_host::protocol
{
namespace
{
using ludus::foundation::uint8;
/// UTF-16 surrogate ranges in `\\u` escapes: a high surrogate must be followed by a low one.
constexpr uint32 HIGH_SURROGATE_FIRST = 0xD800;
constexpr uint32 HIGH_SURROGATE_LAST = 0xDBFF;
constexpr uint32 LOW_SURROGATE_FIRST = 0xDC00;
constexpr uint32 LOW_SURROGATE_LAST = 0xDFFF;
/// Hexadecimal characters of a 64-bit identifier.
constexpr usize HEX_ID_CHARACTERS = 16;

void AppendEscaped(std::string& out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<uint8>(c) < 0x20)
                {
                    constexpr char digits[] = "0123456789abcdef";
                    const uint8 byte = static_cast<uint8>(c);
                    out += "\\u00";
                    out.push_back(digits[byte >> 4]);
                    out.push_back(digits[byte & 0xFU]);
                }
                else
                {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

// Minimal recursive-free scanner state over a compact JSON object.
struct Scanner
{
    std::string_view text;
    usize pos = 0;

    void SkipWs()
    {
        while (pos < text.size())
        {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++pos;
            }
            else
            {
                break;
            }
        }
    }

    [[nodiscard]] bool AtEnd()
    {
        SkipWs();
        return pos >= text.size();
    }

    [[nodiscard]] bool Expect(char c)
    {
        SkipWs();
        if (pos < text.size() && text[pos] == c)
        {
            ++pos;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool ParseHexQuad(uint32& code)
    {
        if (pos + 4 > text.size())
        {
            return false;
        }
        code = 0;
        for (usize i = 0; i < 4; ++i)
        {
            const char digit = text[pos++];
            code <<= 4;
            if (digit >= '0' && digit <= '9')
            {
                code |= static_cast<uint32>(digit - '0');
            }
            else if (digit >= 'a' && digit <= 'f')
            {
                code |= static_cast<uint32>(digit - 'a' + 10);
            }
            else if (digit >= 'A' && digit <= 'F')
            {
                code |= static_cast<uint32>(digit - 'A' + 10);
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    // Parse a JSON string into out. Rejects nested escapes it does not support.
    [[nodiscard]] bool ParseString(std::string& out)
    {
        SkipWs();
        if (pos >= text.size() || text[pos] != '"')
        {
            return false;
        }
        ++pos;
        out.clear();
        while (pos < text.size())
        {
            const char c = text[pos++];
            if (c == '"')
            {
                return ValidUtf8(out, true);
            }
            if (c == '\\')
            {
                if (pos >= text.size())
                {
                    return false;
                }
                const char e = text[pos++];
                switch (e)
                {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        uint32 code = 0;
                        if (ParseHexQuad(code) == false)
                        {
                            return false;
                        }
                        if (code >= HIGH_SURROGATE_FIRST && code <= HIGH_SURROGATE_LAST)
                        {
                            if (pos + 2 > text.size() || text.substr(pos, 2) != "\\u")
                            {
                                return false;
                            }
                            pos += 2;
                            uint32 low = 0;
                            if (ParseHexQuad(low) == false || low < LOW_SURROGATE_FIRST || low > LOW_SURROGATE_LAST)
                            {
                                return false;
                            }
                            code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                        }
                        else if (code >= 0xDC00 && code <= 0xDFFF)
                        {
                            return false;
                        }
                        if (code < 0x80)
                        {
                            out.push_back(static_cast<char>(code));
                        }
                        else if (code < 0x800)
                        {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        else if (code < 0x10000)
                        {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        else
                        {
                            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default:
                        return false;
                }
            }
            else
            {
                if (static_cast<uint8>(c) < 0x20)
                {
                    return false;
                }
                out.push_back(c);
            }
            if (out.size() > kMaxStringFieldBytes)
            {
                return false;
            }
        }
        return false;
    }

    // Parse a bare token (number or true/false) up to a delimiter.
    [[nodiscard]] bool ParseBare(std::string& out)
    {
        SkipWs();
        out.clear();
        while (pos < text.size())
        {
            const char c = text[pos];
            if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                break;
            }
            out.push_back(c);
            ++pos;
            if (out.size() > 64)
            {
                return false;
            }
        }
        return out.empty() == false;
    }
};

// Exactly one bounded array of flat records; nested objects/arrays are rejected
// before parsing a child, so untrusted input cannot cause recursive descent.
// Parses into outRecords, which the caller owns as staging; allocation failure
// is reported as a parse failure.
[[nodiscard]] bool ParseRecords(Scanner& scanner, ludus::foundation::Array<Message>& outRecords)
{
    outRecords.Clear();
    if (scanner.Expect('[') == false)
    {
        return false;
    }
    if (scanner.Expect(']'))
    {
        return true;
    }
    for (;;)
    {
        scanner.SkipWs();
        const usize start = scanner.pos;
        if (outRecords.GetSize() >= kMaxPropertyBatch || scanner.Expect('{') == false)
        {
            return false;
        }
        bool closed = false;
        while (scanner.pos < scanner.text.size())
        {
            const char c = scanner.text[scanner.pos];
            if (c == '"')
            {
                std::string ignored;
                if (scanner.ParseString(ignored) == false)
                {
                    return false;
                }
                continue;
            }
            ++scanner.pos;
            if (c == '}')
            {
                closed = true;
                break;
            }
            if (c == '{' || c == '[' || c == ']')
            {
                return false;
            }
        }
        Message record;
        if (closed == false || Message::Parse(scanner.text.substr(start, scanner.pos - start), record) == false)
        {
            return false;
        }
        if (outRecords.TryAdd(std::move(record)) == false)
        {
            return false;
        }
        if (scanner.Expect(']'))
        {
            return true;
        }
        if (scanner.Expect(',') == false)
        {
            return false;
        }
    }
}

[[nodiscard]] bool IsNumber(std::string_view token)
{
    if (token.empty())
    {
        return false;
    }
    usize i = 0;
    if (token[0] == '-')
    {
        if (token.size() == 1)
        {
            return false;
        }
        i = 1;
    }
    if (token[i] == '0' && i + 1 != token.size())
    {
        return false;
    }
    for (; i < token.size(); ++i)
    {
        if (token[i] < '0' || token[i] > '9')
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool ValidUtf8(std::string_view text, bool allowNul) noexcept
{
    for (usize i = 0; i < text.size();)
    {
        const uint8 first = static_cast<uint8>(text[i++]);
        if (first == 0 && allowNul == false)
        {
            return false;
        }
        if (first < 0x80)
        {
            continue;
        }
        uint32 code = 0;
        usize count = 0;
        uint32 minimum = 0;
        if (first >= 0xC2 && first <= 0xDF)
        {
            code = first & 0x1FU;
            count = 1;
            minimum = 0x80;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            code = first & 0x0FU;
            count = 2;
            minimum = 0x800;
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            code = first & 7U;
            count = 3;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }
        if (count > text.size() - i)
        {
            return false;
        }
        for (usize j = 0; j < count; ++j)
        {
            const uint8 byte = static_cast<uint8>(text[i++]);
            if ((byte & 0xC0U) != 0x80U)
            {
                return false;
            }
            code = (code << 6) | (byte & 0x3FU);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        {
            return false;
        }
    }
    return true;
}

const Message::Field* Message::Find(std::string_view key) const
{
    for (const Field& f : Fields_)
    {
        if (f.Key == key)
        {
            return &f;
        }
    }
    return nullptr;
}

void Message::SetString(std::string_view key, std::string_view value)
{
    Set(Field{std::string(key), std::string(value), ValueType::String});
}

void Message::SetUint(std::string_view key, uint64 value)
{
    Set(Field{std::string(key), std::to_string(value), ValueType::Number});
}

void Message::SetInt(std::string_view key, int64 value)
{
    Set(Field{std::string(key), std::to_string(value), ValueType::Number});
}

void Message::SetBool(std::string_view key, bool value)
{
    Set(Field{std::string(key), value ? "true" : "false", ValueType::Bool});
}

void Message::SetRecords(std::string_view key, std::span<const Message> records)
{
    std::string value = "[";
    for (const Message& record : records)
    {
        if (value.size() > 1)
        {
            value += ',';
        }
        value += record.Serialize();
    }
    value += ']';
    Set(Field{std::string(key), std::move(value), ValueType::Records});
}

bool Message::GetRecords(std::string_view key, ludus::foundation::Array<Message>& outRecords) const
{
    const Field* field = Find(key);
    if (field == nullptr || field->Type != ValueType::Records)
    {
        return false;
    }
    // Stage so a malformed or unallocatable batch leaves outRecords unchanged.
    ludus::foundation::Array<Message> records;
    Scanner scanner{field->Value, 0};
    if (ParseRecords(scanner, records) == false || scanner.AtEnd() == false)
    {
        return false;
    }
    outRecords.Swap(records);
    return true;
}

bool Message::HasOnly(std::initializer_list<std::string_view> keys) const noexcept
{
    for (const auto& field : Fields_)
    {
        bool found = false;
        for (const auto key : keys)
        {
            found = found || field.Key == key;
        }
        if (found == false)
        {
            return false;
        }
    }
    return true;
}

void Message::SetHexId(std::string_view key, uint64 value)
{
    constexpr char digits[] = "0123456789abcdef";
    char buf[16] = {};
    for (usize i = 0; i < sizeof(buf); ++i)
    {
        buf[sizeof(buf) - i - 1] = digits[value & 0xFU];
        value >>= 4;
    }
    SetString(key, std::string_view(buf, sizeof(buf)));
}

void Message::Set(Field field)
{
    for (auto& existing : Fields_)
    {
        if (existing.Key == field.Key)
        {
            existing = std::move(field);
            return;
        }
    }
    // Setters build caller-owned outgoing messages; like the std::string values
    // they hold, exhausting field storage here is fatal rather than reported.
    Fields_.Add(std::move(field));
}

bool Message::GetString(std::string_view key, std::string& out) const
{
    const Field* f = Find(key);
    if (f == nullptr || f->Type != ValueType::String)
    {
        return false;
    }
    out = f->Value;
    return true;
}

bool Message::GetUint(std::string_view key, uint64& out) const
{
    const Field* f = Find(key);
    if (f == nullptr || f->Type != ValueType::Number)
    {
        return false;
    }
    uint64 value = 0;
    for (const char c : f->Value)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        const uint64 digit = static_cast<uint64>(c - '0');
        if (value > (~uint64{0} - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool Message::GetInt(std::string_view key, int64& out) const
{
    const Field* f = Find(key);
    if (f == nullptr || f->Type != ValueType::Number)
    {
        return false;
    }
    bool negative = false;
    std::string_view digits = f->Value;
    if (digits.empty() == false && digits[0] == '-')
    {
        negative = true;
        digits.remove_prefix(1);
    }
    if (digits.empty())
    {
        return false;
    }
    uint64 value = 0;
    const uint64 limit = negative ? (uint64{1} << 63) : (uint64{1} << 63) - 1;
    for (const char c : digits)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        const uint64 digit = static_cast<uint64>(c - '0');
        if (value > (limit - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    out = negative && value != 0 ? -static_cast<int64>(value - 1) - 1 : static_cast<int64>(value);
    return true;
}

bool Message::GetBool(std::string_view key, bool& out) const
{
    const Field* f = Find(key);
    if (f == nullptr || f->Type != ValueType::Bool)
    {
        return false;
    }
    out = (f->Value == "true");
    return true;
}

bool Message::GetHexId(std::string_view key, uint64& out) const
{
    std::string text;
    if (GetString(key, text) == false || text.size() != HEX_ID_CHARACTERS)
    {
        return false;
    }
    uint64 value = 0;
    for (const char c : text)
    {
        value <<= 4;
        if (c >= '0' && c <= '9')
        {
            value |= static_cast<uint64>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            value |= static_cast<uint64>(c - 'a' + 10);
        }
        else
        {
            return false;
        }
    }
    out = value;
    return true;
}

bool Message::Has(std::string_view key) const
{
    return Find(key) != nullptr;
}

std::string Message::Serialize() const
{
    std::string out;
    out.push_back('{');
    bool first = true;
    for (const Field& f : Fields_)
    {
        if (first == false)
        {
            out.push_back(',');
        }
        first = false;
        AppendEscaped(out, f.Key);
        out.push_back(':');
        if (f.Type == ValueType::String)
        {
            AppendEscaped(out, f.Value);
        }
        else
        {
            out += f.Value;
        }
    }
    out.push_back('}');
    return out;
}

bool Message::Parse(std::string_view json, Message& out)
{
    out.Fields_.Clear();
    if (json.size() > kMaxControlFrameBytes)
    {
        return false;
    }
    Scanner s{json, 0};
    if (s.Expect('{') == false)
    {
        return false;
    }
    s.SkipWs();
    if (s.pos < s.text.size() && s.text[s.pos] == '}')
    {
        ++s.pos;
        return s.AtEnd();
    }
    for (;;)
    {
        if (out.Fields_.GetSize() >= kMaxFields)
        {
            return false;
        }
        std::string key;
        if (s.ParseString(key) == false)
        {
            return false;
        }
        if (out.Has(key))
        {
            return false;
        }
        if (s.Expect(':') == false)
        {
            return false;
        }
        s.SkipWs();
        if (s.pos >= s.text.size())
        {
            return false;
        }
        Field field;
        field.Key = key;
        if (s.text[s.pos] == '"')
        {
            if (s.ParseString(field.Value) == false)
            {
                return false;
            }
            field.Type = ValueType::String;
        }
        else if (s.text[s.pos] == '[')
        {
            const usize start = s.pos;
            ludus::foundation::Array<Message> records;
            if (ParseRecords(s, records) == false)
            {
                return false;
            }
            field.Type = ValueType::Records;
            field.Value = s.text.substr(start, s.pos - start);
        }
        else
        {
            std::string token;
            if (s.ParseBare(token) == false)
            {
                return false;
            }
            if (token == "true" || token == "false")
            {
                field.Type = ValueType::Bool;
                field.Value = token;
            }
            else if (IsNumber(token))
            {
                field.Type = ValueType::Number;
                field.Value = token;
            }
            else
            {
                return false; // null / nested / unsupported
            }
        }
        if (out.Fields_.TryAdd(std::move(field)) == false)
        {
            return false;
        }
        if (s.Expect(','))
        {
            continue;
        }
        if (s.Expect('}'))
        {
            break;
        }
        return false;
    }
    return s.AtEnd();
}

// Thanks to Jason Hughes, "What to Look for When Evaluating Middleware for
// Integration", Game Engine Gems, section 1.13 "Platform Portability", p. 12:
// the stream endian audit informs this explicit, unchanged little-endian format.
// Original implementation; review: docs/architecture/primitive-types.md.
bool EncodeFrame(std::string_view payload, ludus::foundation::Array<uint8>& outFrame)
{
    if (payload.size() > kMaxControlFrameBytes)
    {
        return false;
    }
    uint32 length{};
    usize frameSize{};
    if (ludus::foundation::TryIntegerCast(payload.size(), length) == false ||
        ludus::foundation::TryAdd(payload.size(), sizeof(length), frameSize) == false)
    {
        return false;
    }
    // Stage the frame so allocation failure leaves outFrame unchanged.
    ludus::foundation::Array<uint8> frame;
    if (frame.TryEnsureCapacity(frameSize) == false)
    {
        return false;
    }
    frame.Resize(sizeof(length)); // within the reserved capacity; cannot allocate
    const bool written = ludus::foundation::TryWriteLittleEndian(length, frame);
    LUDUS_ASSERT(written);
    (void)written;
    const uint8* bytes = reinterpret_cast<const uint8*>(payload.data());
    frame.AddRange(std::span<const uint8>(bytes, payload.size())); // within the reserved capacity
    outFrame.Swap(frame);
    return true;
}

void FrameReader::Append(const uint8* data, usize size)
{
    if (Failed_ || data == nullptr)
    {
        return;
    }
    usize queuedSize{};
    if (ludus::foundation::TryAdd(Buffer_.GetSize(), size, queuedSize) == false || queuedSize > kMaxCommandQueueBytes)
    {
        Failed_ = true;
        return;
    }
    if (Buffer_.TryAddRange(std::span<const uint8>(data, size)) == false)
    {
        Failed_ = true;
    }
}

bool FrameReader::Next(std::string& outPayload)
{
    if (Failed_ || Buffer_.GetSize() < 4)
    {
        return false;
    }
    uint32 length{};
    const bool read = ludus::foundation::TryReadLittleEndian(Buffer_, length);
    LUDUS_ASSERT(read);
    (void)read;
    if (length > kMaxControlFrameBytes)
    {
        Failed_ = true;
        return false;
    }
    if (Buffer_.GetSize() < 4 + static_cast<usize>(length))
    {
        return false; // incomplete; wait for more bytes
    }
    outPayload.assign(reinterpret_cast<const char*>(Buffer_.GetData() + 4), length);
    Buffer_.RemoveRange(0, 4 + static_cast<usize>(length));
    return true;
}
} // namespace ludus::runtime::game_host::protocol
