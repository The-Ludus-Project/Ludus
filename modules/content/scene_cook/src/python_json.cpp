#include "python_json.h"

#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <cstring>
#include <limits>

namespace ludus::content::scene_cook::detail
{
namespace
{
namespace math = ludus::foundation::math;
constexpr float64 INFINITE_FLOAT = std::numeric_limits<float64>::infinity();
constexpr usize MAX_JSON_NESTING = 900;
constexpr float64 TWO_POW_127 = 170141183460469231731687303715884105728.0;

[[nodiscard]] PythonNumber MakeInteger(foundation::Int128 value) noexcept
{
    PythonNumber number;
    number.Integer = value;
    return number;
}

[[nodiscard]] PythonNumber MakeFloat(float64 value) noexcept
{
    PythonNumber number;
    number.IsFloat = true;
    number.Float = value;
    return number;
}

/// Exact Python comparison of an int with a float: -1, 0 or 1; 2 when unordered (NaN).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] int CompareIntegerFloat(foundation::Int128 integer, float64 value) noexcept
{
    if (math::IsNaN(value))
    {
        return 2;
    }
    if (math::IsFinite(value) == false)
    {
        return value > 0 ? -1 : 1;
    }
    const float64 floor = math::Floor(value);
    if (floor >= TWO_POW_127)
    {
        return -1;
    }
    if (floor < -TWO_POW_127)
    {
        return 1;
    }
    // floor is integral and inside [-2^127, 2^127), so truncation is exact.
    foundation::Int128 truncated;
    (void)foundation::TryTruncateToInt128(floor, truncated);
    if (integer < truncated)
    {
        return -1;
    }
    if (integer > truncated)
    {
        return 1;
    }
    return value > floor ? -1 : 0;
}

/// Python comparison: -1, 0, 1, or 2 when unordered.
[[nodiscard]] int ComparePython(PythonNumber left, PythonNumber right) noexcept
{
    if (left.IsFloat == false && right.IsFloat == false)
    {
        return left.Integer < right.Integer ? -1 : (left.Integer > right.Integer ? 1 : 0);
    }
    if (left.IsFloat == true && right.IsFloat == true)
    {
        if (math::IsNaN(left.Float) || math::IsNaN(right.Float))
        {
            return 2;
        }
        return left.Float < right.Float ? -1 : (left.Float > right.Float ? 1 : 0);
    }
    if (left.IsFloat == false)
    {
        return CompareIntegerFloat(left.Integer, right.Float);
    }
    const int reversed = CompareIntegerFloat(right.Integer, left.Float);
    return reversed == 2 ? 2 : -reversed;
}

[[nodiscard]] bool IsAsciiHexDigit(char value) noexcept
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

[[nodiscard]] uint32 GetHexValue(char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return static_cast<uint32>(value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return static_cast<uint32>(value - 'a' + 10);
    }
    return static_cast<uint32>(value - 'A' + 10);
}

/// Append one code point as UTF-8; surrogates use the generalised (WTF-8) form.
void AppendCodePoint(Array<char>& inoutText, uint32 codePoint, bool& inoutIsOk) noexcept
{
    char bytes[4]{};
    usize count = 0;
    if (codePoint < 0x80)
    {
        bytes[count++] = static_cast<char>(codePoint);
    }
    else if (codePoint < 0x800)
    {
        bytes[count++] = static_cast<char>(0xc0 | (codePoint >> 6));
        bytes[count++] = static_cast<char>(0x80 | (codePoint & 0x3f));
    }
    else if (codePoint < 0x10000)
    {
        bytes[count++] = static_cast<char>(0xe0 | (codePoint >> 12));
        bytes[count++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        bytes[count++] = static_cast<char>(0x80 | (codePoint & 0x3f));
    }
    else
    {
        bytes[count++] = static_cast<char>(0xf0 | (codePoint >> 18));
        bytes[count++] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
        bytes[count++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        bytes[count++] = static_cast<char>(0x80 | (codePoint & 0x3f));
    }
    for (usize i = 0; i < count; ++i)
    {
        if (inoutText.TryAdd(bytes[i]) == false)
        {
            inoutIsOk = false;
        }
    }
}

/// Decode one UTF-8 (surrogatepass) code point; false on malformed input.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool DecodeUtf8(std::string_view text, usize& inoutOffset, uint32& outCodePoint) noexcept
{
    const uint8 lead = static_cast<uint8>(text[inoutOffset]);
    usize count = 0;
    uint32 value = 0;
    uint32 minimum = 0;
    if (lead < 0x80)
    {
        outCodePoint = lead;
        ++inoutOffset;
        return true;
    }
    if (lead >= 0xc2 && lead <= 0xdf)
    {
        count = 1;
        value = lead & 0x1fu;
        minimum = 0x80;
    }
    else if (lead >= 0xe0 && lead <= 0xef)
    {
        count = 2;
        value = lead & 0x0fu;
        minimum = 0x800;
    }
    else if (lead >= 0xf0 && lead <= 0xf4)
    {
        count = 3;
        value = lead & 0x07u;
        minimum = 0x10000;
    }
    else
    {
        return false;
    }
    if (inoutOffset + count >= text.size())
    {
        return false;
    }
    for (usize i = 1; i <= count; ++i)
    {
        const uint8 next = static_cast<uint8>(text[inoutOffset + i]);
        if ((next & 0xc0u) != 0x80u)
        {
            return false;
        }
        value = (value << 6) | (next & 0x3fu);
    }
    if (value < minimum || value > 0x10ffff)
    {
        return false;
    }
    outCodePoint = value;
    inoutOffset += count + 1;
    return true;
}

/// Member count from which parseObject finds duplicate keys through a hash index instead of a scan. A catalog's
/// artifact table has one member per artifact, so scanning made parsing it quadratic in the artifact count.
constexpr usize MIN_INDEXED_OBJECT_MEMBERS = 16;
/// Index slots per member after a rebuild; the index is rebuilt before it becomes more than half full.
constexpr usize MEMBER_INDEX_SLOTS_PER_MEMBER = 4;

/// Open-addressing index of the member keys of the object being parsed. Each slot holds a member index plus one;
/// zero marks an empty slot. Objects with fewer than MIN_INDEXED_OBJECT_MEMBERS members are scanned instead.
class MemberKeyIndex final
{
public:
    /// Whether object already has a member named key. @return false when the index cannot allocate.
    [[nodiscard]] bool
    TryFind(const JsonTree& tree, JsonIndex object, std::string_view key, bool& outIsPresent) noexcept
    {
        const usize count = tree.GetCount(object);
        if (count < MIN_INDEXED_OBJECT_MEMBERS)
        {
            outIsPresent = tree.GetMember(object, key) != NO_JSON_NODE;
            return true;
        }
        if (mSlots.IsEmpty() == true && rebuild(tree, object, count) == false)
        {
            return false;
        }
        outIsPresent = mSlots[findSlot(tree, object, key)] != 0;
        return true;
    }

    /// Index the member just appended at position member. @return false when the index cannot allocate.
    [[nodiscard]] bool TryAdd(const JsonTree& tree, JsonIndex object, usize member) noexcept
    {
        const usize count = member + 1;
        if (count < MIN_INDEXED_OBJECT_MEMBERS)
        {
            return true;
        }
        if (count * 2 > mSlots.GetSize())
        {
            return rebuild(tree, object, count);
        }
        mSlots[findSlot(tree, object, tree.GetKey(object, member))] = count;
        return true;
    }

private:
    [[nodiscard]] static uint64 hashKey(std::string_view key) noexcept
    {
        return foundation::TableHash64(std::span<const uint8>(reinterpret_cast<const uint8*>(key.data()), key.size()));
    }

    /// The slot holding key, or the empty slot where it belongs.
    [[nodiscard]] usize findSlot(const JsonTree& tree, JsonIndex object, std::string_view key) const noexcept
    {
        const usize mask = mSlots.GetSize() - 1;
        usize slot = static_cast<usize>(hashKey(key)) & mask;
        while (mSlots[slot] != 0 && tree.GetKey(object, mSlots[slot] - 1) != key)
        {
            slot = (slot + 1) & mask;
        }
        return slot;
    }

    /// Index the first count members into a power-of-two table at most a quarter full.
    [[nodiscard]] bool rebuild(const JsonTree& tree, JsonIndex object, usize count) noexcept
    {
        usize size = 1;
        while (size < count * MEMBER_INDEX_SLOTS_PER_MEMBER)
        {
            size *= 2;
        }
        mSlots.Clear();
        if (mSlots.TryResize(size, usize{0}) == false)
        {
            return false;
        }
        for (usize member = 0; member < count; ++member)
        {
            mSlots[findSlot(tree, object, tree.GetKey(object, member))] = member + 1;
        }
        return true;
    }

    Array<usize> mSlots;
};

class Parser final
{
public:
    Parser(std::string_view text, JsonTree& inoutTree, MessageText& outMessage) noexcept
        : mText(text), mTree(inoutTree), mMessage(outMessage)
    {
    }

    [[nodiscard]] bool ParseDocument(JsonIndex& outRoot) noexcept
    {
        skipSpace();
        if (parseValue(outRoot, 0) == false)
        {
            return false;
        }
        skipSpace();
        if (mOffset != mText.size())
        {
            return fail("Extra data");
        }
        if (mTree.IsOk() == false || mIsOk == false)
        {
            return fail("out of memory");
        }
        return true;
    }

private:
    [[nodiscard]] bool fail(std::string_view message) noexcept
    {
        char text[MAX_MESSAGE_BYTES + 1]{};
        usize at = 0;
        for (const char value : message)
        {
            if (at < MAX_MESSAGE_BYTES)
            {
                text[at++] = value;
            }
        }
        // Python reports the character offset; the byte offset is close enough for diagnostics.
        char suffix[48]{};
        Array<uint8> digits;
        ByteWriter writer(digits);
        writer.Append(": char ");
        AppendInteger(writer, static_cast<int64>(mOffset));
        for (usize i = 0; i < digits.GetSize() && i < sizeof(suffix) - 1; ++i)
        {
            suffix[i] = static_cast<char>(digits[i]);
        }
        for (usize i = 0; suffix[i] != 0; ++i)
        {
            if (at < MAX_MESSAGE_BYTES)
            {
                text[at++] = suffix[i];
            }
        }
        (void)mMessage.Set(std::string_view(text, at));
        return false;
    }

    void skipSpace() noexcept
    {
        while (mOffset < mText.size() &&
               (mText[mOffset] == ' ' || mText[mOffset] == '\t' || mText[mOffset] == '\n' || mText[mOffset] == '\r'))
        {
            ++mOffset;
        }
    }

    [[nodiscard]] bool startsWith(std::string_view token) const noexcept
    {
        return mText.substr(mOffset, token.size()) == token;
    }

    [[nodiscard]] bool parseValue(JsonIndex& outNode, usize depth) noexcept
    {
        if (depth > MAX_JSON_NESTING)
        {
            return fail("maximum recursion depth exceeded");
        }
        if (mOffset >= mText.size())
        {
            return fail("Expecting value");
        }
        const char lead = mText[mOffset];
        if (lead == '"')
        {
            Array<char> text;
            if (parseString(text) == false)
            {
                return false;
            }
            outNode = mTree.AddString(std::string_view(text.GetData(), text.GetSize()));
            return true;
        }
        if (lead == '{')
        {
            return parseObject(outNode, depth);
        }
        if (lead == '[')
        {
            return parseArray(outNode, depth);
        }
        if (startsWith("null"))
        {
            mOffset += 4;
            outNode = mTree.AddNull();
            return true;
        }
        if (startsWith("true"))
        {
            mOffset += 4;
            outNode = mTree.AddBoolean(true);
            return true;
        }
        if (startsWith("false"))
        {
            mOffset += 5;
            outNode = mTree.AddBoolean(false);
            return true;
        }
        if (startsWith("NaN"))
        {
            mOffset += 3;
            outNode = mTree.AddFloat(std::numeric_limits<float64>::quiet_NaN());
            return true;
        }
        if (startsWith("Infinity"))
        {
            mOffset += 8;
            outNode = mTree.AddFloat(INFINITE_FLOAT);
            return true;
        }
        if (startsWith("-Infinity"))
        {
            mOffset += 9;
            outNode = mTree.AddFloat(-INFINITE_FLOAT);
            return true;
        }
        return parseNumber(outNode);
    }

    [[nodiscard]] bool isDigit(usize offset) const noexcept
    {
        return offset < mText.size() && mText[offset] >= '0' && mText[offset] <= '9';
    }

    [[nodiscard]] bool parseNumber(JsonIndex& outNode) noexcept
    {
        const usize start = mOffset;
        usize at = mOffset;
        if (at < mText.size() && mText[at] == '-')
        {
            ++at;
        }
        if (isDigit(at) == false)
        {
            return fail("Expecting value");
        }
        if (mText[at] == '0')
        {
            ++at;
        }
        else
        {
            while (isDigit(at))
            {
                ++at;
            }
        }
        bool isFloat = false;
        if (at < mText.size() && mText[at] == '.' && isDigit(at + 1))
        {
            isFloat = true;
            at += 1;
            while (isDigit(at))
            {
                ++at;
            }
        }
        if (at < mText.size() && (mText[at] == 'e' || mText[at] == 'E'))
        {
            usize exponent = at + 1;
            if (exponent < mText.size() && (mText[exponent] == '+' || mText[exponent] == '-'))
            {
                ++exponent;
            }
            if (isDigit(exponent))
            {
                isFloat = true;
                at = exponent;
                while (isDigit(at))
                {
                    ++at;
                }
            }
        }
        const std::string_view token = mText.substr(start, at - start);
        mOffset = at;
        if (isFloat)
        {
            Array<float64> values;
            const std::string_view tokens[1] = {token};
            const NumberResult result = ParseFloats(tokens, values);
            if (result == NumberResult::OutOfMemory)
            {
                return fail("out of memory");
            }
            if (result == NumberResult::Syntax)
            {
                return fail("Expecting value");
            }
            if (result == NumberResult::Nonfinite)
            {
                outNode = mTree.AddFloat(token[0] == '-' ? -INFINITE_FLOAT : INFINITE_FLOAT);
                return true;
            }
            outNode = mTree.AddFloat(values[0]);
            return true;
        }
        int64 integer = 0;
        if (ParseInt(token, integer) == false)
        {
            return fail("Integer outside the supported 64-bit range");
        }
        outNode = mTree.AddInteger(integer);
        return true;
    }

    [[nodiscard]] bool parseHexEscape(uint32& outValue) noexcept
    {
        if (mOffset + 4 > mText.size())
        {
            return fail("Invalid \\uXXXX escape");
        }
        uint32 value = 0;
        for (usize i = 0; i < 4; ++i)
        {
            const char digit = mText[mOffset + i];
            if (IsAsciiHexDigit(digit) == false)
            {
                return fail("Invalid \\uXXXX escape");
            }
            value = (value << 4) | GetHexValue(digit);
        }
        mOffset += 4;
        outValue = value;
        return true;
    }

    [[nodiscard]] bool parseString(Array<char>& outText) noexcept
    {
        ++mOffset;
        while (true)
        {
            if (mOffset >= mText.size())
            {
                return fail("Unterminated string starting at");
            }
            const uint8 value = static_cast<uint8>(mText[mOffset]);
            if (value == '"')
            {
                ++mOffset;
                return true;
            }
            if (value < 0x20)
            {
                return fail("Invalid control character at");
            }
            if (value != '\\')
            {
                if (outText.TryAdd(static_cast<char>(value)) == false)
                {
                    mIsOk = false;
                }
                ++mOffset;
                continue;
            }
            ++mOffset;
            if (mOffset >= mText.size())
            {
                return fail("Unterminated string starting at");
            }
            const char escape = mText[mOffset++];
            char simple = 0;
            switch (escape)
            {
                case '"':
                    simple = '"';
                    break;
                case '\\':
                    simple = '\\';
                    break;
                case '/':
                    simple = '/';
                    break;
                case 'b':
                    simple = '\b';
                    break;
                case 'f':
                    simple = '\f';
                    break;
                case 'n':
                    simple = '\n';
                    break;
                case 'r':
                    simple = '\r';
                    break;
                case 't':
                    simple = '\t';
                    break;
                case 'u':
                    break;
                default:
                    return fail("Invalid \\escape");
            }
            if (simple != 0)
            {
                if (outText.TryAdd(simple) == false)
                {
                    mIsOk = false;
                }
                continue;
            }
            uint32 codePoint = 0;
            if (parseHexEscape(codePoint) == false)
            {
                return false;
            }
            if (codePoint >= 0xd800 && codePoint <= 0xdbff && startsWith("\\u"))
            {
                const usize saved = mOffset;
                mOffset += 2;
                uint32 low = 0;
                if (parseHexEscape(low) == false)
                {
                    return false;
                }
                if (low >= 0xdc00 && low <= 0xdfff)
                {
                    codePoint = 0x10000 + (((codePoint - 0xd800) << 10) | (low - 0xdc00));
                }
                else
                {
                    mOffset = saved;
                }
            }
            AppendCodePoint(outText, codePoint, mIsOk);
        }
    }

    [[nodiscard]] bool parseObject(JsonIndex& outNode, usize depth) noexcept
    {
        ++mOffset;
        const JsonIndex object = mTree.AddObject();
        outNode = object;
        skipSpace();
        if (mOffset < mText.size() && mText[mOffset] == '}')
        {
            ++mOffset;
            return true;
        }
        MemberKeyIndex keys;
        while (true)
        {
            if (mOffset >= mText.size() || mText[mOffset] != '"')
            {
                return fail("Expecting property name enclosed in double quotes");
            }
            Array<char> key;
            if (parseString(key) == false)
            {
                return false;
            }
            const std::string_view keyView(key.GetData(), key.GetSize());
            skipSpace();
            if (mOffset >= mText.size() || mText[mOffset] != ':')
            {
                return fail("Expecting ':' delimiter");
            }
            ++mOffset;
            skipSpace();
            JsonIndex value = NO_JSON_NODE;
            if (parseValue(value, depth + 1) == false)
            {
                return false;
            }
            if (mTree.IsOk() == false)
            {
                return fail("out of memory");
            }
            bool isDuplicate = false;
            if (keys.TryFind(mTree, object, keyView, isDuplicate) == false)
            {
                return fail("out of memory");
            }
            if (isDuplicate == true)
            {
                char text[MAX_MESSAGE_BYTES + 1]{};
                constexpr std::string_view PREFIX = "Duplicate JSON field: ";
                usize at = 0;
                for (const std::string_view part : {PREFIX, keyView})
                {
                    for (const char item : part)
                    {
                        if (at < MAX_MESSAGE_BYTES)
                        {
                            text[at++] = item;
                        }
                    }
                }
                (void)mMessage.Set(std::string_view(text, at));
                return false;
            }
            const usize member = mTree.GetCount(object);
            mTree.AppendMember(object, keyView, value);
            if (mTree.IsOk() == true && keys.TryAdd(mTree, object, member) == false)
            {
                return fail("out of memory");
            }
            skipSpace();
            if (mOffset < mText.size() && mText[mOffset] == '}')
            {
                ++mOffset;
                return true;
            }
            if (mOffset >= mText.size() || mText[mOffset] != ',')
            {
                return fail("Expecting ',' delimiter");
            }
            ++mOffset;
            skipSpace();
        }
    }

    [[nodiscard]] bool parseArray(JsonIndex& outNode, usize depth) noexcept
    {
        ++mOffset;
        const JsonIndex array = mTree.AddArray();
        outNode = array;
        skipSpace();
        if (mOffset < mText.size() && mText[mOffset] == ']')
        {
            ++mOffset;
            return true;
        }
        while (true)
        {
            JsonIndex value = NO_JSON_NODE;
            if (parseValue(value, depth + 1) == false)
            {
                return false;
            }
            if (mTree.IsOk() == false)
            {
                return fail("out of memory");
            }
            mTree.Append(array, value);
            skipSpace();
            if (mOffset < mText.size() && mText[mOffset] == ']')
            {
                ++mOffset;
                return true;
            }
            if (mOffset >= mText.size() || mText[mOffset] != ',')
            {
                return fail("Expecting ',' delimiter");
            }
            ++mOffset;
            skipSpace();
        }
    }

    std::string_view mText;
    JsonTree& mTree;
    MessageText& mMessage;
    usize mOffset = 0;
    bool mIsOk = true;
};

void AppendEscapedString(ByteWriter& inoutWriter, std::string_view text, bool isAsciiOnly) noexcept
{
    static constexpr char HEX[] = "0123456789abcdef";
    inoutWriter.Append('"');
    usize offset = 0;
    while (offset < text.size())
    {
        uint32 codePoint = 0;
        const usize start = offset;
        if (DecodeUtf8(text, offset, codePoint) == false)
        {
            codePoint = static_cast<uint8>(text[offset]);
            ++offset;
        }
        switch (codePoint)
        {
            case '"':
                inoutWriter.Append("\\\"");
                continue;
            case '\\':
                inoutWriter.Append("\\\\");
                continue;
            case '\n':
                inoutWriter.Append("\\n");
                continue;
            case '\r':
                inoutWriter.Append("\\r");
                continue;
            case '\t':
                inoutWriter.Append("\\t");
                continue;
            case '\b':
                inoutWriter.Append("\\b");
                continue;
            case '\f':
                inoutWriter.Append("\\f");
                continue;
            default:
                break;
        }
        const bool isControl = codePoint < 0x20;
        if (isControl == false && (codePoint < 0x80 || isAsciiOnly == false))
        {
            inoutWriter.Append(text.substr(start, offset - start));
            continue;
        }
        const auto appendUnit = [&](uint32 unit) {
            char escape[6] =
                {'\\', 'u', HEX[(unit >> 12) & 0xf], HEX[(unit >> 8) & 0xf], HEX[(unit >> 4) & 0xf], HEX[unit & 0xf]};
            inoutWriter.Append(std::string_view(escape, 6));
        };
        if (codePoint >= 0x10000)
        {
            const uint32 value = codePoint - 0x10000;
            appendUnit(0xd800 | (value >> 10));
            appendUnit(0xdc00 | (value & 0x3ff));
        }
        else
        {
            appendUnit(codePoint);
        }
    }
    inoutWriter.Append('"');
}

class Writer final
{
public:
    Writer(const JsonTree& tree, const JsonStyle& style, ByteWriter& inoutWriter) noexcept
        : mTree(tree), mStyle(style), mWriter(inoutWriter)
    {
    }

    [[nodiscard]] bool Write(JsonIndex node, usize depth) noexcept
    {
        const JsonNode& value = mTree.GetNode(node);
        switch (value.Kind)
        {
            case JsonKind::Null:
                mWriter.Append("null");
                return true;
            case JsonKind::Boolean:
                mWriter.Append(value.IsTrue ? "true" : "false");
                return true;
            case JsonKind::Integer:
                AppendInteger(mWriter, value.Integer);
                return true;
            case JsonKind::Float:
                if (math::IsFinite(value.Float) == false)
                {
                    return false;
                }
                AppendFloatRepr(mWriter, value.Float);
                return true;
            case JsonKind::String:
                AppendEscapedString(mWriter, mTree.GetText(node), mStyle.IsAsciiOnly);
                return true;
            case JsonKind::Array:
                return writeArray(node, depth);
            case JsonKind::Object:
                return writeObject(node, depth);
        }
        return false;
    }

private:
    void writeBreak(usize depth) noexcept
    {
        mWriter.Append('\n');
        for (usize i = 0; i < depth * mStyle.Indent; ++i)
        {
            mWriter.Append(' ');
        }
    }

    [[nodiscard]] bool writeArray(JsonIndex node, usize depth) noexcept
    {
        const usize count = mTree.GetCount(node);
        if (count == 0)
        {
            mWriter.Append("[]");
            return true;
        }
        mWriter.Append('[');
        for (usize i = 0; i < count; ++i)
        {
            if (i != 0)
            {
                mWriter.Append(mStyle.Indent != 0 ? std::string_view(",") : mStyle.ItemSeparator);
            }
            if (mStyle.Indent != 0)
            {
                writeBreak(depth + 1);
            }
            if (Write(mTree.GetItem(node, i), depth + 1) == false)
            {
                return false;
            }
        }
        if (mStyle.Indent != 0)
        {
            writeBreak(depth);
        }
        mWriter.Append(']');
        return true;
    }

    [[nodiscard]] bool writeObject(JsonIndex node, usize depth) noexcept
    {
        const usize count = mTree.GetCount(node);
        if (count == 0)
        {
            mWriter.Append("{}");
            return true;
        }
        Array<usize> order;
        if (order.TryResize(count) == false)
        {
            return false;
        }
        for (usize i = 0; i < count; ++i)
        {
            order[i] = i;
        }
        if (mStyle.IsSorted == true)
        {
            // Insertion sort keeps this dependency-free; catalogs hold at most 4096 members.
            for (usize i = 1; i < count; ++i)
            {
                const usize item = order[i];
                usize at = i;
                while (at > 0 && mTree.GetKey(node, item) < mTree.GetKey(node, order[at - 1]))
                {
                    order[at] = order[at - 1];
                    --at;
                }
                order[at] = item;
            }
        }
        mWriter.Append('{');
        for (usize i = 0; i < count; ++i)
        {
            if (i != 0)
            {
                mWriter.Append(mStyle.Indent != 0 ? std::string_view(",") : mStyle.ItemSeparator);
            }
            if (mStyle.Indent != 0)
            {
                writeBreak(depth + 1);
            }
            AppendEscapedString(mWriter, mTree.GetKey(node, order[i]), mStyle.IsAsciiOnly);
            mWriter.Append(mStyle.KeySeparator);
            if (Write(mTree.GetValue(node, order[i]), depth + 1) == false)
            {
                return false;
            }
        }
        if (mStyle.Indent != 0)
        {
            writeBreak(depth);
        }
        mWriter.Append('}');
        return true;
    }

    const JsonTree& mTree;
    const JsonStyle& mStyle;
    ByteWriter& mWriter;
};
} // namespace

JsonIndex JsonTree::addNode(JsonKind kind) noexcept
{
    JsonNode node;
    node.Kind = kind;
    if (mNodes.TryAdd(static_cast<JsonNode&&>(node)) == false)
    {
        mIsOk = false;
        return 0;
    }
    return mNodes.GetSize() - 1;
}

JsonIndex JsonTree::AddNull() noexcept
{
    return addNode(JsonKind::Null);
}

JsonIndex JsonTree::AddBoolean(bool isTrue) noexcept
{
    const JsonIndex node = addNode(JsonKind::Boolean);
    if (mIsOk == true)
    {
        mNodes[node].IsTrue = isTrue;
    }
    return node;
}

JsonIndex JsonTree::AddInteger(int64 value) noexcept
{
    const JsonIndex node = addNode(JsonKind::Integer);
    if (mIsOk == true)
    {
        mNodes[node].Integer = value;
    }
    return node;
}

JsonIndex JsonTree::AddFloat(float64 value) noexcept
{
    const JsonIndex node = addNode(JsonKind::Float);
    if (mIsOk == true)
    {
        mNodes[node].Float = value;
    }
    return node;
}

JsonIndex JsonTree::AddString(std::string_view value) noexcept
{
    const JsonIndex node = addNode(JsonKind::String);
    if (mIsOk == false)
    {
        return node;
    }
    const usize offset = mText.GetSize();
    if (mText.TryAddRange(std::span<const char>(value.data(), value.size())) == false)
    {
        mIsOk = false;
        return node;
    }
    mNodes[node].TextOffset = offset;
    mNodes[node].TextSize = value.size();
    return node;
}

JsonIndex JsonTree::AddArray() noexcept
{
    return addNode(JsonKind::Array);
}

JsonIndex JsonTree::AddObject() noexcept
{
    return addNode(JsonKind::Object);
}

void JsonTree::Append(JsonIndex array, JsonIndex value) noexcept
{
    if (mIsOk == false)
    {
        return;
    }
    if (mNodes[array].Children.TryAdd(value) == false)
    {
        mIsOk = false;
    }
}

void JsonTree::SetMember(JsonIndex object, std::string_view key, JsonIndex value) noexcept
{
    if (mIsOk == false)
    {
        return;
    }
    const usize count = GetCount(object);
    for (usize i = 0; i < count; ++i)
    {
        if (GetKey(object, i) == key)
        {
            mNodes[object].Children[i * 2 + 1] = value;
            return;
        }
    }
    const JsonIndex keyNode = AddString(key);
    if (mIsOk == false || mNodes[object].Children.TryAdd(keyNode) == false ||
        mNodes[object].Children.TryAdd(value) == false)
    {
        mIsOk = false;
    }
}

void JsonTree::AppendMember(JsonIndex object, std::string_view key, JsonIndex value) noexcept
{
    if (mIsOk == false)
    {
        return;
    }
    const JsonIndex keyNode = AddString(key);
    if (mIsOk == false || mNodes[object].Children.TryAdd(keyNode) == false ||
        mNodes[object].Children.TryAdd(value) == false)
    {
        mIsOk = false;
    }
}

JsonIndex JsonTree::CopyFrom(const JsonTree& source, JsonIndex node) noexcept
{
    const JsonNode& value = source.GetNode(node);
    switch (value.Kind)
    {
        case JsonKind::Null:
            return AddNull();
        case JsonKind::Boolean:
            return AddBoolean(value.IsTrue);
        case JsonKind::Integer:
            return AddInteger(value.Integer);
        case JsonKind::Float:
            return AddFloat(value.Float);
        case JsonKind::String: {
            // Copy through a local buffer: the source may be this tree, whose text may reallocate.
            Array<char> text;
            const std::string_view view = source.GetText(node);
            if (text.TryAddRange(std::span<const char>(view.data(), view.size())) == false)
            {
                mIsOk = false;
                return 0;
            }
            return AddString(std::string_view(text.GetData(), text.GetSize()));
        }
        case JsonKind::Array: {
            const JsonIndex array = AddArray();
            const usize count = source.GetCount(node);
            for (usize i = 0; i < count && mIsOk == true; ++i)
            {
                const JsonIndex item = CopyFrom(source, source.GetItem(node, i));
                Append(array, item);
            }
            return array;
        }
        case JsonKind::Object: {
            const JsonIndex object = AddObject();
            const usize count = source.GetCount(node);
            for (usize i = 0; i < count && mIsOk == true; ++i)
            {
                const JsonIndex key = CopyFrom(source, source.GetNode(node).Children[i * 2]);
                const JsonIndex item = CopyFrom(source, source.GetValue(node, i));
                if (mIsOk == true &&
                    (mNodes[object].Children.TryAdd(key) == false || mNodes[object].Children.TryAdd(item) == false))
                {
                    mIsOk = false;
                }
            }
            return object;
        }
    }
    return 0;
}

std::string_view JsonTree::GetText(JsonIndex node) const noexcept
{
    const JsonNode& value = mNodes[node];
    if (value.TextSize == 0)
    {
        return {};
    }
    return std::string_view(mText.GetData() + value.TextOffset, value.TextSize);
}

usize JsonTree::GetCount(JsonIndex node) const noexcept
{
    const JsonNode& value = mNodes[node];
    return value.Kind == JsonKind::Object ? value.Children.GetSize() / 2 : value.Children.GetSize();
}

JsonIndex JsonTree::GetMember(JsonIndex object, std::string_view key) const noexcept
{
    if (object == NO_JSON_NODE || mNodes[object].Kind != JsonKind::Object)
    {
        return NO_JSON_NODE;
    }
    const usize count = GetCount(object);
    for (usize i = 0; i < count; ++i)
    {
        if (GetKey(object, i) == key)
        {
            return GetValue(object, i);
        }
    }
    return NO_JSON_NODE;
}

bool JsonTree::HasExactKeys(JsonIndex object, std::span<const std::string_view> keys) const noexcept
{
    if (object == NO_JSON_NODE || mNodes[object].Kind != JsonKind::Object || GetCount(object) != keys.size())
    {
        return false;
    }
    for (const std::string_view key : keys)
    {
        if (GetMember(object, key) == NO_JSON_NODE)
        {
            return false;
        }
    }
    return true;
}

bool JsonTree::IsString(JsonIndex node, std::string_view value) const noexcept
{
    return node != NO_JSON_NODE && mNodes[node].Kind == JsonKind::String && GetText(node) == value;
}

bool AreJsonEqual(const JsonTree& left, JsonIndex leftNode, const JsonTree& right, JsonIndex rightNode) noexcept
{
    PythonNumber leftNumber;
    PythonNumber rightNumber;
    if (GetPythonNumber(left, leftNode, leftNumber) == true && GetPythonNumber(right, rightNode, rightNumber) == true)
    {
        return IsEqualPython(leftNumber, rightNumber);
    }
    const JsonKind kind = left.GetKind(leftNode);
    if (kind != right.GetKind(rightNode))
    {
        return false;
    }
    switch (kind)
    {
        case JsonKind::Null:
            return true;
        case JsonKind::String:
            return left.GetText(leftNode) == right.GetText(rightNode);
        case JsonKind::Array: {
            const usize count = left.GetCount(leftNode);
            if (count != right.GetCount(rightNode))
            {
                return false;
            }
            for (usize i = 0; i < count; ++i)
            {
                if (AreJsonEqual(left, left.GetItem(leftNode, i), right, right.GetItem(rightNode, i)) == false)
                {
                    return false;
                }
            }
            return true;
        }
        case JsonKind::Object: {
            const usize count = left.GetCount(leftNode);
            if (count != right.GetCount(rightNode))
            {
                return false;
            }
            for (usize i = 0; i < count; ++i)
            {
                const JsonIndex other = right.GetMember(rightNode, left.GetKey(leftNode, i));
                if (other == NO_JSON_NODE || AreJsonEqual(left, left.GetValue(leftNode, i), right, other) == false)
                {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

bool GetPythonNumber(const JsonTree& tree, JsonIndex node, PythonNumber& outNumber) noexcept
{
    const JsonNode& value = tree.GetNode(node);
    switch (value.Kind)
    {
        case JsonKind::Boolean:
            outNumber = MakeInteger(foundation::Int128(value.IsTrue == true ? 1 : 0));
            return true;
        case JsonKind::Integer:
            outNumber = MakeInteger(foundation::Int128(value.Integer));
            return true;
        case JsonKind::Float:
            outNumber = MakeFloat(value.Float);
            return true;
        default:
            return false;
    }
}

PythonNumber AddPython(PythonNumber left, PythonNumber right) noexcept
{
    if (left.IsFloat == false && right.IsFloat == false)
    {
        foundation::Int128 result;
        if (foundation::TryAdd(left.Integer, right.Integer, result) == true)
        {
            return MakeInteger(result);
        }
    }
    return MakeFloat(left.ToFloat() + right.ToFloat());
}

PythonNumber SubtractPython(PythonNumber left, PythonNumber right) noexcept
{
    if (left.IsFloat == false && right.IsFloat == false)
    {
        foundation::Int128 result;
        if (foundation::TrySubtract(left.Integer, right.Integer, result) == true)
        {
            return MakeInteger(result);
        }
    }
    return MakeFloat(left.ToFloat() - right.ToFloat());
}

PythonNumber MultiplyPython(PythonNumber left, PythonNumber right) noexcept
{
    if (left.IsFloat == false && right.IsFloat == false)
    {
        foundation::Int128 result;
        if (foundation::TryMultiply(left.Integer, right.Integer, result) == true)
        {
            return MakeInteger(result);
        }
    }
    return MakeFloat(left.ToFloat() * right.ToFloat());
}

bool IsLessPython(PythonNumber left, PythonNumber right) noexcept
{
    return ComparePython(left, right) == -1;
}

bool IsLessOrEqualPython(PythonNumber left, PythonNumber right) noexcept
{
    const int result = ComparePython(left, right);
    return result == -1 || result == 0;
}

bool IsEqualPython(PythonNumber left, PythonNumber right) noexcept
{
    return ComparePython(left, right) == 0;
}

void PythonSum::Add(PythonNumber value) noexcept
{
    if (mIsFloat == false)
    {
        if (value.IsFloat == false)
        {
            foundation::Int128 result;
            if (foundation::TryAdd(mInteger, value.Integer, result) == true)
            {
                mInteger = result;
                return;
            }
        }
        // int + item through the generic path, then the float loop.
        mFloat = foundation::ConvertToFloat64(mInteger) + value.ToFloat();
        mIsFloat = true;
        mCompensation = 0.0;
        return;
    }
    if (value.IsFloat == true)
    {
        const float64 item = value.Float;
        const float64 total = mFloat + item;
        if (math::Abs(mFloat) >= math::Abs(item))
        {
            mCompensation += (mFloat - total) + item;
        }
        else
        {
            mCompensation += (item - total) + mFloat;
        }
        mFloat = total;
        return;
    }
    mFloat += foundation::ConvertToFloat64(value.Integer);
}

PythonNumber PythonSum::GetResult() const noexcept
{
    if (mIsFloat == false)
    {
        return MakeInteger(mInteger);
    }
    float64 result = mFloat;
    if (mCompensation != 0.0 && math::IsFinite(mCompensation))
    {
        result += mCompensation;
    }
    return MakeFloat(result);
}

JsonIndex AddPythonNumber(JsonTree& inoutTree, PythonNumber value) noexcept
{
    int64 integer = 0;
    if (value.IsFloat == false && foundation::TryIntegerCast(value.Integer, integer) == true)
    {
        return inoutTree.AddInteger(integer);
    }
    return inoutTree.AddFloat(value.ToFloat());
}

bool ParsePythonJson(std::span<const uint8> input,
                     JsonTree& inoutTree,
                     JsonIndex& outRoot,
                     MessageText& outMessage) noexcept
{
    std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
    // json.detect_encoding: a UTF-8 signature selects utf-8-sig; NUL patterns select UTF-16/32.
    if (text.size() >= 3 && text.substr(0, 3) == "\xef\xbb\xbf")
    {
        text.remove_prefix(3);
    }
    else if (text.size() >= 2 && (text[0] == 0 || text[1] == 0))
    {
        (void)outMessage.Set("UTF-16 and UTF-32 scene documents are not supported");
        return false;
    }
    usize offset = 0;
    while (offset < text.size())
    {
        uint32 codePoint = 0;
        if (DecodeUtf8(text, offset, codePoint) == false)
        {
            (void)outMessage.Set("'utf-8' codec can't decode the document");
            return false;
        }
    }
    Parser parser(text, inoutTree, outMessage);
    return parser.ParseDocument(outRoot);
}

bool WriteJson(const JsonTree& tree, JsonIndex node, const JsonStyle& style, ByteWriter& inoutWriter) noexcept
{
    Writer writer(tree, style, inoutWriter);
    return writer.Write(node, 0) && inoutWriter.IsOk();
}

bool WriteCanonicalJson(const JsonTree& tree, JsonIndex node, Array<uint8>& outBytes) noexcept
{
    outBytes.Clear();
    ByteWriter writer(outBytes);
    const JsonStyle style;
    if (WriteJson(tree, node, style, writer) == false)
    {
        return false;
    }
    writer.Append('\n');
    return writer.IsOk();
}
} // namespace ludus::content::scene_cook::detail
