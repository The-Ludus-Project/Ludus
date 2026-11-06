#include "internal/reference_hdr_file.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include "internal/viewport_reference_capture.h"

#include <QFile>

#include <cstring>
#include <span>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace renderer = graphics::renderer;

constexpr char HDR_MAGIC[] = "LUHDR01";
constexpr usize HDR_MAGIC_BYTES = sizeof(HDR_MAGIC);
constexpr uint32 HDR_SCHEMA = 1;
/// LUHDR01 header bytes before the pixel records.
constexpr usize HDR_HEADER_BYTES = 256;
/// LUHDR01 bytes per pixel: three float32 means and a uint32 committed count.
constexpr usize HDR_PIXEL_BYTES = 16;
/// The flags the format defines (scripts/python/capture_hdr.py).
constexpr uint32 HDR_KNOWN_FLAGS = 511;
constexpr uint32 HDR_ORTHOGRAPHIC_FLAG = 4;
constexpr uint32 HDR_REVERSE_Z_FLAG = 8;
constexpr uint32 HDR_INFINITE_FAR_FLAG = 16;
constexpr uint32 HDR_PARTIAL_FLAG = 32;
/// Header offsets of the fields read here (Renderer::EncodeReferenceHdr).
constexpr usize OFFSET_SCHEMA = 8;
constexpr usize OFFSET_HEADER_BYTES = 20;
constexpr usize OFFSET_WIDTH = 24;
constexpr usize OFFSET_HEIGHT = 28;
constexpr usize OFFSET_TARGET_SAMPLES = 32;
constexpr usize OFFSET_FLAGS = 44;
constexpr usize OFFSET_OPTICAL_WAVELENGTH = 92;
constexpr usize OFFSET_WORLD_TO_CLIP = 96;
constexpr usize OFFSET_CAMERA_POSITION = 160;
constexpr usize MATRIX_COLUMNS = 4;
constexpr usize MATRIX_ROWS = 4;
constexpr usize RGB = 3;
/// Larger images than the Reference window can capture are not baselines.
constexpr uint32 MAXIMUM_HDR_SIDE = 16384;
/// Grey shown for a pixel without samples.
constexpr int32 MISSING_PIXEL_GREY = 64;
constexpr float32 DISPLAY_CHANNEL_MAXIMUM = 255;

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

[[nodiscard]] int32 ToDisplayChannel(float32 value) noexcept
{
    return static_cast<int32>(math::Floor(static_cast<float64>(value * DISPLAY_CHANNEL_MAXIMUM) + 0.5));
}
} // namespace

bool ReadReferenceHdrFile(const QString& path, ReferenceHdrFile& outFile, QString& outError)
{
    QFile source(path);
    if (source.open(QIODevice::ReadOnly) == false)
    {
        outError =
            QStringLiteral("Could not open %1: %2. Choose a readable .luhdr file.").arg(path, source.errorString());
        return false;
    }
    const QByteArray data = source.readAll();
    const std::span<const uint8> bytes{reinterpret_cast<const uint8*>(data.constData()),
                                       static_cast<usize>(data.size())};
    if (bytes.size() < HDR_HEADER_BYTES || std::memcmp(bytes.data(), HDR_MAGIC, HDR_MAGIC_BYTES) != 0 ||
        ReadUint32(bytes, OFFSET_SCHEMA) != HDR_SCHEMA || ReadUint32(bytes, OFFSET_HEADER_BYTES) != HDR_HEADER_BYTES)
    {
        outError = QStringLiteral("%1 is not a LUHDR01 capture. Save a linear capture with Reference capture → Save…, "
                                  "or choose a reference from tests/scenes/diffraction/references.")
                       .arg(path);
        return false;
    }
    const uint32 width = ReadUint32(bytes, OFFSET_WIDTH);
    const uint32 height = ReadUint32(bytes, OFFSET_HEIGHT);
    const uint32 flags = ReadUint32(bytes, OFFSET_FLAGS);
    const usize pixels = static_cast<usize>(width) * height;
    if (width == 0 || height == 0 || width > MAXIMUM_HDR_SIDE || height > MAXIMUM_HDR_SIDE ||
        (flags & ~HDR_KNOWN_FLAGS) != 0 || bytes.size() != HDR_HEADER_BYTES + pixels * HDR_PIXEL_BYTES)
    {
        outError = QStringLiteral("%1 is truncated or has an unsupported extent (%2 x %3) or flags. Save the capture "
                                  "again.")
                       .arg(path)
                       .arg(width)
                       .arg(height);
        return false;
    }
    ReferenceHdrFile file;
    file.Extent = QSize(static_cast<int32>(width), static_cast<int32>(height));
    file.Flags = flags;
    file.TargetSamples = ReadUint32(bytes, OFFSET_TARGET_SAMPLES);
    file.OpticalWavelengthWorld = ReadFloat32(bytes, OFFSET_OPTICAL_WAVELENGTH);
    for (usize column = 0; column < MATRIX_COLUMNS; ++column)
    {
        const usize offset = OFFSET_WORLD_TO_CLIP + column * MATRIX_ROWS * sizeof(float32);
        constexpr usize W_ELEMENT = 3;
        file.View.WorldToClip.Columns[column] = {ReadFloat32(bytes, offset),
                                                 ReadFloat32(bytes, offset + sizeof(float32)),
                                                 ReadFloat32(bytes, offset + 2 * sizeof(float32)),
                                                 ReadFloat32(bytes, offset + W_ELEMENT * sizeof(float32))};
    }
    file.View.CameraPosition = {ReadFloat32(bytes, OFFSET_CAMERA_POSITION),
                                ReadFloat32(bytes, OFFSET_CAMERA_POSITION + sizeof(float32)),
                                ReadFloat32(bytes, OFFSET_CAMERA_POSITION + 2 * sizeof(float32))};
    file.View.Depth = (flags & HDR_REVERSE_Z_FLAG) != 0 ? renderer::DepthConvention::ReverseZ
                                                        : renderer::DepthConvention::Conventional;
    file.View.InfiniteFar = (flags & HDR_INFINITE_FAR_FLAG) != 0;
    file.View.OrthographicCamera = (flags & HDR_ORTHOGRAPHIC_FLAG) != 0;
    if (file.Pixels.TryResize(pixels) == false)
    {
        outError = QStringLiteral("Could not allocate the %1 x %2 baseline. Close other captures and try again.")
                       .arg(width)
                       .arg(height);
        return false;
    }
    uint32 minimum = 0;
    uint32 maximum = 0;
    for (usize pixel = 0; pixel < pixels; ++pixel)
    {
        const usize record = HDR_HEADER_BYTES + pixel * HDR_PIXEL_BYTES;
        const math::Vector3 radiance{ReadFloat32(bytes, record),
                                     ReadFloat32(bytes, record + sizeof(float32)),
                                     ReadFloat32(bytes, record + 2 * sizeof(float32))};
        for (const float32 channel : {radiance.X, radiance.Y, radiance.Z})
        {
            if (math::IsFinite(channel) == false || channel < 0)
            {
                outError = QStringLiteral("%1 has a non-finite or negative radiance at pixel %2. Save the capture "
                                          "again.")
                               .arg(path)
                               .arg(pixel);
                return false;
            }
        }
        const uint32 samples = ReadUint32(bytes, record + RGB * sizeof(float32));
        file.Pixels[pixel].Radiance = radiance;
        file.Pixels[pixel].Samples = samples;
        minimum = pixel == 0 ? samples : math::Min(minimum, samples);
        maximum = math::Max(maximum, samples);
    }
    file.Info.Width = width;
    file.Info.Height = height;
    file.Info.TargetSamples = file.TargetSamples;
    file.Info.MinimumSamples = minimum;
    file.Info.MaximumSamples = maximum;
    file.Info.Complete = (flags & HDR_PARTIAL_FLAG) == 0 && minimum >= file.TargetSamples;
    outFile = Move(file);
    return true;
}

QImage MakeReferenceHdrDisplayImage(const ReferenceHdrFile& file)
{
    QImage image(file.Extent, QImage::Format_RGB32);
    if (image.isNull() == true)
    {
        return image;
    }
    for (int32 row = 0; row < file.Extent.height(); ++row)
    {
        for (int32 column = 0; column < file.Extent.width(); ++column)
        {
            const renderer::ReferencePixel& pixel =
                file.Pixels[static_cast<usize>(row) * static_cast<usize>(file.Extent.width()) +
                            static_cast<usize>(column)];
            if (pixel.Samples == 0)
            {
                image.setPixel(column, row, qRgb(MISSING_PIXEL_GREY, MISSING_PIXEL_GREY, MISSING_PIXEL_GREY));
                continue;
            }
            const math::Vector3 display = EncodeReferenceDisplayColor(pixel.Radiance);
            image.setPixel(column,
                           row,
                           qRgb(ToDisplayChannel(display.X), ToDisplayChannel(display.Y), ToDisplayChannel(display.Z)));
        }
    }
    return image;
}
} // namespace ludus::editor
