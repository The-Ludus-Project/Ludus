// Measures the production editor texture viewer and graph readback conversion.
// No window/GPU is needed; each sample checks the produced pixels independently.
#include "internal/texture_inspection.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QString>
#include <QTextStream>

#include <cstdio>
#include <initializer_list>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace profile = ludus::foundation::profiling;
namespace
{
constexpr uint32 SMALL_WIDTH = 320;
constexpr uint32 SMALL_HEIGHT = 180;
constexpr uint32 LARGE_WIDTH = 1280;
constexpr uint32 LARGE_HEIGHT = 720;
constexpr int32 WARMUP_SAMPLES = 3;
constexpr int32 MEASURED_SAMPLES = 20;
constexpr int32 TRACE_ARGUMENT_COUNT = 3;
constexpr uint8 OPAQUE = 255;
constexpr uint8 HALF_ONE_HIGH_BYTE = 0x3c;
constexpr uint8 FLOAT_ONE_HIGH_BYTE = 0x3f;
constexpr uint8 FLOAT_ONE_SECOND_BYTE = 0x80;
constexpr usize ALPHA_CHANNEL = 3;

bool Measure(TexelEncoding encoding, uint32 width, uint32 height, QTextStream& inoutOutput)
{
    InspectedTexture texture;
    texture.Encoding = encoding;
    texture.Width = width;
    texture.Height = height;
    const uint32 texelBytes = GetTexelBytes(encoding);
    texture.Bytes.resize(static_cast<qsizetype>(width) * height * texelBytes);
    texture.Bytes.fill('\0');
    uint8* bytes = reinterpret_cast<uint8*>(texture.Bytes.data());
    for (usize pixel = 0; pixel < static_cast<usize>(width) * height; ++pixel)
    {
        uint8* texel = bytes + pixel * texelBytes;
        if (encoding == TexelEncoding::Rgba8Unorm)
        {
            texel[0] = OPAQUE;
            // Nonopaque input alpha must still produce the viewer's opaque output.
        }
        else if (encoding == TexelEncoding::Rgba16Float)
        {
            texel[1] = HALF_ONE_HIGH_BYTE;
        }
        else
        {
            // Alternating clear depth and zero exercises the automatic range pass.
            texel[2] = pixel % 2 == 0 ? FLOAT_ONE_SECOND_BYTE : 0;
            texel[ALPHA_CHANNEL] = pixel % 2 == 0 ? FLOAT_ONE_HIGH_BYTE : 0;
        }
    }
    TextureDisplaySettings settings;
    settings.Range = encoding == TexelEncoding::Depth32Float ? TextureRangeMode::Automatic : TextureRangeMode::Manual;
    for (int32 sample = -WARMUP_SAMPLES; sample < MEASURED_SAMPLES; ++sample)
    {
        TextureValueRange range;
        QElapsedTimer timer;
        timer.start();
        QImage image;
        {
            LUDUS_PROFILE_SCOPE(EditorTextureDisplayBenchmark);
            image = MakeDisplayImage(texture, settings, range);
        }
        const int64 elapsedNanoseconds = timer.nsecsElapsed();
        if (image.isNull() == true || range.HasValues == false)
        {
            return false;
        }
        // Validate outside the measurement; a benchmark of an empty/wrong image fails.
        for (uint32 y = 0; y < height; ++y)
        {
            const uint8* row = image.constScanLine(static_cast<int32>(y));
            for (uint32 x = 0; x < width; ++x)
            {
                const uint8* pixel = row + static_cast<usize>(x) * GetTexelBytes(TexelEncoding::Rgba8Unorm);
                const uint8 expected = encoding == TexelEncoding::Depth32Float ? (x % 2 == 0 ? OPAQUE : 0) : OPAQUE;
                if (pixel[0] != expected || pixel[1] != (encoding == TexelEncoding::Depth32Float ? expected : 0) ||
                    pixel[2] != (encoding == TexelEncoding::Depth32Float ? expected : 0) ||
                    pixel[ALPHA_CHANNEL] != OPAQUE)
                {
                    return false;
                }
            }
        }
        if (sample >= 0)
        {
            inoutOutput << "EDTEXTURE," << static_cast<uint32>(encoding) << ',' << width << ',' << height << ','
                        << sample << ',' << elapsedNanoseconds << '\n';
        }
    }
    return true;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    QString tracePath;
    if (argumentCount == TRACE_ARGUMENT_COUNT && QString::fromUtf8(arguments[1]) == QStringLiteral("--trace-output"))
    {
        tracePath = QString::fromUtf8(arguments[2]);
        profile::RegisterThreadForTrace("Editor texture benchmark");
        if (profile::BeginCapture(2) == false)
        {
            return 1;
        }
    }
    else if (argumentCount != 1)
    {
        return 1;
    }
    QTextStream output(stdout);
    bool isCorrect = true;
    for (const TexelEncoding encoding :
         {TexelEncoding::Rgba8Unorm, TexelEncoding::Rgba16Float, TexelEncoding::Depth32Float})
    {
        isCorrect = Measure(encoding, SMALL_WIDTH, SMALL_HEIGHT, output) == true && isCorrect == true;
        isCorrect = Measure(encoding, LARGE_WIDTH, LARGE_HEIGHT, output) == true && isCorrect == true;
    }
    if (tracePath.isEmpty() == false)
    {
        profile::EndCapture();
        Array<uint8> bytes;
        QFile trace(tracePath);
        if (profile::SerializePerfettoTrace(bytes) == false || trace.open(QIODevice::WriteOnly) == false ||
            trace.write(reinterpret_cast<const char*>(bytes.GetData()), static_cast<qint64>(bytes.GetSize())) !=
                static_cast<qint64>(bytes.GetSize()))
        {
            return 1;
        }
    }
    return isCorrect == true ? 0 : 1;
}
