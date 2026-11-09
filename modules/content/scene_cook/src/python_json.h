#pragma once

// Private Python-compatible JSON document model for the scene asset catalog.
//
// Reproduces the CPython 3.12 json module semantics the retired scene_assets.py
// relied on: json.loads of bytes (UTF-8 with surrogatepass, NaN/Infinity tokens,
// duplicate keys rejected by the object hook), dict insertion order, int versus
// float preservation, and json.dumps(sort_keys=True, separators=(",", ":"),
// allow_nan=False) with ensure_ascii. Integers are bounded to int64.

#include "python_compat.h"

#include <ludus/foundation/base/int128.hpp>
#include <ludus/foundation/containers/array.hpp>

#include <ludus/content/content.h>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::detail
{
/// Index of a node inside a JsonTree.
using JsonIndex = usize;
/// Sentinel returned when a member or node does not exist.
inline constexpr JsonIndex NO_JSON_NODE = ~usize{0};

/// Python type of one JSON value.
enum class JsonKind : uint8
{
    Null,
    Boolean,
    Integer,
    Float,
    String,
    Array,
    Object,
};

/// One value; objects store alternating key (String) and value node indices.
struct JsonNode final
{
    JsonKind Kind = JsonKind::Null;
    bool IsTrue = false;
    int64 Integer = 0;
    float64 Float = 0.0;
    usize TextOffset = 0;
    usize TextSize = 0;
    Array<JsonIndex> Children;
};

/// Arena of JSON nodes; every mutation reports allocation failure through IsOk.
class JsonTree final
{
public:
    [[nodiscard]] JsonIndex AddNull() noexcept;
    [[nodiscard]] JsonIndex AddBoolean(bool isTrue) noexcept;
    [[nodiscard]] JsonIndex AddInteger(int64 value) noexcept;
    [[nodiscard]] JsonIndex AddFloat(float64 value) noexcept;
    [[nodiscard]] JsonIndex AddString(std::string_view value) noexcept;
    [[nodiscard]] JsonIndex AddArray() noexcept;
    [[nodiscard]] JsonIndex AddObject() noexcept;
    /// list.append.
    void Append(JsonIndex array, JsonIndex value) noexcept;
    /// dict[key] = value: replaces in place, or appends a new key.
    void SetMember(JsonIndex object, std::string_view key, JsonIndex value) noexcept;
    /// Append a new member without looking for an existing key; the caller guarantees key is absent.
    void AppendMember(JsonIndex object, std::string_view key, JsonIndex value) noexcept;
    /// Deep copy of a node from another (or the same) tree.
    [[nodiscard]] JsonIndex CopyFrom(const JsonTree& source, JsonIndex node) noexcept;

    [[nodiscard]] JsonKind GetKind(JsonIndex node) const noexcept
    {
        return mNodes[node].Kind;
    }
    [[nodiscard]] const JsonNode& GetNode(JsonIndex node) const noexcept
    {
        return mNodes[node];
    }
    [[nodiscard]] std::string_view GetText(JsonIndex node) const noexcept;
    /// Item count of an array, or member count of an object.
    [[nodiscard]] usize GetCount(JsonIndex node) const noexcept;
    [[nodiscard]] JsonIndex GetItem(JsonIndex array, usize index) const noexcept
    {
        return mNodes[array].Children[index];
    }
    [[nodiscard]] std::string_view GetKey(JsonIndex object, usize index) const noexcept
    {
        return GetText(mNodes[object].Children[index * 2]);
    }
    [[nodiscard]] JsonIndex GetValue(JsonIndex object, usize index) const noexcept
    {
        return mNodes[object].Children[index * 2 + 1];
    }
    /// dict.get(key); NO_JSON_NODE when absent or when the node is not an object.
    [[nodiscard]] JsonIndex GetMember(JsonIndex object, std::string_view key) const noexcept;
    /// isinstance(value, dict) and set(value) == set(keys).
    [[nodiscard]] bool HasExactKeys(JsonIndex object, std::span<const std::string_view> keys) const noexcept;
    [[nodiscard]] bool IsString(JsonIndex node, std::string_view value) const noexcept;
    [[nodiscard]] bool IsOk() const noexcept
    {
        return mIsOk;
    }
    [[nodiscard]] usize GetNodeCount() const noexcept
    {
        return mNodes.GetSize();
    }
    void MarkFailed() noexcept
    {
        mIsOk = false;
    }

private:
    [[nodiscard]] JsonIndex addNode(JsonKind kind) noexcept;
    Array<JsonNode> mNodes;
    Array<char> mText;
    bool mIsOk = true;
};

/// Python == between two JSON values (numbers and booleans compare numerically).
[[nodiscard]] bool
AreJsonEqual(const JsonTree& left, JsonIndex leftNode, const JsonTree& right, JsonIndex rightNode) noexcept;

/// Python number used by the arithmetic the reference performed on JSON values.
struct PythonNumber final
{
    bool IsFloat = false;
    /// The integer overflowed 128 bits; Float then holds the nearest float64.
    foundation::Int128 Integer;
    float64 Float = 0.0;
    [[nodiscard]] float64 ToFloat() const noexcept
    {
        return IsFloat == true ? Float : foundation::ConvertToFloat64(Integer);
    }
};
/// Number view of an int, float or bool node; false for other kinds.
[[nodiscard]] bool GetPythonNumber(const JsonTree& tree, JsonIndex node, PythonNumber& outNumber) noexcept;
[[nodiscard]] PythonNumber AddPython(PythonNumber left, PythonNumber right) noexcept;
[[nodiscard]] PythonNumber SubtractPython(PythonNumber left, PythonNumber right) noexcept;
[[nodiscard]] PythonNumber MultiplyPython(PythonNumber left, PythonNumber right) noexcept;
/// Python a < b / a <= b for numbers.
[[nodiscard]] bool IsLessPython(PythonNumber left, PythonNumber right) noexcept;
[[nodiscard]] bool IsLessOrEqualPython(PythonNumber left, PythonNumber right) noexcept;
[[nodiscard]] bool IsEqualPython(PythonNumber left, PythonNumber right) noexcept;
/// builtin sum() with start 0 (int fast path, then Neumaier float path).
class PythonSum final
{
public:
    void Add(PythonNumber value) noexcept;
    [[nodiscard]] PythonNumber GetResult() const noexcept;

private:
    bool mIsFloat = false;
    foundation::Int128 mInteger;
    float64 mFloat = 0.0;
    float64 mCompensation = 0.0;
};
/// Node for a number (int64 when it fits, otherwise float).
[[nodiscard]] JsonIndex AddPythonNumber(JsonTree& inoutTree, PythonNumber value) noexcept;

/// json.loads(bytes, object_pairs_hook=reject duplicates).
[[nodiscard]] bool ParsePythonJson(std::span<const uint8> input,
                                   JsonTree& inoutTree,
                                   JsonIndex& outRoot,
                                   MessageText& outMessage) noexcept;

/// json.dumps formatting options.
struct JsonStyle final
{
    bool IsSorted = true;
    bool IsAsciiOnly = true;
    /// Item separator, ", " or ",".
    std::string_view ItemSeparator = ",";
    /// Key separator, ": " or ":".
    std::string_view KeySeparator = ":";
    /// Indent width; zero selects single-line output.
    usize Indent = 0;
};
/// json.dumps(value, ...); false for nonfinite floats (allow_nan=False) or allocation failure.
[[nodiscard]] bool
WriteJson(const JsonTree& tree, JsonIndex node, const JsonStyle& style, ByteWriter& inoutWriter) noexcept;
/// canonical(): json.dumps(sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n".
[[nodiscard]] bool WriteCanonicalJson(const JsonTree& tree, JsonIndex node, Array<uint8>& outBytes) noexcept;
} // namespace ludus::content::scene_cook::detail
