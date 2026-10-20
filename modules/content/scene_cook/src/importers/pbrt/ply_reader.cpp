#include "ply_reader.h"

#include "pbrt_tokenizer.h"

#include <ludus/foundation/math/scalar.hpp>

#include <bit>
#include <cstring>

namespace ludus::content::scene_cook::importers
{
namespace
{
using foundation::core::int16;
using foundation::core::int8;
using foundation::core::uint16;
using foundation::core::uint64;

enum class PlyFormat : uint8
{
    Ascii,
    BinaryLittleEndian,
    BinaryBigEndian,
};

enum class ScalarType : uint8
{
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float32,
    Float64,
};

/// Destination of a vertex property.
enum class VertexSlot : uint8
{
    None,
    X,
    Y,
    Z,
    NormalX,
    NormalY,
    NormalZ,
    U,
    V,
};

struct Property final
{
    ScalarType Type = ScalarType::Float32;
    bool IsList = false;
    ScalarType CountType = ScalarType::Uint8;
    VertexSlot Slot = VertexSlot::None;
    bool IsVertexIndices = false;
};

constexpr usize MAX_PROPERTIES = 32;
constexpr usize MAX_ELEMENTS = 8;
constexpr uint32 MAX_POLYGON_VERTICES = 64;

struct Element final
{
    std::string_view Name;
    uint64 Count = 0;
    Property Properties[MAX_PROPERTIES];
    usize PropertyCount = 0;
};

struct Header final
{
    PlyFormat Format = PlyFormat::Ascii;
    Element Elements[MAX_ELEMENTS];
    usize ElementCount = 0;
    usize BodyOffset = 0;
};

[[nodiscard]] bool ParseScalarType(std::string_view text, ScalarType& outType) noexcept
{
    struct Name final
    {
        std::string_view Text;
        ScalarType Type;
    };
    static constexpr Name NAMES[] = {
        {"char", ScalarType::Int8},
        {"int8", ScalarType::Int8},
        {"uchar", ScalarType::Uint8},
        {"uint8", ScalarType::Uint8},
        {"short", ScalarType::Int16},
        {"int16", ScalarType::Int16},
        {"ushort", ScalarType::Uint16},
        {"uint16", ScalarType::Uint16},
        {"int", ScalarType::Int32},
        {"int32", ScalarType::Int32},
        {"uint", ScalarType::Uint32},
        {"uint32", ScalarType::Uint32},
        {"float", ScalarType::Float32},
        {"float32", ScalarType::Float32},
        {"double", ScalarType::Float64},
        {"float64", ScalarType::Float64},
    };
    for (const Name& name : NAMES)
    {
        if (name.Text == text)
        {
            outType = name.Type;
            return true;
        }
    }
    return false;
}

[[nodiscard]] usize GetScalarBytes(ScalarType type) noexcept
{
    switch (type)
    {
        case ScalarType::Int8:
        case ScalarType::Uint8:
            return 1;
        case ScalarType::Int16:
        case ScalarType::Uint16:
            return 2;
        case ScalarType::Int32:
        case ScalarType::Uint32:
        case ScalarType::Float32:
            return 4;
        case ScalarType::Float64:
            return 8;
    }
    return 0;
}

[[nodiscard]] VertexSlot ClassifyVertexProperty(std::string_view name) noexcept
{
    struct Name final
    {
        std::string_view Text;
        VertexSlot Slot;
    };
    static constexpr Name NAMES[] = {
        {"x", VertexSlot::X},
        {"y", VertexSlot::Y},
        {"z", VertexSlot::Z},
        {"nx", VertexSlot::NormalX},
        {"ny", VertexSlot::NormalY},
        {"nz", VertexSlot::NormalZ},
        {"u", VertexSlot::U},
        {"v", VertexSlot::V},
        {"s", VertexSlot::U},
        {"t", VertexSlot::V},
        {"texture_u", VertexSlot::U},
        {"texture_v", VertexSlot::V},
        {"texture_s", VertexSlot::U},
        {"texture_t", VertexSlot::V},
    };
    for (const Name& entry : NAMES)
    {
        if (entry.Text == name)
        {
            return entry.Slot;
        }
    }
    return VertexSlot::None;
}

/// Split the next whitespace-separated word from inoutLine.
[[nodiscard]] std::string_view TakeWord(std::string_view& inoutLine) noexcept
{
    usize start = 0;
    while (start < inoutLine.size() &&
           (inoutLine[start] == ' ' || inoutLine[start] == '\t' || inoutLine[start] == '\r'))
    {
        ++start;
    }
    usize end = start;
    while (end < inoutLine.size() && inoutLine[end] != ' ' && inoutLine[end] != '\t' && inoutLine[end] != '\r')
    {
        ++end;
    }
    const std::string_view word = inoutLine.substr(start, end - start);
    inoutLine.remove_prefix(end);
    return word;
}

[[nodiscard]] bool ParseCount(std::string_view text, uint64& outValue) noexcept
{
    if (text.empty() == true || text.size() > 19)
    {
        return false;
    }
    uint64 value = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        value = value * 10 + static_cast<uint64>(digit - '0');
    }
    outValue = value;
    return true;
}

[[nodiscard]] bool ParseHeader(std::span<const uint8> data, Header& outHeader, std::string_view& outMessage) noexcept
{
    const std::string_view text(reinterpret_cast<const char*>(data.data()),
                                data.size() < MAX_PLY_HEADER_BYTES ? data.size() : MAX_PLY_HEADER_BYTES);
    usize at = 0;
    usize lineNumber = 0;
    bool hasFormat = false;
    while (true)
    {
        const usize newline = text.find('\n', at);
        if (newline == std::string_view::npos)
        {
            outMessage = "PLY header is unterminated or too large";
            return false;
        }
        std::string_view line = text.substr(at, newline - at);
        at = newline + 1;
        ++lineNumber;
        const std::string_view keyword = TakeWord(line);
        if (lineNumber == 1)
        {
            if (keyword != "ply")
            {
                outMessage = "missing PLY magic";
                return false;
            }
            continue;
        }
        if (keyword == "end_header")
        {
            break;
        }
        if (keyword == "comment" || keyword == "obj_info" || keyword.empty() == true)
        {
            continue;
        }
        if (keyword == "format")
        {
            const std::string_view name = TakeWord(line);
            if (name == "ascii")
            {
                outHeader.Format = PlyFormat::Ascii;
            }
            else if (name == "binary_little_endian")
            {
                outHeader.Format = PlyFormat::BinaryLittleEndian;
            }
            else if (name == "binary_big_endian")
            {
                outHeader.Format = PlyFormat::BinaryBigEndian;
            }
            else
            {
                outMessage = "unknown PLY format";
                return false;
            }
            hasFormat = true;
            continue;
        }
        if (keyword == "element")
        {
            if (outHeader.ElementCount == MAX_ELEMENTS)
            {
                outMessage = "too many PLY elements";
                return false;
            }
            Element& element = outHeader.Elements[outHeader.ElementCount++];
            element.Name = TakeWord(line);
            if (ParseCount(TakeWord(line), element.Count) == false)
            {
                outMessage = "invalid PLY element count";
                return false;
            }
            continue;
        }
        if (keyword == "property")
        {
            if (outHeader.ElementCount == 0)
            {
                outMessage = "PLY property before element";
                return false;
            }
            Element& element = outHeader.Elements[outHeader.ElementCount - 1];
            if (element.PropertyCount == MAX_PROPERTIES)
            {
                outMessage = "too many PLY properties";
                return false;
            }
            Property& property = element.Properties[element.PropertyCount++];
            std::string_view typeName = TakeWord(line);
            if (typeName == "list")
            {
                property.IsList = true;
                if (ParseScalarType(TakeWord(line), property.CountType) == false ||
                    property.CountType == ScalarType::Float32 || property.CountType == ScalarType::Float64)
                {
                    outMessage = "invalid PLY list count type";
                    return false;
                }
                typeName = TakeWord(line);
            }
            if (ParseScalarType(typeName, property.Type) == false)
            {
                outMessage = "unknown PLY property type";
                return false;
            }
            const std::string_view name = TakeWord(line);
            if (element.Name == "vertex" && property.IsList == false)
            {
                property.Slot = ClassifyVertexProperty(name);
            }
            property.IsVertexIndices = element.Name == "face" && property.IsList == true &&
                                       (name == "vertex_indices" || name == "vertex_index");
            continue;
        }
        outMessage = "unknown PLY header keyword";
        return false;
    }
    if (hasFormat == false)
    {
        outMessage = "missing PLY format";
        return false;
    }
    outHeader.BodyOffset = at;
    return true;
}

void Summarise(const Header& header, PlyHeaderSummary& outSummary) noexcept
{
    outSummary = PlyHeaderSummary{};
    for (usize e = 0; e < header.ElementCount; ++e)
    {
        const Element& element = header.Elements[e];
        if (element.Name == "vertex")
        {
            outSummary.VertexCount = element.Count > 0xffffffffULL ? 0xffffffffU : static_cast<uint32>(element.Count);
            for (usize p = 0; p < element.PropertyCount; ++p)
            {
                const VertexSlot slot = element.Properties[p].Slot;
                outSummary.HasNormals = outSummary.HasNormals == true || slot == VertexSlot::NormalX;
                outSummary.HasUvs = outSummary.HasUvs == true || slot == VertexSlot::U;
            }
        }
        else if (element.Name == "face")
        {
            outSummary.FaceCount = element.Count > 0xffffffffULL ? 0xffffffffU : static_cast<uint32>(element.Count);
            for (usize p = 0; p < element.PropertyCount; ++p)
            {
                outSummary.HasExtraFaceProperties =
                    outSummary.HasExtraFaceProperties == true || (element.Properties[p].IsList == false);
            }
        }
    }
}

/// Assemble a binary scalar of `size` bytes (1, 2, 4 or 8) in the body's byte order.
[[nodiscard]] uint64 LoadRaw(const uint8* bytes, usize size, bool isLittleEndian) noexcept
{
    uint64 raw = 0;
    if (isLittleEndian == true)
    {
        for (usize i = size; i != 0; --i)
        {
            raw = (raw << 8) | bytes[i - 1];
        }
        return raw;
    }
    for (usize i = 0; i < size; ++i)
    {
        raw = (raw << 8) | bytes[i];
    }
    return raw;
}

/// Interpret raw scalar bits as type and widen to float64.
[[nodiscard]] float64 ConvertRaw(ScalarType type, uint64 raw) noexcept
{
    switch (type)
    {
        case ScalarType::Int8:
            return static_cast<int8>(static_cast<uint8>(raw));
        case ScalarType::Uint8:
        case ScalarType::Uint16:
        case ScalarType::Uint32:
            return static_cast<float64>(raw);
        case ScalarType::Int16:
            return static_cast<int16>(static_cast<uint16>(raw));
        case ScalarType::Int32:
            return static_cast<int32>(static_cast<uint32>(raw));
        case ScalarType::Float32:
            return static_cast<float64>(std::bit_cast<float32>(static_cast<uint32>(raw)));
        case ScalarType::Float64:
            return std::bit_cast<float64>(raw);
    }
    return 0.0;
}

/// Body cursor that reads scalars in either binary byte order or as ASCII words.
class BodyReader final
{
public:
    BodyReader(std::span<const uint8> body, PlyFormat format) noexcept : mBody(body), mFormat(format) {}

    [[nodiscard]] bool IsBinary() const noexcept
    {
        return mFormat != PlyFormat::Ascii;
    }

    [[nodiscard]] bool IsLittleEndian() const noexcept
    {
        return mFormat == PlyFormat::BinaryLittleEndian;
    }

    [[nodiscard]] usize GetRemainingBytes() const noexcept
    {
        return mBody.size() - mOffset;
    }

    /// Consume count binary bytes; nullptr (nothing consumed) when the body is shorter.
    [[nodiscard]] const uint8* TakeOrNull(usize count) noexcept
    {
        if (mBody.size() - mOffset < count)
        {
            return nullptr;
        }
        const uint8* bytes = mBody.data() + mOffset;
        mOffset += count;
        return bytes;
    }

    /// Unconsumed bytes; valid for GetRemainingBytes() bytes.
    [[nodiscard]] const uint8* PeekRemaining() const noexcept
    {
        return mBody.data() + mOffset;
    }

    [[nodiscard]] bool ReadScalar(ScalarType type, float64& outValue) noexcept
    {
        if (mFormat == PlyFormat::Ascii)
        {
            return readAscii(outValue);
        }
        const usize size = GetScalarBytes(type);
        const uint8* bytes = TakeOrNull(size);
        if (bytes == nullptr)
        {
            return false;
        }
        outValue = ConvertRaw(type, LoadRaw(bytes, size, IsLittleEndian()));
        return true;
    }

private:
    [[nodiscard]] bool readAscii(float64& outValue) noexcept
    {
        while (mOffset < mBody.size() &&
               (mBody[mOffset] == ' ' || mBody[mOffset] == '\t' || mBody[mOffset] == '\n' || mBody[mOffset] == '\r'))
        {
            ++mOffset;
        }
        const usize start = mOffset;
        while (mOffset < mBody.size() && mBody[mOffset] != ' ' && mBody[mOffset] != '\t' && mBody[mOffset] != '\n' &&
               mBody[mOffset] != '\r')
        {
            ++mOffset;
        }
        if (mOffset == start)
        {
            return false;
        }
        return ParseNumber(std::string_view(reinterpret_cast<const char*>(mBody.data() + start), mOffset - start),
                           outValue);
    }

    std::span<const uint8> mBody;
    PlyFormat mFormat;
    usize mOffset = 0;
};

[[nodiscard]] bool IsValidIndex(float64 value, uint64 vertexCount) noexcept
{
    return value >= 0.0 && value < static_cast<float64>(vertexCount) && math::Floor(value) == value;
}

/// Store one decoded vertex row (slots indexed by VertexSlot) into the mesh.
void StoreVertex(const float64 (&slots)[9], usize vertex, IrMesh& inoutMesh) noexcept
{
    for (usize k = 0; k < 3; ++k)
    {
        inoutMesh.Positions[vertex * 3 + k] = static_cast<float32>(slots[1 + k]);
        if (inoutMesh.Normals.IsEmpty() == false)
        {
            inoutMesh.Normals[vertex * 3 + k] = static_cast<float32>(slots[4 + k]);
        }
    }
    if (inoutMesh.Uvs.IsEmpty() == false)
    {
        inoutMesh.Uvs[vertex * 2] = static_cast<float32>(slots[7]);
        inoutMesh.Uvs[vertex * 2 + 1] = static_cast<float32>(1.0 - slots[8]);
    }
}

[[nodiscard]] bool HasOnlyScalarProperties(const Element& element) noexcept
{
    for (usize p = 0; p < element.PropertyCount; ++p)
    {
        if (element.Properties[p].IsList == true)
        {
            return false;
        }
    }
    return true;
}

/// Binary rows of fixed width: bounds-check a whole row, then decode it with the
/// same conversions as BodyReader::ReadScalar.
[[nodiscard]] bool ReadScalarRows(BodyReader& inoutReader,
                                  const Element& element,
                                  bool isVertex,
                                  IrMesh& inoutMesh,
                                  std::string_view& outMessage) noexcept
{
    usize rowBytes = 0;
    for (usize p = 0; p < element.PropertyCount; ++p)
    {
        rowBytes += GetScalarBytes(element.Properties[p].Type);
    }
    const bool isLittleEndian = inoutReader.IsLittleEndian();
    for (uint64 row = 0; row < element.Count; ++row)
    {
        const uint8* bytes = inoutReader.TakeOrNull(rowBytes);
        if (bytes == nullptr)
        {
            outMessage = "PLY body is truncated or malformed";
            return false;
        }
        if (isVertex == false)
        {
            continue;
        }
        float64 slots[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        for (usize p = 0; p < element.PropertyCount; ++p)
        {
            const Property& property = element.Properties[p];
            const usize size = GetScalarBytes(property.Type);
            slots[static_cast<usize>(property.Slot)] = ConvertRaw(property.Type, LoadRaw(bytes, size, isLittleEndian));
            bytes += size;
        }
        StoreVertex(slots, static_cast<usize>(row), inoutMesh);
    }
    return true;
}

[[nodiscard]] bool IsSingleIndexList(const Element& element) noexcept
{
    return element.PropertyCount == 1 && element.Properties[0].IsList == true &&
           element.Properties[0].IsVertexIndices == true;
}

/// Binary face rows holding only a vertex index list: bounds-check each polygon
/// once, then validate and fan-triangulate exactly as the general row loop does.
[[nodiscard]] bool ReadIndexLists(BodyReader& inoutReader,
                                  const Element& element,
                                  bool hasVertices,
                                  uint64 vertexCount,
                                  IrMesh& inoutMesh,
                                  std::string_view& outMessage) noexcept
{
    const Property& property = element.Properties[0];
    const usize countBytes = GetScalarBytes(property.CountType);
    const usize indexBytes = GetScalarBytes(property.Type);
    const bool isLittleEndian = inoutReader.IsLittleEndian();
    // Triangles are the common case; a failed reservation only loses the hint.
    const uint64 boundedFaces = inoutReader.GetRemainingBytes() / (countBytes + 3 * indexBytes);
    const uint64 expectedFaces = element.Count < boundedFaces ? element.Count : boundedFaces;
    (void)inoutMesh.Indices.TryEnsureCapacity(inoutMesh.Indices.GetSize() + static_cast<usize>(expectedFaces) * 3);
    for (uint64 row = 0; row < element.Count; ++row)
    {
        const uint8* countData = inoutReader.TakeOrNull(countBytes);
        const float64 countValue = countData == nullptr
                                       ? -1.0
                                       : ConvertRaw(property.CountType, LoadRaw(countData, countBytes, isLittleEndian));
        if (countValue < 0.0 || countValue > MAX_POLYGON_VERTICES)
        {
            outMessage = "PLY list count out of range";
            return false;
        }
        const uint32 count = static_cast<uint32>(countValue);
        const uint8* indexData = inoutReader.TakeOrNull(count * indexBytes);
        // A short polygon still validates its complete indices first, as the row loop would.
        const uint32 available =
            indexData != nullptr ? count : static_cast<uint32>(inoutReader.GetRemainingBytes() / indexBytes);
        if (indexData == nullptr)
        {
            indexData = inoutReader.PeekRemaining();
        }
        uint32 polygon[MAX_POLYGON_VERTICES]{};
        for (uint32 k = 0; k < available; ++k)
        {
            const float64 value =
                ConvertRaw(property.Type, LoadRaw(indexData + k * indexBytes, indexBytes, isLittleEndian));
            if (hasVertices == false || IsValidIndex(value, vertexCount) == false)
            {
                outMessage = "PLY face index out of range";
                return false;
            }
            polygon[k] = static_cast<uint32>(value);
        }
        if (available < count)
        {
            outMessage = "PLY body is truncated or malformed";
            return false;
        }
        for (uint32 k = 2; k < count; ++k)
        {
            if (inoutMesh.Indices.TryAdd(polygon[0]) == false || inoutMesh.Indices.TryAdd(polygon[k - 1]) == false ||
                inoutMesh.Indices.TryAdd(polygon[k]) == false)
            {
                outMessage = "out of memory";
                return false;
            }
        }
    }
    return true;
}
} // namespace

bool ReadPlyHeader(std::span<const uint8> data, PlyHeaderSummary& outSummary, std::string_view& outMessage) noexcept
{
    Header header;
    if (ParseHeader(data, header, outMessage) == false)
    {
        return false;
    }
    Summarise(header, outSummary);
    return true;
}

bool ReadPly(std::span<const uint8> data,
             IrMesh& outMesh,
             PlyHeaderSummary& outSummary,
             std::string_view& outMessage) noexcept
{
    Header header;
    if (ParseHeader(data, header, outMessage) == false)
    {
        return false;
    }
    Summarise(header, outSummary);
    outMesh = IrMesh{};
    BodyReader reader(data.subspan(header.BodyOffset), header.Format);
    uint64 vertexCount = 0;
    bool hasVertices = false;
    for (usize e = 0; e < header.ElementCount; ++e)
    {
        const Element& element = header.Elements[e];
        const bool isVertex = element.Name == "vertex";
        const bool isFace = element.Name == "face";
        if (isVertex == true)
        {
            if (element.Count == 0 || element.Count > 0x7fffffffULL)
            {
                outMessage = "PLY vertex count out of range";
                return false;
            }
            // Every stored property takes at least one byte (two in ASCII: a digit and a
            // separator), which bounds hostile counts before allocating.
            const uint64 bytesPerValue = header.Format == PlyFormat::Ascii ? 2 : 1;
            const uint64 minimumRowBytes = bytesPerValue * (element.PropertyCount == 0 ? 1 : element.PropertyCount);
            if (element.Count > (data.size() - header.BodyOffset) / minimumRowBytes)
            {
                outMessage = "PLY body is truncated";
                return false;
            }
            vertexCount = element.Count;
            hasVertices = true;
            const usize count = static_cast<usize>(element.Count);
            const bool hasNormals = outSummary.HasNormals;
            const bool hasUvs = outSummary.HasUvs;
            if (outMesh.Positions.TryResize(count * 3) == false ||
                (hasNormals == true && outMesh.Normals.TryResize(count * 3) == false) ||
                (hasUvs == true && outMesh.Uvs.TryResize(count * 2) == false))
            {
                outMessage = "out of memory";
                return false;
            }
        }
        if (reader.IsBinary() == true && HasOnlyScalarProperties(element) == true)
        {
            if (ReadScalarRows(reader, element, isVertex, outMesh, outMessage) == false)
            {
                return false;
            }
            continue;
        }
        if (reader.IsBinary() == true && isFace == true && IsSingleIndexList(element) == true)
        {
            if (ReadIndexLists(reader, element, hasVertices, vertexCount, outMesh, outMessage) == false)
            {
                return false;
            }
            continue;
        }
        for (uint64 row = 0; row < element.Count; ++row)
        {
            float64 slots[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            for (usize p = 0; p < element.PropertyCount; ++p)
            {
                const Property& property = element.Properties[p];
                if (property.IsList == false)
                {
                    float64 value = 0.0;
                    if (reader.ReadScalar(property.Type, value) == false)
                    {
                        outMessage = "PLY body is truncated or malformed";
                        return false;
                    }
                    slots[static_cast<usize>(property.Slot)] = value;
                    continue;
                }
                float64 countValue = 0.0;
                if (reader.ReadScalar(property.CountType, countValue) == false || countValue < 0.0 ||
                    countValue > MAX_POLYGON_VERTICES)
                {
                    outMessage = "PLY list count out of range";
                    return false;
                }
                const uint32 count = static_cast<uint32>(countValue);
                uint32 polygon[MAX_POLYGON_VERTICES]{};
                for (uint32 k = 0; k < count; ++k)
                {
                    float64 value = 0.0;
                    if (reader.ReadScalar(property.Type, value) == false)
                    {
                        outMessage = "PLY body is truncated or malformed";
                        return false;
                    }
                    if (isFace == true && property.IsVertexIndices == true)
                    {
                        if (hasVertices == false || IsValidIndex(value, vertexCount) == false)
                        {
                            outMessage = "PLY face index out of range";
                            return false;
                        }
                        polygon[k] = static_cast<uint32>(value);
                    }
                }
                if (isFace == true && property.IsVertexIndices == true)
                {
                    for (uint32 k = 2; k < count; ++k)
                    {
                        if (outMesh.Indices.TryAdd(polygon[0]) == false ||
                            outMesh.Indices.TryAdd(polygon[k - 1]) == false ||
                            outMesh.Indices.TryAdd(polygon[k]) == false)
                        {
                            outMessage = "out of memory";
                            return false;
                        }
                    }
                }
            }
            if (isVertex == true)
            {
                StoreVertex(slots, static_cast<usize>(row), outMesh);
            }
        }
    }
    if (hasVertices == false || outMesh.Indices.IsEmpty() == true)
    {
        outMessage = "PLY has no triangles";
        return false;
    }
    for (usize i = 0; i < outMesh.Positions.GetSize(); ++i)
    {
        if (math::IsFinite(outMesh.Positions[i]) == false)
        {
            outMessage = "PLY position is not finite";
            return false;
        }
    }
    return true;
}
} // namespace ludus::content::scene_cook::importers
