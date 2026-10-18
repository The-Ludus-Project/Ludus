#include "gltf_reader.h"

#include "../front_end_support.h"
#include "../transform_utilities.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <bit>

namespace ludus::content::scene_cook::importers::gltf
{
namespace
{
using foundation::StringStatus;
using foundation::core::int16;
using foundation::core::int64;
using foundation::core::uint16;
using foundation::core::uint64;
using foundation::parsing::JsonDocument;
using foundation::parsing::JsonLimits;
using foundation::parsing::JsonValue;
using foundation::parsing::ParseError;
using foundation::parsing::ParseStatus;
namespace math = foundation::math;

// GLB container layout (glTF 2.0 section 4.4): magic, version and total length
// as little-endian uint32 words, then chunks of (length, type, data).
constexpr uint32 GLB_MAGIC = 0x46546c67;
constexpr uint32 GLB_VERSION = 2;
constexpr uint32 GLB_CHUNK_JSON = 0x4e4f534a;
constexpr uint32 GLB_CHUNK_BIN = 0x004e4942;
constexpr usize GLB_HEADER_BYTES = 12;
/// Most vertices a non-indexed primitive may address with the IR's uint32 indices.
constexpr usize MAX_IMPLICIT_INDEX_VERTICES = usize{~uint32{0}};
constexpr usize GLB_CHUNK_HEADER_BYTES = 8;
constexpr usize GLB_WORD_BYTES = sizeof(uint32);
constexpr usize GLB_MAGIC_OFFSET = 0;
constexpr usize GLB_VERSION_OFFSET = 4;
constexpr usize GLB_LENGTH_OFFSET = 8;
/// Offset of a chunk's type after its length word.
constexpr usize GLB_CHUNK_TYPE_OFFSET = 4;
/// Chunk data is padded to this alignment (section 4.4.3).
constexpr usize GLB_CHUNK_ALIGNMENT = 4;
/// yyjson's DOM needs several times the input; bound it explicitly.
constexpr usize MAX_JSON_WORKSPACE_BYTES = usize{2048} * 1024 * 1024;
constexpr uint32 COMPONENT_BYTE = 5120;
constexpr uint32 COMPONENT_UNSIGNED_BYTE = 5121;
constexpr uint32 COMPONENT_SHORT = 5122;
constexpr uint32 COMPONENT_UNSIGNED_SHORT = 5123;
constexpr uint32 COMPONENT_UNSIGNED_INT = 5125;
constexpr uint32 COMPONENT_FLOAT = 5126;
constexpr uint32 MODE_TRIANGLES = 4;
constexpr uint32 MODE_TRIANGLE_STRIP = 5;
constexpr uint32 MODE_TRIANGLE_FAN = 6;
constexpr uint32 WRAP_REPEAT = 10497;
constexpr uint32 WRAP_CLAMP_TO_EDGE = 33071;
constexpr uint32 WRAP_MIRRORED_REPEAT = 33648;
/// Accessor element sizes and byteStride rules (section 3.6.2.4): strides are
/// multiples of the 4-byte component alignment between 4 and 252 bytes.
constexpr usize ACCESSOR_STRIDE_ALIGNMENT = 4;
constexpr uint64 MIN_BYTE_STRIDE = 4;
constexpr uint64 MAX_BYTE_STRIDE = 252;
constexpr usize UINT16_COMPONENT_BYTES = sizeof(uint16);
constexpr usize UINT32_COMPONENT_BYTES = sizeof(uint32);
/// Components per accessor type (section 3.6.2.1, "type").
constexpr usize VECTOR4_COMPONENTS = 4;
constexpr usize MATRIX2_COMPONENTS = 4;
constexpr usize MATRIX3_COMPONENTS = 9;
constexpr usize MATRIX_ELEMENTS = 16;
/// Divisors of normalized signed/unsigned integer components (section 3.11).
constexpr float32 INT8_NORMALIZATION = 127.0F;
constexpr float32 UINT8_NORMALIZATION = 255.0F;
constexpr float32 INT16_NORMALIZATION = 32767.0F;
constexpr float32 UINT16_NORMALIZATION = 65535.0F;
/// Largest integer every float64 represents exactly (2^53): integral JSON reals above it
/// may have been rounded and are rejected as unsigned values.
constexpr float64 MAX_EXACT_INTEGER_REAL = 9007199254740992.0;
constexpr usize RGB_COMPONENTS = IR_RGB_CHANNEL_COUNT;
constexpr usize RGBA_COMPONENTS = 4;
/// Index of the alpha factor in an RGBA colour.
constexpr usize ALPHA_COMPONENT = 3;
/// Node rotation quaternion (x, y, z, w) and the index of w.
constexpr usize QUATERNION_COMPONENTS = 4;
constexpr usize QUATERNION_W = 3;
/// UV pairs per vertex in IrMesh::Uvs.
constexpr usize UV_COMPONENTS = 2;
/// Column-major 4x4 matrix layout of math::Matrix4d (Elements[4 * column + row]).
constexpr usize MATRIX_DIMENSION = 4;
constexpr usize TRANSLATION_COLUMN = 3;
/// RFC 4648 base64: four 6-bit digits encode three bytes; alphabet offsets of a-z,
/// 0-9 and the two symbol digits (standard '+' '/' or URL-safe '-' '_').
constexpr usize BASE64_DIGITS_PER_GROUP = 4;
constexpr usize BASE64_BYTES_PER_GROUP = 3;
constexpr uint32 BASE64_DIGIT_BITS = 6;
constexpr uint32 BITS_PER_BYTE = 8;
constexpr uint32 BYTE_MASK = 0xffU;
constexpr int32 BASE64_LOWERCASE_OFFSET = 26;
constexpr int32 BASE64_DECIMAL_OFFSET = 52;
constexpr int32 BASE64_PLUS_DIGIT = 62;
constexpr int32 BASE64_SLASH_DIGIT = 63;
/// RFC 3986 percent-encoding: "%" followed by two hexadecimal digits.
constexpr int32 HEXADECIMAL_RADIX = 16;
constexpr int32 HEXADECIMAL_LETTER_OFFSET = 10;
/// Prefix of an RFC 2397 data URI.
constexpr std::string_view DATA_URI_PREFIX = "data:";
/// Largest spot outer cone (KHR_lights_punctual: outerConeAngle <= pi / 2).
constexpr float64 MAX_OUTER_CONE_DEGREES = 90.0;
/// Default spot outer cone (pi / 4) from KHR_lights_punctual.
constexpr float64 DEFAULT_OUTER_CONE_RADIANS = 0.7853981633974483;
/// Smallest spot outer cone and separation from the inner cone. LUSCN needs the
/// float32 cosines to stay strictly ordered, which these keep distinct.
constexpr float64 MIN_OUTER_CONE_DEGREES = 0.1;
constexpr float64 MIN_CONE_SEPARATION_DEGREES = 0.05;

/// Extensions whose semantics the profile implements; others may only be optional.
constexpr usize SUPPORTED_EXTENSION_COUNT = 3;
constexpr foundation::StaticArray<std::string_view, SUPPORTED_EXTENSION_COUNT> SUPPORTED_EXTENSIONS{
    "KHR_lights_punctual",
    "KHR_materials_emissive_strength",
    "KHR_mesh_quantization",
};

/// Outcome of reading one optional JSON member.
enum class Field : uint8
{
    Missing,
    Ok,
    Invalid,
};

[[nodiscard]] Field GetUnsigned(JsonValue object, std::string_view key, uint64& outValue) noexcept
{
    const JsonValue value = object.Get(key);
    if (value.Valid() == false)
    {
        return Field::Missing;
    }
    if (value.Integer(outValue) == true)
    {
        return Field::Ok;
    }
    // Some writers emit integral reals such as 0.0.
    float64 number = 0.0;
    if (value.Number(number) == true && number >= 0.0 && number <= MAX_EXACT_INTEGER_REAL &&
        math::Floor(number) == number)
    {
        outValue = static_cast<uint64>(number);
        return Field::Ok;
    }
    return Field::Invalid;
}

[[nodiscard]] Field GetNumber(JsonValue object, std::string_view key, float64& outValue) noexcept
{
    const JsonValue value = object.Get(key);
    if (value.Valid() == false)
    {
        return Field::Missing;
    }
    float64 number = 0.0;
    if (value.Number(number) == false || math::IsFinite(number) == false)
    {
        return Field::Invalid;
    }
    outValue = number;
    return Field::Ok;
}

[[nodiscard]] Field GetString(JsonValue object, std::string_view key, std::string_view& outValue) noexcept
{
    const JsonValue value = object.Get(key);
    if (value.Valid() == false)
    {
        return Field::Missing;
    }
    return value.String(outValue) == true ? Field::Ok : Field::Invalid;
}

[[nodiscard]] Field GetBoolean(JsonValue object, std::string_view key, bool& outValue) noexcept
{
    const JsonValue value = object.Get(key);
    if (value.Valid() == false)
    {
        return Field::Missing;
    }
    return value.Boolean(outValue) == true ? Field::Ok : Field::Invalid;
}

/// Exactly outValues.size() finite numbers.
[[nodiscard]] Field GetNumbers(JsonValue object, std::string_view key, std::span<float64> outValues) noexcept
{
    const JsonValue value = object.Get(key);
    if (value.Valid() == false)
    {
        return Field::Missing;
    }
    if (value.Array() == false || value.Count() != outValues.size())
    {
        return Field::Invalid;
    }
    for (usize i = 0; i < outValues.size(); ++i)
    {
        float64 number = 0.0;
        if (value.At(i).Number(number) == false || math::IsFinite(number) == false)
        {
            return Field::Invalid;
        }
        outValues[i] = number;
    }
    return Field::Ok;
}

/// Number of elements of an optional top-level array (zero when absent).
[[nodiscard]] usize CountArray(JsonValue root, std::string_view key) noexcept
{
    const JsonValue value = root.Get(key);
    return value.Valid() == true && value.Array() == true ? value.Count() : 0;
}

/// Little-endian word at offset at; callers check the bounds first (zero otherwise).
[[nodiscard]] uint32 ReadUint32(std::span<const uint8> bytes, usize at) noexcept
{
    uint32 value = 0;
    return at <= bytes.size() && foundation::core::TryReadLittleEndian(bytes.subspan(at), value) == true ? value : 0;
}

/// Little-endian 16-bit component at the start of bytes.
[[nodiscard]] uint16 ReadUint16(const uint8* bytes) noexcept
{
    uint16 value = 0;
    return foundation::core::TryReadLittleEndian(std::span<const uint8>(bytes, UINT16_COMPONENT_BYTES), value) == true
               ? value
               : 0;
}

[[nodiscard]] int32 DecodeBase64Digit(char value) noexcept
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + BASE64_LOWERCASE_OFFSET;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + BASE64_DECIMAL_OFFSET;
    }
    if (value == '+' || value == '-')
    {
        return BASE64_PLUS_DIGIT;
    }
    if (value == '/' || value == '_')
    {
        return BASE64_SLASH_DIGIT;
    }
    return -1;
}

/// RFC 4648 base64 (standard or URL alphabet, optional padding).
/// @return Ok, Invalid for malformed text, Limit over maximumBytes, OutOfMemory.
[[nodiscard]] Status DecodeBase64(std::string_view text, usize maximumBytes, Array<uint8>& outBytes) noexcept
{
    while (text.empty() == false && text.back() == '=')
    {
        text.remove_suffix(1);
    }
    // A trailing group of one digit carries fewer than eight bits.
    const usize trailingDigits = text.size() % BASE64_DIGITS_PER_GROUP;
    if (trailingDigits == 1)
    {
        return Status::Invalid;
    }
    const usize size =
        text.size() / BASE64_DIGITS_PER_GROUP * BASE64_BYTES_PER_GROUP + (trailingDigits == 0 ? 0 : trailingDigits - 1);
    if (size > maximumBytes)
    {
        return Status::Limit;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(size) == false)
    {
        return Status::OutOfMemory;
    }
    uint32 accumulator = 0;
    uint32 bits = 0;
    usize written = 0;
    for (const char value : text)
    {
        const int32 digit = DecodeBase64Digit(value);
        if (digit < 0)
        {
            return Status::Invalid;
        }
        accumulator = (accumulator << BASE64_DIGIT_BITS) | static_cast<uint32>(digit);
        bits += BASE64_DIGIT_BITS;
        if (bits >= BITS_PER_BYTE)
        {
            bits -= BITS_PER_BYTE;
            bytes[written++] = static_cast<uint8>((accumulator >> bits) & BYTE_MASK);
        }
    }
    outBytes.Swap(bytes);
    return Status::Ok;
}

/// RFC 3986 percent-decoding of a relative URI reference.
[[nodiscard]] bool DecodeUriPath(std::string_view uri, String& outPath) noexcept
{
    outPath.Clear();
    for (usize i = 0; i < uri.size(); ++i)
    {
        char value = uri[i];
        if (value == '%')
        {
            const auto decode = [](char digit) noexcept -> int32 {
                if (digit >= '0' && digit <= '9')
                {
                    return digit - '0';
                }
                if (digit >= 'a' && digit <= 'f')
                {
                    return digit - 'a' + HEXADECIMAL_LETTER_OFFSET;
                }
                if (digit >= 'A' && digit <= 'F')
                {
                    return digit - 'A' + HEXADECIMAL_LETTER_OFFSET;
                }
                return -1;
            };
            if (i + 2 >= uri.size() || decode(uri[i + 1]) < 0 || decode(uri[i + 2]) < 0)
            {
                return false;
            }
            value = static_cast<char>(decode(uri[i + 1]) * HEXADECIMAL_RADIX + decode(uri[i + 2]));
            i += 2;
        }
        if (value == '\0' || value == '?' || value == '#' ||
            outPath.TryAppend(std::string_view(&value, 1)) != StringStatus::Ok)
        {
            return false;
        }
    }
    return true;
}

/// True when uri names a scheme ("http:", "file:") rather than a relative path.
[[nodiscard]] bool HasUriScheme(std::string_view uri) noexcept
{
    const usize colon = uri.find(':');
    const usize slash = uri.find('/');
    return colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash);
}

/// Media-type hint for an embedded image's decoder ("embedded.png" style names).
[[nodiscard]] std::string_view GetDecoderName(std::string_view mimeType) noexcept
{
    if (mimeType == "image/png")
    {
        return "embedded.png";
    }
    if (mimeType == "image/jpeg")
    {
        return "embedded.jpg";
    }
    return "embedded.bin";
}

[[nodiscard]] usize GetComponentBytes(uint32 componentType) noexcept
{
    switch (componentType)
    {
        case COMPONENT_BYTE:
        case COMPONENT_UNSIGNED_BYTE:
            return 1;
        case COMPONENT_SHORT:
        case COMPONENT_UNSIGNED_SHORT:
            return UINT16_COMPONENT_BYTES;
        case COMPONENT_UNSIGNED_INT:
        case COMPONENT_FLOAT:
            return UINT32_COMPONENT_BYTES;
        default:
            break;
    }
    return 0;
}

[[nodiscard]] usize GetTypeComponents(std::string_view type) noexcept
{
    if (type == "SCALAR")
    {
        return 1;
    }
    if (type == "VEC2")
    {
        return 2;
    }
    if (type == "VEC3")
    {
        return IR_VECTOR_COMPONENT_COUNT;
    }
    if (type == "VEC4")
    {
        return VECTOR4_COMPONENTS;
    }
    if (type == "MAT2")
    {
        return MATRIX2_COMPONENTS;
    }
    if (type == "MAT3")
    {
        return MATRIX3_COMPONENTS;
    }
    if (type == "MAT4")
    {
        return MATRIX_ELEMENTS;
    }
    return 0;
}

/// One component converted per glTF 2.0 section 3.11 (normalized integers).
[[nodiscard]] float32 ReadComponent(const uint8* bytes, uint32 componentType, bool isNormalized) noexcept
{
    switch (componentType)
    {
        case COMPONENT_BYTE: {
            const float32 value = static_cast<float32>(static_cast<signed char>(bytes[0]));
            return isNormalized == true ? math::Max(value / INT8_NORMALIZATION, -1.0F) : value;
        }
        case COMPONENT_UNSIGNED_BYTE: {
            const float32 value = static_cast<float32>(bytes[0]);
            return isNormalized == true ? value / UINT8_NORMALIZATION : value;
        }
        case COMPONENT_SHORT: {
            const float32 value = static_cast<float32>(static_cast<int16>(ReadUint16(bytes)));
            return isNormalized == true ? math::Max(value / INT16_NORMALIZATION, -1.0F) : value;
        }
        case COMPONENT_UNSIGNED_SHORT: {
            const float32 value = static_cast<float32>(ReadUint16(bytes));
            return isNormalized == true ? value / UINT16_NORMALIZATION : value;
        }
        case COMPONENT_UNSIGNED_INT:
            return static_cast<float32>(ReadUint32({bytes, UINT32_COMPONENT_BYTES}, 0));
        default:
            break;
    }
    return std::bit_cast<float32>(ReadUint32({bytes, UINT32_COMPONENT_BYTES}, 0));
}

/// glTF node TRS: translation, unit quaternion (x, y, z, w) and scale, as T * R * S.
[[nodiscard]] math::Matrix4d ComposeTransform(const float64 (&translation)[IR_VECTOR_COMPONENT_COUNT],
                                              const float64 (&rotation)[QUATERNION_COMPONENTS],
                                              const float64 (&scale)[IR_VECTOR_COMPONENT_COUNT]) noexcept
{
    const float64 x = rotation[0];
    const float64 y = rotation[1];
    const float64 z = rotation[2];
    const float64 w = rotation[QUATERNION_W];
    // Rotation matrix rows of the unit quaternion, scaled per column.
    const float64 rotationRows[IR_VECTOR_COMPONENT_COUNT][IR_VECTOR_COMPONENT_COUNT] = {
        {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)},
        {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)},
        {2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)},
    };
    math::Matrix4d matrix = math::Matrix4d::Identity();
    for (usize column = 0; column < IR_VECTOR_COMPONENT_COUNT; ++column)
    {
        for (usize row = 0; row < IR_VECTOR_COMPONENT_COUNT; ++row)
        {
            matrix.Elements[MATRIX_DIMENSION * column + row] = rotationRows[row][column] * scale[column];
        }
        matrix.Elements[MATRIX_DIMENSION * TRANSLATION_COLUMN + column] = translation[column];
    }
    return matrix;
}

[[nodiscard]] bool IsMatrixFinite(const math::Matrix4d& matrix) noexcept
{
    for (const float64 value : matrix.Elements)
    {
        if (math::IsFinite(value) == false)
        {
            return false;
        }
    }
    return true;
}

/// Give every index its own vertex (positions, uvs and tangents are copied).
[[nodiscard]] Status SplitCorners(IrMesh& inoutMesh) noexcept
{
    IrMesh split;
    const usize corners = inoutMesh.Indices.GetSize();
    const bool hasUvs = inoutMesh.Uvs.IsEmpty() == false;
    const bool hasTangents = inoutMesh.Tangents.IsEmpty() == false;
    if (split.Positions.TryResize(corners * IR_VECTOR_COMPONENT_COUNT) == false ||
        split.Indices.TryResize(corners) == false ||
        (hasUvs == true && split.Uvs.TryResize(corners * UV_COMPONENTS) == false) ||
        (hasTangents == true && split.Tangents.TryResize(corners * IR_TANGENT_COMPONENT_COUNT) == false))
    {
        return Status::OutOfMemory;
    }
    for (usize corner = 0; corner < corners; ++corner)
    {
        const usize vertex = inoutMesh.Indices[corner];
        for (usize k = 0; k < IR_VECTOR_COMPONENT_COUNT; ++k)
        {
            split.Positions[corner * IR_VECTOR_COMPONENT_COUNT + k] =
                inoutMesh.Positions[vertex * IR_VECTOR_COMPONENT_COUNT + k];
        }
        for (usize k = 0; hasUvs == true && k < UV_COMPONENTS; ++k)
        {
            split.Uvs[corner * UV_COMPONENTS + k] = inoutMesh.Uvs[vertex * UV_COMPONENTS + k];
        }
        for (usize k = 0; hasTangents == true && k < IR_TANGENT_COMPONENT_COUNT; ++k)
        {
            split.Tangents[corner * IR_TANGENT_COMPONENT_COUNT + k] =
                inoutMesh.Tangents[vertex * IR_TANGENT_COMPONENT_COUNT + k];
        }
        split.Indices[corner] = static_cast<uint32>(corner);
    }
    inoutMesh = static_cast<IrMesh&&>(split);
    return Status::Ok;
}

/// A buffer's bytes once loaded: the GLB BIN chunk, a decoded data URI or a file.
struct LoadedBuffer final
{
    Array<uint8> Owned;
    std::span<const uint8> Bytes;
    bool IsLoaded = false;
    bool IsAvailable = false;
};

/// A validated buffer view: its bytes inside the buffer and declared stride (zero: packed).
struct BufferViewSlice final
{
    std::span<const uint8> Bytes;
    usize Stride = 0;
    bool IsAvailable = false;
};

/// Accessor description after validation against its buffer view and buffer.
struct AccessorView final
{
    std::span<const uint8> Bytes;
    usize Count = 0;
    usize Stride = 0;
    usize Components = 0;
    uint32 ComponentType = COMPONENT_FLOAT;
    bool IsNormalized = false;
    /// No bufferView: every element is zero.
    bool IsZero = false;
};

/// IR data of one glTF mesh primitive.
struct LoadedPrimitive final
{
    uint32 Mesh = IR_NONE;
    uint32 Material = IR_NONE;
};

/// IR data of one glTF mesh: its primitives and, when instanced, its IR object.
struct LoadedMesh final
{
    bool IsLoaded = false;
    uint32 FirstPrimitive = 0;
    uint32 PrimitiveCount = 0;
    uint32 Object = IR_NONE;
    uint32 References = 0;
};

/// One node that places a mesh, found while traversing the scene.
struct MeshPlacement final
{
    uint32 Mesh = IR_NONE;
    uint32 Node = IR_NONE;
    math::Matrix4d Transform = math::Matrix4d::Identity();
};

class GltfReader final
{
public:
    GltfReader(SourceResolver& inoutResolver,
               SceneIr& outScene,
               DiagnosticLog& inoutLog,
               const Cancellation* cancelOrNull) noexcept
        : mResolver(inoutResolver), mScene(outScene), mLog(inoutLog), mCancelOrNull(cancelOrNull)
    {
    }

    [[nodiscard]] Status Read(std::span<const uint8> entryBytes) noexcept;

private:
    void report(DiagnosticSeverity severity, std::string_view code, std::string_view message) noexcept
    {
        if (mLog.Add(severity, code, mScene.SourceFiles[0].GetView(), 0, message) == false)
        {
            mHasOverflowed = true;
        }
    }
    void fail(std::string_view code, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Error, code, message);
    }
    void warn(std::string_view code, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Warning, code, message);
    }
    void inform(std::string_view code, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Info, code, message);
    }
    /// Report a schema violation at "array[index].member".
    void failAt(std::string_view array, usize index, std::string_view problem) noexcept
    {
        MessageBuilder message;
        (void)message.Append(array).Append("[").AppendUnsigned(index).Append("]: ").Append(problem);
        fail(codes::MALFORMED_INPUT, message.GetView());
    }

    [[nodiscard]] Status readContainer(std::span<const uint8> entryBytes, std::string_view& outJson) noexcept;
    [[nodiscard]] bool checkAssetAndExtensions() noexcept;
    [[nodiscard]] Status loadBuffer(usize index, const LoadedBuffer*& outBufferOrNull) noexcept;
    [[nodiscard]] Status getBufferView(usize index, BufferViewSlice& outSlice) noexcept;
    [[nodiscard]] Status getAccessor(usize index, AccessorView& outView, bool& outIsUsable) noexcept;
    [[nodiscard]] Status readFloats(usize accessor,
                                    std::string_view attribute,
                                    usize components,
                                    Array<float32>& outValues,
                                    bool& outIsUsable) noexcept;
    [[nodiscard]] Status
    readIndices(usize accessor, Array<uint32>& outIndices, usize vertexCount, bool& outIsUsable) noexcept;
    [[nodiscard]] Status
    getTexture(JsonValue textureInfo, TextureEncoding encoding, std::string_view role, uint32& outTexture) noexcept;
    [[nodiscard]] Status getImage(usize index, IrTexture& inoutTexture, bool& outIsUsable) noexcept;
    [[nodiscard]] Status getMaterial(JsonValue materialIndex, uint32& outMaterial) noexcept;
    [[nodiscard]] Status loadMaterial(usize index, uint32& outMaterial) noexcept;
    [[nodiscard]] Status loadMesh(usize index) noexcept;
    [[nodiscard]] Status loadPrimitive(JsonValue primitive) noexcept;
    [[nodiscard]] Status traverseScene() noexcept;
    [[nodiscard]] Status visitNode(usize node, const math::Matrix4d& world) noexcept;
    [[nodiscard]] bool getLocalTransform(usize node, JsonValue object, math::Matrix4d& outLocal) noexcept;
    void readCamera(JsonValue cameraIndex, const math::Matrix4d& world) noexcept;
    [[nodiscard]] Status readLight(JsonValue lightIndex, const math::Matrix4d& world) noexcept;
    [[nodiscard]] Status placeMeshes() noexcept;

    SourceResolver& mResolver;
    SceneIr& mScene;
    DiagnosticLog& mLog;
    const Cancellation* mCancelOrNull = nullptr;
    bool mHasOverflowed = false;

    JsonDocument mDocument;
    JsonValue mRoot;
    std::span<const uint8> mBinaryChunk;
    bool mHasBinaryChunk = false;
    Array<LoadedBuffer> mBuffers;
    /// IR material per glTF material (IR_NONE until first use) and the default material.
    Array<uint32> mMaterials;
    uint32 mDefaultMaterial = IR_NONE;
    /// IR texture per glTF texture for sRGB and linear use.
    Array<uint32> mSrgbTextures;
    Array<uint32> mLinearTextures;
    /// IR embedded image per glTF image.
    Array<uint32> mEmbeddedImages;
    Array<LoadedMesh> mMeshes;
    Array<LoadedPrimitive> mPrimitives;
    Array<MeshPlacement> mPlacements;
    Array<uint8> mIsNodeVisited;
    bool mHasCamera = false;
    uint32 mDroppedPlacements = 0;
};

Status GltfReader::readContainer(std::span<const uint8> entryBytes, std::string_view& outJson) noexcept
{
    if (entryBytes.size() < GLB_WORD_BYTES || ReadUint32(entryBytes, GLB_MAGIC_OFFSET) != GLB_MAGIC)
    {
        if (entryBytes.size() > MAX_JSON_BYTES)
        {
            fail(codes::LIMIT_EXCEEDED, "glTF JSON exceeds the size limit");
            return Status::Invalid;
        }
        outJson = std::string_view(reinterpret_cast<const char*>(entryBytes.data()), entryBytes.size());
        return Status::Ok;
    }
    // GLB: 12-byte header, a JSON chunk, then an optional BIN chunk (glTF 2.0 section 4.4).
    if (entryBytes.size() < GLB_HEADER_BYTES + GLB_CHUNK_HEADER_BYTES ||
        ReadUint32(entryBytes, GLB_VERSION_OFFSET) != GLB_VERSION)
    {
        fail(codes::MALFORMED_INPUT, "GLB header is truncated or not version 2");
        return Status::Invalid;
    }
    const usize length = ReadUint32(entryBytes, GLB_LENGTH_OFFSET);
    if (length > entryBytes.size() || length < GLB_HEADER_BYTES + GLB_CHUNK_HEADER_BYTES)
    {
        fail(codes::MALFORMED_INPUT, "GLB length does not match the file");
        return Status::Invalid;
    }
    usize at = GLB_HEADER_BYTES;
    bool hasJson = false;
    while (length - at >= GLB_CHUNK_HEADER_BYTES)
    {
        const usize chunkLength = ReadUint32(entryBytes, at);
        const uint32 chunkType = ReadUint32(entryBytes, at + GLB_CHUNK_TYPE_OFFSET);
        at += GLB_CHUNK_HEADER_BYTES;
        if (chunkLength > length - at)
        {
            fail(codes::MALFORMED_INPUT, "GLB chunk is truncated");
            return Status::Invalid;
        }
        const std::span<const uint8> chunk = entryBytes.subspan(at, chunkLength);
        if (hasJson == false)
        {
            if (chunkType != GLB_CHUNK_JSON)
            {
                fail(codes::MALFORMED_INPUT, "GLB does not start with a JSON chunk");
                return Status::Invalid;
            }
            if (chunkLength > MAX_JSON_BYTES)
            {
                fail(codes::LIMIT_EXCEEDED, "GLB JSON chunk exceeds the size limit");
                return Status::Invalid;
            }
            // Trailing space padding is valid JSON whitespace.
            outJson = std::string_view(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            hasJson = true;
        }
        else if (chunkType == GLB_CHUNK_BIN && mHasBinaryChunk == false)
        {
            mBinaryChunk = chunk;
            mHasBinaryChunk = true;
        }
        // Unknown chunk types are skipped (section 4.4.3).
        const usize paddedLength = (chunkLength + GLB_CHUNK_ALIGNMENT - 1) / GLB_CHUNK_ALIGNMENT * GLB_CHUNK_ALIGNMENT;
        at += paddedLength > length - at ? length - at : paddedLength;
    }
    if (hasJson == false)
    {
        fail(codes::MALFORMED_INPUT, "GLB has no JSON chunk");
        return Status::Invalid;
    }
    return Status::Ok;
}

bool GltfReader::checkAssetAndExtensions() noexcept
{
    const JsonValue asset = mRoot.Get("asset");
    std::string_view version;
    if (asset.Valid() == false || GetString(asset, "version", version) != Field::Ok)
    {
        fail(codes::MALFORMED_INPUT, "asset.version is missing");
        return false;
    }
    std::string_view minimum;
    const bool hasMinimum = GetString(asset, "minVersion", minimum) == Field::Ok;
    if (version.substr(0, 2) != "2." || (hasMinimum == true && minimum != "2.0"))
    {
        fail(codes::UNSUPPORTED_FEATURE, "only glTF 2.0 assets are supported");
        return false;
    }
    const auto isSupported = [](std::string_view name) noexcept {
        for (const std::string_view supported : SUPPORTED_EXTENSIONS)
        {
            if (supported == name)
            {
                return true;
            }
        }
        return false;
    };
    bool isAccepted = true;
    const JsonValue required = mRoot.Get("extensionsRequired");
    for (usize i = 0; required.Valid() == true && i < required.Count(); ++i)
    {
        std::string_view name;
        if (required.At(i).String(name) == false || isSupported(name) == false)
        {
            MessageBuilder message;
            (void)message.Append("required extension is not supported: ").Append(name);
            fail(codes::EXTENSION_REQUIRED, message.GetView());
            isAccepted = false;
        }
    }
    const JsonValue used = mRoot.Get("extensionsUsed");
    for (usize i = 0; used.Valid() == true && i < used.Count(); ++i)
    {
        std::string_view name;
        if (used.At(i).String(name) == true && isSupported(name) == false)
        {
            MessageBuilder message;
            (void)message.Append("optional extension ignored: ").Append(name);
            inform(codes::EXTENSION_IGNORED, message.GetView());
        }
    }
    if (CountArray(mRoot, "animations") != 0 || CountArray(mRoot, "skins") != 0)
    {
        warn(codes::ANIMATION_IGNORED, "animations and skins ignored; the static node transforms are imported");
    }
    return isAccepted;
}

Status GltfReader::loadBuffer(usize index, const LoadedBuffer*& outBufferOrNull) noexcept
{
    outBufferOrNull = nullptr;
    if (index >= mBuffers.GetSize())
    {
        failAt("buffers", index, "index out of range");
        return Status::Ok;
    }
    LoadedBuffer& buffer = mBuffers[index];
    outBufferOrNull = &buffer;
    if (buffer.IsLoaded == true)
    {
        return Status::Ok;
    }
    buffer.IsLoaded = true;
    const JsonValue object = mRoot.Get("buffers").At(index);
    uint64 byteLength = 0;
    if (GetUnsigned(object, "byteLength", byteLength) != Field::Ok || byteLength > MAX_BUFFER_BYTES)
    {
        failAt("buffers", index, "byteLength is missing or exceeds the buffer limit");
        return Status::Ok;
    }
    std::string_view uri;
    const Field hasUri = GetString(object, "uri", uri);
    if (hasUri == Field::Invalid)
    {
        failAt("buffers", index, "uri is not a string");
        return Status::Ok;
    }
    if (hasUri == Field::Missing)
    {
        // Only the first buffer of a GLB may omit uri; it names the BIN chunk.
        if (index != 0 || mHasBinaryChunk == false)
        {
            warn(codes::DEPENDENCY_UNREADABLE,
                 "buffer without uri or GLB binary chunk (for example a compression fallback)");
            return Status::Ok;
        }
        buffer.Bytes = mBinaryChunk;
    }
    else if (uri.starts_with(DATA_URI_PREFIX) == true)
    {
        const usize comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
        {
            failAt("buffers", index, "data URI is not base64");
            return Status::Ok;
        }
        const Status decoded = DecodeBase64(uri.substr(comma + 1), MAX_BUFFER_BYTES, buffer.Owned);
        if (decoded == Status::OutOfMemory)
        {
            return decoded;
        }
        if (decoded != Status::Ok)
        {
            failAt("buffers", index, "data URI is malformed or too large");
            return Status::Ok;
        }
        buffer.Bytes = buffer.Owned.AsSpan();
    }
    else
    {
        String path;
        String relative;
        ResolvedSource resolved;
        if (HasUriScheme(uri) == true || DecodeUriPath(uri, path) == false ||
            JoinRelativePath(GetDirectoryPrefix(mScene.SourceFiles[0].GetView()), path.GetView(), relative) == false ||
            mResolver.Resolve(relative.GetView(), resolved) == false)
        {
            warn(codes::DEPENDENCY_UNREADABLE, "buffer uri is remote, malformed or outside the import root");
            return Status::Ok;
        }
        const Status read = mResolver.Read(resolved, MAX_BUFFER_BYTES, buffer.Owned);
        if (read == Status::OutOfMemory)
        {
            return read;
        }
        if (read != Status::Ok)
        {
            warn(codes::DEPENDENCY_UNREADABLE, "cannot read an external buffer");
            return Status::Ok;
        }
        if (AddDataFile(mScene, resolved.Relative.GetView(), Hash(buffer.Owned.AsSpan())) == false)
        {
            return Status::OutOfMemory;
        }
        buffer.Bytes = buffer.Owned.AsSpan();
    }
    if (buffer.Bytes.size() < byteLength)
    {
        failAt("buffers", index, "data is shorter than byteLength");
        return Status::Ok;
    }
    buffer.Bytes = buffer.Bytes.first(static_cast<usize>(byteLength));
    buffer.IsAvailable = true;
    return Status::Ok;
}

Status GltfReader::getBufferView(usize index, BufferViewSlice& outSlice) noexcept
{
    outSlice = BufferViewSlice{};
    const JsonValue views = mRoot.Get("bufferViews");
    if (views.Valid() == false || index >= views.Count())
    {
        failAt("bufferViews", index, "index out of range");
        return Status::Ok;
    }
    const JsonValue view = views.At(index);
    uint64 buffer = 0;
    uint64 offset = 0;
    uint64 length = 0;
    uint64 stride = 0;
    const Field strideField = GetUnsigned(view, "byteStride", stride);
    if (GetUnsigned(view, "buffer", buffer) != Field::Ok || GetUnsigned(view, "byteLength", length) != Field::Ok ||
        GetUnsigned(view, "byteOffset", offset) == Field::Invalid || strideField == Field::Invalid ||
        (strideField == Field::Ok &&
         (stride < MIN_BYTE_STRIDE || stride > MAX_BYTE_STRIDE || stride % ACCESSOR_STRIDE_ALIGNMENT != 0)))
    {
        failAt("bufferViews", index, "buffer, byteOffset, byteLength or byteStride is invalid");
        return Status::Ok;
    }
    const LoadedBuffer* loadedOrNull = nullptr;
    const Status loaded = loadBuffer(static_cast<usize>(buffer), loadedOrNull);
    if (loaded != Status::Ok || loadedOrNull == nullptr || loadedOrNull->IsAvailable == false)
    {
        return loaded;
    }
    const std::span<const uint8> bytes = loadedOrNull->Bytes;
    if (offset > bytes.size() || length > bytes.size() - offset)
    {
        failAt("bufferViews", index, "range exceeds its buffer");
        return Status::Ok;
    }
    outSlice.Bytes = bytes.subspan(static_cast<usize>(offset), static_cast<usize>(length));
    outSlice.Stride = static_cast<usize>(stride);
    outSlice.IsAvailable = true;
    return Status::Ok;
}

Status GltfReader::getAccessor(usize index, AccessorView& outView, bool& outIsUsable) noexcept
{
    outIsUsable = false;
    const JsonValue accessors = mRoot.Get("accessors");
    if (accessors.Valid() == false || index >= accessors.Count())
    {
        failAt("accessors", index, "index out of range");
        return Status::Ok;
    }
    const JsonValue accessor = accessors.At(index);
    uint64 componentType = 0;
    uint64 count = 0;
    uint64 offset = 0;
    std::string_view type;
    bool isNormalized = false;
    if (GetUnsigned(accessor, "componentType", componentType) != Field::Ok ||
        GetComponentBytes(static_cast<uint32>(componentType)) == 0 ||
        GetUnsigned(accessor, "count", count) != Field::Ok || count == 0 ||
        GetString(accessor, "type", type) != Field::Ok || GetTypeComponents(type) == 0 ||
        GetUnsigned(accessor, "byteOffset", offset) == Field::Invalid ||
        GetBoolean(accessor, "normalized", isNormalized) == Field::Invalid)
    {
        failAt("accessors", index, "componentType, count, type, byteOffset or normalized is invalid");
        return Status::Ok;
    }
    if (accessor.Get("sparse").Valid() == true)
    {
        warn(codes::PRIMITIVE_SKIPPED, "sparse accessors are not supported; the primitive is skipped");
        return Status::Ok;
    }
    outView.Count = static_cast<usize>(count);
    outView.Components = GetTypeComponents(type);
    outView.ComponentType = static_cast<uint32>(componentType);
    outView.IsNormalized = isNormalized;
    const usize elementBytes = GetComponentBytes(outView.ComponentType) * outView.Components;
    uint64 viewIndex = 0;
    const Field hasView = GetUnsigned(accessor, "bufferView", viewIndex);
    if (hasView == Field::Invalid)
    {
        failAt("accessors", index, "bufferView is invalid");
        return Status::Ok;
    }
    if (hasView == Field::Missing)
    {
        // Zero-filled accessors have no data to bound them: admit them as if they
        // occupied a buffer of the binary data policy.
        if (outView.Count > MAX_BUFFER_BYTES / elementBytes)
        {
            failAt("accessors", index, "count without a bufferView exceeds the buffer limit");
            return Status::Ok;
        }
        outView.IsZero = true;
        outIsUsable = true;
        return Status::Ok;
    }
    BufferViewSlice slice;
    const Status viewed = getBufferView(static_cast<usize>(viewIndex), slice);
    if (viewed != Status::Ok || slice.IsAvailable == false)
    {
        return viewed;
    }
    const std::span<const uint8> bytes = slice.Bytes;
    outView.Stride = slice.Stride != 0 ? slice.Stride : elementBytes;
    const usize available = offset <= bytes.size() ? bytes.size() - static_cast<usize>(offset) : 0;
    if (offset % GetComponentBytes(outView.ComponentType) != 0 || outView.Stride < elementBytes ||
        available < elementBytes || outView.Count - 1 > (available - elementBytes) / outView.Stride)
    {
        failAt("accessors", index, "elements are misaligned or exceed their buffer view");
        return Status::Ok;
    }
    outView.Bytes = bytes.subspan(static_cast<usize>(offset));
    outIsUsable = true;
    return Status::Ok;
}

Status GltfReader::readFloats(usize accessor,
                              std::string_view attribute,
                              usize components,
                              Array<float32>& outValues,
                              bool& outIsUsable) noexcept
{
    AccessorView view;
    const Status status = getAccessor(accessor, view, outIsUsable);
    if (status != Status::Ok || outIsUsable == false)
    {
        return status;
    }
    if (view.Components != components || (view.ComponentType == COMPONENT_UNSIGNED_INT))
    {
        MessageBuilder message;
        (void)message.Append(attribute).Append(" accessor has an unsupported type or component type");
        fail(codes::MALFORMED_INPUT, message.GetView());
        outIsUsable = false;
        return Status::Ok;
    }
    if (outValues.TryResize(view.Count * components, 0.0F) == false)
    {
        return Status::OutOfMemory;
    }
    if (view.IsZero == true)
    {
        return Status::Ok;
    }
    const usize componentBytes = GetComponentBytes(view.ComponentType);
    for (usize i = 0; i < view.Count; ++i)
    {
        const uint8* element = view.Bytes.data() + i * view.Stride;
        for (usize k = 0; k < components; ++k)
        {
            const float32 value = ReadComponent(element + k * componentBytes, view.ComponentType, view.IsNormalized);
            if (math::IsFinite(value) == false)
            {
                MessageBuilder message;
                (void)message.Append(attribute).Append(" accessor holds a nonfinite value");
                fail(codes::MALFORMED_INPUT, message.GetView());
                outIsUsable = false;
                return Status::Ok;
            }
            outValues[i * components + k] = value;
        }
    }
    return Status::Ok;
}

Status GltfReader::readIndices(usize accessor, Array<uint32>& outIndices, usize vertexCount, bool& outIsUsable) noexcept
{
    AccessorView view;
    const Status status = getAccessor(accessor, view, outIsUsable);
    if (status != Status::Ok || outIsUsable == false)
    {
        return status;
    }
    if (view.Components != 1 || view.IsNormalized == true ||
        (view.ComponentType != COMPONENT_UNSIGNED_BYTE && view.ComponentType != COMPONENT_UNSIGNED_SHORT &&
         view.ComponentType != COMPONENT_UNSIGNED_INT))
    {
        fail(codes::MALFORMED_INPUT, "index accessor must be an unnormalized unsigned SCALAR");
        outIsUsable = false;
        return Status::Ok;
    }
    if (outIndices.TryResize(view.Count, 0U) == false)
    {
        return Status::OutOfMemory;
    }
    if (view.IsZero == true)
    {
        return Status::Ok;
    }
    const usize componentBytes = GetComponentBytes(view.ComponentType);
    for (usize i = 0; i < view.Count; ++i)
    {
        const uint8* element = view.Bytes.data() + i * view.Stride;
        uint32 value = element[0];
        if (componentBytes == UINT16_COMPONENT_BYTES)
        {
            value = ReadUint16(element);
        }
        else if (componentBytes == UINT32_COMPONENT_BYTES)
        {
            value = ReadUint32({element, UINT32_COMPONENT_BYTES}, 0);
        }
        if (value >= vertexCount)
        {
            fail(codes::MALFORMED_INPUT, "index exceeds the primitive's vertex count");
            outIsUsable = false;
            return Status::Ok;
        }
        outIndices[i] = value;
    }
    return Status::Ok;
}

Status GltfReader::getImage(usize index, IrTexture& inoutTexture, bool& outIsUsable) noexcept
{
    outIsUsable = false;
    const JsonValue images = mRoot.Get("images");
    if (images.Valid() == false || index >= images.Count())
    {
        failAt("images", index, "index out of range");
        return Status::Ok;
    }
    if (mEmbeddedImages[index] != IR_NONE)
    {
        inoutTexture.EmbeddedImage = mEmbeddedImages[index];
        outIsUsable = true;
        return Status::Ok;
    }
    const JsonValue image = images.At(index);
    std::string_view uri;
    std::string_view mimeType;
    (void)GetString(image, "mimeType", mimeType);
    const Field hasUri = GetString(image, "uri", uri);
    if (hasUri == Field::Ok && uri.starts_with(DATA_URI_PREFIX) == false)
    {
        String path;
        String relative;
        ResolvedSource resolved;
        if (HasUriScheme(uri) == true || DecodeUriPath(uri, path) == false ||
            JoinRelativePath(GetDirectoryPrefix(mScene.SourceFiles[0].GetView()), path.GetView(), relative) == false ||
            mResolver.Resolve(relative.GetView(), resolved) == false)
        {
            warn(codes::TEXTURE_UNREADABLE, "image uri is remote, malformed or outside the import root");
            return Status::Ok;
        }
        inoutTexture.File = static_cast<String&&>(resolved.Relative);
        outIsUsable = true;
        return Status::Ok;
    }
    IrEmbeddedImage embedded;
    MessageBuilder label;
    (void)label.Append(mScene.SourceFiles[0].GetView()).Append("#images[").AppendUnsigned(index).Append("]");
    if (embedded.Label.TryAssign(label.GetView()) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    if (hasUri == Field::Ok)
    {
        const usize comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
        {
            warn(codes::TEXTURE_UNREADABLE, "image data URI is not base64");
            return Status::Ok;
        }
        const std::string_view header = uri.substr(5, comma - 5);
        mimeType = header.substr(0, header.find(';'));
        const Status decoded = DecodeBase64(uri.substr(comma + 1), MAX_EMBEDDED_IMAGE_BYTES, embedded.Bytes);
        if (decoded == Status::OutOfMemory)
        {
            return decoded;
        }
        if (decoded != Status::Ok)
        {
            warn(codes::TEXTURE_UNREADABLE, "image data URI is malformed or too large");
            return Status::Ok;
        }
    }
    else
    {
        uint64 viewIndex = 0;
        if (GetUnsigned(image, "bufferView", viewIndex) != Field::Ok)
        {
            failAt("images", index, "neither uri nor bufferView is given");
            return Status::Ok;
        }
        BufferViewSlice slice;
        const Status viewed = getBufferView(static_cast<usize>(viewIndex), slice);
        if (viewed != Status::Ok || slice.IsAvailable == false)
        {
            return viewed;
        }
        const std::span<const uint8> bytes = slice.Bytes;
        if (bytes.size() > MAX_EMBEDDED_IMAGE_BYTES)
        {
            warn(codes::TEXTURE_UNREADABLE, "embedded image exceeds the size limit");
            return Status::Ok;
        }
        if (embedded.Bytes.TryAddRange(bytes) == false)
        {
            return Status::OutOfMemory;
        }
    }
    if (embedded.DecoderName.TryAssign(GetDecoderName(mimeType)) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    mEmbeddedImages[index] = static_cast<uint32>(mScene.EmbeddedImages.GetSize());
    if (mScene.EmbeddedImages.TryAdd(static_cast<IrEmbeddedImage&&>(embedded)) == false)
    {
        return Status::OutOfMemory;
    }
    inoutTexture.EmbeddedImage = mEmbeddedImages[index];
    outIsUsable = true;
    return Status::Ok;
}

Status GltfReader::getTexture(JsonValue textureInfo,
                              TextureEncoding encoding,
                              std::string_view role,
                              uint32& outTexture) noexcept
{
    outTexture = IR_NONE;
    if (textureInfo.Valid() == false)
    {
        return Status::Ok;
    }
    uint64 index = 0;
    uint64 texCoord = 0;
    if (GetUnsigned(textureInfo, "index", index) != Field::Ok ||
        GetUnsigned(textureInfo, "texCoord", texCoord) == Field::Invalid || index >= mSrgbTextures.GetSize())
    {
        fail(codes::MALFORMED_INPUT, "textureInfo.index is missing or out of range");
        return Status::Ok;
    }
    if (texCoord != 0)
    {
        MessageBuilder message;
        (void)message.Append(role)
            .Append(" texture uses TEXCOORD_")
            .AppendUnsigned(texCoord)
            .Append("; only TEXCOORD_0 is imported, so the texture is dropped");
        warn(codes::TEXTURE_DROPPED, message.GetView());
        return Status::Ok;
    }
    const JsonValue extensions = textureInfo.Get("extensions");
    if (extensions.Valid() == true && extensions.Get("KHR_texture_transform").Valid() == true)
    {
        warn(codes::TEXTURE_APPROXIMATED, "KHR_texture_transform offset/rotation/scale ignored");
    }
    Array<uint32>& cache = encoding == TextureEncoding::Srgb ? mSrgbTextures : mLinearTextures;
    if (cache[static_cast<usize>(index)] != IR_NONE)
    {
        outTexture = cache[static_cast<usize>(index)];
        return Status::Ok;
    }
    const JsonValue texture = mRoot.Get("textures").At(static_cast<usize>(index));
    IrTexture irTexture;
    irTexture.Encoding = encoding;
    irTexture.SourceFile = 0;
    std::string_view name;
    if (GetString(texture, "name", name) == Field::Ok && irTexture.Name.TryAssign(name) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    uint64 sampler = 0;
    if (GetUnsigned(texture, "sampler", sampler) == Field::Ok)
    {
        const JsonValue samplers = mRoot.Get("samplers");
        if (samplers.Valid() == false || sampler >= samplers.Count())
        {
            fail(codes::MALFORMED_INPUT, "texture sampler index out of range");
            return Status::Ok;
        }
        uint64 wrapS = WRAP_REPEAT;
        uint64 wrapT = WRAP_REPEAT;
        (void)GetUnsigned(samplers.At(static_cast<usize>(sampler)), "wrapS", wrapS);
        (void)GetUnsigned(samplers.At(static_cast<usize>(sampler)), "wrapT", wrapT);
        if (wrapS == WRAP_MIRRORED_REPEAT || wrapT == WRAP_MIRRORED_REPEAT)
        {
            warn(codes::TEXTURE_APPROXIMATED, "mirrored-repeat wrapping cooked as repeat");
        }
        else if (wrapS != wrapT)
        {
            warn(codes::TEXTURE_APPROXIMATED, "different wrap modes per axis cooked as one mode");
        }
        irTexture.IsRepeated = wrapS != WRAP_CLAMP_TO_EDGE && wrapT != WRAP_CLAMP_TO_EDGE;
    }
    uint64 source = 0;
    if (GetUnsigned(texture, "source", source) != Field::Ok)
    {
        // KHR_texture_basisu, EXT_texture_webp and similar name their source in an extension.
        warn(codes::TEXTURE_UNREADABLE,
             "texture has no core image source (compressed-texture extensions are unsupported)");
        return Status::Ok;
    }
    bool isUsable = false;
    const Status imaged = getImage(static_cast<usize>(source), irTexture, isUsable);
    if (imaged != Status::Ok || isUsable == false)
    {
        return imaged;
    }
    outTexture = static_cast<uint32>(mScene.Textures.GetSize());
    cache[static_cast<usize>(index)] = outTexture;
    return mScene.Textures.TryAdd(static_cast<IrTexture&&>(irTexture)) == true ? Status::Ok : Status::OutOfMemory;
}

Status GltfReader::loadMaterial(usize index, uint32& outMaterial) noexcept
{
    const JsonValue object = mRoot.Get("materials").At(index);
    IrMaterial material;
    material.SourceFile = 0;
    if (material.SourceType.TryAssign("gltf-metallic-roughness") != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    std::string_view name;
    if (GetString(object, "name", name) == Field::Ok && material.Name.TryAssign(name) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    if (material.Name.IsEmpty() == true)
    {
        MessageBuilder fallback;
        (void)fallback.Append("materials[").AppendUnsigned(index).Append("]");
        if (material.Name.TryAssign(fallback.GetView()) != StringStatus::Ok)
        {
            return Status::OutOfMemory;
        }
    }
    float64 baseColour[RGBA_COMPONENTS] = {1.0, 1.0, 1.0, 1.0};
    float64 metallic = 1.0;
    float64 roughness = 1.0;
    const JsonValue pbr = object.Get("pbrMetallicRoughness");
    Status status = Status::Ok;
    if (pbr.Valid() == true)
    {
        if (GetNumbers(pbr, "baseColorFactor", baseColour) == Field::Invalid ||
            GetNumber(pbr, "metallicFactor", metallic) == Field::Invalid ||
            GetNumber(pbr, "roughnessFactor", roughness) == Field::Invalid)
        {
            failAt("materials", index, "pbrMetallicRoughness factors are invalid");
        }
        status =
            getTexture(pbr.Get("baseColorTexture"), TextureEncoding::Srgb, "baseColor", material.BaseColourTexture);
        uint32 metallicRoughness = IR_NONE;
        if (status == Status::Ok)
        {
            status = getTexture(pbr.Get("metallicRoughnessTexture"),
                                TextureEncoding::Linear,
                                "metallicRoughness",
                                metallicRoughness);
        }
        // Blue holds metalness and green roughness (glTF 2.0 section 3.9.2).
        material.MetallicTexture = metallicRoughness;
        material.MetallicChannel = 2;
        material.RoughnessTexture = metallicRoughness;
        material.RoughnessChannel = 1;
    }
    for (usize k = 0; k < RGB_COMPONENTS; ++k)
    {
        material.BaseColour[k] = math::Clamp(baseColour[k], 0.0, 1.0);
    }
    material.Opacity = math::Clamp(baseColour[ALPHA_COMPONENT], 0.0, 1.0);
    material.Metallic = math::Clamp(metallic, 0.0, 1.0);
    material.Roughness = math::Clamp(roughness, 0.0, 1.0);
    float64 normalScale = 1.0;
    float64 occlusionStrength = 1.0;
    const JsonValue normal = object.Get("normalTexture");
    const JsonValue occlusion = object.Get("occlusionTexture");
    if (normal.Valid() == true)
    {
        (void)GetNumber(normal, "scale", normalScale);
    }
    if (occlusion.Valid() == true)
    {
        (void)GetNumber(occlusion, "strength", occlusionStrength);
    }
    material.NormalScale = normalScale;
    material.OcclusionStrength = occlusionStrength;
    if (status == Status::Ok)
    {
        status = getTexture(normal, TextureEncoding::Linear, "normal", material.NormalTexture);
    }
    if (status == Status::Ok)
    {
        status = getTexture(occlusion, TextureEncoding::Linear, "occlusion", material.OcclusionTexture);
    }
    if (status == Status::Ok)
    {
        status = getTexture(object.Get("emissiveTexture"), TextureEncoding::Srgb, "emissive", material.EmissionTexture);
    }
    if (status != Status::Ok)
    {
        return status;
    }
    float64 emissive[RGB_COMPONENTS] = {0.0, 0.0, 0.0};
    if (GetNumbers(object, "emissiveFactor", emissive) == Field::Invalid)
    {
        failAt("materials", index, "emissiveFactor is invalid");
    }
    float64 emissiveStrength = 1.0;
    const JsonValue extensions = object.Get("extensions");
    if (extensions.Valid() == true)
    {
        const JsonValue strength = extensions.Get("KHR_materials_emissive_strength");
        if (strength.Valid() == true && GetNumber(strength, "emissiveStrength", emissiveStrength) == Field::Invalid)
        {
            failAt("materials", index, "emissiveStrength is invalid");
        }
        for (usize i = 0; i < extensions.MemberCount(); ++i)
        {
            std::string_view key;
            JsonValue ignored;
            if (extensions.MemberAt(i, key, ignored) == false || key == "KHR_materials_emissive_strength")
            {
                continue;
            }
            const bool isTransmission = key == "KHR_materials_transmission" || key == "KHR_materials_volume" ||
                                        key == "KHR_materials_diffuse_transmission";
            MessageBuilder message;
            (void)message.Append(key).Append(" not represented by the metallic-roughness profile");
            warn(isTransmission == true ? codes::TRANSMISSION_DROPPED : codes::MATERIAL_APPROXIMATED,
                 message.GetView());
            material.IsApproximated = true;
        }
    }
    for (usize k = 0; k < RGB_COMPONENTS; ++k)
    {
        material.Emission[k] = math::Max(emissive[k], 0.0) * math::Max(emissiveStrength, 0.0);
    }
    std::string_view alphaMode = "OPAQUE";
    (void)GetString(object, "alphaMode", alphaMode);
    float64 alphaCutoff = .5;
    (void)GetNumber(object, "alphaCutoff", alphaCutoff);
    material.AlphaCutoff = alphaCutoff;
    material.AlphaMode = alphaMode == "MASK"    ? IrAlphaMode::Mask
                         : alphaMode == "BLEND" ? IrAlphaMode::Blend
                                                : IrAlphaMode::Opaque;
    bool isDoubleSided = false;
    (void)GetBoolean(object, "doubleSided", isDoubleSided);
    material.IsDoubleSided = isDoubleSided;
    if (material.AlphaMode != IrAlphaMode::Opaque)
    {
        warn(codes::ALPHA_APPROXIMATED, "glTF alphaMode MASK/BLEND cooked as opaque");
    }
    if (material.IsDoubleSided == true)
    {
        inform(codes::SIDEDNESS_IGNORED, "doubleSided is not stored by LUSCN v3");
    }
    if (material.NormalTexture != IR_NONE)
    {
        warn(codes::NORMALMAP_DROPPED, "glTF normalTexture not stored by LUSCN v3");
    }
    if (material.OcclusionTexture != IR_NONE)
    {
        inform(codes::TEXTURE_DROPPED, "glTF occlusionTexture not stored by LUSCN v3");
    }
    if (material.EmissionTexture != IR_NONE)
    {
        warn(codes::EMISSION_APPROXIMATED, "glTF emissiveTexture dropped; emissiveFactor is used as uniform emission");
    }
    if (ComputeRec709Luminance(material.Emission) > 0.0)
    {
        inform(codes::EMISSION_APPROXIMATED, "glTF emissive colour read as linear RGB luminance in nits");
    }
    outMaterial = static_cast<uint32>(mScene.Materials.GetSize());
    return mScene.Materials.TryAdd(static_cast<IrMaterial&&>(material)) == true ? Status::Ok : Status::OutOfMemory;
}

Status GltfReader::getMaterial(JsonValue materialIndex, uint32& outMaterial) noexcept
{
    outMaterial = IR_NONE;
    uint64 index = 0;
    if (materialIndex.Valid() == false || materialIndex.Integer(index) == false)
    {
        // The default material is white, fully metallic and fully rough (section 3.9.6).
        if (mDefaultMaterial == IR_NONE)
        {
            IrMaterial material;
            material.BaseColour[0] = material.BaseColour[1] = material.BaseColour[2] = 1.0;
            material.Metallic = 1.0;
            material.Roughness = 1.0;
            if (material.Name.TryAssign("(glTF default)") != StringStatus::Ok ||
                material.SourceType.TryAssign("gltf-default") != StringStatus::Ok)
            {
                return Status::OutOfMemory;
            }
            mDefaultMaterial = static_cast<uint32>(mScene.Materials.GetSize());
            if (mScene.Materials.TryAdd(static_cast<IrMaterial&&>(material)) == false)
            {
                return Status::OutOfMemory;
            }
        }
        outMaterial = mDefaultMaterial;
        return Status::Ok;
    }
    if (index >= mMaterials.GetSize())
    {
        fail(codes::MALFORMED_INPUT, "primitive material index out of range");
        return Status::Ok;
    }
    if (mMaterials[static_cast<usize>(index)] == IR_NONE)
    {
        uint32 loaded = IR_NONE;
        const Status status = loadMaterial(static_cast<usize>(index), loaded);
        if (status != Status::Ok)
        {
            return status;
        }
        mMaterials[static_cast<usize>(index)] = loaded;
    }
    outMaterial = mMaterials[static_cast<usize>(index)];
    return Status::Ok;
}

Status GltfReader::loadPrimitive(JsonValue primitive) noexcept
{
    uint64 mode = MODE_TRIANGLES;
    if (GetUnsigned(primitive, "mode", mode) == Field::Invalid)
    {
        fail(codes::MALFORMED_INPUT, "primitive mode is invalid");
        return Status::Ok;
    }
    if (mode != MODE_TRIANGLES && mode != MODE_TRIANGLE_STRIP && mode != MODE_TRIANGLE_FAN)
    {
        warn(codes::PRIMITIVE_SKIPPED, "point and line primitives skipped");
        return Status::Ok;
    }
    if (primitive.Get("targets").Valid() == true)
    {
        warn(codes::ANIMATION_IGNORED, "morph targets ignored; the base geometry is imported");
    }
    const JsonValue primitiveExtensions = primitive.Get("extensions");
    const bool isDraco =
        primitiveExtensions.Valid() == true && primitiveExtensions.Get("KHR_draco_mesh_compression").Valid() == true;
    const JsonValue attributes = primitive.Get("attributes");
    uint64 positionAccessor = 0;
    if (attributes.Valid() == false || GetUnsigned(attributes, "POSITION", positionAccessor) != Field::Ok)
    {
        warn(codes::PRIMITIVE_SKIPPED, "primitive without POSITION skipped");
        return Status::Ok;
    }
    if (isDraco == true)
    {
        const JsonValue position = mRoot.Get("accessors").At(static_cast<usize>(positionAccessor));
        if (position.Valid() == true && position.Get("bufferView").Valid() == false)
        {
            warn(codes::PRIMITIVE_SKIPPED, "Draco-compressed primitive without an uncompressed fallback skipped");
            return Status::Ok;
        }
    }
    for (usize i = 0; i < attributes.MemberCount(); ++i)
    {
        std::string_view key;
        JsonValue ignored;
        if (attributes.MemberAt(i, key, ignored) == true && key != "POSITION" && key != "NORMAL" &&
            key != "TEXCOORD_0" && key != "TANGENT")
        {
            MessageBuilder message;
            (void)message.Append("vertex attribute ignored: ").Append(key);
            inform(codes::ATTRIBUTE_IGNORED, message.GetView());
        }
    }
    IrMesh mesh;
    bool isUsable = false;
    Status status = readFloats(static_cast<usize>(positionAccessor),
                               "POSITION",
                               IR_VECTOR_COMPONENT_COUNT,
                               mesh.Positions,
                               isUsable);
    if (status != Status::Ok || isUsable == false)
    {
        return status;
    }
    const usize vertexCount = mesh.GetVertexCount();
    const auto readOptional =
        [&](std::string_view key, usize components, Array<float32>& outValues) noexcept -> Status {
        uint64 accessor = 0;
        if (GetUnsigned(attributes, key, accessor) != Field::Ok)
        {
            return Status::Ok;
        }
        bool isRead = false;
        const Status read = readFloats(static_cast<usize>(accessor), key, components, outValues, isRead);
        if (read == Status::Ok && (isRead == false || outValues.GetSize() != vertexCount * components))
        {
            if (isRead == true)
            {
                MessageBuilder message;
                (void)message.Append(key).Append(" count differs from POSITION; attribute dropped");
                warn(codes::ATTRIBUTE_IGNORED, message.GetView());
            }
            outValues.Clear();
        }
        return read;
    };
    status = readOptional("NORMAL", IR_VECTOR_COMPONENT_COUNT, mesh.Normals);
    if (status == Status::Ok)
    {
        status = readOptional("TEXCOORD_0", UV_COMPONENTS, mesh.Uvs);
    }
    if (status == Status::Ok)
    {
        status = readOptional("TANGENT", IR_TANGENT_COMPONENT_COUNT, mesh.Tangents);
    }
    if (status != Status::Ok)
    {
        return status;
    }
    Array<uint32> indices;
    uint64 indexAccessor = 0;
    const Field hasIndices = GetUnsigned(primitive, "indices", indexAccessor);
    if (hasIndices == Field::Invalid)
    {
        fail(codes::MALFORMED_INPUT, "primitive indices is invalid");
        return Status::Ok;
    }
    if (hasIndices == Field::Ok)
    {
        status = readIndices(static_cast<usize>(indexAccessor), indices, vertexCount, isUsable);
        if (status != Status::Ok || isUsable == false)
        {
            return status;
        }
    }
    else
    {
        if (vertexCount > MAX_IMPLICIT_INDEX_VERTICES || indices.TryResize(vertexCount) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; i < vertexCount; ++i)
        {
            indices[i] = static_cast<uint32>(i);
        }
    }
    // Strips and fans become lists (section 3.7.2.1); a short list is skipped.
    if (mode == MODE_TRIANGLES)
    {
        if (indices.GetSize() % IR_TRIANGLE_CORNER_COUNT != 0)
        {
            warn(codes::GEOMETRY_REPAIRED, "triangle list length is not a multiple of three; tail ignored");
        }
        if (mesh.Indices.TryAddRange(std::span<const uint32>(indices.GetData(),
                                                             indices.GetSize() / IR_TRIANGLE_CORNER_COUNT *
                                                                 IR_TRIANGLE_CORNER_COUNT)) == false)
        {
            return Status::OutOfMemory;
        }
    }
    else
    {
        for (usize i = 2; i < indices.GetSize(); ++i)
        {
            uint32 a = mode == MODE_TRIANGLE_FAN ? indices[0] : indices[i - 2];
            uint32 b = indices[i - 1];
            const uint32 c = indices[i];
            if (mode == MODE_TRIANGLE_STRIP && i % 2 == 1)
            {
                const uint32 swapped = a;
                a = b;
                b = swapped;
            }
            if (mesh.Indices.TryAdd(a) == false || mesh.Indices.TryAdd(b) == false || mesh.Indices.TryAdd(c) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    if (mesh.Indices.IsEmpty() == true)
    {
        warn(codes::PRIMITIVE_SKIPPED, "primitive without a complete triangle skipped");
        return Status::Ok;
    }
    // Without NORMAL, glTF 2.0 section 3.7.2.1 asks for flat normals: give every
    // triangle corner its own vertex so the generated normals cannot be shared.
    if (mesh.Normals.IsEmpty() == true)
    {
        status = SplitCorners(mesh);
        if (status != Status::Ok)
        {
            return status;
        }
    }
    LoadedPrimitive loaded;
    status = getMaterial(primitive.Get("material"), loaded.Material);
    if (status != Status::Ok || loaded.Material == IR_NONE)
    {
        // An invalid material index was already reported as an error.
        return status;
    }
    loaded.Mesh = static_cast<uint32>(mScene.Meshes.GetSize());
    if (mScene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == false || mPrimitives.TryAdd(loaded) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

Status GltfReader::loadMesh(usize index) noexcept
{
    LoadedMesh& loaded = mMeshes[index];
    if (loaded.IsLoaded == true)
    {
        return Status::Ok;
    }
    loaded.IsLoaded = true;
    loaded.FirstPrimitive = static_cast<uint32>(mPrimitives.GetSize());
    const JsonValue primitives = mRoot.Get("meshes").At(index).Get("primitives");
    if (primitives.Valid() == false || primitives.Array() == false)
    {
        failAt("meshes", index, "primitives is missing");
        return Status::Ok;
    }
    for (usize p = 0; p < primitives.Count(); ++p)
    {
        if (IsCancelled(mCancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        const Status status = loadPrimitive(primitives.At(p));
        if (status != Status::Ok)
        {
            return status;
        }
    }
    mMeshes[index].PrimitiveCount = static_cast<uint32>(mPrimitives.GetSize()) - mMeshes[index].FirstPrimitive;
    return Status::Ok;
}

bool GltfReader::getLocalTransform(usize node, JsonValue object, math::Matrix4d& outLocal) noexcept
{
    float64 matrix[MATRIX_ELEMENTS] = {};
    const Field hasMatrix = GetNumbers(object, "matrix", matrix);
    float64 translation[IR_VECTOR_COMPONENT_COUNT] = {0.0, 0.0, 0.0};
    float64 rotation[QUATERNION_COMPONENTS] = {0.0, 0.0, 0.0, 1.0};
    float64 scale[IR_VECTOR_COMPONENT_COUNT] = {1.0, 1.0, 1.0};
    const Field hasTranslation = GetNumbers(object, "translation", translation);
    const Field hasRotation = GetNumbers(object, "rotation", rotation);
    const Field hasScale = GetNumbers(object, "scale", scale);
    if (hasMatrix == Field::Invalid || hasTranslation == Field::Invalid || hasRotation == Field::Invalid ||
        hasScale == Field::Invalid)
    {
        failAt("nodes", node, "matrix, translation, rotation or scale is invalid");
        return false;
    }
    if (hasMatrix == Field::Ok)
    {
        // Column-major, like math::Matrix4d (Elements[4 * column + row]).
        for (usize i = 0; i < MATRIX_ELEMENTS; ++i)
        {
            outLocal.Elements[i] = matrix[i];
        }
        return true;
    }
    const float64 length = math::Sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] +
                                      rotation[2] * rotation[2] + rotation[QUATERNION_W] * rotation[QUATERNION_W]);
    if (length <= 0.0)
    {
        failAt("nodes", node, "rotation is a zero quaternion");
        return false;
    }
    for (float64& component : rotation)
    {
        component /= length;
    }
    outLocal = ComposeTransform(translation, rotation, scale);
    return true;
}

void GltfReader::readCamera(JsonValue cameraIndex, const math::Matrix4d& world) noexcept
{
    uint64 index = 0;
    const JsonValue cameras = mRoot.Get("cameras");
    if (cameraIndex.Integer(index) == false || cameras.Valid() == false || index >= cameras.Count())
    {
        fail(codes::MALFORMED_INPUT, "node camera index out of range");
        return;
    }
    if (mHasCamera == true)
    {
        inform(codes::CAMERA_APPROXIMATED, "only the first camera in scene order is imported");
        return;
    }
    const JsonValue camera = cameras.At(static_cast<usize>(index));
    std::string_view type;
    (void)GetString(camera, "type", type);
    IrCamera& target = mScene.Camera;
    // Cameras look down -Z with +Y up in their node's frame (section 3.10).
    target.Eye = TransformPoint(world, math::Vector3d{0.0, 0.0, 0.0});
    target.Target = target.Eye + TransformDirection(world, math::Vector3d{0.0, 0.0, -1.0});
    target.Up = TransformDirection(world, math::Vector3d{0.0, 1.0, 0.0});
    target.IsPresent = true;
    mHasCamera = true;
    const JsonValue perspective = camera.Get("perspective");
    if (type == "perspective" && perspective.Valid() == true)
    {
        float64 fov = 0.0;
        if (GetNumber(perspective, "yfov", fov) == Field::Ok && fov > 0.0 && fov < math::kPi)
        {
            target.VerticalFov = fov;
        }
        else
        {
            warn(codes::CAMERA_DEFAULTED, "camera yfov is invalid; 45 degrees used");
        }
        (void)GetNumber(perspective, "aspectRatio", target.AspectRatio);
        (void)GetNumber(perspective, "znear", target.NearClip);
        (void)GetNumber(perspective, "zfar", target.FarClip);
        return;
    }
    warn(codes::CAMERA_APPROXIMATED, "orthographic camera imported as a 45-degree perspective camera");
}

Status GltfReader::readLight(JsonValue lightIndex, const math::Matrix4d& world) noexcept
{
    uint64 index = 0;
    const JsonValue extension = mRoot.Get("extensions").Get("KHR_lights_punctual");
    const JsonValue lights = extension.Valid() == true ? extension.Get("lights") : JsonValue{};
    if (lightIndex.Integer(index) == false || lights.Valid() == false || index >= lights.Count())
    {
        fail(codes::MALFORMED_INPUT, "KHR_lights_punctual light index out of range");
        return Status::Ok;
    }
    const JsonValue light = lights.At(static_cast<usize>(index));
    std::string_view type;
    float64 colour[RGB_COMPONENTS] = {1.0, 1.0, 1.0};
    float64 intensity = 1.0;
    float64 range = 0.0;
    if (GetString(light, "type", type) != Field::Ok || GetNumbers(light, "color", colour) == Field::Invalid ||
        GetNumber(light, "intensity", intensity) == Field::Invalid ||
        GetNumber(light, "range", range) == Field::Invalid)
    {
        fail(codes::MALFORMED_INPUT, "KHR_lights_punctual light has an invalid type, color, intensity or range");
        return Status::Ok;
    }
    IrAnalyticLight analytic;
    analytic.SourceFile = 0;
    // Point and spot intensity is candela, directional illuminance is lux; colour is a
    // linear RGB multiplier. The cook normalises Strength by its luminance, so Scale
    // carries intensity x luminance to keep colour x intensity.
    analytic.Strength = math::Vector3d{colour[0], colour[1], colour[2]};
    analytic.Scale = math::Max(intensity, 0.0) * ComputeRec709Luminance(colour);
    analytic.RangeMetres = range;
    analytic.PositionWorld = TransformPoint(world, math::Vector3d{0.0, 0.0, 0.0});
    const math::Vector3d direction = TransformDirection(world, math::Vector3d{0.0, 0.0, -1.0});
    const float64 length = math::Length(direction);
    analytic.DirectionWorld = length > 0.0 ? direction * (1.0 / length) : math::Vector3d{0.0, 0.0, -1.0};
    if (type == "directional")
    {
        analytic.Kind = IrLightKind::Directional;
        ++mScene.Lights.Distant;
    }
    else if (type == "point")
    {
        analytic.Kind = IrLightKind::Point;
        ++mScene.Lights.Point;
    }
    else if (type == "spot")
    {
        analytic.Kind = IrLightKind::Spot;
        ++mScene.Lights.Spot;
        float64 inner = 0.0;
        float64 outer = DEFAULT_OUTER_CONE_RADIANS;
        const JsonValue spot = light.Get("spot");
        if (spot.Valid() == true)
        {
            (void)GetNumber(spot, "innerConeAngle", inner);
            (void)GetNumber(spot, "outerConeAngle", outer);
        }
        analytic.OuterAngleDegrees =
            math::Clamp(math::RadiansToDegrees(outer), MIN_OUTER_CONE_DEGREES, MAX_OUTER_CONE_DEGREES);
        analytic.InnerAngleDegrees = math::Clamp(math::RadiansToDegrees(inner), 0.0, analytic.OuterAngleDegrees);
        if (analytic.InnerAngleDegrees >= analytic.OuterAngleDegrees - MIN_CONE_SEPARATION_DEGREES)
        {
            analytic.InnerAngleDegrees = math::Max(analytic.OuterAngleDegrees - MIN_CONE_SEPARATION_DEGREES, 0.0);
            warn(codes::LIGHT_APPROXIMATED, "spot cone angles narrowed so the inner cone is strictly inside");
        }
    }
    else
    {
        ++mScene.Lights.Other;
        warn(codes::LIGHT_SKIPPED, "unknown KHR_lights_punctual type skipped");
        return Status::Ok;
    }
    if (range > 0.0)
    {
        inform(codes::LIGHT_APPROXIMATED, "light range not stored; the cook derives one range from the scene extent");
    }
    return mScene.AnalyticLights.TryAdd(analytic) == true ? Status::Ok : Status::OutOfMemory;
}

Status GltfReader::visitNode(usize node, const math::Matrix4d& world) noexcept
{
    // Explicit stack: (node, parent world) pairs, depth-first in declaration order.
    struct Frame final
    {
        usize Node = 0;
        uint32 Depth = 0;
        math::Matrix4d Parent = math::Matrix4d::Identity();
    };
    Array<Frame> stack;
    Frame first;
    first.Node = node;
    first.Parent = world;
    if (stack.TryAdd(first) == false)
    {
        return Status::OutOfMemory;
    }
    const JsonValue nodes = mRoot.Get("nodes");
    while (stack.IsEmpty() == false)
    {
        const Frame frame = stack.GetLast();
        stack.RemoveLast();
        if (IsCancelled(mCancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        if (frame.Node >= mIsNodeVisited.GetSize())
        {
            fail(codes::MALFORMED_INPUT, "node index out of range");
            continue;
        }
        if (mIsNodeVisited[frame.Node] != 0)
        {
            failAt("nodes", frame.Node, "node has more than one parent or is part of a cycle");
            continue;
        }
        mIsNodeVisited[frame.Node] = 1;
        if (frame.Depth >= MAX_NODE_DEPTH)
        {
            failAt("nodes", frame.Node, "hierarchy exceeds the depth limit");
            continue;
        }
        const JsonValue object = nodes.At(frame.Node);
        math::Matrix4d local = math::Matrix4d::Identity();
        if (getLocalTransform(frame.Node, object, local) == false)
        {
            continue;
        }
        const math::Matrix4d current = MultiplyMatrices(frame.Parent, local);
        if (IsMatrixFinite(current) == false)
        {
            failAt("nodes", frame.Node, "world transform is not finite");
            continue;
        }
        uint64 meshIndex = 0;
        if (GetUnsigned(object, "mesh", meshIndex) == Field::Ok)
        {
            if (meshIndex >= mMeshes.GetSize())
            {
                failAt("nodes", frame.Node, "mesh index out of range");
            }
            else if (mPlacements.GetSize() >= MAX_PLACEMENTS)
            {
                ++mDroppedPlacements;
            }
            else
            {
                MeshPlacement placement;
                placement.Mesh = static_cast<uint32>(meshIndex);
                placement.Node = static_cast<uint32>(frame.Node);
                placement.Transform = current;
                if (mPlacements.TryAdd(placement) == false)
                {
                    return Status::OutOfMemory;
                }
                ++mMeshes[static_cast<usize>(meshIndex)].References;
            }
        }
        if (object.Get("skin").Valid() == true)
        {
            inform(codes::ANIMATION_IGNORED, "skinned node imported in its bind pose without skinning");
        }
        const JsonValue camera = object.Get("camera");
        if (camera.Valid() == true)
        {
            readCamera(camera, current);
        }
        const JsonValue extensions = object.Get("extensions");
        const JsonValue lightLink = extensions.Valid() == true ? extensions.Get("KHR_lights_punctual") : JsonValue{};
        if (lightLink.Valid() == true)
        {
            const Status lit = readLight(lightLink.Get("light"), current);
            if (lit != Status::Ok)
            {
                return lit;
            }
        }
        const JsonValue children = object.Get("children");
        // Push in reverse so children are visited in declaration order.
        for (usize i = children.Valid() == true ? children.Count() : 0; i > 0; --i)
        {
            uint64 child = 0;
            if (children.At(i - 1).Integer(child) == false || child >= mIsNodeVisited.GetSize())
            {
                failAt("nodes", frame.Node, "child index out of range");
                continue;
            }
            Frame next;
            next.Node = static_cast<usize>(child);
            next.Depth = frame.Depth + 1;
            next.Parent = current;
            if (stack.TryAdd(next) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    return Status::Ok;
}

Status GltfReader::traverseScene() noexcept
{
    const usize sceneCount = CountArray(mRoot, "scenes");
    uint64 sceneIndex = 0;
    const Field hasScene = GetUnsigned(mRoot, "scene", sceneIndex);
    if (hasScene == Field::Invalid || (hasScene == Field::Ok && sceneIndex >= sceneCount))
    {
        fail(codes::MALFORMED_INPUT, "scene index out of range");
        return Status::Ok;
    }
    if (sceneCount > 1)
    {
        inform(codes::DIRECTIVE_IGNORED, "only the default scene is imported");
    }
    const math::Matrix4d identity = math::Matrix4d::Identity();
    if (sceneCount != 0)
    {
        const JsonValue roots = mRoot.Get("scenes").At(static_cast<usize>(sceneIndex)).Get("nodes");
        for (usize i = 0; roots.Valid() == true && i < roots.Count(); ++i)
        {
            uint64 node = 0;
            if (roots.At(i).Integer(node) == false || node >= mIsNodeVisited.GetSize())
            {
                fail(codes::MALFORMED_INPUT, "scene root node index out of range");
                continue;
            }
            const Status status = visitNode(static_cast<usize>(node), identity);
            if (status != Status::Ok)
            {
                return status;
            }
        }
        return Status::Ok;
    }
    // No scenes: every node without a parent is a root.
    inform(codes::DIRECTIVE_IGNORED, "the asset declares no scene; all root nodes are imported");
    Array<uint8> hasParent;
    if (hasParent.TryResize(mIsNodeVisited.GetSize(), uint8{0}) == false)
    {
        return Status::OutOfMemory;
    }
    const JsonValue nodes = mRoot.Get("nodes");
    for (usize n = 0; n < hasParent.GetSize(); ++n)
    {
        const JsonValue children = nodes.At(n).Get("children");
        for (usize i = 0; children.Valid() == true && i < children.Count(); ++i)
        {
            uint64 child = 0;
            if (children.At(i).Integer(child) == true && child < hasParent.GetSize())
            {
                hasParent[static_cast<usize>(child)] = 1;
            }
        }
    }
    for (usize n = 0; n < hasParent.GetSize(); ++n)
    {
        if (hasParent[n] != 0)
        {
            continue;
        }
        const Status status = visitNode(n, identity);
        if (status != Status::Ok)
        {
            return status;
        }
    }
    return Status::Ok;
}

Status GltfReader::placeMeshes() noexcept
{
    for (const MeshPlacement& placement : mPlacements)
    {
        const Status loaded = loadMesh(placement.Mesh);
        if (loaded != Status::Ok)
        {
            return loaded;
        }
        LoadedMesh& mesh = mMeshes[placement.Mesh];
        // A mesh placed by several nodes stays instanced: one IR object, one instance per node.
        if (mesh.References > 1 && mesh.Object == IR_NONE)
        {
            IrObject object;
            MessageBuilder name;
            (void)name.Append("meshes[").AppendUnsigned(placement.Mesh).Append("]");
            if (object.Name.TryAssign(name.GetView()) != StringStatus::Ok)
            {
                return Status::OutOfMemory;
            }
            for (uint32 p = 0; p < mesh.PrimitiveCount; ++p)
            {
                const LoadedPrimitive& primitive = mPrimitives[mesh.FirstPrimitive + p];
                IrShape shape;
                shape.Mesh = primitive.Mesh;
                shape.Material = primitive.Material;
                shape.SourceFile = 0;
                ApplyMaterialEmission(mScene.Materials[primitive.Material],
                                      mScene.Materials[primitive.Material].IsDoubleSided,
                                      shape);
                if (object.Shapes.TryAdd(static_cast<uint32>(mScene.Shapes.GetSize())) == false ||
                    mScene.Shapes.TryAdd(shape) == false)
                {
                    return Status::OutOfMemory;
                }
            }
            mesh.Object = static_cast<uint32>(mScene.Objects.GetSize());
            if (mScene.Objects.TryAdd(static_cast<IrObject&&>(object)) == false)
            {
                return Status::OutOfMemory;
            }
        }
        if (mesh.Object != IR_NONE)
        {
            IrInstance instance;
            instance.Object = mesh.Object;
            instance.Transform = placement.Transform;
            if (mScene.Instances.TryAdd(instance) == false)
            {
                return Status::OutOfMemory;
            }
            continue;
        }
        for (uint32 p = 0; p < mesh.PrimitiveCount; ++p)
        {
            const LoadedPrimitive& primitive = mPrimitives[mesh.FirstPrimitive + p];
            IrShape shape;
            shape.Mesh = primitive.Mesh;
            shape.Material = primitive.Material;
            shape.Transform = placement.Transform;
            shape.SourceFile = 0;
            ApplyMaterialEmission(mScene.Materials[primitive.Material],
                                  mScene.Materials[primitive.Material].IsDoubleSided,
                                  shape);
            if (mScene.WorldShapes.TryAdd(static_cast<uint32>(mScene.Shapes.GetSize())) == false ||
                mScene.Shapes.TryAdd(shape) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    if (mDroppedPlacements != 0)
    {
        warn(codes::INSTANCES_TRUNCATED, "mesh placements beyond the placement limit dropped");
    }
    // Emissive materials in use count as area-light sources in the report.
    Array<uint8> isCounted;
    if (isCounted.TryResize(mScene.Materials.GetSize(), uint8{0}) == false)
    {
        return Status::OutOfMemory;
    }
    for (const IrShape& shape : mScene.Shapes)
    {
        if (shape.Material != IR_NONE && isCounted[shape.Material] == 0 &&
            ComputeRec709Luminance(mScene.Materials[shape.Material].Emission) > 0.0)
        {
            isCounted[shape.Material] = 1;
            ++mScene.Lights.Area;
        }
    }
    return Status::Ok;
}

Status GltfReader::Read(std::span<const uint8> entryBytes) noexcept
{
    std::string_view json;
    Status status = readContainer(entryBytes, json);
    if (status != Status::Ok)
    {
        return status;
    }
    JsonLimits limits;
    limits.MaxInputBytes = MAX_JSON_BYTES;
    limits.MaxWorkspaceBytes = MAX_JSON_WORKSPACE_BYTES;
    limits.MaxDepth = foundation::parsing::MAX_JSON_DEPTH;
    limits.MaxObjectMembers = foundation::parsing::MAX_JSON_OBJECT_MEMBERS;
    limits.MaxArrayElements = MAX_ARRAY_ENTRIES;
    limits.MaxValues = MAX_JSON_BYTES;
    limits.MaxStringBytes = MAX_JSON_BYTES;
    ParseError error;
    const ParseStatus parsed = mDocument.Read(json, error, limits);
    if (parsed != ParseStatus::Ok)
    {
        if (parsed == ParseStatus::OutOfMemory)
        {
            return Status::OutOfMemory;
        }
        MessageBuilder message;
        (void)message.Append(parsed == ParseStatus::LimitExceeded ? "glTF JSON exceeds a parser limit"
                                                                  : "glTF JSON is malformed");
        if (error.Offset != foundation::parsing::UNKNOWN_BYTE_OFFSET)
        {
            (void)message.Append(" at byte ").AppendUnsigned(error.Offset);
        }
        fail(parsed == ParseStatus::LimitExceeded ? codes::LIMIT_EXCEEDED : codes::MALFORMED_INPUT, message.GetView());
        return Status::Invalid;
    }
    mRoot = mDocument.Root();
    if (mRoot.Object() == false)
    {
        fail(codes::MALFORMED_INPUT, "glTF JSON root is not an object");
        return Status::Invalid;
    }
    if (checkAssetAndExtensions() == false)
    {
        return Status::Invalid;
    }
    if (mBuffers.TryResize(CountArray(mRoot, "buffers")) == false ||
        mMaterials.TryResize(CountArray(mRoot, "materials"), IR_NONE) == false ||
        mSrgbTextures.TryResize(CountArray(mRoot, "textures"), IR_NONE) == false ||
        mLinearTextures.TryResize(CountArray(mRoot, "textures"), IR_NONE) == false ||
        mEmbeddedImages.TryResize(CountArray(mRoot, "images"), IR_NONE) == false ||
        mMeshes.TryResize(CountArray(mRoot, "meshes")) == false ||
        mIsNodeVisited.TryResize(CountArray(mRoot, "nodes"), uint8{0}) == false)
    {
        return Status::OutOfMemory;
    }
    status = traverseScene();
    if (status == Status::Ok)
    {
        status = placeMeshes();
    }
    if (status == Status::Ok && mHasOverflowed == true)
    {
        status = Status::OutOfMemory;
    }
    return status;
}
} // namespace

Status ReadScene(std::span<const uint8> entryBytes,
                 std::string_view entryName,
                 SourceResolver& inoutResolver,
                 SceneIr& outScene,
                 DiagnosticLog& inoutLog,
                 const Cancellation* cancelOrNull) noexcept
{
    SceneIr freshScene;
    outScene = static_cast<SceneIr&&>(freshScene);
    if (AddSourceFile(outScene, entryName, Hash(entryBytes)) == false)
    {
        return Status::OutOfMemory;
    }
    GltfReader reader(inoutResolver, outScene, inoutLog, cancelOrNull);
    Status status = reader.Read(entryBytes);
    if (status == Status::Ok && outScene.Shapes.IsEmpty() == true && inoutLog.HasErrors() == false)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Error,
                           codes::LIMIT_EXCEEDED,
                           entryName,
                           0,
                           "the glTF scene places no triangle meshes");
    }
    if (status == Status::Ok && inoutLog.HasErrors() == true)
    {
        status = Status::Invalid;
    }
    return status;
}
} // namespace ludus::content::scene_cook::importers::gltf
