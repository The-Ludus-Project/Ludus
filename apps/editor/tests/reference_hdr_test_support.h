#pragma once

// Writes synthetic LUHDR01 files for the editor's linear-baseline tests, in Renderer::EncodeReferenceHdr's layout
// (scripts/python/capture_hdr.py documents it).

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QByteArray>
#include <QFile>
#include <QSize>
#include <QString>

#include <cstring>

namespace ludus::editor::tests
{
/// What a synthetic LUHDR01 file holds. Pixel (column, row) has radiance Grey + ColumnSlope * column in every channel
/// and one committed sample.
struct TestReferenceHdr final
{
    QSize Extent{4, 3};
    foundation::uint32 Flags = 0;
    foundation::float32 OpticalWavelengthWorld = 0;
    foundation::math::Matrix4 WorldToClip = foundation::math::Matrix4::Identity();
    foundation::math::Vector3 CameraPosition{};
    foundation::float32 Grey = 0;
    foundation::float32 ColumnSlope = 0;
};

inline constexpr foundation::uint32 TEST_HDR_CPU_ORACLE_FLAG = 1;
inline constexpr foundation::uint32 TEST_HDR_REVERSE_Z_FLAG = 8;
inline constexpr foundation::uint32 TEST_HDR_OPTICAL_FLAG = 256;
inline constexpr foundation::usize TEST_HDR_HEADER_BYTES = 256;
inline constexpr foundation::usize TEST_HDR_PIXEL_BYTES = 16;

// The writers take (bytes, offset, value) like FoundationBase's byte-order codecs.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
inline void PutTestHdrUint32(QByteArray& inoutBytes, foundation::usize offset, foundation::uint32 value)
{
    constexpr foundation::uint32 BYTE_BITS = 8;
    constexpr foundation::uint32 BYTE_MASK = 0xFF;
    for (foundation::usize i = 0; i < sizeof(value); ++i)
    {
        const qsizetype index = static_cast<qsizetype>(offset) + static_cast<qsizetype>(i);
        inoutBytes[index] = static_cast<char>((value >> (BYTE_BITS * i)) & BYTE_MASK);
    }
}

inline void PutTestHdrFloat32(QByteArray& inoutBytes, foundation::usize offset, foundation::float32 value)
{
    foundation::uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PutTestHdrUint32(inoutBytes, offset, bits);
}

// NOLINTEND(bugprone-easily-swappable-parameters)

/// The file's bytes.
[[nodiscard]] inline QByteArray MakeTestReferenceHdr(const TestReferenceHdr& description)
{
    const foundation::usize pixels = static_cast<foundation::usize>(description.Extent.width()) *
                                     static_cast<foundation::usize>(description.Extent.height());
    QByteArray bytes(static_cast<qsizetype>(TEST_HDR_HEADER_BYTES + pixels * TEST_HDR_PIXEL_BYTES), 0);
    std::memcpy(bytes.data(), "LUHDR01", sizeof("LUHDR01"));
    constexpr foundation::uint32 SCHEMA = 1;
    constexpr foundation::uint32 PROFILE = 8;
    constexpr foundation::uint32 UNIFORM_AREA_SAMPLER = 1;
    PutTestHdrUint32(bytes, 8, SCHEMA);
    PutTestHdrUint32(bytes, 12, PROFILE);
    PutTestHdrUint32(bytes, 16, UNIFORM_AREA_SAMPLER);
    PutTestHdrUint32(bytes, 20, static_cast<foundation::uint32>(TEST_HDR_HEADER_BYTES));
    PutTestHdrUint32(bytes, 24, static_cast<foundation::uint32>(description.Extent.width()));
    PutTestHdrUint32(bytes, 28, static_cast<foundation::uint32>(description.Extent.height()));
    PutTestHdrUint32(bytes, 32, 1);
    PutTestHdrUint32(bytes, 36, 1);
    PutTestHdrUint32(bytes, 40, 1);
    PutTestHdrUint32(bytes, 44, description.Flags);
    PutTestHdrFloat32(bytes, 92, description.OpticalWavelengthWorld);
    constexpr foundation::usize WORLD_TO_CLIP_OFFSET = 96;
    constexpr foundation::usize CAMERA_OFFSET = 160;
    for (foundation::usize column = 0; column < 4; ++column)
    {
        const foundation::math::Vector4& values = description.WorldToClip.Columns[column];
        const foundation::usize offset = WORLD_TO_CLIP_OFFSET + column * 4 * sizeof(foundation::float32);
        PutTestHdrFloat32(bytes, offset, values.X);
        PutTestHdrFloat32(bytes, offset + 4, values.Y);
        PutTestHdrFloat32(bytes, offset + 8, values.Z);
        PutTestHdrFloat32(bytes, offset + 12, values.W);
    }
    PutTestHdrFloat32(bytes, CAMERA_OFFSET, description.CameraPosition.X);
    PutTestHdrFloat32(bytes, CAMERA_OFFSET + 4, description.CameraPosition.Y);
    PutTestHdrFloat32(bytes, CAMERA_OFFSET + 8, description.CameraPosition.Z);
    for (foundation::usize pixel = 0; pixel < pixels; ++pixel)
    {
        const foundation::usize column = pixel % static_cast<foundation::usize>(description.Extent.width());
        const foundation::float32 grey =
            description.Grey + description.ColumnSlope * static_cast<foundation::float32>(column);
        const foundation::usize record = TEST_HDR_HEADER_BYTES + pixel * TEST_HDR_PIXEL_BYTES;
        PutTestHdrFloat32(bytes, record, grey);
        PutTestHdrFloat32(bytes, record + 4, grey);
        PutTestHdrFloat32(bytes, record + 8, grey);
        PutTestHdrUint32(bytes, record + 12, 1);
    }
    return bytes;
}

/// Writes the file. Returns false when it cannot be written.
[[nodiscard]] inline bool WriteTestReferenceHdr(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) == true && file.write(bytes) == bytes.size();
}
} // namespace ludus::editor::tests
