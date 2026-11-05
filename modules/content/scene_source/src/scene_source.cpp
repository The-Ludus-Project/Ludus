#include <ludus/content/scene_source/scene_source.h>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <initializer_list>

namespace ludus::content::scene_source
{
namespace
{
using foundation::Array;
using foundation::StaticArray;
using foundation::String;
using foundation::StringStatus;
using foundation::parsing::JsonDocument;
using foundation::parsing::JsonLimits;
using foundation::parsing::JsonValue;
using foundation::parsing::ParseError;
using foundation::parsing::ParseStatus;
namespace math = foundation::math;

/// Components of a JSON position, colour or direction array.
constexpr usize VECTOR_COMPONENT_COUNT = 3;
/// Fewest corners of a polygon.
constexpr usize MIN_POLYGON_VERTICES = 3;
/// Deepest container nesting a valid document uses: root, objects, object, positions, position.
constexpr usize MAX_DOCUMENT_DEPTH = 8;
/// Most members of any object the format defines.
constexpr usize MAX_DOCUMENT_MEMBERS = 16;
/// Every JSON value takes at least two bytes of text ("0,"), which bounds the value count.
constexpr usize MAX_DOCUMENT_VALUES = MAX_SCENE_SOURCE_BYTES / 2;
/// yyjson stores about 16 bytes per value besides its copy of the input.
constexpr usize MAX_DOCUMENT_WORKSPACE_BYTES = MAX_DOCUMENT_VALUES * 16 + MAX_SCENE_SOURCE_BYTES * 2;
/// A polygon is planar when every corner lies within this fraction of its longest edge
/// of the plane through its first corner; dimensionless.
constexpr float64 PLANARITY_TOLERANCE = 1e-9;
/// A polygon is strictly convex when, for every edge, the sine of the angle at which each
/// other corner lies to its left exceeds this; dimensionless. Rejects collinear corners.
constexpr float64 CONVEXITY_SINE_TOLERANCE = 1e-9;
/// The camera's up direction must leave at least this sine with its view direction.
constexpr float64 CAMERA_UP_SINE_TOLERANCE = 1e-6;
/// Exclusive upper bound of the vertical field of view, in degrees.
constexpr float64 MAX_VERTICAL_FOV_DEGREES = 180.0;
/// Largest finite binary64 value: the open upper bound of emitted radiance.
constexpr float64 MAX_FINITE_FLOAT64 = 1.7976931348623157e308;
/// Longest JSON path kept for a diagnostic; half a message, leaving room for its text.
constexpr usize MAX_PATH_BYTES = MAX_MESSAGE_BYTES / 2;
constexpr std::string_view DIFFUSE_MODEL = "diffuse";
constexpr std::string_view METALLIC_ROUGHNESS_MODEL = "metallic_roughness";

/// @brief Bounded JSON path of the member being read, for diagnostics.
class JsonPath final
{
public:
    JsonPath() noexcept = default;
    JsonPath(const JsonPath& parent, std::string_view member) noexcept : JsonPath(parent)
    {
        if (mLength != 0)
        {
            append(".");
        }
        append(member);
    }
    JsonPath(const JsonPath& parent, usize index) noexcept : JsonPath(parent)
    {
        append("[");
        StaticArray<char, MAX_INDEX_DIGITS> digits{};
        usize count = 0;
        do
        {
            digits[count] = static_cast<char>('0' + index % DECIMAL_BASE);
            index /= DECIMAL_BASE;
            ++count;
        } while (index != 0 && count < MAX_INDEX_DIGITS);
        while (count != 0)
        {
            --count;
            append(std::string_view(&digits[count], 1));
        }
        append("]");
    }

    [[nodiscard]] LUDUS_INLINE std::string_view GetView() const noexcept
    {
        return std::string_view(mText.GetData(), mLength);
    }

private:
    static constexpr usize MAX_INDEX_DIGITS = 20;
    static constexpr usize DECIMAL_BASE = 10;

    void append(std::string_view text) noexcept
    {
        for (const char item : text)
        {
            if (mLength == mText.GetSize())
            {
                return;
            }
            mText[mLength] = item;
            ++mLength;
        }
    }

    StaticArray<char, MAX_PATH_BYTES> mText{};
    usize mLength = 0;
};

/// @brief Records the first failure of one parse.
class Failure final
{
public:
    explicit Failure(SceneSourceError& outError) noexcept : mError(outError) {}

    /// Record `what` at `path` and return `status`.
    SceneSourceStatus Fail(SceneSourceStatus status, std::string_view what, const JsonPath& path) noexcept
    {
        mError.Status = status;
        StaticArray<char, MAX_MESSAGE_BYTES> text{};
        usize size = 0;
        const std::string_view location = path.GetView();
        for (const std::string_view part :
             {location, location.empty() == true ? std::string_view{} : std::string_view(": "), what})
        {
            for (const char item : part)
            {
                if (size == text.GetSize())
                {
                    break;
                }
                text[size] = item;
                ++size;
            }
        }
        // The bounded copy above never exceeds MessageText's capacity.
        [[maybe_unused]] const bool isSet = mError.Message.Set(std::string_view(text.GetData(), size));
        LUDUS_ASSERT(isSet == true);
        return status;
    }

private:
    SceneSourceError& mError;
};

/// Reject members outside `allowed` and report the first of `required` that is missing.
[[nodiscard]] SceneSourceStatus CheckMembers(JsonValue value,
                                             std::initializer_list<std::string_view> required,
                                             std::initializer_list<std::string_view> optional,
                                             const JsonPath& path,
                                             Failure& inoutFailure) noexcept
{
    if (value.Object() == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be an object", path);
    }
    for (usize i = 0; i < value.MemberCount(); ++i)
    {
        std::string_view key;
        JsonValue member;
        [[maybe_unused]] const bool isMember = value.MemberAt(i, key, member);
        LUDUS_ASSERT(isMember == true);
        bool isKnown = false;
        for (const std::initializer_list<std::string_view>& names : {required, optional})
        {
            for (const std::string_view name : names)
            {
                isKnown = isKnown == true || name == key;
            }
        }
        if (isKnown == false)
        {
            return inoutFailure.Fail(SceneSourceStatus::Malformed, "unknown member", JsonPath(path, key));
        }
    }
    for (const std::string_view name : required)
    {
        if (value.Get(name).Valid() == false)
        {
            return inoutFailure.Fail(SceneSourceStatus::Malformed, "missing member", JsonPath(path, name));
        }
    }
    return SceneSourceStatus::Success;
}

[[nodiscard]] SceneSourceStatus
ReadNumber(JsonValue value, const JsonPath& path, Failure& inoutFailure, float64& outNumber) noexcept
{
    float64 number = 0.0;
    if (value.Number(number) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a number", path);
    }
    if (math::IsFinite(number) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue, "must be finite", path);
    }
    outNumber = number;
    return SceneSourceStatus::Success;
}

/// Read a number in [minimum, maximum].
[[nodiscard]] SceneSourceStatus ReadBoundedNumber(JsonValue value,
                                                  float64 minimum,
                                                  float64 maximum,
                                                  const JsonPath& path,
                                                  Failure& inoutFailure,
                                                  float64& outNumber) noexcept
{
    float64 number = 0.0;
    const SceneSourceStatus status = ReadNumber(value, path, inoutFailure, number);
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    if (number < minimum || number > maximum)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue, "is out of range", path);
    }
    outNumber = number;
    return SceneSourceStatus::Success;
}

/// Read an array of three numbers, each in [minimum, maximum].
[[nodiscard]] SceneSourceStatus ReadVector(JsonValue value,
                                           float64 minimum,
                                           float64 maximum,
                                           const JsonPath& path,
                                           Failure& inoutFailure,
                                           math::Vector3d& outVector) noexcept
{
    if (value.Array() == false || value.Count() != VECTOR_COMPONENT_COUNT)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be an array of three numbers", path);
    }
    StaticArray<float64, VECTOR_COMPONENT_COUNT> components{};
    for (usize i = 0; i < VECTOR_COMPONENT_COUNT; ++i)
    {
        const SceneSourceStatus status =
            ReadBoundedNumber(value.At(i), minimum, maximum, JsonPath(path, i), inoutFailure, components[i]);
        if (status != SceneSourceStatus::Success)
        {
            return status;
        }
    }
    outVector = {components[0], components[1], components[2]};
    return SceneSourceStatus::Success;
}

/// Read a string of at most MAX_SCENE_SOURCE_TEXT_BYTES; empty strings are rejected unless allowed.
[[nodiscard]] SceneSourceStatus
ReadText(JsonValue value, bool canBeEmpty, const JsonPath& path, Failure& inoutFailure, String& outText) noexcept
{
    std::string_view text;
    if (value.String(text) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a string", path);
    }
    if (text.empty() == true && canBeEmpty == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue, "must not be empty", path);
    }
    if (text.size() > MAX_SCENE_SOURCE_TEXT_BYTES)
    {
        return inoutFailure.Fail(SceneSourceStatus::Limit, "is too long", path);
    }
    if (outText.TryAssign(text) != StringStatus::Ok)
    {
        return inoutFailure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the text", path);
    }
    return SceneSourceStatus::Success;
}

[[nodiscard]] SceneSourceStatus
ReadCamera(JsonValue value, const JsonPath& path, Failure& inoutFailure, SourceCamera& outCamera) noexcept
{
    SceneSourceStatus status =
        CheckMembers(value, {"eye", "target", "up", "vertical_fov_degrees"}, {}, path, inoutFailure);
    SourceCamera camera;
    camera.IsPresent = true;
    float64 fovDegrees = 0.0;
    const float64 limit = MAX_SCENE_SOURCE_COORDINATE_METRES;
    if (status == SceneSourceStatus::Success)
    {
        status = ReadVector(value.Get("eye"), -limit, limit, JsonPath(path, "eye"), inoutFailure, camera.EyeWorld);
    }
    if (status == SceneSourceStatus::Success)
    {
        status =
            ReadVector(value.Get("target"), -limit, limit, JsonPath(path, "target"), inoutFailure, camera.TargetWorld);
    }
    if (status == SceneSourceStatus::Success)
    {
        status = ReadVector(value.Get("up"), -limit, limit, JsonPath(path, "up"), inoutFailure, camera.UpWorld);
    }
    if (status == SceneSourceStatus::Success)
    {
        status = ReadNumber(value.Get("vertical_fov_degrees"),
                            JsonPath(path, "vertical_fov_degrees"),
                            inoutFailure,
                            fovDegrees);
    }
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    if (fovDegrees <= 0.0 || fovDegrees >= MAX_VERTICAL_FOV_DEGREES)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                 "must be in (0, 180)",
                                 JsonPath(path, "vertical_fov_degrees"));
    }
    const math::Vector3d viewWorld = camera.TargetWorld - camera.EyeWorld;
    const float64 viewLength = math::Length(viewWorld);
    const float64 upLength = math::Length(camera.UpWorld);
    if (viewLength <= 0.0)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue, "must differ from eye", JsonPath(path, "target"));
    }
    if (upLength <= 0.0 ||
        math::Length(math::Cross(viewWorld, camera.UpWorld)) <= CAMERA_UP_SINE_TOLERANCE * viewLength * upLength)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                 "must not be parallel to the view direction",
                                 JsonPath(path, "up"));
    }
    camera.VerticalFovRadians = math::DegreesToRadians(fovDegrees);
    outCamera = camera;
    return SceneSourceStatus::Success;
}

[[nodiscard]] SceneSourceStatus
ReadEmission(JsonValue value, const JsonPath& path, Failure& inoutFailure, SourceMaterial& inoutMaterial) noexcept
{
    SceneSourceStatus status = CheckMembers(value, {"radiance"}, {"two_sided"}, path, inoutFailure);
    if (status == SceneSourceStatus::Success)
    {
        // The cooked format bounds emission; the source format only requires finite nonnegative radiance.
        status = ReadVector(value.Get("radiance"),
                            0.0,
                            MAX_FINITE_FLOAT64,
                            JsonPath(path, "radiance"),
                            inoutFailure,
                            inoutMaterial.EmittedRadiance);
    }
    const JsonValue twoSided = value.Get("two_sided");
    if (status == SceneSourceStatus::Success && twoSided.Valid() == true &&
        twoSided.Boolean(inoutMaterial.IsEmissionTwoSided) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a boolean", JsonPath(path, "two_sided"));
    }
    return status;
}

[[nodiscard]] SceneSourceStatus
ReadMix(JsonValue value, const JsonPath& path, Failure& inoutFailure, SourceMaterial& inoutMaterial) noexcept
{
    const JsonPath conductorPath(path, "conductor");
    const JsonValue conductor = value.Get("conductor");
    SceneSourceStatus status = CheckMembers(conductor, {"alpha", "eta", "k"}, {}, conductorPath, inoutFailure);
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    status = ReadBoundedNumber(value.Get("weight"),
                               0.0,
                               1.0,
                               JsonPath(path, "weight"),
                               inoutFailure,
                               inoutMaterial.MixWeight);
    if (status == SceneSourceStatus::Success)
    {
        status = ReadBoundedNumber(conductor.Get("alpha"),
                                   static_cast<float64>(scene_format::MIN_MATERIAL_ALPHA),
                                   1.0,
                                   JsonPath(conductorPath, "alpha"),
                                   inoutFailure,
                                   inoutMaterial.Conductor.Alpha);
    }
    constexpr float64 MAXIMUM = static_cast<float64>(scene_format::MAX_MATERIAL_OPTICAL_CONSTANT);
    if (status == SceneSourceStatus::Success)
    {
        status = ReadVector(conductor.Get("eta"),
                            0.0,
                            MAXIMUM,
                            JsonPath(conductorPath, "eta"),
                            inoutFailure,
                            inoutMaterial.Conductor.Eta);
    }
    if (status == SceneSourceStatus::Success && (static_cast<float32>(inoutMaterial.Conductor.Eta.X) <= 0 ||
                                                 static_cast<float32>(inoutMaterial.Conductor.Eta.Y) <= 0 ||
                                                 static_cast<float32>(inoutMaterial.Conductor.Eta.Z) <= 0))
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                 "requires positive RGB indices",
                                 JsonPath(conductorPath, "eta"));
    }
    if (status == SceneSourceStatus::Success)
    {
        status = ReadVector(conductor.Get("k"),
                            0.0,
                            MAXIMUM,
                            JsonPath(conductorPath, "k"),
                            inoutFailure,
                            inoutMaterial.Conductor.Extinction);
    }
    return status;
}

[[nodiscard]] SceneSourceStatus
ReadProjectBrdf(JsonValue value, const JsonPath& path, Failure& inoutFailure, SourceProjectBrdf& outBrdf) noexcept
{
    SceneSourceStatus status =
        CheckMembers(value, {"version", "weights", "powers"}, {"fixed_dimension_reflection"}, path, inoutFailure);
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    constexpr uint64 PROGRAM_VERSION = 1;
    uint64 version = 0;
    if (value.Get("version").Integer(version) == false || version != PROGRAM_VERSION)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                 "expected BRDF program version 1",
                                 JsonPath(path, "version"));
    }
    status = ReadVector(value.Get("weights"), 0.0, 1.0, JsonPath(path, "weights"), inoutFailure, outBrdf.Weights);
    if (status == SceneSourceStatus::Success)
    {
        const float32 total = static_cast<float32>(outBrdf.Weights.X) + static_cast<float32>(outBrdf.Weights.Y) +
                              static_cast<float32>(outBrdf.Weights.Z);
        if (total < scene_format::LUDUS_PROJECT_BRDF_MIN_WEIGHT)
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                     "increase the total lobe weight to at least 0.001",
                                     JsonPath(path, "weights"));
        }
        status = ReadVector(value.Get("powers"),
                            0.0,
                            static_cast<float64>(scene_format::LUDUS_PROJECT_BRDF_MAX_POWER),
                            JsonPath(path, "powers"),
                            inoutFailure,
                            outBrdf.Powers);
    }
    const JsonValue guarantee = value.Get("fixed_dimension_reflection");
    if (status == SceneSourceStatus::Success && guarantee.Valid() == true &&
        guarantee.Boolean(outBrdf.HasFixedDimensionReflection) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed,
                                 "expected a boolean",
                                 JsonPath(path, "fixed_dimension_reflection"));
    }
    return status;
}

[[nodiscard]] SceneSourceStatus
ReadMaterial(JsonValue value, const JsonPath& path, Failure& inoutFailure, SourceMaterial& outMaterial) noexcept
{
    SceneSourceStatus status = CheckMembers(value,
                                            {"name", "model", "base_color"},
                                            {"metallic", "roughness", "emission", "weight", "conductor", "brdf"},
                                            path,
                                            inoutFailure);
    if (status == SceneSourceStatus::Success)
    {
        status = ReadText(value.Get("name"), false, JsonPath(path, "name"), inoutFailure, outMaterial.Name);
    }
    std::string_view model;
    if (status == SceneSourceStatus::Success && value.Get("model").String(model) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a string", JsonPath(path, "model"));
    }
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    const bool isMix = model == "mix";
    if (isMix == false && (value.Get("weight").Valid() == true || value.Get("conductor").Valid() == true))
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue, "weight and conductor require the mix model", path);
    }
    const bool isProjectBrdf = model == "project_brdf";
    if (isProjectBrdf == false && value.Get("brdf").Valid() == true)
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                 "brdf requires the project_brdf model",
                                 JsonPath(path, "brdf"));
    }
    if (model == DIFFUSE_MODEL || isMix == true)
    {
        outMaterial.Model = isMix == true ? SourceMaterialModel::Mix : SourceMaterialModel::Diffuse;
        for (const std::string_view member : {std::string_view("metallic"), std::string_view("roughness")})
        {
            if (value.Get(member).Valid() == true)
            {
                return inoutFailure.Fail(SceneSourceStatus::Malformed,
                                         "applies to metallic_roughness only",
                                         JsonPath(path, member));
            }
        }
    }
    else if (model == METALLIC_ROUGHNESS_MODEL)
    {
        outMaterial.Model = SourceMaterialModel::MetallicRoughness;
    }
    else if (isProjectBrdf == true)
    {
        const std::string_view name = outMaterial.Name.GetView();
        const usize separator = name.find('/');
        if (separator == 0 || separator == std::string_view::npos || separator + 1 == name.size())
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                     "use a project/name BRDF ID",
                                     JsonPath(path, "name"));
        }
        if (value.Get("metallic").Valid() == true)
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                     "metallic applies to metallic_roughness only",
                                     JsonPath(path, "metallic"));
        }
        outMaterial.Model = SourceMaterialModel::ProjectBrdf;
    }
    else
    {
        return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                 "must be diffuse, metallic_roughness, mix or project_brdf",
                                 JsonPath(path, "model"));
    }
    status = ReadVector(value.Get("base_color"),
                        0.0,
                        1.0,
                        JsonPath(path, "base_color"),
                        inoutFailure,
                        outMaterial.BaseColor);
    const JsonValue metallic = value.Get("metallic");
    if (status == SceneSourceStatus::Success && metallic.Valid() == true)
    {
        status = ReadBoundedNumber(metallic, 0.0, 1.0, JsonPath(path, "metallic"), inoutFailure, outMaterial.Metallic);
    }
    const JsonValue roughness = value.Get("roughness");
    if (status == SceneSourceStatus::Success && (roughness.Valid() == true || isProjectBrdf == true))
    {
        const float64 minimum = isProjectBrdf == true ? scene_format::MIN_AUTHORED_ROUGHNESS : 0.0;
        status = ReadBoundedNumber(roughness,
                                   minimum,
                                   1.0,
                                   JsonPath(path, "roughness"),
                                   inoutFailure,
                                   outMaterial.Roughness);
    }
    if (status == SceneSourceStatus::Success && isProjectBrdf == true)
    {
        status = ReadProjectBrdf(value.Get("brdf"), JsonPath(path, "brdf"), inoutFailure, outMaterial.Brdf);
    }
    if (status == SceneSourceStatus::Success && isMix == true)
    {
        status = ReadMix(value, path, inoutFailure, outMaterial);
    }
    const JsonValue emission = value.Get("emission");
    if (status == SceneSourceStatus::Success && emission.Valid() == true)
    {
        status = ReadEmission(emission, JsonPath(path, "emission"), inoutFailure, outMaterial);
    }
    return status;
}

/// Check that a polygon's corners are distinct, planar and strictly convex in their winding.
[[nodiscard]] bool IsPlanarStrictlyConvex(const Array<math::Vector3d>& positions,
                                          std::span<const uint32> corners) noexcept
{
    // Newell's method: the area-weighted normal of a planar polygon of either winding.
    math::Vector3d normal{};
    float64 longestEdge = 0.0;
    for (usize i = 0; i < corners.size(); ++i)
    {
        const math::Vector3d current = positions[corners[i]];
        const math::Vector3d next = positions[corners[(i + 1) % corners.size()]];
        normal = normal + math::Cross(current, next);
        longestEdge = math::Max(longestEdge, math::Length(next - current));
    }
    const float64 normalLength = math::Length(normal);
    if (normalLength <= 0.0 || longestEdge <= 0.0)
    {
        return false;
    }
    const math::Vector3d unitNormal = normal * (1.0 / normalLength);
    const math::Vector3d origin = positions[corners[0]];
    for (const uint32 corner : corners)
    {
        if (math::Abs(math::Dot(positions[corner] - origin, unitNormal)) > PLANARITY_TOLERANCE * longestEdge)
        {
            return false;
        }
    }
    // Every other corner lies strictly to the left of every edge; this also rejects
    // repeated corners and self-intersecting (star) polygons.
    for (usize i = 0; i < corners.size(); ++i)
    {
        const math::Vector3d start = positions[corners[i]];
        const math::Vector3d edge = positions[corners[(i + 1) % corners.size()]] - start;
        const float64 edgeLength = math::Length(edge);
        for (usize k = 0; k < corners.size(); ++k)
        {
            if (k == i || k == (i + 1) % corners.size())
            {
                continue;
            }
            const math::Vector3d toCorner = positions[corners[k]] - start;
            const float64 sine = math::Dot(math::Cross(edge, toCorner), unitNormal);
            if (sine <= CONVEXITY_SINE_TOLERANCE * edgeLength * math::Length(toCorner))
            {
                return false;
            }
        }
    }
    return true;
}

/// Running totals checked against the document-wide limits.
struct DocumentTotals final
{
    usize Positions = 0;
    usize Corners = 0;
};

[[nodiscard]] SceneSourceStatus ReadPositions(JsonValue value,
                                              const JsonPath& path,
                                              Failure& inoutFailure,
                                              DocumentTotals& inoutTotals,
                                              SourceObject& inoutObject) noexcept
{
    if (value.Array() == false || value.Count() == 0)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a nonempty array", path);
    }
    if (value.Count() > MAX_SCENE_SOURCE_POSITIONS - inoutTotals.Positions)
    {
        return inoutFailure.Fail(SceneSourceStatus::Limit, "the document has too many positions", path);
    }
    inoutTotals.Positions += value.Count();
    if (inoutObject.PositionsWorld.TryResize(value.Count()) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the positions", path);
    }
    const float64 limit = MAX_SCENE_SOURCE_COORDINATE_METRES;
    for (usize i = 0; i < value.Count(); ++i)
    {
        const SceneSourceStatus status =
            ReadVector(value.At(i), -limit, limit, JsonPath(path, i), inoutFailure, inoutObject.PositionsWorld[i]);
        if (status != SceneSourceStatus::Success)
        {
            return status;
        }
    }
    return SceneSourceStatus::Success;
}

[[nodiscard]] SceneSourceStatus ReadPolygons(JsonValue value,
                                             const JsonPath& path,
                                             Failure& inoutFailure,
                                             DocumentTotals& inoutTotals,
                                             SourceObject& inoutObject) noexcept
{
    if (value.Array() == false || value.Count() == 0)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a nonempty array", path);
    }
    if (inoutObject.PolygonVertexCounts.TryResize(value.Count()) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the polygons", path);
    }
    const usize positionCount = inoutObject.PositionsWorld.GetSize();
    for (usize p = 0; p < value.Count(); ++p)
    {
        const JsonPath polygonPath(path, p);
        const JsonValue polygon = value.At(p);
        if (polygon.Array() == false)
        {
            return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be an array of position indices", polygonPath);
        }
        const usize cornerCount = polygon.Count();
        if (cornerCount < MIN_POLYGON_VERTICES || cornerCount > MAX_SCENE_SOURCE_POLYGON_VERTICES)
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidGeometry, "must have 3 to 64 corners", polygonPath);
        }
        if (cornerCount > MAX_SCENE_SOURCE_CORNERS - inoutTotals.Corners)
        {
            return inoutFailure.Fail(SceneSourceStatus::Limit, "the document has too many corners", polygonPath);
        }
        inoutTotals.Corners += cornerCount;
        const usize first = inoutObject.PolygonIndices.GetSize();
        if (inoutObject.PolygonIndices.TryResize(first + cornerCount) == false)
        {
            return inoutFailure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the polygons", polygonPath);
        }
        for (usize c = 0; c < cornerCount; ++c)
        {
            uint64 index = 0;
            if (polygon.At(c).Integer(index) == false)
            {
                return inoutFailure.Fail(SceneSourceStatus::Malformed,
                                         "must be a nonnegative integer",
                                         JsonPath(polygonPath, c));
            }
            if (index >= positionCount)
            {
                return inoutFailure.Fail(SceneSourceStatus::InvalidGeometry,
                                         "names no position of the object",
                                         JsonPath(polygonPath, c));
            }
            inoutObject.PolygonIndices[first + c] = static_cast<uint32>(index);
        }
        inoutObject.PolygonVertexCounts[p] = static_cast<uint32>(cornerCount);
        if (IsPlanarStrictlyConvex(
                inoutObject.PositionsWorld,
                std::span<const uint32>(inoutObject.PolygonIndices.GetData() + first, cornerCount)) == false)
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidGeometry,
                                     "must be planar and strictly convex, with distinct corners",
                                     polygonPath);
        }
    }
    return SceneSourceStatus::Success;
}

[[nodiscard]] SceneSourceStatus ReadObject(JsonValue value,
                                           const Array<SourceMaterial>& materials,
                                           const JsonPath& path,
                                           Failure& inoutFailure,
                                           DocumentTotals& inoutTotals,
                                           SourceObject& outObject) noexcept
{
    SceneSourceStatus status =
        CheckMembers(value, {"name", "material", "positions", "polygons"}, {}, path, inoutFailure);
    if (status == SceneSourceStatus::Success)
    {
        status = ReadText(value.Get("name"), false, JsonPath(path, "name"), inoutFailure, outObject.Name);
    }
    std::string_view material;
    if (status == SceneSourceStatus::Success && value.Get("material").String(material) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a string", JsonPath(path, "material"));
    }
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    bool isFound = false;
    for (usize i = 0; i < materials.GetSize() && isFound == false; ++i)
    {
        if (materials[i].Name.GetView() == material)
        {
            outObject.Material = i;
            isFound = true;
        }
    }
    if (isFound == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::UnresolvedReference,
                                 "names no declared material",
                                 JsonPath(path, "material"));
    }
    status = ReadPositions(value.Get("positions"), JsonPath(path, "positions"), inoutFailure, inoutTotals, outObject);
    if (status == SceneSourceStatus::Success)
    {
        status = ReadPolygons(value.Get("polygons"), JsonPath(path, "polygons"), inoutFailure, inoutTotals, outObject);
    }
    return status;
}

/// Reject a name that an earlier entry of the same table already uses.
template <typename Entry>
[[nodiscard]] bool IsNameRepeated(const Array<Entry>& entries, usize count, std::string_view name) noexcept
{
    for (usize i = 0; i < count; ++i)
    {
        if (entries[i].Name.GetView() == name)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] SceneSourceStatus ReadDocument(JsonValue root, Failure& inoutFailure, SceneSource& outScene) noexcept
{
    const JsonPath rootPath;
    SceneSourceStatus status = CheckMembers(root,
                                            {"format", "version", "name", "materials", "objects"},
                                            {"description", "camera"},
                                            rootPath,
                                            inoutFailure);
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }
    std::string_view firstKey;
    JsonValue format;
    std::string_view formatText;
    if (root.MemberAt(0, firstKey, format) == false || firstKey != "format" || format.String(formatText) == false ||
        formatText != SCENE_SOURCE_FORMAT)
    {
        return inoutFailure.Fail(SceneSourceStatus::UnsupportedVersion,
                                 "the first member must be \"format\": \"ludus.scene\"",
                                 JsonPath(rootPath, "format"));
    }
    uint64 version = 0;
    if (root.Get("version").Integer(version) == false || version != SCENE_SOURCE_VERSION)
    {
        return inoutFailure.Fail(SceneSourceStatus::UnsupportedVersion, "must be 1", JsonPath(rootPath, "version"));
    }
    status = ReadText(root.Get("name"), false, JsonPath(rootPath, "name"), inoutFailure, outScene.Name);
    const JsonValue description = root.Get("description");
    if (status == SceneSourceStatus::Success && description.Valid() == true)
    {
        status = ReadText(description, true, JsonPath(rootPath, "description"), inoutFailure, outScene.Description);
    }
    const JsonValue camera = root.Get("camera");
    if (status == SceneSourceStatus::Success && camera.Valid() == true)
    {
        status = ReadCamera(camera, JsonPath(rootPath, "camera"), inoutFailure, outScene.Camera);
    }
    if (status != SceneSourceStatus::Success)
    {
        return status;
    }

    const JsonPath materialsPath(rootPath, "materials");
    const JsonValue materials = root.Get("materials");
    if (materials.Array() == false || materials.Count() == 0)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a nonempty array", materialsPath);
    }
    if (materials.Count() > MAX_SCENE_SOURCE_MATERIALS)
    {
        return inoutFailure.Fail(SceneSourceStatus::Limit, "has too many materials", materialsPath);
    }
    if (outScene.Materials.TryResize(materials.Count()) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the materials", materialsPath);
    }
    for (usize i = 0; i < materials.Count(); ++i)
    {
        const JsonPath path(materialsPath, i);
        status = ReadMaterial(materials.At(i), path, inoutFailure, outScene.Materials[i]);
        if (status != SceneSourceStatus::Success)
        {
            return status;
        }
        if (IsNameRepeated(outScene.Materials, i, outScene.Materials[i].Name.GetView()) == true)
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidValue,
                                     "repeats a material name",
                                     JsonPath(path, "name"));
        }
    }

    const JsonPath objectsPath(rootPath, "objects");
    const JsonValue objects = root.Get("objects");
    if (objects.Array() == false || objects.Count() == 0)
    {
        return inoutFailure.Fail(SceneSourceStatus::Malformed, "must be a nonempty array", objectsPath);
    }
    if (objects.Count() > MAX_SCENE_SOURCE_OBJECTS)
    {
        return inoutFailure.Fail(SceneSourceStatus::Limit, "has too many objects", objectsPath);
    }
    if (outScene.Objects.TryResize(objects.Count()) == false)
    {
        return inoutFailure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the objects", objectsPath);
    }
    DocumentTotals totals;
    for (usize i = 0; i < objects.Count(); ++i)
    {
        const JsonPath path(objectsPath, i);
        status = ReadObject(objects.At(i), outScene.Materials, path, inoutFailure, totals, outScene.Objects[i]);
        if (status != SceneSourceStatus::Success)
        {
            return status;
        }
        if (IsNameRepeated(outScene.Objects, i, outScene.Objects[i].Name.GetView()) == true)
        {
            return inoutFailure.Fail(SceneSourceStatus::InvalidValue, "repeats an object name", JsonPath(path, "name"));
        }
    }
    return SceneSourceStatus::Success;
}

[[nodiscard]] LUDUS_INLINE bool IsJsonWhitespace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

/// Advance `inoutAt` past JSON whitespace.
void SkipWhitespace(std::string_view text, usize& inoutAt) noexcept
{
    while (inoutAt < text.size() && IsJsonWhitespace(text[inoutAt]) == true)
    {
        ++inoutAt;
    }
}

/// Consume `token` at `inoutAt` after optional whitespace.
[[nodiscard]] bool ConsumeToken(std::string_view text, std::string_view token, usize& inoutAt) noexcept
{
    SkipWhitespace(text, inoutAt);
    if (text.substr(inoutAt, token.size()) != token)
    {
        return false;
    }
    inoutAt += token.size();
    return true;
}
} // namespace

std::string_view GetSceneSourceStatusName(SceneSourceStatus status) noexcept
{
    switch (status)
    {
        case SceneSourceStatus::Success:
            return "success";
        case SceneSourceStatus::Malformed:
            return "malformed";
        case SceneSourceStatus::UnsupportedVersion:
            return "unsupported_version";
        case SceneSourceStatus::InvalidValue:
            return "invalid_value";
        case SceneSourceStatus::UnresolvedReference:
            return "unresolved_reference";
        case SceneSourceStatus::InvalidGeometry:
            return "invalid_geometry";
        case SceneSourceStatus::Limit:
            return "limit";
        case SceneSourceStatus::OutOfMemory:
            return "out_of_memory";
    }
    return "unknown";
}

bool IsSceneSourcePrefix(std::string_view prefix) noexcept
{
    usize at = 0;
    if (ConsumeToken(prefix, "{", at) == false || ConsumeToken(prefix, "\"format\"", at) == false ||
        ConsumeToken(prefix, ":", at) == false || ConsumeToken(prefix, "\"", at) == false)
    {
        return false;
    }
    const std::string_view value = prefix.substr(at);
    return value.size() > SCENE_SOURCE_FORMAT.size() &&
           value.substr(0, SCENE_SOURCE_FORMAT.size()) == SCENE_SOURCE_FORMAT &&
           value[SCENE_SOURCE_FORMAT.size()] == '"';
}

SceneSourceStatus ParseSceneSource(std::string_view text, SceneSource& outScene, SceneSourceError& outError) noexcept
{
    SceneSourceError error;
    Failure failure(error);
    const JsonPath rootPath;
    if (text.size() > MAX_SCENE_SOURCE_BYTES)
    {
        const SceneSourceStatus status = failure.Fail(SceneSourceStatus::Limit, "the document is too large", rootPath);
        outError = error;
        return status;
    }
    JsonLimits limits;
    limits.MaxInputBytes = MAX_SCENE_SOURCE_BYTES;
    limits.MaxWorkspaceBytes = MAX_DOCUMENT_WORKSPACE_BYTES;
    limits.MaxDepth = MAX_DOCUMENT_DEPTH;
    limits.MaxObjectMembers = MAX_DOCUMENT_MEMBERS;
    limits.MaxArrayElements = MAX_SCENE_SOURCE_CORNERS;
    limits.MaxValues = MAX_DOCUMENT_VALUES;
    limits.MaxStringBytes = MAX_SCENE_SOURCE_TEXT_BYTES;
    JsonDocument document;
    ParseError parseError;
    SceneSourceStatus status = SceneSourceStatus::Success;
    switch (document.Read(text, parseError, limits))
    {
        case ParseStatus::Ok:
            break;
        case ParseStatus::LimitExceeded:
            status = failure.Fail(SceneSourceStatus::Limit, "the document exceeds a parser limit", rootPath);
            break;
        case ParseStatus::OutOfMemory:
            status = failure.Fail(SceneSourceStatus::OutOfMemory, "cannot allocate the parser workspace", rootPath);
            break;
        case ParseStatus::DuplicateKey:
            status = failure.Fail(SceneSourceStatus::Malformed, "the document repeats a member", rootPath);
            break;
        case ParseStatus::InvalidSyntax:
        case ParseStatus::InvalidEncoding:
        case ParseStatus::InvalidState:
            status = failure.Fail(SceneSourceStatus::Malformed, "the document is not valid JSON", rootPath);
            break;
    }
    SceneSource scene;
    if (status == SceneSourceStatus::Success)
    {
        status = ReadDocument(document.Root(), failure, scene);
    }
    if (status != SceneSourceStatus::Success)
    {
        outError = error;
        return status;
    }
    outScene = static_cast<SceneSource&&>(scene);
    return SceneSourceStatus::Success;
}
} // namespace ludus::content::scene_source
