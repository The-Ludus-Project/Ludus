#include "obj_reader.h"

#include "../decimal_number.h"
#include "../front_end_support.h"
#include "../name_index.h"

#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers::obj
{
namespace
{
using foundation::StringStatus;
using foundation::core::uint64;
namespace math = foundation::math;

constexpr usize READ_CHUNK_BYTES = 65536;
constexpr uint32 CANCEL_POLL_LINES = 4096;
/// Smoothing keys from here up are unique per face, so unsmoothed faces never share
/// a welded vertex and the cook's generated normals stay flat.
constexpr uint32 FLAT_SMOOTHING_KEY_BASE = uint32{1} << 31;
constexpr usize RGB_COMPONENTS = IR_RGB_CHANNEL_COUNT;
/// Components stored per texture coordinate (u, v).
constexpr usize UV_COMPONENTS = 2;
/// Most components of a `v` record: x y z and the rational weight w.
constexpr usize MAX_POSITION_COMPONENTS = 4;
/// Largest admitted face-index magnitude, so the signed result and relative-index
/// arithmetic cannot overflow.
constexpr uint64 MAX_INDEX_MAGNITUDE = uint64{1} << 62;
/// Map options -o, -s and -t take up to three numbers (u, v and w).
constexpr usize MAX_MAP_VECTOR_ARGUMENTS = 3;
/// `-imfchan m` selects the matte channel, stored after red, green and blue.
constexpr uint8 MATTE_CHANNEL = 3;
/// Prefix of the MTL texture-map statements (map_Kd, map_Ns, ...).
constexpr std::string_view MAP_KEYWORD_PREFIX = "map_";
/// Largest Phong exponent considered; higher values are already a perfect mirror.
constexpr float64 MAX_SPECULAR_EXPONENT = 100000.0;
/// A diffuse colour whose largest channel is below this (linear) has no diffuse term;
/// with a strong specular colour the material is mapped to a conductor.
constexpr float64 CONDUCTOR_MAX_DIFFUSE = .04;
/// Illumination models are small integers (0-10); the clamp keeps the cast defined.
constexpr float64 MAX_ILLUMINATION_MODEL = 100.0;
/// MTL illumination models with transparency or refraction (MTL appendix B2, illum):
/// 4 glass with ray-traced reflection, 6 refraction, 7 refraction with Fresnel,
/// 9 glass without ray-traced reflection.
constexpr int32 ILLUMINATION_GLASS_RAY_TRACED = 4;
constexpr int32 ILLUMINATION_REFRACTION = 6;
constexpr int32 ILLUMINATION_REFRACTION_FRESNEL = 7;
constexpr int32 ILLUMINATION_GLASS = 9;
/// Default diffuse colour when an MTL material has no Kd (SceneIR's default grey).
constexpr float64 DEFAULT_DIFFUSE = .5;

[[nodiscard]] bool IsBlank(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\f' || value == '\v';
}

/// Whitespace-separated words of one logical line.
class WordCursor final
{
public:
    explicit WordCursor(std::string_view text) noexcept : mText(text) {}
    /// Next word; false at the end of the line.
    [[nodiscard]] bool Next(std::string_view& outWord) noexcept
    {
        skipBlanks();
        if (mAt >= mText.size())
        {
            return false;
        }
        const usize start = mAt;
        while (mAt < mText.size() && IsBlank(mText[mAt]) == false)
        {
            ++mAt;
        }
        outWord = mText.substr(start, mAt - start);
        return true;
    }
    /// The unread remainder with surrounding blanks removed.
    [[nodiscard]] std::string_view GetRest() noexcept
    {
        skipBlanks();
        usize end = mText.size();
        while (end > mAt && IsBlank(mText[end - 1]) == true)
        {
            --end;
        }
        return mText.substr(mAt, end - mAt);
    }
    /// Position of the cursor, for re-reading from an earlier word.
    [[nodiscard]] LUDUS_INLINE usize GetPosition() const noexcept
    {
        return mAt;
    }
    LUDUS_INLINE void SetPosition(usize position) noexcept
    {
        mAt = position;
    }

private:
    void skipBlanks() noexcept
    {
        while (mAt < mText.size() && IsBlank(mText[mAt]) == true)
        {
            ++mAt;
        }
    }

    std::string_view mText;
    usize mAt = 0;
};

/// Logical lines of a ByteSource: '#' comments stripped by the caller, '\'
/// continuations joined, CR/LF and NUL-free.
class LineReader final
{
public:
    explicit LineReader(ByteSource& inoutSource) noexcept : mSource(inoutSource) {}
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    /// Read the next logical line into a view valid until the next call.
    /// @return Ok (outHasLine false at the end), IoError, Limit for an overlong line,
    /// Invalid for a NUL byte, or OutOfMemory.
    [[nodiscard]] Status Next(std::string_view& outLine, bool& outHasLine) noexcept
    {
        outHasLine = false;
        mLine.Clear();
        mFirstLine = mNextLine;
        bool hasBytes = false;
        while (true)
        {
            if (mOffset == mSize)
            {
                if (mIsAtEnd == true)
                {
                    break;
                }
                const Status read = mSource.Read(std::span<uint8>(mBuffer.GetData(), READ_CHUNK_BYTES), mSize);
                mOffset = 0;
                if (read != Status::Ok)
                {
                    return read;
                }
                if (mSize == 0)
                {
                    mIsAtEnd = true;
                    break;
                }
            }
            const char value = static_cast<char>(mBuffer[mOffset++]);
            hasBytes = true;
            if (value == '\n')
            {
                ++mNextLine;
                // A trailing backslash joins the next physical line.
                if (mLine.IsEmpty() == false && mLine.GetLast() == '\\')
                {
                    mLine.RemoveLast();
                    if (mLine.TryAdd(' ') == false)
                    {
                        return Status::OutOfMemory;
                    }
                    continue;
                }
                break;
            }
            if (value == '\0')
            {
                return Status::Invalid;
            }
            if (value == '\r')
            {
                continue;
            }
            if (mLine.GetSize() >= MAX_LINE_BYTES)
            {
                return Status::Limit;
            }
            if (mLine.TryAdd(value) == false)
            {
                return Status::OutOfMemory;
            }
        }
        if (hasBytes == false)
        {
            return Status::Ok;
        }
        outHasLine = true;
        outLine = std::string_view(mLine.GetData(), mLine.GetSize());
        return Status::Ok;
    }
    /// One-based physical line where the last logical line started.
    [[nodiscard]] LUDUS_INLINE uint32 GetLineNumber() const noexcept
    {
        return mFirstLine;
    }

private:
    ByteSource& mSource;
    foundation::StaticArray<uint8, READ_CHUNK_BYTES> mBuffer{};
    usize mSize = 0;
    usize mOffset = 0;
    bool mIsAtEnd = false;
    uint32 mNextLine = 1;
    uint32 mFirstLine = 1;
    Array<char> mLine;
};

/// The v, vt and vn pools of an OBJ file.
enum class VertexPool : uint8
{
    Position,
    TextureCoordinate,
    Normal,
};

/// One face corner: pool indices plus the smoothing key used for welding.
struct Corner final
{
    uint32 Position = IR_NONE;
    uint32 Uv = IR_NONE;
    uint32 Normal = IR_NONE;
    uint32 Smoothing = 0;
};

[[nodiscard]] bool IsCornerLess(const Corner& left, const Corner& right) noexcept
{
    if (left.Position != right.Position)
    {
        return left.Position < right.Position;
    }
    if (left.Uv != right.Uv)
    {
        return left.Uv < right.Uv;
    }
    if (left.Normal != right.Normal)
    {
        return left.Normal < right.Normal;
    }
    return left.Smoothing < right.Smoothing;
}

[[nodiscard]] bool AreCornersEqual(const Corner& left, const Corner& right) noexcept
{
    return left.Position == right.Position && left.Uv == right.Uv && left.Normal == right.Normal &&
           left.Smoothing == right.Smoothing;
}

/// Faces of one (group, material) pair; becomes one IrMesh and one world shape.
struct MeshBuilder final
{
    String Group;
    String Material;
    /// Triangle list of corners (three per triangle).
    Array<Corner> Corners;
    uint32 FirstLine = 0;
    bool HasUv = false;
    bool HasMissingUv = false;
    bool HasNormal = false;
    bool HasMissingNormal = false;
};

/// MTL statements of one material, before the metallic-roughness mapping.
struct MtlTerms final
{
    float64 Diffuse[RGB_COMPONENTS] = {DEFAULT_DIFFUSE, DEFAULT_DIFFUSE, DEFAULT_DIFFUSE};
    float64 Specular[RGB_COMPONENTS] = {0.0, 0.0, 0.0};
    float64 SpecularExponent = 0.0;
    float64 Dissolve = 1.0;
    float64 PbrRoughness = 1.0;
    float64 PbrMetallic = 0.0;
    int32 Illumination = -1;
    bool HasDissolve = false;
    bool HasTransparency = false;
    bool HasPbrRoughness = false;
    bool HasPbrMetallic = false;
    bool HasPbrMetallicFactor = false;
};

/// Texture-map statement options that the reader understands.
struct MapOptions final
{
    bool IsRepeated = true;
    bool HasTransform = false;
    float64 BumpMultiplier = 1.0;
    uint8 Channel = 0;
    bool HasChannel = false;
};

class WavefrontReader final
{
public:
    WavefrontReader(SourceResolver& inoutResolver,
                    SceneIr& outScene,
                    DiagnosticLog& inoutLog,
                    const Cancellation* cancelOrNull) noexcept
        : mResolver(inoutResolver), mScene(outScene), mLog(inoutLog), mCancelOrNull(cancelOrNull)
    {
    }

    [[nodiscard]] Status ReadEntry(ByteSource& inoutEntry) noexcept;
    [[nodiscard]] Status Finish() noexcept;

private:
    void report(DiagnosticSeverity severity, std::string_view code, uint32 line, std::string_view message) noexcept
    {
        const std::string_view file = mCurrentFile < mScene.SourceFiles.GetSize()
                                          ? mScene.SourceFiles[mCurrentFile].GetView()
                                          : std::string_view();
        if (mLog.Add(severity, code, file, line, message) == false)
        {
            mHasOverflowed = true;
        }
    }
    void fail(std::string_view code, uint32 line, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Error, code, line, message);
    }
    void warn(std::string_view code, uint32 line, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Warning, code, line, message);
    }
    void inform(std::string_view code, uint32 line, std::string_view message) noexcept
    {
        report(DiagnosticSeverity::Info, code, line, message);
    }

    [[nodiscard]] Status readLine(std::string_view line, uint32 lineNumber) noexcept;
    [[nodiscard]] Status readVector(WordCursor& inoutWords, VertexPool pool, uint32 lineNumber) noexcept;
    [[nodiscard]] Status readFace(WordCursor& inoutWords, uint32 lineNumber) noexcept;
    [[nodiscard]] bool resolveIndex(std::string_view text, usize count, uint32& outIndex) noexcept;
    [[nodiscard]] Status selectBuilder(uint32 lineNumber) noexcept;
    [[nodiscard]] Status loadLibraries(WordCursor& inoutWords, uint32 lineNumber) noexcept;
    [[nodiscard]] Status loadLibrary(std::string_view name, uint32 lineNumber) noexcept;
    [[nodiscard]] Status readLibrary(std::span<const uint8> bytes, std::string_view directory) noexcept;
    [[nodiscard]] Status finishMaterial(const MtlTerms& terms, uint32 line) noexcept;
    [[nodiscard]] Status
    readMap(WordCursor& inoutWords, std::string_view keyword, uint32 lineNumber, std::string_view directory) noexcept;
    [[nodiscard]] bool normalizePath(std::string_view raw, uint32 lineNumber, String& outPath) noexcept;
    [[nodiscard]] Status registerTexture(std::string_view raw,
                                         TextureEncoding encoding,
                                         std::string_view directory,
                                         const MapOptions& options,
                                         uint32 lineNumber,
                                         uint32& outTexture) noexcept;
    [[nodiscard]] Status buildMesh(MeshBuilder& inoutBuilder) noexcept;
    [[nodiscard]] uint32 findMaterial(std::string_view name) const noexcept;

    SourceResolver& mResolver;
    SceneIr& mScene;
    DiagnosticLog& mLog;
    const Cancellation* mCancelOrNull = nullptr;
    uint32 mCurrentFile = 0;
    bool mHasOverflowed = false;

    Array<float32> mPositions;
    Array<float32> mUvs;
    Array<float32> mNormals;
    Array<MeshBuilder> mBuilders;
    /// "group\nmaterial" to an index into mBuilders.
    NameIndex mBuilderIndex;
    uint32 mCurrentBuilder = IR_NONE;
    String mGroup;
    String mMaterial;
    uint32 mSmoothing = 0;
    uint32 mFaceCount = 0;
    uint64 mTotalCorners = 0;

    /// Material name to IR material index.
    NameIndex mMaterials;
    Array<String> mLibraries;
    /// Material being read from an MTL library; IR_NONE outside newmtl blocks.
    uint32 mOpenMaterial = IR_NONE;
    MtlTerms mOpenTerms;
    uint32 mOpenLine = 0;
};

[[nodiscard]] bool ParseInteger(std::string_view text, int64& outValue) noexcept
{
    const bool isNegative = text.empty() == false && text.front() == '-';
    const std::string_view digits = isNegative == true ? text.substr(1) : text;
    uint64 magnitude = 0;
    if (digits.empty() == true ||
        foundation::ParseUint64(digits.data(), digits.size(), magnitude) != foundation::NumberParseStatus::Success ||
        magnitude > MAX_INDEX_MAGNITUDE)
    {
        return false;
    }
    outValue = isNegative == true ? -static_cast<int64>(magnitude) : static_cast<int64>(magnitude);
    return true;
}

[[nodiscard]] bool AreNamesEqual(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (usize i = 0; i < left.size(); ++i)
    {
        char a = left[i];
        char b = right[i];
        a = a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a;
        b = b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b;
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

bool WavefrontReader::resolveIndex(std::string_view text, usize count, uint32& outIndex) noexcept
{
    int64 value = 0;
    if (ParseInteger(text, value) == false || value == 0)
    {
        return false;
    }
    const int64 index = value > 0 ? value - 1 : static_cast<int64>(count) + value;
    if (index < 0 || index >= static_cast<int64>(count))
    {
        return false;
    }
    outIndex = static_cast<uint32>(index);
    return true;
}

Status WavefrontReader::readVector(WordCursor& inoutWords, VertexPool pool, uint32 lineNumber) noexcept
{
    // v x y z [w], vt u [v [w]], vn x y z.
    const bool isUv = pool == VertexPool::TextureCoordinate;
    const usize minimum = isUv == true ? 1 : IR_VECTOR_COMPONENT_COUNT;
    const usize maximum = pool == VertexPool::Position ? MAX_POSITION_COMPONENTS : IR_VECTOR_COMPONENT_COUNT;
    Array<float32>& target = pool == VertexPool::Position ? mPositions : isUv == true ? mUvs : mNormals;
    constexpr usize MAX_COMPONENTS = 7;
    foundation::StaticArray<float64, MAX_COMPONENTS> values{};
    usize count = 0;
    std::string_view word;
    while (inoutWords.Next(word) == true)
    {
        float64 value = 0.0;
        if (count == MAX_COMPONENTS || ParseNumber(word, value) == false)
        {
            fail(codes::MALFORMED_INPUT, lineNumber, "malformed number in a vertex record");
            return Status::Ok;
        }
        values[count++] = value;
    }
    if (count < minimum)
    {
        fail(codes::MALFORMED_INPUT, lineNumber, "vertex record has too few components");
        return Status::Ok;
    }
    if (count > maximum)
    {
        // v x y z r g b: per-vertex colours are a common, unofficial extension.
        inform(codes::ATTRIBUTE_IGNORED, lineNumber, "extra vertex components (colours or weights) ignored");
    }
    const usize storedCount = isUv == true ? UV_COMPONENTS : IR_VECTOR_COMPONENT_COUNT;
    if (target.GetSize() / storedCount >= MAX_POOL_ENTRIES)
    {
        fail(codes::LIMIT_EXCEEDED, lineNumber, "vertex pool exceeds the OBJ profile limit");
        return Status::Ok;
    }
    const foundation::StaticArray<float32, IR_VECTOR_COMPONENT_COUNT> stored{
        static_cast<float32>(values[0]),
        isUv == true ? static_cast<float32>(1.0 - values[1]) : static_cast<float32>(values[1]),
        static_cast<float32>(values[2]),
    };
    return target.TryAddRange(std::span<const float32>(stored.GetData(), storedCount)) == true ? Status::Ok
                                                                                               : Status::OutOfMemory;
}

Status WavefrontReader::selectBuilder(uint32 lineNumber) noexcept
{
    if (mCurrentBuilder != IR_NONE)
    {
        return Status::Ok;
    }
    // A newline never occurs inside a logical line, so it separates the two names.
    String key;
    const std::string_view parts[] = {mGroup.GetView(), std::string_view("\n"), mMaterial.GetView()};
    if (key.TryAppendMany(parts) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    const uint32 existing = mBuilderIndex.Find(key.GetView());
    if (existing != NameIndex::NOT_FOUND)
    {
        mCurrentBuilder = existing;
        return Status::Ok;
    }
    if (mBuilders.GetSize() >= MAX_MESH_GROUPS)
    {
        fail(codes::LIMIT_EXCEEDED, lineNumber, "distinct group/material pairs exceed the OBJ profile limit");
        return Status::Ok;
    }
    MeshBuilder* builderOrNull = mBuilders.TryAddInPlace();
    if (builderOrNull == nullptr || builderOrNull->Group.TryAssign(mGroup.GetView()) != StringStatus::Ok ||
        builderOrNull->Material.TryAssign(mMaterial.GetView()) != StringStatus::Ok ||
        mBuilderIndex.TryAdd(key.GetView(), static_cast<uint32>(mBuilders.GetSize() - 1)) == false)
    {
        return Status::OutOfMemory;
    }
    builderOrNull->FirstLine = lineNumber;
    mCurrentBuilder = static_cast<uint32>(mBuilders.GetSize() - 1);
    return Status::Ok;
}

Status WavefrontReader::readFace(WordCursor& inoutWords, uint32 lineNumber) noexcept
{
    foundation::StaticArray<Corner, MAX_FACE_CORNERS> corners{};
    usize count = 0;
    bool hasUv = false;
    bool hasMissingUv = false;
    bool hasNormal = false;
    bool hasMissingNormal = false;
    std::string_view word;
    while (inoutWords.Next(word) == true)
    {
        if (count == MAX_FACE_CORNERS)
        {
            warn(codes::PRIMITIVE_SKIPPED, lineNumber, "face with more than 64 corners skipped");
            return Status::Ok;
        }
        // v, v/vt, v//vn or v/vt/vn.
        const usize firstSlash = word.find('/');
        const std::string_view positionText = word.substr(0, firstSlash);
        std::string_view uvText;
        std::string_view normalText;
        if (firstSlash != std::string_view::npos)
        {
            const std::string_view rest = word.substr(firstSlash + 1);
            const usize secondSlash = rest.find('/');
            uvText = rest.substr(0, secondSlash);
            normalText = secondSlash == std::string_view::npos ? std::string_view() : rest.substr(secondSlash + 1);
        }
        Corner& corner = corners[count];
        if (resolveIndex(positionText, mPositions.GetSize() / IR_VECTOR_COMPONENT_COUNT, corner.Position) == false ||
            (uvText.empty() == false && resolveIndex(uvText, mUvs.GetSize() / UV_COMPONENTS, corner.Uv) == false) ||
            (normalText.empty() == false &&
             resolveIndex(normalText, mNormals.GetSize() / IR_VECTOR_COMPONENT_COUNT, corner.Normal) == false))
        {
            fail(codes::MALFORMED_INPUT, lineNumber, "face index is zero, malformed or out of range");
            return Status::Ok;
        }
        hasUv = hasUv == true || uvText.empty() == false;
        hasMissingUv = hasMissingUv == true || uvText.empty() == true;
        hasNormal = hasNormal == true || normalText.empty() == false;
        hasMissingNormal = hasMissingNormal == true || normalText.empty() == true;
        ++count;
    }
    if (count < IR_TRIANGLE_CORNER_COUNT)
    {
        warn(codes::PRIMITIVE_SKIPPED, lineNumber, "face with fewer than three corners skipped");
        return Status::Ok;
    }
    // A fan over count corners has count - 2 triangles.
    mTotalCorners += (count - 2) * IR_TRIANGLE_CORNER_COUNT;
    if (mTotalCorners > MAX_TOTAL_CORNERS)
    {
        fail(codes::LIMIT_EXCEEDED, lineNumber, "face corners exceed the OBJ profile limit");
        return Status::Ok;
    }
    const Status selected = selectBuilder(lineNumber);
    if (selected != Status::Ok)
    {
        return selected;
    }
    ++mFaceCount;
    // Unsmoothed faces get a unique key so they never share welded vertices.
    const uint32 smoothing =
        mSmoothing != 0 ? mSmoothing : FLAT_SMOOTHING_KEY_BASE | (mFaceCount & ~FLAT_SMOOTHING_KEY_BASE);
    MeshBuilder& builder = mBuilders[mCurrentBuilder];
    builder.HasUv = builder.HasUv == true || hasUv == true;
    builder.HasMissingUv = builder.HasMissingUv == true || hasMissingUv == true;
    builder.HasNormal = builder.HasNormal == true || hasNormal == true;
    builder.HasMissingNormal = builder.HasMissingNormal == true || hasMissingNormal == true;
    uint32 degenerate = 0;
    for (usize i = 0; i < count; ++i)
    {
        corners[i].Smoothing = smoothing;
    }
    // Fan triangulation (exact for convex polygons).
    for (usize i = 1; i + 1 < count; ++i)
    {
        const Corner& a = corners[0];
        const Corner& b = corners[i];
        const Corner& c = corners[i + 1];
        if (a.Position == b.Position || b.Position == c.Position || a.Position == c.Position)
        {
            ++degenerate;
            continue;
        }
        if (builder.Corners.TryAdd(a) == false || builder.Corners.TryAdd(b) == false ||
            builder.Corners.TryAdd(c) == false)
        {
            return Status::OutOfMemory;
        }
    }
    if (degenerate != 0)
    {
        inform(codes::GEOMETRY_REPAIRED, lineNumber, "degenerate triangles with repeated positions removed");
    }
    return Status::Ok;
}

bool WavefrontReader::normalizePath(std::string_view raw, uint32 lineNumber, String& outPath) noexcept
{
    outPath.Clear();
    usize start = 0;
    while (true)
    {
        const usize separator = raw.find('\\', start);
        if (outPath.TryAppend(raw.substr(start, separator - start)) != StringStatus::Ok)
        {
            return false;
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        if (outPath.TryAppend("/") != StringStatus::Ok)
        {
            return false;
        }
        start = separator + 1;
    }
    if (raw.find('\\') != std::string_view::npos)
    {
        inform(codes::PATH_NORMALIZED, lineNumber, "'\\' path separators read as '/'");
    }
    return true;
}

Status WavefrontReader::loadLibraries(WordCursor& inoutWords, uint32 lineNumber) noexcept
{
    // The statement lists one or more file names; a single name with spaces is
    // tried as a whole first.
    const usize start = inoutWords.GetPosition();
    const std::string_view whole = inoutWords.GetRest();
    ResolvedSource probe;
    String normalized;
    if (normalizePath(whole, lineNumber, normalized) == false)
    {
        return Status::OutOfMemory;
    }
    if (whole.find(' ') != std::string_view::npos && mResolver.Resolve(normalized.GetView(), probe) == true)
    {
        Array<uint8> ignored;
        if (mResolver.Read(probe, MAX_MTL_BYTES, ignored) == Status::Ok)
        {
            return loadLibrary(whole, lineNumber);
        }
    }
    inoutWords.SetPosition(start);
    std::string_view name;
    while (inoutWords.Next(name) == true)
    {
        const Status loaded = loadLibrary(name, lineNumber);
        if (loaded != Status::Ok)
        {
            return loaded;
        }
    }
    return Status::Ok;
}

Status WavefrontReader::loadLibrary(std::string_view name, uint32 lineNumber) noexcept
{
    String normalized;
    if (normalizePath(name, lineNumber, normalized) == false)
    {
        return Status::OutOfMemory;
    }
    String relative;
    ResolvedSource resolved;
    if (JoinRelativePath(GetDirectoryPrefix(mScene.SourceFiles[0].GetView()), normalized.GetView(), relative) ==
            false ||
        mResolver.Resolve(relative.GetView(), resolved) == false)
    {
        warn(codes::DEPENDENCY_UNREADABLE, lineNumber, "material library path is missing or outside the import root");
        return Status::Ok;
    }
    for (const String& library : mLibraries)
    {
        if (library.GetView() == resolved.Relative.GetView())
        {
            return Status::Ok;
        }
    }
    if (mLibraries.GetSize() >= MAX_MATERIAL_LIBRARIES)
    {
        warn(codes::LIMIT_EXCEEDED, lineNumber, "material library limit reached");
        return Status::Ok;
    }
    String libraryName;
    if (libraryName.TryAssign(resolved.Relative.GetView()) != StringStatus::Ok ||
        mLibraries.TryAdd(static_cast<String&&>(libraryName)) == false)
    {
        return Status::OutOfMemory;
    }
    Array<uint8> bytes;
    const Status read = mResolver.Read(resolved, MAX_MTL_BYTES, bytes);
    if (read == Status::OutOfMemory)
    {
        return read;
    }
    if (read != Status::Ok)
    {
        warn(codes::DEPENDENCY_UNREADABLE,
             lineNumber,
             read == Status::Limit ? "material library exceeds the size limit" : "cannot read the material library");
        return Status::Ok;
    }
    const uint32 previousFile = mCurrentFile;
    mCurrentFile = static_cast<uint32>(mScene.SourceFiles.GetSize());
    if (AddSourceFile(mScene, resolved.Relative.GetView(), Hash(bytes.AsSpan())) == false)
    {
        return Status::OutOfMemory;
    }
    const Status status = readLibrary(bytes.AsSpan(), GetDirectoryPrefix(resolved.Relative.GetView()));
    mCurrentFile = previousFile;
    return status;
}

uint32 WavefrontReader::findMaterial(std::string_view name) const noexcept
{
    const uint32 index = mMaterials.Find(name);
    return index == NameIndex::NOT_FOUND ? IR_NONE : index;
}

Status WavefrontReader::registerTexture(std::string_view raw,
                                        TextureEncoding encoding,
                                        std::string_view directory,
                                        const MapOptions& options,
                                        uint32 lineNumber,
                                        uint32& outTexture) noexcept
{
    outTexture = IR_NONE;
    String normalized;
    if (normalizePath(raw, lineNumber, normalized) == false)
    {
        return Status::OutOfMemory;
    }
    String relative;
    ResolvedSource resolved;
    if (normalized.IsEmpty() == true || JoinRelativePath(directory, normalized.GetView(), relative) == false ||
        mResolver.Resolve(relative.GetView(), resolved) == false)
    {
        warn(codes::TEXTURE_UNREADABLE, lineNumber, "texture path is missing or outside the import root");
        return Status::Ok;
    }
    for (usize i = 0; i < mScene.Textures.GetSize(); ++i)
    {
        const IrTexture& existing = mScene.Textures[i];
        if (existing.File.GetView() == resolved.Relative.GetView() && existing.Encoding == encoding &&
            existing.IsRepeated == options.IsRepeated)
        {
            outTexture = static_cast<uint32>(i);
            return Status::Ok;
        }
    }
    IrTexture texture;
    texture.Encoding = encoding;
    texture.IsRepeated = options.IsRepeated;
    texture.SourceFile = mCurrentFile;
    texture.Line = lineNumber;
    if (texture.Name.TryAssign(normalized.GetView()) != StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    texture.File = static_cast<String&&>(resolved.Relative);
    outTexture = static_cast<uint32>(mScene.Textures.GetSize());
    return mScene.Textures.TryAdd(static_cast<IrTexture&&>(texture)) == true ? Status::Ok : Status::OutOfMemory;
}

Status WavefrontReader::readMap(WordCursor& inoutWords,
                                std::string_view keyword,
                                uint32 lineNumber,
                                std::string_view directory) noexcept
{
    MapOptions options;
    std::string_view file;
    while (true)
    {
        const usize start = inoutWords.GetPosition();
        std::string_view word;
        if (inoutWords.Next(word) == false)
        {
            break;
        }
        if (word.size() < 2 || word.front() != '-')
        {
            inoutWords.SetPosition(start);
            file = inoutWords.GetRest();
            break;
        }
        std::string_view argument;
        if (word == "-o" || word == "-s" || word == "-t")
        {
            // Up to three numbers; a non-number ends the list.
            bool hasNonDefault = false;
            for (usize i = 0; i < MAX_MAP_VECTOR_ARGUMENTS; ++i)
            {
                const usize before = inoutWords.GetPosition();
                float64 value = 0.0;
                if (inoutWords.Next(argument) == false || ParseNumber(argument, value) == false)
                {
                    inoutWords.SetPosition(before);
                    break;
                }
                const float64 identity = word == "-s" ? 1.0 : 0.0;
                hasNonDefault = hasNonDefault == true || value != identity;
            }
            options.HasTransform = options.HasTransform == true || hasNonDefault == true;
        }
        else if (word == "-mm")
        {
            (void)inoutWords.Next(argument);
            (void)inoutWords.Next(argument);
        }
        else if (word == "-clamp")
        {
            if (inoutWords.Next(argument) == true)
            {
                options.IsRepeated = argument != "on";
            }
        }
        else if (word == "-bm")
        {
            float64 value = 1.0;
            if (inoutWords.Next(argument) == true && ParseNumber(argument, value) == true)
            {
                options.BumpMultiplier = value;
            }
        }
        else if (word == "-imfchan")
        {
            if (inoutWords.Next(argument) == true)
            {
                options.HasChannel = true;
                options.Channel = argument == "g" ? 1 : argument == "b" ? 2 : argument == "m" ? MATTE_CHANNEL : 0;
            }
        }
        else if (word == "-blendu" || word == "-blendv" || word == "-boost" || word == "-texres" || word == "-type" ||
                 word == "-cc")
        {
            (void)inoutWords.Next(argument);
        }
        else
        {
            // An unknown option: treat the rest as the file name.
            inoutWords.SetPosition(start);
            file = inoutWords.GetRest();
            break;
        }
    }
    if (mOpenMaterial == IR_NONE)
    {
        warn(codes::SYNTAX_ERROR, lineNumber, "texture map statement outside a newmtl block");
        return Status::Ok;
    }
    if (file.empty() == true)
    {
        warn(codes::TEXTURE_UNREADABLE, lineNumber, "texture map statement without a file name");
        return Status::Ok;
    }
    if (options.HasTransform == true)
    {
        warn(codes::TEXTURE_APPROXIMATED, lineNumber, "texture -o/-s/-t transform ignored");
    }
    const bool isColour = AreNamesEqual(keyword, "map_Kd") == true || AreNamesEqual(keyword, "map_Ks") == true ||
                          AreNamesEqual(keyword, "map_Ke") == true;
    uint32 texture = IR_NONE;
    const Status registered = registerTexture(file,
                                              isColour == true ? TextureEncoding::Srgb : TextureEncoding::Linear,
                                              directory,
                                              options,
                                              lineNumber,
                                              texture);
    if (registered != Status::Ok || texture == IR_NONE)
    {
        return registered;
    }
    IrMaterial& material = mScene.Materials[mOpenMaterial];
    if (AreNamesEqual(keyword, "map_Kd") == true)
    {
        material.BaseColourTexture = texture;
    }
    else if (AreNamesEqual(keyword, "map_Pr") == true)
    {
        material.RoughnessTexture = texture;
        material.RoughnessChannel = options.HasChannel == true ? options.Channel : 0;
        mOpenTerms.HasPbrRoughness = true;
    }
    else if (AreNamesEqual(keyword, "map_Pm") == true)
    {
        material.MetallicTexture = texture;
        material.MetallicChannel = options.HasChannel == true ? options.Channel : 0;
        // The map carries the metalness itself unless a Pm factor scales it.
        if (mOpenTerms.HasPbrMetallicFactor == false)
        {
            mOpenTerms.PbrMetallic = 1.0;
        }
        mOpenTerms.HasPbrMetallic = true;
    }
    else if (AreNamesEqual(keyword, "map_Ks") == true)
    {
        material.SpecularTexture = texture;
    }
    else if (AreNamesEqual(keyword, "map_Ns") == true)
    {
        material.SpecularExponentTexture = texture;
    }
    else if (AreNamesEqual(keyword, "map_d") == true)
    {
        material.OpacityTexture = texture;
    }
    else if (AreNamesEqual(keyword, "map_Ke") == true)
    {
        material.EmissionTexture = texture;
    }
    else if (AreNamesEqual(keyword, "norm") == true)
    {
        material.NormalTexture = texture;
        material.NormalTextureKind = IrNormalTextureKind::TangentSpace;
        material.NormalScale = options.BumpMultiplier;
    }
    else
    {
        // bump, map_bump and map_Bump carry a height map.
        material.NormalTexture = texture;
        material.NormalTextureKind = IrNormalTextureKind::Height;
        material.NormalScale = options.BumpMultiplier;
    }
    return Status::Ok;
}

// Thanks to Bruce Walter, Stephen R. Marschner, Hongsong Li and Kenneth E.
// Torrance, "Microfacet Models for Refraction through Rough Surfaces",
// Eurographics Symposium on Rendering 2007, section 5.2
// (https://doi.org/10.2312/EGWR/EGSR07/195-206), for the Phong-exponent to
// Beckmann-width relation alpha_b = sqrt(2 / (Ns + 2)). The perceptual
// roughness stored here is sqrt(alpha_b), treating the Beckmann width as the
// GGX alpha (= roughness^2) of the engine's metallic-roughness model.
Status WavefrontReader::finishMaterial(const MtlTerms& terms, uint32 line) noexcept
{
    IrMaterial& material = mScene.Materials[mOpenMaterial];
    const bool isPbr = terms.HasPbrRoughness == true || terms.HasPbrMetallic == true;
    for (usize k = 0; k < RGB_COMPONENTS; ++k)
    {
        material.BaseColour[k] = math::Clamp(terms.Diffuse[k], 0.0, 1.0);
    }
    if (isPbr == true)
    {
        material.Roughness = math::Clamp(terms.PbrRoughness, 0.0, 1.0);
        material.Metallic = math::Clamp(terms.PbrMetallic, 0.0, 1.0);
        if (material.SourceType.TryAssign("mtl-pbr") != StringStatus::Ok)
        {
            return Status::OutOfMemory;
        }
    }
    else
    {
        const float64 exponent = math::Clamp(terms.SpecularExponent, 0.0, MAX_SPECULAR_EXPONENT);
        const bool hasSpecular = terms.Illumination != 0 && terms.Illumination != 1;
        material.Roughness = hasSpecular == true ? math::Sqrt(math::Sqrt(2.0 / (exponent + 2.0))) : 1.0;
        const float64 diffuseMaximum = math::Max(terms.Diffuse[0], math::Max(terms.Diffuse[1], terms.Diffuse[2]));
        const float64 specularMaximum = math::Max(terms.Specular[0], math::Max(terms.Specular[1], terms.Specular[2]));
        // A surface with no diffuse term and a strong specular colour (mirrors, polished
        // metals) becomes a conductor whose base colour is the specular colour.
        if (hasSpecular == true && diffuseMaximum < CONDUCTOR_MAX_DIFFUSE && specularMaximum > .5 &&
            material.BaseColourTexture == IR_NONE)
        {
            material.Metallic = 1.0;
            for (usize k = 0; k < RGB_COMPONENTS; ++k)
            {
                material.BaseColour[k] = math::Clamp(terms.Specular[k], 0.0, 1.0);
            }
        }
        material.IsApproximated = true;
        if (material.SourceType.TryAssign("mtl-phong") != StringStatus::Ok)
        {
            return Status::OutOfMemory;
        }
        inform(codes::MATERIAL_APPROXIMATED, line, "Phong-style MTL material mapped to metallic-roughness");
    }
    const float64 opacity = terms.HasDissolve == true ? terms.Dissolve : 1.0;
    material.Opacity = math::Clamp(opacity, 0.0, 1.0);
    if (material.Opacity < 1.0 || material.OpacityTexture != IR_NONE)
    {
        material.AlphaMode = IrAlphaMode::Blend;
        warn(codes::ALPHA_APPROXIMATED, line, "MTL dissolve/opacity cooked as opaque");
    }
    if (terms.Illumination == ILLUMINATION_GLASS_RAY_TRACED || terms.Illumination == ILLUMINATION_REFRACTION ||
        terms.Illumination == ILLUMINATION_REFRACTION_FRESNEL || terms.Illumination == ILLUMINATION_GLASS)
    {
        warn(codes::TRANSMISSION_DROPPED, line, "MTL refraction/glass illumination model cooked as opaque");
    }
    if (material.NormalTexture != IR_NONE)
    {
        warn(codes::NORMALMAP_DROPPED, line, "MTL bump/normal map not stored by LUSCN v3");
    }
    if (material.SpecularTexture != IR_NONE || material.SpecularExponentTexture != IR_NONE)
    {
        inform(codes::TEXTURE_DROPPED, line, "MTL map_Ks/map_Ns ignored by the metallic-roughness mapping");
    }
    if (material.EmissionTexture != IR_NONE)
    {
        warn(codes::EMISSION_APPROXIMATED, line, "MTL map_Ke dropped; Ke is used as uniform emission");
    }
    if (ComputeRec709Luminance(material.Emission) > 0.0)
    {
        inform(codes::EMISSION_APPROXIMATED, line, "MTL Ke read as linear RGB luminance in nits");
    }
    return Status::Ok;
}

Status WavefrontReader::readLibrary(std::span<const uint8> bytes, std::string_view directory) noexcept
{
    MemoryByteSource source(bytes);
    LineReader lines(source);
    mOpenMaterial = IR_NONE;
    while (true)
    {
        std::string_view line;
        bool hasLine = false;
        const Status read = lines.Next(line, hasLine);
        const uint32 lineNumber = lines.GetLineNumber();
        if (read == Status::OutOfMemory)
        {
            return read;
        }
        if (read != Status::Ok)
        {
            warn(codes::MALFORMED_INPUT, lineNumber, "material library line is overlong or contains NUL; rest skipped");
            break;
        }
        if (hasLine == false)
        {
            break;
        }
        const usize comment = line.find('#');
        WordCursor words(line.substr(0, comment));
        std::string_view keyword;
        if (words.Next(keyword) == false)
        {
            continue;
        }
        if (keyword == "newmtl")
        {
            if (mOpenMaterial != IR_NONE)
            {
                const Status finished = finishMaterial(mOpenTerms, mOpenLine);
                if (finished != Status::Ok)
                {
                    return finished;
                }
            }
            const std::string_view name = words.GetRest();
            if (findMaterial(name) != IR_NONE)
            {
                inform(codes::NAME_REDEFINED, lineNumber, name);
                mOpenMaterial = IR_NONE;
                continue;
            }
            IrMaterial material;
            material.SourceFile = mCurrentFile;
            material.Line = lineNumber;
            if (material.Name.TryAssign(name) != StringStatus::Ok ||
                mMaterials.TryAdd(name, static_cast<uint32>(mScene.Materials.GetSize())) == false ||
                mScene.Materials.TryAdd(static_cast<IrMaterial&&>(material)) == false)
            {
                return Status::OutOfMemory;
            }
            mOpenMaterial = static_cast<uint32>(mScene.Materials.GetSize() - 1);
            mOpenTerms = MtlTerms{};
            mOpenLine = lineNumber;
            continue;
        }
        if (keyword.size() >= MAP_KEYWORD_PREFIX.size() &&
            (AreNamesEqual(keyword.substr(0, MAP_KEYWORD_PREFIX.size()), MAP_KEYWORD_PREFIX) == true ||
             keyword == "bump" || keyword == "norm" || keyword == "disp" || keyword == "decal" || keyword == "refl"))
        {
            if (AreNamesEqual(keyword, "map_Ka") == true || keyword == "decal" || keyword == "refl" ||
                AreNamesEqual(keyword, "map_aat") == true || AreNamesEqual(keyword, "map_Ps") == true)
            {
                inform(codes::TEXTURE_DROPPED,
                       lineNumber,
                       "MTL map statement ignored (ambient, decal, reflection or sheen)");
                continue;
            }
            if (keyword == "disp")
            {
                inform(codes::DISPLACEMENT_IGNORED, lineNumber, "MTL displacement map ignored");
                continue;
            }
            const bool isKnown = AreNamesEqual(keyword, "map_Kd") == true || AreNamesEqual(keyword, "map_Ks") == true ||
                                 AreNamesEqual(keyword, "map_Ke") == true || AreNamesEqual(keyword, "map_Ns") == true ||
                                 AreNamesEqual(keyword, "map_d") == true ||
                                 AreNamesEqual(keyword, "map_bump") == true || keyword == "bump" || keyword == "norm" ||
                                 AreNamesEqual(keyword, "map_Pr") == true || AreNamesEqual(keyword, "map_Pm") == true;
            if (isKnown == false)
            {
                warn(codes::UNKNOWN_DIRECTIVE, lineNumber, keyword);
                continue;
            }
            const Status mapped = readMap(words, keyword, lineNumber, directory);
            if (mapped != Status::Ok)
            {
                return mapped;
            }
            continue;
        }
        if (mOpenMaterial == IR_NONE)
        {
            warn(codes::SYNTAX_ERROR, lineNumber, "material statement outside a newmtl block");
            continue;
        }
        // Numeric statements.
        constexpr usize MAX_VALUES = 3;
        foundation::StaticArray<float64, MAX_VALUES> values{};
        usize count = 0;
        bool isNumeric = true;
        std::string_view word;
        const usize afterKeyword = words.GetPosition();
        while (count < MAX_VALUES && words.Next(word) == true)
        {
            if (ParseNumber(word, values[count]) == false)
            {
                isNumeric = false;
                break;
            }
            ++count;
        }
        IrMaterial& material = mScene.Materials[mOpenMaterial];
        const bool isColour =
            keyword == "Kd" || keyword == "Ks" || keyword == "Ke" || keyword == "Ka" || keyword == "Tf";
        if (isColour == true)
        {
            if (isNumeric == false || count == 0)
            {
                // "Kd spectral file.rfl" and "Kd xyz x y z" are outside the profile.
                warn(codes::SPECTRUM_APPROXIMATED, lineNumber, "MTL spectral/CIEXYZ colour statement ignored");
                continue;
            }
            // One value is a grey colour.
            const float64 colour[RGB_COMPONENTS] = {values[0],
                                                    count > 1 ? values[1] : values[0],
                                                    count > 2 ? values[2] : values[0]};
            for (usize k = 0; k < RGB_COMPONENTS; ++k)
            {
                if (keyword == "Kd")
                {
                    mOpenTerms.Diffuse[k] = colour[k];
                }
                else if (keyword == "Ks")
                {
                    mOpenTerms.Specular[k] = colour[k];
                }
                else if (keyword == "Ke")
                {
                    material.Emission[k] = math::Max(colour[k], 0.0);
                }
            }
            if (keyword == "Tf" && (colour[0] != 1.0 || colour[1] != 1.0 || colour[2] != 1.0) &&
                (colour[0] != 0.0 || colour[1] != 0.0 || colour[2] != 0.0))
            {
                warn(codes::TRANSMISSION_DROPPED, lineNumber, "MTL transmission filter Tf ignored");
            }
            continue;
        }
        if (keyword == "d" || keyword == "Tr" || keyword == "Ns" || keyword == "Pr" || keyword == "Pm" ||
            keyword == "illum")
        {
            if (keyword == "d")
            {
                // "d -halo factor" is a view-dependent dissolve.
                words.SetPosition(afterKeyword);
                if (words.Next(word) == true && word == "-halo")
                {
                    inform(codes::ALPHA_APPROXIMATED, lineNumber, "MTL d -halo read as a constant dissolve");
                    isNumeric = words.Next(word) == true && ParseNumber(word, values[0]) == true;
                    count = isNumeric == true ? 1 : 0;
                }
            }
            if (isNumeric == false || count == 0)
            {
                warn(codes::MALFORMED_INPUT, lineNumber, "malformed number in a material statement");
                continue;
            }
            if (keyword == "d")
            {
                mOpenTerms.Dissolve = values[0];
                mOpenTerms.HasDissolve = true;
            }
            else if (keyword == "Tr")
            {
                // Tr is 1 - d; an explicit d wins.
                mOpenTerms.HasTransparency = true;
                if (mOpenTerms.HasDissolve == false)
                {
                    mOpenTerms.Dissolve = 1.0 - values[0];
                    mOpenTerms.HasDissolve = true;
                }
            }
            else if (keyword == "Ns")
            {
                mOpenTerms.SpecularExponent = values[0];
            }
            else if (keyword == "Pr")
            {
                mOpenTerms.PbrRoughness = values[0];
                mOpenTerms.HasPbrRoughness = true;
            }
            else if (keyword == "Pm")
            {
                mOpenTerms.PbrMetallic = values[0];
                mOpenTerms.HasPbrMetallic = true;
                mOpenTerms.HasPbrMetallicFactor = true;
            }
            else
            {
                // Illumination models are small integers; anything else is out of range.
                mOpenTerms.Illumination = static_cast<int32>(math::Clamp(values[0], -1.0, MAX_ILLUMINATION_MODEL));
            }
            continue;
        }
        if (keyword == "Ni" || keyword == "Ps" || keyword == "Pc" || keyword == "Pcr" || keyword == "aniso" ||
            keyword == "anisor" || keyword == "sharpness")
        {
            inform(codes::DIRECTIVE_IGNORED, lineNumber, "MTL statement without a metallic-roughness mapping ignored");
            continue;
        }
        warn(codes::UNKNOWN_DIRECTIVE, lineNumber, keyword);
    }
    if (mOpenMaterial != IR_NONE)
    {
        const Status finished = finishMaterial(mOpenTerms, mOpenLine);
        mOpenMaterial = IR_NONE;
        return finished;
    }
    return Status::Ok;
}

Status WavefrontReader::readLine(std::string_view line, uint32 lineNumber) noexcept
{
    const usize comment = line.find('#');
    WordCursor words(line.substr(0, comment));
    std::string_view keyword;
    if (words.Next(keyword) == false)
    {
        return Status::Ok;
    }
    if (keyword == "v")
    {
        return readVector(words, VertexPool::Position, lineNumber);
    }
    if (keyword == "vt")
    {
        return readVector(words, VertexPool::TextureCoordinate, lineNumber);
    }
    if (keyword == "vn")
    {
        return readVector(words, VertexPool::Normal, lineNumber);
    }
    if (keyword == "f")
    {
        return readFace(words, lineNumber);
    }
    if (keyword == "o" || keyword == "g")
    {
        const std::string_view name = words.GetRest();
        if (name != mGroup.GetView())
        {
            if (mGroup.TryAssign(name) != StringStatus::Ok)
            {
                return Status::OutOfMemory;
            }
            mCurrentBuilder = IR_NONE;
        }
        return Status::Ok;
    }
    if (keyword == "usemtl")
    {
        const std::string_view name = words.GetRest();
        if (name != mMaterial.GetView())
        {
            if (mMaterial.TryAssign(name) != StringStatus::Ok)
            {
                return Status::OutOfMemory;
            }
            mCurrentBuilder = IR_NONE;
        }
        return Status::Ok;
    }
    if (keyword == "s")
    {
        std::string_view value;
        int64 group = 0;
        if (words.Next(value) == false || value == "off" || (ParseInteger(value, group) == true && group <= 0))
        {
            mSmoothing = 0;
        }
        else if (value == "on")
        {
            mSmoothing = 1;
        }
        else if (ParseInteger(value, group) == true)
        {
            mSmoothing = static_cast<uint32>(group % FLAT_SMOOTHING_KEY_BASE);
            mSmoothing = mSmoothing == 0 ? 1 : mSmoothing;
        }
        else
        {
            warn(codes::MALFORMED_INPUT, lineNumber, "malformed smoothing group; smoothing off");
            mSmoothing = 0;
        }
        return Status::Ok;
    }
    if (keyword == "mtllib")
    {
        return loadLibraries(words, lineNumber);
    }
    if (keyword == "l" || keyword == "p")
    {
        warn(codes::PRIMITIVE_SKIPPED, lineNumber, "OBJ line and point elements skipped");
        return Status::Ok;
    }
    if (keyword == "vp" || keyword == "cstype" || keyword == "deg" || keyword == "bmat" || keyword == "step" ||
        keyword == "curv" || keyword == "curv2" || keyword == "surf" || keyword == "parm" || keyword == "trim" ||
        keyword == "hole" || keyword == "scrv" || keyword == "sp" || keyword == "end" || keyword == "con")
    {
        warn(codes::PRIMITIVE_SKIPPED, lineNumber, "OBJ free-form geometry skipped");
        return Status::Ok;
    }
    if (keyword == "mg" || keyword == "bevel" || keyword == "c_interp" || keyword == "d_interp" || keyword == "lod" ||
        keyword == "maplib" || keyword == "usemap" || keyword == "shadow_obj" || keyword == "trace_obj" ||
        keyword == "ctech" || keyword == "stech")
    {
        inform(codes::DIRECTIVE_IGNORED, lineNumber, "OBJ display/render attribute ignored");
        return Status::Ok;
    }
    warn(codes::UNKNOWN_DIRECTIVE, lineNumber, keyword);
    return Status::Ok;
}

Status WavefrontReader::ReadEntry(ByteSource& inoutEntry) noexcept
{
    LineReader lines(inoutEntry);
    uint32 linesRead = 0;
    while (true)
    {
        if (++linesRead % CANCEL_POLL_LINES == 0 && IsCancelled(mCancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        std::string_view line;
        bool hasLine = false;
        const Status read = lines.Next(line, hasLine);
        if (read == Status::OutOfMemory || read == Status::IoError)
        {
            return read;
        }
        if (read != Status::Ok)
        {
            fail(codes::MALFORMED_INPUT,
                 lines.GetLineNumber(),
                 read == Status::Limit ? "line exceeds the 64 KiB limit" : "NUL byte in an OBJ file");
            return Status::Ok;
        }
        if (hasLine == false)
        {
            return Status::Ok;
        }
        const Status status = readLine(line, lines.GetLineNumber());
        if (status != Status::Ok)
        {
            return status;
        }
        if (mHasOverflowed == true)
        {
            return Status::OutOfMemory;
        }
    }
}

Status WavefrontReader::buildMesh(MeshBuilder& inoutBuilder) noexcept
{
    const Array<Corner>& corners = inoutBuilder.Corners;
    Array<uint32> order;
    if (order.TryResize(corners.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < order.GetSize(); ++i)
    {
        order[i] = static_cast<uint32>(i);
    }
    // Weld equal corner tuples; ties keep corner order so the result is deterministic.
    foundation::Sort(order.GetData(), order.GetSize(), [&](uint32 left, uint32 right) noexcept {
        if (AreCornersEqual(corners[left], corners[right]) == true)
        {
            return left < right;
        }
        return IsCornerLess(corners[left], corners[right]);
    });
    IrMesh mesh;
    const bool hasUvs = inoutBuilder.HasUv;
    const bool hasNormals = inoutBuilder.HasNormal == true && inoutBuilder.HasMissingNormal == false;
    if (mesh.Indices.TryResize(corners.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    uint32 vertexCount = 0;
    for (usize i = 0; i < order.GetSize(); ++i)
    {
        const Corner& corner = corners[order[i]];
        if (i == 0 || AreCornersEqual(corner, corners[order[i - 1]]) == false)
        {
            const float32* position = mPositions.GetData() + usize{corner.Position} * IR_VECTOR_COMPONENT_COUNT;
            if (mesh.Positions.TryAddRange(std::span<const float32>(position, IR_VECTOR_COMPONENT_COUNT)) == false)
            {
                return Status::OutOfMemory;
            }
            if (hasUvs == true)
            {
                const float32 missing[UV_COMPONENTS] = {0.0F, 0.0F};
                const float32* uv = corner.Uv != IR_NONE ? mUvs.GetData() + usize{corner.Uv} * UV_COMPONENTS : missing;
                if (mesh.Uvs.TryAddRange(std::span<const float32>(uv, UV_COMPONENTS)) == false)
                {
                    return Status::OutOfMemory;
                }
            }
            if (hasNormals == true && mesh.Normals.TryAddRange(std::span<const float32>(
                                          mNormals.GetData() + usize{corner.Normal} * IR_VECTOR_COMPONENT_COUNT,
                                          IR_VECTOR_COMPONENT_COUNT)) == false)
            {
                return Status::OutOfMemory;
            }
            ++vertexCount;
        }
        mesh.Indices[order[i]] = vertexCount - 1;
    }
    inoutBuilder.Corners.Clear();
    if (inoutBuilder.HasUv == true && inoutBuilder.HasMissingUv == true)
    {
        warn(codes::ATTRIBUTE_IGNORED, inoutBuilder.FirstLine, "faces without vt in a textured group get uv (0, 0)");
    }
    if (inoutBuilder.HasNormal == true && inoutBuilder.HasMissingNormal == true)
    {
        warn(codes::ATTRIBUTE_IGNORED,
             inoutBuilder.FirstLine,
             "group mixes faces with and without vn; normals generated");
    }
    IrShape shape;
    shape.Mesh = static_cast<uint32>(mScene.Meshes.GetSize());
    shape.Material = findMaterial(inoutBuilder.Material.GetView());
    shape.SourceFile = 0;
    shape.Line = inoutBuilder.FirstLine;
    if (shape.Material == IR_NONE && inoutBuilder.Material.IsEmpty() == false)
    {
        mCurrentFile = 0;
        warn(codes::NAME_UNDEFINED, inoutBuilder.FirstLine, inoutBuilder.Material.GetView());
    }
    if (shape.Material != IR_NONE)
    {
        ApplyMaterialEmission(mScene.Materials[shape.Material], false, shape);
    }
    if (mScene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == false ||
        mScene.WorldShapes.TryAdd(static_cast<uint32>(mScene.Shapes.GetSize())) == false ||
        mScene.Shapes.TryAdd(shape) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

Status WavefrontReader::Finish() noexcept
{
    mCurrentFile = 0;
    for (MeshBuilder& builder : mBuilders)
    {
        if (IsCancelled(mCancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        if (builder.Corners.IsEmpty() == true)
        {
            continue;
        }
        const Status built = buildMesh(builder);
        if (built != Status::Ok)
        {
            return built;
        }
    }
    // Emissive materials in use count as area-light sources in the report.
    Array<uint8> isUsed;
    if (isUsed.TryResize(mScene.Materials.GetSize(), uint8{0}) == false)
    {
        return Status::OutOfMemory;
    }
    for (const IrShape& shape : mScene.Shapes)
    {
        if (shape.Material != IR_NONE && isUsed[shape.Material] == 0 &&
            ComputeRec709Luminance(mScene.Materials[shape.Material].Emission) > 0.0)
        {
            isUsed[shape.Material] = 1;
            ++mScene.Lights.Area;
        }
    }
    if (mScene.Shapes.IsEmpty() == true && mLog.HasErrors() == false)
    {
        fail(codes::LIMIT_EXCEEDED, 0, "the OBJ file has no triangle faces");
    }
    return mHasOverflowed == true ? Status::OutOfMemory : Status::Ok;
}
} // namespace

Status ReadScene(ByteSource& inoutEntry,
                 std::string_view entryName,
                 SourceResolver& inoutResolver,
                 SceneIr& outScene,
                 DiagnosticLog& inoutLog,
                 const Cancellation* cancelOrNull) noexcept
{
    SceneIr freshScene;
    outScene = static_cast<SceneIr&&>(freshScene);
    if (AddSourceFile(outScene, entryName, Digest{}) == false)
    {
        return Status::OutOfMemory;
    }
    HashingByteSource source(inoutEntry);
    WavefrontReader reader(inoutResolver, outScene, inoutLog, cancelOrNull);
    Status status = reader.ReadEntry(source);
    // Hash the unread tail too, so the digest names the whole entry file.
    if (status == Status::Ok || status == Status::Invalid)
    {
        const Status drained = source.Drain();
        status = drained != Status::Ok ? drained : status;
    }
    outScene.SourceDigests[0] = source.GetDigest();
    if (status == Status::Ok)
    {
        status = reader.Finish();
    }
    if (status == Status::Ok && inoutLog.HasErrors() == true)
    {
        status = Status::Invalid;
    }
    return status;
}
} // namespace ludus::content::scene_cook::importers::obj
