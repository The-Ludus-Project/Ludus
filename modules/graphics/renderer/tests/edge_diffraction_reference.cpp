#include "edge_diffraction_reference.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/graphics/renderer/lighting_layout.h>

#include <cstring>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
namespace math = foundation::math;
namespace threading = foundation::threading;
namespace
{
constexpr uint8 LUHDR_MAGIC[8] = {'L', 'U', 'H', 'D', 'R', '0', '1', 0};
constexpr uint32 LUHDR_SCHEMA = 1;
/// The uniform-area light sampler; optical captures require it.
constexpr uint32 LUHDR_UNIFORM_AREA_SAMPLER = 1;
/// Flags the LUHDR01 decoder knows (scripts/python/capture_hdr.py).
constexpr uint32 LUHDR_KNOWN_FLAGS = 511;
/// The header offsets of the LUHDR01 fields this module uses (Renderer::EncodeReferenceHdr).
constexpr usize OFFSET_SCHEMA = 8;
constexpr usize OFFSET_PROFILE = 12;
constexpr usize OFFSET_SAMPLER = 16;
constexpr usize OFFSET_HEADER_BYTES = 20;
constexpr usize OFFSET_WIDTH = 24;
constexpr usize OFFSET_HEIGHT = 28;
constexpr usize OFFSET_TARGET_SAMPLES = 32;
constexpr usize OFFSET_MAX_DEPTH = 36;
constexpr usize OFFSET_ROULETTE_DEPTH = 40;
constexpr usize OFFSET_FLAGS = 44;
constexpr usize OFFSET_LIGHT_COUNT = 88;
constexpr usize OFFSET_OPTICAL_WAVELENGTH = 92;
constexpr usize OFFSET_WORLD_TO_CLIP = 96;
constexpr usize OFFSET_CAMERA_POSITION = 160;
constexpr usize MATRIX_ELEMENTS = 16;
constexpr usize RGB = 3;
/// The editor's headless reference capture builds the authored camera with these planes, in metres.
constexpr float32 AUTHORED_NEAR_PLANE_WORLD = 0.01F;
constexpr float32 AUTHORED_FAR_PLANE_WORLD = 1000;
/// Significant digits of the profile's numbers.
constexpr int32 PROFILE_DIGITS = 9;

// The byte codecs take (bytes, offset, value) like FoundationBase's byte-order codecs; IsClose compares two values.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] uint32 ReadUint32(std::span<const uint8> bytes, usize offset) noexcept
{
    uint32 value = 0;
    const bool isRead = TryReadLittleEndian(bytes.subspan(offset, sizeof(uint32)), value);
    return isRead == true ? value : 0;
}

[[nodiscard]] float32 ReadFloat32(std::span<const uint8> bytes, usize offset) noexcept
{
    const uint32 bits = ReadUint32(bytes, offset);
    float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void WriteUint32(std::span<uint8> bytes, usize offset, uint32 value) noexcept
{
    const bool isWritten = TryWriteLittleEndian(value, bytes.subspan(offset, sizeof(uint32)));
    LUDUS_ASSERT(isWritten == true);
}

void WriteFloat32(std::span<uint8> bytes, usize offset, float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteUint32(bytes, offset, bits);
}

[[nodiscard]] float32 GetMatrixElement(const math::Matrix4& matrix, usize index) noexcept
{
    const math::Vector4& column = matrix.Columns[index / 4];
    switch (index % 4)
    {
        case 0:
            return column.X;
        case 1:
            return column.Y;
        case 2:
            return column.Z;
        default:
            return column.W;
    }
}

void SetMatrixElement(math::Matrix4& inoutMatrix, usize index, float32 value) noexcept
{
    math::Vector4& column = inoutMatrix.Columns[index / 4];
    switch (index % 4)
    {
        case 0:
            column.X = value;
            break;
        case 1:
            column.Y = value;
            break;
        case 2:
            column.Z = value;
            break;
        default:
            column.W = value;
            break;
    }
}

/// |first - second| relative to the larger magnitude, with an absolute floor of the tolerance itself.
[[nodiscard]] bool IsClose(float32 first, float32 second, float64 tolerance) noexcept
{
    const float64 a = static_cast<float64>(first);
    const float64 b = static_cast<float64>(second);
    return math::Abs(a - b) <= tolerance * math::Max(1.0, math::Max(math::Abs(a), math::Abs(b)));
}

// NOLINTEND(bugprone-easily-swappable-parameters)

/// The RGB mean of a pixel.
[[nodiscard]] float64 GetPixelRadiance(const DiffractionCapture& capture, usize pixel) noexcept
{
    const float32* rgb = capture.Radiance.GetData() + pixel * RGB;
    return (static_cast<float64>(rgb[0]) + static_cast<float64>(rgb[1]) + static_cast<float64>(rgb[2])) / RGB;
}

/// Appends ASCII text.
[[nodiscard]] bool TryAppendText(Array<uint8>& inoutText, std::string_view text) noexcept
{
    for (const char character : text)
    {
        if (inoutText.TryAdd(static_cast<uint8>(character)) == false)
        {
            return false;
        }
    }
    return true;
}

/// Appends an unsigned decimal integer.
[[nodiscard]] bool TryAppendUnsigned(Array<uint8>& inoutText, uint64 value) noexcept
{
    constexpr usize MAXIMUM_DIGITS = 20;
    StaticArray<char, MAXIMUM_DIGITS> digits{};
    usize count = 0;
    do
    {
        digits[count] = static_cast<char>('0' + value % 10);
        ++count;
        value /= 10;
    } while (value > 0);
    for (usize i = count; i > 0; --i)
    {
        if (inoutText.TryAdd(static_cast<uint8>(digits[i - 1])) == false)
        {
            return false;
        }
    }
    return true;
}

/// Appends a finite value as d.dddddddde+-x with PROFILE_DIGITS significant digits; zero as 0.
[[nodiscard]] bool TryAppendScientific(Array<uint8>& inoutText, float64 value) noexcept
{
    if (math::IsFinite(value) == false)
    {
        return TryAppendText(inoutText, "nan");
    }
    if (value == 0)
    {
        return TryAppendText(inoutText, "0");
    }
    if (value < 0 && TryAppendText(inoutText, "-") == false)
    {
        return false;
    }
    const float64 magnitude = math::Abs(value);
    int32 exponent = static_cast<int32>(math::Floor(math::Log(magnitude) / math::Log(10.0)));
    const float64 lowest = math::Pow(10.0, static_cast<float64>(PROFILE_DIGITS - 1));
    float64 mantissa =
        math::Floor(magnitude / math::Pow(10.0, static_cast<float64>(exponent - PROFILE_DIGITS + 1)) + 0.5);
    // Logarithm rounding or the rounding above may leave one digit too many or too few.
    if (mantissa >= lowest * 10)
    {
        mantissa = math::Floor(mantissa / 10 + 0.5);
        ++exponent;
    }
    else if (mantissa < lowest)
    {
        mantissa *= 10;
        --exponent;
    }
    const uint64 digits = static_cast<uint64>(mantissa);
    const uint64 leading = digits / static_cast<uint64>(lowest);
    const uint64 trailing = digits % static_cast<uint64>(lowest);
    if (TryAppendUnsigned(inoutText, leading) == false || TryAppendText(inoutText, ".") == false)
    {
        return false;
    }
    // Zero-pad the fraction to PROFILE_DIGITS - 1 digits.
    for (uint64 scale = static_cast<uint64>(lowest) / 10; scale > 0 && trailing < scale; scale /= 10)
    {
        if (TryAppendText(inoutText, "0") == false)
        {
            return false;
        }
    }
    if (trailing > 0 && TryAppendUnsigned(inoutText, trailing) == false)
    {
        return false;
    }
    if (TryAppendText(inoutText, exponent < 0 ? "e-" : "e+") == false)
    {
        return false;
    }
    return TryAppendUnsigned(inoutText, static_cast<uint64>(exponent < 0 ? -exponent : exponent));
}

/// The rows one reference job renders.
struct ReferenceRows final
{
    const EdgeDiffractionScene* SceneOrNull = nullptr;
    const DiffractionSettings* SettingsOrNull = nullptr;
    DiffractionCapture* CaptureOrNull = nullptr;
    uint32 FirstRow = 0;
    uint32 EndRow = 0;
    bool HasFailed = false;
};

/// Renders a band of reference rows; a model that does not apply marks the band failed.
threading::JobOutcome RenderReferenceRows(void* context) noexcept
{
    ReferenceRows& rows = *static_cast<ReferenceRows*>(context);
    LUDUS_ASSERT(rows.SceneOrNull != nullptr && rows.SettingsOrNull != nullptr && rows.CaptureOrNull != nullptr);
    DiffractionCapture& capture = *rows.CaptureOrNull;
    for (uint32 row = rows.FirstRow; row < rows.EndRow; ++row)
    {
        for (uint32 column = 0; column < capture.Width; ++column)
        {
            const usize pixel = static_cast<usize>(row) * capture.Width + column;
            math::Vector3d wallWorld{};
            float64 radiance = 0;
            if (TryFindDiffractionWallPoint(*rows.SceneOrNull, column, row, capture.Width, capture.Height, wallWorld) ==
                    true &&
                TryComputeDiffractionWallRadiance(*rows.SceneOrNull, *rows.SettingsOrNull, wallWorld, radiance) ==
                    false)
            {
                rows.HasFailed = true;
                return threading::JobOutcome::Failed;
            }
            for (usize channel = 0; channel < RGB; ++channel)
            {
                capture.Radiance[pixel * RGB + channel] = static_cast<float32>(radiance);
            }
            capture.Samples[pixel] = 1;
        }
    }
    return threading::JobOutcome::Succeeded;
}
} // namespace

bool TryDecodeLuhdr(std::span<const uint8> bytes, DiffractionCapture& outCapture) noexcept
{
    if (bytes.size() < LUHDR_HEADER_BYTES || std::memcmp(bytes.data(), LUHDR_MAGIC, sizeof(LUHDR_MAGIC)) != 0 ||
        ReadUint32(bytes, OFFSET_SCHEMA) != LUHDR_SCHEMA ||
        ReadUint32(bytes, OFFSET_HEADER_BYTES) != LUHDR_HEADER_BYTES)
    {
        return false;
    }
    const uint32 width = ReadUint32(bytes, OFFSET_WIDTH);
    const uint32 height = ReadUint32(bytes, OFFSET_HEIGHT);
    const uint64 pixels = static_cast<uint64>(width) * height;
    if (width == 0 || height == 0 || bytes.size() != LUHDR_HEADER_BYTES + pixels * LUHDR_PIXEL_BYTES)
    {
        return false;
    }
    DiffractionCapture capture;
    capture.Width = width;
    capture.Height = height;
    capture.Flags = ReadUint32(bytes, OFFSET_FLAGS);
    capture.OpticalWavelengthWorld = ReadFloat32(bytes, OFFSET_OPTICAL_WAVELENGTH);
    for (usize i = 0; i < MATRIX_ELEMENTS; ++i)
    {
        SetMatrixElement(capture.WorldToClip, i, ReadFloat32(bytes, OFFSET_WORLD_TO_CLIP + i * sizeof(float32)));
    }
    capture.CameraPositionWorld = {ReadFloat32(bytes, OFFSET_CAMERA_POSITION),
                                   ReadFloat32(bytes, OFFSET_CAMERA_POSITION + sizeof(float32)),
                                   ReadFloat32(bytes, OFFSET_CAMERA_POSITION + 2 * sizeof(float32))};
    if (capture.Radiance.TryResize(static_cast<usize>(pixels) * RGB) == false ||
        capture.Samples.TryResize(static_cast<usize>(pixels)) == false)
    {
        return false;
    }
    for (usize pixel = 0; pixel < pixels; ++pixel)
    {
        const usize record = LUHDR_HEADER_BYTES + pixel * LUHDR_PIXEL_BYTES;
        for (usize channel = 0; channel < RGB; ++channel)
        {
            const float32 value = ReadFloat32(bytes, record + channel * sizeof(float32));
            if (math::IsFinite(value) == false || value < 0)
            {
                return false;
            }
            capture.Radiance[pixel * RGB + channel] = value;
        }
        capture.Samples[pixel] = ReadUint32(bytes, record + RGB * sizeof(float32));
    }
    outCapture = Move(capture);
    return true;
}

bool TryEncodeLuhdr(const DiffractionCapture& capture, Array<uint8>& outBytes) noexcept
{
    const usize pixels = static_cast<usize>(capture.Width) * capture.Height;
    if (pixels == 0 || capture.Radiance.GetSize() != pixels * RGB || capture.Samples.GetSize() != pixels ||
        (capture.Flags & ~LUHDR_KNOWN_FLAGS) != 0)
    {
        return false;
    }
    Array<uint8> bytes;
    if (bytes.TryResize(LUHDR_HEADER_BYTES + pixels * LUHDR_PIXEL_BYTES, 0) == false)
    {
        return false;
    }
    const std::span<uint8> view = bytes.AsSpan();
    std::memcpy(view.data(), LUHDR_MAGIC, sizeof(LUHDR_MAGIC));
    WriteUint32(view, OFFSET_SCHEMA, LUHDR_SCHEMA);
    WriteUint32(view, OFFSET_PROFILE, LIGHTING_PROFILE_VERSION);
    WriteUint32(view, OFFSET_SAMPLER, LUHDR_UNIFORM_AREA_SAMPLER);
    WriteUint32(view, OFFSET_HEADER_BYTES, LUHDR_HEADER_BYTES);
    WriteUint32(view, OFFSET_WIDTH, capture.Width);
    WriteUint32(view, OFFSET_HEIGHT, capture.Height);
    // One committed sample of depth one: the oracle evaluates each pixel once and exactly.
    WriteUint32(view, OFFSET_TARGET_SAMPLES, 1);
    WriteUint32(view, OFFSET_MAX_DEPTH, 1);
    WriteUint32(view, OFFSET_ROULETTE_DEPTH, 1);
    WriteUint32(view, OFFSET_FLAGS, capture.Flags);
    WriteUint32(view, OFFSET_LIGHT_COUNT, 1);
    WriteFloat32(view, OFFSET_OPTICAL_WAVELENGTH, capture.OpticalWavelengthWorld);
    for (usize i = 0; i < MATRIX_ELEMENTS; ++i)
    {
        WriteFloat32(view, OFFSET_WORLD_TO_CLIP + i * sizeof(float32), GetMatrixElement(capture.WorldToClip, i));
    }
    WriteFloat32(view, OFFSET_CAMERA_POSITION, capture.CameraPositionWorld.X);
    WriteFloat32(view, OFFSET_CAMERA_POSITION + sizeof(float32), capture.CameraPositionWorld.Y);
    WriteFloat32(view, OFFSET_CAMERA_POSITION + 2 * sizeof(float32), capture.CameraPositionWorld.Z);
    for (usize pixel = 0; pixel < pixels; ++pixel)
    {
        const usize record = LUHDR_HEADER_BYTES + pixel * LUHDR_PIXEL_BYTES;
        for (usize channel = 0; channel < RGB; ++channel)
        {
            WriteFloat32(view, record + channel * sizeof(float32), capture.Radiance[pixel * RGB + channel]);
        }
        WriteUint32(view, record + RGB * sizeof(float32), capture.Samples[pixel]);
    }
    outBytes = Move(bytes);
    return true;
}

bool TryRenderDiffractionReference(const EdgeDiffractionScene& scene,
                                   const DiffractionSettings& settings,
                                   DiffractionExtent extent,
                                   uint32 workerCount,
                                   DiffractionCapture& outCapture) noexcept
{
    const uint32 width = extent.Width;
    const uint32 height = extent.Height;
    if (width == 0 || height == 0 || settings.LightQuadratureNodes == 0 ||
        math::IsFinite(settings.WavelengthWorld) == false || settings.WavelengthWorld <= 0)
    {
        return false;
    }
    DiffractionCapture capture;
    capture.Width = width;
    capture.Height = height;
    const bool isOptical = settings.Model != DiffractionModel::Geometric;
    capture.Flags = LUHDR_CPU_ORACLE_FLAG | LUHDR_CENTER_RAYS_FLAG | LUHDR_REVERSE_Z_FLAG |
                    (isOptical == true ? LUHDR_OPTICAL_FLAG : 0U);
    capture.OpticalWavelengthWorld = isOptical == true ? static_cast<float32>(settings.WavelengthWorld) : 0.0F;
    const math::Vector3 eye{static_cast<float32>(scene.EyeWorld.X),
                            static_cast<float32>(scene.EyeWorld.Y),
                            static_cast<float32>(scene.EyeWorld.Z)};
    const math::Vector3 target{static_cast<float32>(scene.TargetWorld.X),
                               static_cast<float32>(scene.TargetWorld.Y),
                               static_cast<float32>(scene.TargetWorld.Z)};
    const math::Vector3 up{static_cast<float32>(scene.UpWorld.X),
                           static_cast<float32>(scene.UpWorld.Y),
                           static_cast<float32>(scene.UpWorld.Z)};
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    if (math::TryLookAt(eye, target, up, worldToView) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(static_cast<float32>(scene.VerticalFovRadians),
                                     static_cast<float32>(width) / static_cast<float32>(height),
                                     AUTHORED_NEAR_PLANE_WORLD,
                                     AUTHORED_FAR_PLANE_WORLD,
                                     viewToClip) != math::MathStatus::Success)
    {
        return false;
    }
    capture.WorldToClip = viewToClip * worldToView;
    capture.CameraPositionWorld = eye;
    const usize pixels = static_cast<usize>(width) * height;
    if (capture.Radiance.TryResize(pixels * RGB, 0.0F) == false || capture.Samples.TryResize(pixels, 0U) == false)
    {
        return false;
    }
    // One job per band of rows; bands outnumber workers so that slow rows (where the obstacles' edges see the wall)
    // balance out.
    constexpr uint32 BANDS = 64;
    const uint32 bandCount = math::Min(BANDS, height);
    StaticArray<ReferenceRows, BANDS> bands{};
    for (uint32 band = 0; band < bandCount; ++band)
    {
        bands[band].SceneOrNull = &scene;
        bands[band].SettingsOrNull = &settings;
        bands[band].CaptureOrNull = &capture;
        bands[band].FirstRow = height * band / bandCount;
        bands[band].EndRow = height * (band + 1) / bandCount;
    }
    threading::JobSystem jobs;
    threading::JobGraph graph;
    if (jobs.Initialize({ .WorkerCount = workerCount }) != threading::JobStatus::Ok ||
        graph.Initialize({ .MaxJobs = BANDS, .MaxDependencies = 0 }) != threading::JobStatus::Ok)
    {
        return false;
    }
    for (uint32 band = 0; band < bandCount; ++band)
    {
        threading::JobHandle handle;
        if (graph.Add(RenderReferenceRows, &bands[band], handle) != threading::JobStatus::Ok)
        {
            return false;
        }
    }
    threading::JobOutcome outcome = threading::JobOutcome::Pending;
    if (graph.Seal() != threading::JobStatus::Ok || jobs.Submit(graph) != threading::JobStatus::Ok ||
        jobs.Wait(outcome) != threading::JobStatus::Ok)
    {
        return false;
    }
    if (jobs.Shutdown() != threading::JobStatus::Ok)
    {
        return false;
    }
    for (uint32 band = 0; band < bandCount; ++band)
    {
        if (bands[band].HasFailed == true)
        {
            return false;
        }
    }
    if (outcome != threading::JobOutcome::Succeeded)
    {
        return false;
    }
    outCapture = Move(capture);
    return true;
}

bool TryCompareDiffractionCapture(const DiffractionCapture& reference,
                                  const DiffractionCapture& capture,
                                  const DiffractionTolerance& tolerance,
                                  DiffractionComparison& outComparison) noexcept
{
    if (reference.Width != capture.Width || reference.Height != capture.Height || reference.Width == 0)
    {
        return false;
    }
    DiffractionComparison comparison;
    comparison.HasMatchingCamera =
        IsClose(reference.CameraPositionWorld.X, capture.CameraPositionWorld.X, tolerance.Camera) == true &&
        IsClose(reference.CameraPositionWorld.Y, capture.CameraPositionWorld.Y, tolerance.Camera) == true &&
        IsClose(reference.CameraPositionWorld.Z, capture.CameraPositionWorld.Z, tolerance.Camera) == true;
    for (usize i = 0; i < MATRIX_ELEMENTS; ++i)
    {
        comparison.HasMatchingCamera =
            comparison.HasMatchingCamera == true && IsClose(GetMatrixElement(reference.WorldToClip, i),
                                                            GetMatrixElement(capture.WorldToClip, i),
                                                            tolerance.Camera) == true;
    }
    comparison.HasMatchingWavelength =
        reference.OpticalWavelengthWorld == capture.OpticalWavelengthWorld ||
        (reference.OpticalWavelengthWorld > 0 &&
         IsClose(reference.OpticalWavelengthWorld, capture.OpticalWavelengthWorld, 1e-6) == true);
    float64 largestColumn = 0;
    float64 referenceTotal = 0;
    float64 captureTotal = 0;
    float64 differenceTotal = 0;
    // First pass: the largest column, to decide which columns are significant.
    for (uint32 column = 0; column < reference.Width; ++column)
    {
        float64 referenceColumn = 0;
        for (uint32 row = 0; row < reference.Height; ++row)
        {
            const usize pixel = static_cast<usize>(row) * reference.Width + column;
            if (reference.Samples[pixel] > 0 && capture.Samples[pixel] > 0)
            {
                referenceColumn += GetPixelRadiance(reference, pixel);
            }
        }
        largestColumn = math::Max(largestColumn, referenceColumn);
    }
    if (largestColumn <= 0)
    {
        return false;
    }
    float64 worstError = -1;
    for (uint32 column = 0; column < reference.Width; ++column)
    {
        float64 referenceColumn = 0;
        float64 captureColumn = 0;
        for (uint32 row = 0; row < reference.Height; ++row)
        {
            const usize pixel = static_cast<usize>(row) * reference.Width + column;
            if (reference.Samples[pixel] == 0 || capture.Samples[pixel] == 0)
            {
                continue;
            }
            const float64 expected = GetPixelRadiance(reference, pixel);
            const float64 measured = GetPixelRadiance(capture, pixel);
            referenceColumn += expected;
            captureColumn += measured;
            differenceTotal += math::Abs(measured - expected);
            ++comparison.ComparedPixels;
        }
        referenceTotal += referenceColumn;
        captureTotal += captureColumn;
        if (referenceColumn < tolerance.SignificantFraction * largestColumn)
        {
            continue;
        }
        ++comparison.SignificantColumns;
        const float64 ratio = captureColumn / referenceColumn;
        if (math::Abs(ratio - 1) > worstError)
        {
            worstError = math::Abs(ratio - 1);
            comparison.WorstColumn = column;
            comparison.WorstColumnRatio = ratio;
        }
    }
    comparison.TotalRatio = captureTotal / referenceTotal;
    comparison.RelativeL1 = differenceTotal / referenceTotal;
    comparison.IsPassing = comparison.HasMatchingCamera == true && comparison.HasMatchingWavelength == true &&
                           worstError <= tolerance.ColumnRatio;
    outComparison = comparison;
    return true;
}

bool TryWriteDiffractionProfile(const EdgeDiffractionScene& scene,
                                const DiffractionCapture& reference,
                                const DiffractionCapture* captureOrNull,
                                Array<uint8>& outText) noexcept
{
    if (captureOrNull != nullptr &&
        (captureOrNull->Width != reference.Width || captureOrNull->Height != reference.Height))
    {
        return false;
    }
    const uint32 centreRow = reference.Height / 2;
    Array<uint8> text;
    bool isWritten = TryAppendText(
        text,
        "column,wall_x_m,reference_centre_row,reference_column_mean,capture_centre_row,capture_column_mean,ratio\n");
    for (uint32 column = 0; column < reference.Width && isWritten == true; ++column)
    {
        float64 referenceSum = 0;
        float64 captureSum = 0;
        uint32 count = 0;
        for (uint32 row = 0; row < reference.Height; ++row)
        {
            const usize pixel = static_cast<usize>(row) * reference.Width + column;
            const bool isCaptured = captureOrNull == nullptr || captureOrNull->Samples[pixel] > 0;
            if (reference.Samples[pixel] == 0 || isCaptured == false)
            {
                continue;
            }
            referenceSum += GetPixelRadiance(reference, pixel);
            if (captureOrNull != nullptr)
            {
                captureSum += GetPixelRadiance(*captureOrNull, pixel);
            }
            ++count;
        }
        const usize centrePixel = static_cast<usize>(centreRow) * reference.Width + column;
        math::Vector3d wallWorld{};
        const bool hasWall =
            TryFindDiffractionWallPoint(scene, column, centreRow, reference.Width, reference.Height, wallWorld);
        isWritten = TryAppendUnsigned(text, column) == true && TryAppendText(text, ",") == true &&
                    (hasWall == false || TryAppendScientific(text, wallWorld.X) == true) &&
                    TryAppendText(text, ",") == true;
        // Reference centre row and column mean.
        isWritten = isWritten == true &&
                    (reference.Samples[centrePixel] == 0 ||
                     TryAppendScientific(text, GetPixelRadiance(reference, centrePixel)) == true) &&
                    TryAppendText(text, ",") == true &&
                    (count == 0 || TryAppendScientific(text, referenceSum / static_cast<float64>(count)) == true) &&
                    TryAppendText(text, ",") == true;
        // Capture centre row, column mean and the ratio of the column sums.
        if (isWritten == true && captureOrNull != nullptr)
        {
            isWritten = (captureOrNull->Samples[centrePixel] == 0 ||
                         TryAppendScientific(text, GetPixelRadiance(*captureOrNull, centrePixel)) == true) &&
                        TryAppendText(text, ",") == true &&
                        (count == 0 || TryAppendScientific(text, captureSum / static_cast<float64>(count)) == true) &&
                        TryAppendText(text, ",") == true &&
                        (referenceSum <= 0 || TryAppendScientific(text, captureSum / referenceSum) == true);
        }
        else if (isWritten == true)
        {
            isWritten = TryAppendText(text, ",,") == true;
        }
        isWritten = isWritten == true && TryAppendText(text, "\n") == true;
    }
    if (isWritten == false)
    {
        return false;
    }
    outText = Move(text);
    return true;
}
} // namespace ludus::graphics::renderer::testing
