#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QImage>
#include <QSize>
#include <QString>

namespace ludus::editor
{
/// A saved linear capture read back from a LUHDR01 file: its pixels, sample counts and the camera it was taken from.
/// Any producer's file qualifies, for example a Reference capture, a Compare graphs baseline or the edge-diffraction
/// oracle's references in tests/scenes/diffraction/references. The layout is Renderer::EncodeReferenceHdr's
/// (scripts/python/capture_hdr.py documents it).
struct ReferenceHdrFile final
{
    QSize Extent;
    foundation::uint32 Flags = 0;
    foundation::uint32 TargetSamples = 0;
    /// Positive for optical transport, in metres; zero for geometric transport.
    foundation::float32 OpticalWavelengthWorld = 0;
    /// The camera: WorldToClip, CameraPosition, Depth, InfiniteFar and OrthographicCamera; other members default.
    graphics::renderer::ViewDescription View{};
    /// Extent.width() * Extent.height() pixels, top-left first. Only Radiance and Samples are read from the file.
    foundation::Array<graphics::renderer::ReferencePixel> Pixels;
    /// The capture's sample counts, extent and completion, as a live capture reports them.
    graphics::renderer::ReferenceCaptureInfo Info{};
};

/// Reads a LUHDR01 file. Returns false, leaving outFile unchanged, with outError naming the problem and the next step:
/// unreadable, another format, a truncated or oversized image, or non-finite or negative radiance.
[[nodiscard]] bool ReadReferenceHdrFile(const QString& path, ReferenceHdrFile& outFile, QString& outError);

/// The file's display image: each pixel's radiance through EncodeReferenceDisplayColor, grey where no sample exists.
[[nodiscard]] QImage MakeReferenceHdrDisplayImage(const ReferenceHdrFile& file);
} // namespace ludus::editor
