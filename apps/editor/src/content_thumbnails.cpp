#include "internal/content_thumbnails.h"
#include "design_tokens.h"

#include <ludus/content/scene_cook/limits.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPair>
#include <QTimer>
#include <QtMath>

#if !defined(Q_OS_WASM)
#    include <QThreadPool>
#endif

#include <new>
#include <utility>

namespace ludus::editor
{
namespace
{
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
namespace format = content::scene_format;

/// Bytes of the width, height and byte count header in front of a cooked texture's texels.
constexpr qint64 TEXTURE_HEADER_BYTES = sizeof(format::TextureRecord);
/// Source rows averaged into one thumbnail row. Each is one contiguous read; two hide most aliasing on
/// downsampled textures while keeping a 4096 texel tall texture to a few hundred small reads.
constexpr int VERTICAL_SAMPLES = 2;
/// Checkerboard cells across a translucent thumbnail.
constexpr int CHECKER_CELLS = 4;
/// Largest alpha byte, opaque.
constexpr int OPAQUE_ALPHA_BYTE = 255;
/// Number of 8 bit channel values.
constexpr int CHANNEL_VALUES = 256;
/// Side, in texels, of the base colour texture copy a material sphere samples.
constexpr int MATERIAL_TEXTURE_SIDE = 64;
/// Gap between the material sphere and the thumbnail edge, as a fraction of the side.
constexpr qreal SPHERE_MARGIN = 0.06;
/// Albedo of an untextured material; scene_format materials carry no base colour factor, and white fully lit reads
/// as a blank square, so the preview shows a light neutral grey.
constexpr qreal UNTEXTURED_ALBEDO = 0.8;
/// Key light direction toward the light (upper left, in front), before normalisation.
constexpr qreal KEY_LIGHT_X = -0.45;
constexpr qreal KEY_LIGHT_Y = 0.55;
constexpr qreal KEY_LIGHT_Z = 0.7;
/// Constant fill light, as a fraction of the albedo, so the unlit side keeps its colour.
constexpr qreal AMBIENT_LIGHT = 0.18;
/// Reflectance at normal incidence of dielectrics (about 4 percent for common materials).
constexpr qreal DIELECTRIC_REFLECTANCE = 0.04;
/// Lowest roughness shaded; smoother highlights would shrink below one thumbnail pixel.
constexpr qreal MIN_PREVIEW_ROUGHNESS = 0.08;
/// Brightness of the key light's specular highlight.
constexpr qreal SPECULAR_INTENSITY = 1.2;
/// Defaults scene_assets.cpp writes when a field is absent.
constexpr qreal DEFAULT_METALLIC = 0.0;
constexpr qreal DEFAULT_ROUGHNESS = 0.5;
/// Emission components a thumbnail shows, linear RGB.
constexpr int EMISSION_COMPONENTS = 3;
/// IEC 61966-2-1 (sRGB) transfer function constants.
constexpr qreal SRGB_LINEAR_LIMIT = 0.04045;
constexpr qreal SRGB_ENCODED_LIMIT = 0.0031308;
constexpr qreal SRGB_LINEAR_SLOPE = 12.92;
constexpr qreal SRGB_OFFSET = 0.055;
constexpr qreal SRGB_SCALE = 1.055;
constexpr qreal SRGB_EXPONENT = 2.4;
constexpr int BITS_PER_BYTE = 8;
constexpr int SECOND_BYTE = 1;
constexpr int THIRD_BYTE = 2;
constexpr int FOURTH_BYTE = 3;
constexpr int RED = 0;
constexpr int GREEN = 1;
constexpr int BLUE = 2;
constexpr int ALPHA = 3;

[[nodiscard]] uint32 DecodeUint32(const char* bytes) noexcept
{
    const auto byte = [bytes](int index) { return static_cast<uint32>(static_cast<unsigned char>(bytes[index])); };
    return byte(0) | (byte(SECOND_BYTE) << BITS_PER_BYTE) | (byte(THIRD_BYTE) << (BITS_PER_BYTE * 2)) |
           (byte(FOURTH_BYTE) << (BITS_PER_BYTE * FOURTH_BYTE));
}

// Thanks to the International Electrotechnical Commission, IEC 61966-2-1:1999, "Multimedia systems and equipment -
// Colour measurement and management - Part 2-1: Default RGB colour space - sRGB", for the piecewise transfer
// function used by both conversions below.
[[nodiscard]] qreal DecodeSrgb(qreal encoded) noexcept
{
    return encoded <= SRGB_LINEAR_LIMIT ? encoded / SRGB_LINEAR_SLOPE
                                        : qPow((encoded + SRGB_OFFSET) / SRGB_SCALE, SRGB_EXPONENT);
}

[[nodiscard]] int EncodeSrgb(qreal linear) noexcept
{
    const qreal clamped = qBound(0.0, linear, 1.0);
    const qreal encoded = clamped <= SRGB_ENCODED_LIMIT ? clamped * SRGB_LINEAR_SLOPE
                                                        : SRGB_SCALE * qPow(clamped, 1.0 / SRGB_EXPONENT) - SRGB_OFFSET;
    return qBound(0, qRound(encoded * OPAQUE_ALPHA_BYTE), OPAQUE_ALPHA_BYTE);
}

/// Decoded linear value of every 8 bit sRGB code.
struct SrgbTable final
{
    qreal Values[CHANNEL_VALUES]{};
    SrgbTable() noexcept
    {
        for (int code = 0; code < CHANNEL_VALUES; ++code)
        {
            Values[code] = DecodeSrgb(static_cast<qreal>(code) / OPAQUE_ALPHA_BYTE);
        }
    }
};

/// Box-filter the texels of a cooked texture into an RGBA8888 image no larger than side by side.
[[nodiscard]] bool ReadTexture(const QString& path, int side, QImage& outImage, QString& outFailure)
{
    if (side <= 0)
    {
        outFailure = QStringLiteral("The thumbnail size is empty.");
        return false;
    }
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outFailure = QStringLiteral("The cooked texture cannot be opened: %1").arg(file.errorString());
        return false;
    }
    const QByteArray header = file.read(TEXTURE_HEADER_BYTES);
    if (header.size() != TEXTURE_HEADER_BYTES)
    {
        outFailure = QStringLiteral("The cooked texture is shorter than its header.");
        return false;
    }
    const uint32 width = DecodeUint32(header.constData());
    const uint32 height = DecodeUint32(header.constData() + sizeof(uint32));
    const uint32 byteCount = DecodeUint32(header.constData() + 2 * sizeof(uint32));
    if (width == 0 || height == 0 || width > format::MAX_TEXTURE_DIMENSION || height > format::MAX_TEXTURE_DIMENSION ||
        uint64{byteCount} != uint64{width} * uint64{height} * format::TEXEL_BYTES ||
        file.size() < TEXTURE_HEADER_BYTES + static_cast<qint64>(byteCount))
    {
        outFailure = QStringLiteral("The cooked texture header is outside the texture limits or its texels are "
                                    "missing; recook the scene that owns it.");
        return false;
    }
    const uint32 longest = width > height ? width : height;
    const int outWidth = qMax(1, qRound(static_cast<qreal>(width) * side / longest));
    const int outHeight = qMax(1, qRound(static_cast<qreal>(height) * side / longest));
    QImage image(outWidth, outHeight, QImage::Format_RGBA8888);
    if (image.isNull() == true)
    {
        outFailure = QStringLiteral("Out of memory for the thumbnail.");
        return false;
    }
    const qint64 rowBytes = static_cast<qint64>(width) * format::TEXEL_BYTES;
    QByteArray row(rowBytes, Qt::Uninitialized);
    QVector<uint32> sums(static_cast<qsizetype>(outWidth) * format::TEXEL_BYTES);
    QVector<uint32> counts(outWidth);
    for (int y = 0; y < outHeight; ++y)
    {
        const uint32 firstRow = static_cast<uint32>(static_cast<uint64>(y) * height / outHeight);
        const uint32 endRow = qMax(firstRow + 1, static_cast<uint32>(static_cast<uint64>(y + 1) * height / outHeight));
        const uint32 span = endRow - firstRow;
        const int samples = qMin(VERTICAL_SAMPLES, static_cast<int>(span));
        sums.fill(0);
        counts.fill(0);
        for (int sample = 0; sample < samples; ++sample)
        {
            const uint32 sourceRow = firstRow + static_cast<uint32>((2 * sample + 1) * span / (2 * samples));
            if (file.seek(TEXTURE_HEADER_BYTES + static_cast<qint64>(sourceRow) * rowBytes) == false ||
                file.read(row.data(), rowBytes) != rowBytes)
            {
                outFailure = QStringLiteral("The cooked texture changed or became unreadable while it was read.");
                return false;
            }
            const unsigned char* texels = reinterpret_cast<const unsigned char*>(row.constData());
            for (int x = 0; x < outWidth; ++x)
            {
                const uint32 firstColumn = static_cast<uint32>(static_cast<uint64>(x) * width / outWidth);
                const uint32 endColumn =
                    qMax(firstColumn + 1, static_cast<uint32>(static_cast<uint64>(x + 1) * width / outWidth));
                for (uint32 column = firstColumn; column < endColumn; ++column)
                {
                    for (uint32 channel = 0; channel < format::TEXEL_BYTES; ++channel)
                    {
                        sums[x * format::TEXEL_BYTES + channel] += texels[column * format::TEXEL_BYTES + channel];
                    }
                    ++counts[x];
                }
            }
        }
        uchar* line = image.scanLine(y);
        for (int x = 0; x < outWidth; ++x)
        {
            for (uint32 channel = 0; channel < format::TEXEL_BYTES; ++channel)
            {
                line[x * format::TEXEL_BYTES + channel] =
                    static_cast<uchar>((sums[x * format::TEXEL_BYTES + channel] + counts[x] / 2) / counts[x]);
            }
        }
    }
    outImage = std::move(image);
    return true;
}

[[nodiscard]] bool HasTranslucency(const QImage& image) noexcept
{
    for (int y = 0; y < image.height(); ++y)
    {
        const uchar* line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
        {
            if (line[x * format::TEXEL_BYTES + ALPHA] != OPAQUE_ALPHA_BYTE)
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool TryMakeTextureThumbnail(const QString& path, int side, QImage& outImage, QString& outFailure)
{
    QImage texels;
    if (ReadTexture(path, side, texels, outFailure) == false)
    {
        return false;
    }
    QImage result(texels.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull() == true)
    {
        outFailure = QStringLiteral("Out of memory for the thumbnail.");
        return false;
    }
    result.fill(Qt::transparent);
    QPainter painter(&result);
    if (HasTranslucency(texels) == true)
    {
        // Translucent texels over the viewport grid colours, which are the same in both appearances.
        const int cell = qMax(1, qMax(result.width(), result.height()) / CHECKER_CELLS);
        for (int y = 0; y < result.height(); y += cell)
        {
            for (int x = 0; x < result.width(); x += cell)
            {
                const bool isMajor = ((x / cell) + (y / cell)) % 2 == 0;
                painter.fillRect(x,
                                 y,
                                 cell,
                                 cell,
                                 QColor::fromRgb(design::GetThemeArgb(isMajor == true ? design::ThemeColour::GridMajor
                                                                                      : design::ThemeColour::GridMinor,
                                                                      true)));
            }
        }
    }
    painter.drawImage(0, 0, texels);
    painter.end();
    outImage = std::move(result);
    return true;
}

// Thanks to James F. Blinn, "Models of Light Reflection for Computer Synthesized Pictures", SIGGRAPH '77, Computer
// Graphics 11(2), pages 192-198 (https://doi.org/10.1145/965141.563893), for the half-vector highlight used below.
// The thumbnail only needs to tell rough from glossy and metal from dielectric at a glance; it is not the
// renderer's BSDF.
bool TryMakeMaterialThumbnail(const QString& path,
                              const QHash<QString, QString>& texturePaths,
                              int side,
                              QImage& outImage,
                              QString& outFailure)
{
    if (side <= 0)
    {
        outFailure = QStringLiteral("The thumbnail size is empty.");
        return false;
    }
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        outFailure = QStringLiteral("The cooked material cannot be opened: %1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = file.read(static_cast<qint64>(content::scene_cook::MAX_METADATA_BYTES) + 1);
    if (static_cast<usize>(bytes.size()) > content::scene_cook::MAX_METADATA_BYTES)
    {
        outFailure = QStringLiteral("The cooked material is larger than the metadata limit.");
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        outFailure = QStringLiteral("The cooked material is not a JSON object: %1").arg(error.errorString());
        return false;
    }
    const QJsonObject material = document.object();
    const qreal metallic = qBound(0.0, material.value(QStringLiteral("metallic")).toDouble(DEFAULT_METALLIC), 1.0);
    const qreal roughness =
        qBound(MIN_PREVIEW_ROUGHNESS, material.value(QStringLiteral("roughness")).toDouble(DEFAULT_ROUGHNESS), 1.0);
    const QJsonArray emissionValues = material.value(QStringLiteral("emission")).toArray();
    qreal emission[EMISSION_COMPONENTS]{};
    for (int component = 0; component < EMISSION_COMPONENTS && component < emissionValues.size(); ++component)
    {
        emission[component] = qMax(0.0, emissionValues.at(component).toDouble(0.0));
    }
    QImage texture;
    const QString textureId = material.value(QStringLiteral("texture")).toString();
    if (textureId.isEmpty() == false && texturePaths.contains(textureId) == true)
    {
        QString textureFailure;
        // A missing or broken texture still previews the material's shading, untextured.
        (void)ReadTexture(texturePaths.value(textureId), MATERIAL_TEXTURE_SIDE, texture, textureFailure);
    }
    static const SrgbTable SRGB;
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull() == true)
    {
        outFailure = QStringLiteral("Out of memory for the thumbnail.");
        return false;
    }
    image.fill(Qt::transparent);
    const qreal lightLength = qSqrt(KEY_LIGHT_X * KEY_LIGHT_X + KEY_LIGHT_Y * KEY_LIGHT_Y + KEY_LIGHT_Z * KEY_LIGHT_Z);
    const qreal lightX = KEY_LIGHT_X / lightLength;
    const qreal lightY = KEY_LIGHT_Y / lightLength;
    const qreal lightZ = KEY_LIGHT_Z / lightLength;
    // Half vector between the light and a viewer on +Z.
    const qreal halfLength = qSqrt(lightX * lightX + lightY * lightY + (lightZ + 1.0) * (lightZ + 1.0));
    const qreal halfX = lightX / halfLength;
    const qreal halfY = lightY / halfLength;
    const qreal halfZ = (lightZ + 1.0) / halfLength;
    // Squared roughness maps to a highlight exponent, so rough materials spread a dim highlight and smooth ones focus
    // a bright one.
    const qreal alpha = roughness * roughness;
    const qreal exponent = 2.0 / (alpha * alpha) - 2.0;
    const qreal centre = static_cast<qreal>(side) * 0.5;
    const qreal radius = centre * (1.0 - 2.0 * SPHERE_MARGIN);
    for (int y = 0; y < side; ++y)
    {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < side; ++x)
        {
            const qreal offsetX = (static_cast<qreal>(x) + 0.5 - centre) / radius;
            const qreal offsetY = (centre - static_cast<qreal>(y) - 0.5) / radius;
            const qreal distanceSquared = offsetX * offsetX + offsetY * offsetY;
            const qreal coverage = qBound(0.0, (1.0 - qSqrt(distanceSquared)) * radius + 0.5, 1.0);
            if (coverage <= 0.0)
            {
                continue;
            }
            const qreal normalX = qBound(-1.0, offsetX, 1.0);
            const qreal normalY = qBound(-1.0, offsetY, 1.0);
            const qreal normalZ = qSqrt(qMax(0.0, 1.0 - normalX * normalX - normalY * normalY));
            qreal albedo[EMISSION_COMPONENTS] = {UNTEXTURED_ALBEDO, UNTEXTURED_ALBEDO, UNTEXTURED_ALBEDO};
            if (texture.isNull() == false)
            {
                // Planar projection keeps the whole texture visible on the front of the sphere.
                const int textureX =
                    qBound(0, static_cast<int>((normalX + 1.0) * 0.5 * texture.width()), texture.width() - 1);
                const int textureY =
                    qBound(0, static_cast<int>((1.0 - normalY) * 0.5 * texture.height()), texture.height() - 1);
                const uchar* texel =
                    texture.constScanLine(textureY) + static_cast<usize>(textureX) * format::TEXEL_BYTES;
                albedo[RED] = SRGB.Values[texel[RED]];
                albedo[GREEN] = SRGB.Values[texel[GREEN]];
                albedo[BLUE] = SRGB.Values[texel[BLUE]];
            }
            const qreal diffuseCosine = qMax(0.0, normalX * lightX + normalY * lightY + normalZ * lightZ);
            const qreal halfCosine = qMax(0.0, normalX * halfX + normalY * halfY + normalZ * halfZ);
            const qreal highlight = diffuseCosine > 0.0 ? SPECULAR_INTENSITY * qPow(halfCosine, exponent) : 0.0;
            int encoded[EMISSION_COMPONENTS]{};
            for (int channel = 0; channel < EMISSION_COMPONENTS; ++channel)
            {
                const qreal diffuse = albedo[channel] * (1.0 - metallic);
                const qreal reflectance =
                    DIELECTRIC_REFLECTANCE + (albedo[channel] - DIELECTRIC_REFLECTANCE) * metallic;
                const qreal ambient = AMBIENT_LIGHT * (diffuse + reflectance);
                encoded[channel] =
                    EncodeSrgb(ambient + diffuse * diffuseCosine + reflectance * highlight + emission[channel]);
            }
            const int opacity = qRound(coverage * OPAQUE_ALPHA_BYTE);
            line[x] = qRgba(encoded[RED] * opacity / OPAQUE_ALPHA_BYTE,
                            encoded[GREEN] * opacity / OPAQUE_ALPHA_BYTE,
                            encoded[BLUE] * opacity / OPAQUE_ALPHA_BYTE,
                            opacity);
        }
    }
    outImage = std::move(image);
    return true;
}

ContentThumbnailResult MakeContentThumbnail(const ContentThumbnailRequest& request)
{
    ContentThumbnailResult result;
    result.Id = request.Id;
    result.Key = request.Key;
    result.DevicePixelRatio = request.DevicePixelRatio;
    bool isMade = false;
    if (request.Type == ContentType::Texture)
    {
        isMade = TryMakeTextureThumbnail(request.Path, request.Side, result.Image, result.Failure);
    }
    else if (request.Type == ContentType::Material)
    {
        isMade =
            TryMakeMaterialThumbnail(request.Path, request.TexturePaths, request.Side, result.Image, result.Failure);
    }
    else
    {
        result.Failure = QStringLiteral("This type has no thumbnail.");
    }
    if (isMade == false)
    {
        result.Image = QImage();
    }
    return result;
}

/// Finished work handed from workers to the UI thread, tagged with the cache generation it was started in.
struct ContentThumbnailCache::Results final
{
    QMutex Mutex;
    QVector<QPair<uint64, ContentThumbnailResult>> Finished;
};

ContentThumbnailCache::ContentThumbnailCache(QObject* parentOrNull)
    : QObject(parentOrNull), mResultsOrNull(new (std::nothrow) Results)
{
    mEntries.setMaxCost(CONTENT_THUMBNAIL_CACHE_KIBIBYTES);
#if !defined(Q_OS_WASM)
    mPoolOrNull = new (std::nothrow) QThreadPool;
    if (mPoolOrNull != nullptr)
    {
        mPoolOrNull->setMaxThreadCount(CONTENT_THUMBNAIL_WORKERS);
        mPoolOrNull->setObjectName(QStringLiteral("contentThumbnails"));
    }
#endif
}

ContentThumbnailCache::~ContentThumbnailCache()
{
#if !defined(Q_OS_WASM)
    if (mPoolOrNull != nullptr)
    {
        // Drop queued work; deleting the pool waits only for the running thumbnails, which are bounded reads.
        mPoolOrNull->clear();
        delete mPoolOrNull;
        mPoolOrNull = nullptr;
    }
#endif
    delete mResultsOrNull;
}

// A resource ID and its artifact key are both strings, but every caller names them from one ContentRow.
QPixmap ContentThumbnailCache::Find(const QString& id, // NOLINT(bugprone-easily-swappable-parameters)
                                    const QString& key)
{
    const Entry* entry = mEntries.object(id);
    return entry != nullptr && entry->Key == key && entry->HasFailed == false ? entry->Pixmap : QPixmap();
}

bool ContentThumbnailCache::HasFailed(const QString& id, // NOLINT(bugprone-easily-swappable-parameters)
                                      const QString& key) const
{
    const Entry* entry = mEntries.object(id);
    return entry != nullptr && entry->Key == key && entry->HasFailed == true;
}

void ContentThumbnailCache::Request(const ContentThumbnailRequest& request)
{
    if (request.Id.isEmpty() == true || HasContentThumbnail(request.Type) == false || mResultsOrNull == nullptr)
    {
        return;
    }
    const Entry* entry = mEntries.object(request.Id);
    if ((entry != nullptr && entry->Key == request.Key) ||
        (mRunningKeys.contains(request.Id) == true && mRunningKeys.value(request.Id) == request.Key))
    {
        return;
    }
    // Newest first: a request repeated by a repaint moves to the top, ahead of rows that scrolled away.
    if (mQueuedKeys.contains(request.Id) == true)
    {
        for (qsizetype index = 0; index < mQueue.size(); ++index)
        {
            if (mQueue[index].Id == request.Id)
            {
                mQueue.removeAt(index);
                break;
            }
        }
    }
    mQueue.append(request);
    mQueuedKeys.insert(request.Id, request.Key);
    if (mQueue.size() > CONTENT_THUMBNAIL_QUEUE_LIMIT)
    {
        mQueuedKeys.remove(mQueue.front().Id);
        mQueue.removeFirst();
    }
    dispatch();
}

void ContentThumbnailCache::Invalidate(const QString& id)
{
    (void)mEntries.remove(id);
}

void ContentThumbnailCache::Clear()
{
    ++mGeneration;
    mEntries.clear();
    mQueue.clear();
    mQueuedKeys.clear();
    mRunningKeys.clear();
#if !defined(Q_OS_WASM)
    if (mPoolOrNull != nullptr)
    {
        mPoolOrNull->clear();
    }
#endif
}

int ContentThumbnailCache::GetCachedCount() const noexcept
{
    return static_cast<int>(mEntries.count());
}

int ContentThumbnailCache::GetQueuedCount() const noexcept
{
    return static_cast<int>(mQueue.size());
}

int ContentThumbnailCache::GetRunningCount() const noexcept
{
    return static_cast<int>(mRunningKeys.size());
}

usize ContentThumbnailCache::GetGeneratedCount() const noexcept
{
    return mGeneratedCount;
}

bool ContentThumbnailCache::IsIdle() const noexcept
{
    return mQueue.isEmpty() == true && mRunningKeys.isEmpty() == true;
}

void ContentThumbnailCache::dispatch()
{
    while (mQueue.isEmpty() == false && mRunningKeys.size() < CONTENT_THUMBNAIL_WORKERS)
    {
        const ContentThumbnailRequest request = mQueue.takeLast();
        mQueuedKeys.remove(request.Id);
        mRunningKeys.insert(request.Id, request.Key);
        const uint64 generation = mGeneration;
        Results* results = mResultsOrNull;
#if !defined(Q_OS_WASM)
        if (mPoolOrNull != nullptr)
        {
            // The worker touches only its copied request and the mutex-guarded results; the destructor waits for it
            // before results are freed, and Qt drops the queued delivery once this object is gone.
            mPoolOrNull->start([this, results, request, generation]() {
                ContentThumbnailResult result = MakeContentThumbnail(request);
                {
                    const QMutexLocker locker(&results->Mutex);
                    results->Finished.append({generation, result});
                }
                QMetaObject::invokeMethod(this, &ContentThumbnailCache::deliver, Qt::QueuedConnection);
            });
            continue;
        }
#endif
        // The browser has one thread: generate one thumbnail per event loop turn so painting and input interleave.
        QTimer::singleShot(0, this, [this, results, request, generation]() {
            results->Finished.append({generation, MakeContentThumbnail(request)});
            deliver();
        });
    }
}

void ContentThumbnailCache::deliver()
{
    QVector<QPair<uint64, ContentThumbnailResult>> finished;
    {
        const QMutexLocker locker(&mResultsOrNull->Mutex);
        finished.swap(mResultsOrNull->Finished);
    }
    for (const QPair<uint64, ContentThumbnailResult>& item : finished)
    {
        if (item.first != mGeneration)
        {
            continue;
        }
        const ContentThumbnailResult& result = item.second;
        if (mRunningKeys.contains(result.Id) == true && mRunningKeys.value(result.Id) == result.Key)
        {
            mRunningKeys.remove(result.Id);
        }
        ++mGeneratedCount;
        accept(result);
        Q_EMIT ThumbnailReady(result.Id);
    }
    dispatch();
}

void ContentThumbnailCache::accept(const ContentThumbnailResult& result)
{
    Entry* entry = new (std::nothrow) Entry;
    if (entry == nullptr)
    {
        return;
    }
    entry->Key = result.Key;
    constexpr qint64 BYTES_PER_KIBIBYTE = 1024;
    qint64 cost = 1;
    if (result.Image.isNull() == true)
    {
        entry->HasFailed = true;
    }
    else
    {
        entry->Pixmap = QPixmap::fromImage(result.Image);
        entry->Pixmap.setDevicePixelRatio(result.DevicePixelRatio);
        cost = qMax<qint64>(1, result.Image.sizeInBytes() / BYTES_PER_KIBIBYTE);
    }
    (void)mEntries.insert(result.Id, entry, cost);
}
} // namespace ludus::editor
