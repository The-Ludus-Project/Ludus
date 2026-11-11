#pragma once

// Content browser thumbnails (docs/development/editor-workspace.md, "S3 content browser").
//
// Textures draw a downsampled copy of their cooked RGBA8 texels and materials a small shaded sphere. Both are
// produced from the cooked artifact bytes alone, off the UI thread, only for rows a view actually paints, and kept
// in one bounded cache keyed by resource ID and artifact key, so a recooked asset (new key) is redrawn and an
// unchanged one never is.

#include "internal/content_types.h"

#include <ludus/foundation/base/types.h>

#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QVector>

class QThreadPool;

namespace ludus::editor
{
/// Logical side, in pixels, of a thumbnail in the content list.
inline constexpr int CONTENT_THUMBNAIL_SIZE = 32;

/// Most thumbnails generated at once. Generation reads a few rows of one artifact, so two workers keep the disk and
/// one core busy without competing with the editor's frame work.
inline constexpr int CONTENT_THUMBNAIL_WORKERS = 2;

/// Most requests waiting for a worker. Older requests are dropped first: they belong to rows that have scrolled
/// away, and a view that paints them again asks again.
inline constexpr int CONTENT_THUMBNAIL_QUEUE_LIMIT = 256;

/// Byte budget of cached thumbnail pixmaps, in kibibytes (about 450 thumbnails at 48 by 48 device pixels).
inline constexpr int CONTENT_THUMBNAIL_CACHE_KIBIBYTES = 16 * 1024;

/// Everything one thumbnail needs, copied to the worker. Paths are absolute.
struct ContentThumbnailRequest final
{
    /// Stable resource ID.
    QString Id;
    /// Artifact key from the catalog; it changes whenever the cooked bytes or a dependency's bytes change.
    QString Key;
    ContentType Type = ContentType::Texture;
    /// The cooked artifact.
    QString Path;
    /// Cooked texture artifacts a material may reference, by resource ID.
    QHash<QString, QString> TexturePaths;
    /// Side of the square thumbnail in device pixels.
    int Side = CONTENT_THUMBNAIL_SIZE;
    /// Device pixel ratio the pixmap is drawn at.
    qreal DevicePixelRatio = 1.0;
};

/// A generated thumbnail, or why none could be made.
struct ContentThumbnailResult final
{
    QString Id;
    QString Key;
    /// Null when generation failed.
    QImage Image;
    /// Readable reason when Image is null.
    QString Failure;
    qreal DevicePixelRatio = 1.0;
};

/// Read a cooked texture artifact (a 12 byte width, height and byte count header followed by RGBA8 texels, see
/// SceneFormat's TextureRecord) and box-filter it to fit a side by side square, over a checkerboard where it is
/// translucent. Reads only the texel rows it samples. Returns false with a reason when the file is missing, short or
/// outside SceneFormat's texture limits. Thread-safe; call it on a worker.
[[nodiscard]] bool TryMakeTextureThumbnail(const QString& path, int side, QImage& outImage, QString& outFailure);

/// Shade a sphere with a cooked material description (the JSON scene_assets.cpp writes: "texture", "metallic",
/// "roughness" and "emission") lit by one key light, sampling its base colour texture from texturePaths when the
/// material names one. Returns false with a reason when the description is missing or malformed. Thread-safe.
[[nodiscard]] bool TryMakeMaterialThumbnail(const QString& path,
                                            const QHash<QString, QString>& texturePaths,
                                            int side,
                                            QImage& outImage,
                                            QString& outFailure);

/// Generate the thumbnail a request describes. Thread-safe.
[[nodiscard]] ContentThumbnailResult MakeContentThumbnail(const ContentThumbnailRequest& request);

/// Bounded, lazily filled thumbnail cache owned by the UI thread. Request() never blocks: generation runs on a small
/// private thread pool (on the browser, one request per event loop turn), and ThumbnailReady reports each finished
/// ID. Destruction drops queued requests and waits only for the at most CONTENT_THUMBNAIL_WORKERS running ones.
class ContentThumbnailCache final : public QObject
{
    Q_OBJECT
public:
    explicit ContentThumbnailCache(QObject* parentOrNull = nullptr);
    ~ContentThumbnailCache() override;

    /// The cached thumbnail of id at key, or a null pixmap when it is missing, stale or failed.
    [[nodiscard]] QPixmap Find(const QString& id, const QString& key);
    /// Whether generating id at key failed; the browser then draws the type icon.
    [[nodiscard]] bool HasFailed(const QString& id, const QString& key) const;
    /// Queue generation unless id at key is cached, failed, queued or running.
    void Request(const ContentThumbnailRequest& request);
    /// Forget id, so its next request generates again.
    void Invalidate(const QString& id);
    /// Forget everything and drop queued requests; running ones finish but are not stored.
    void Clear();

    /// Thumbnails currently cached, including failures.
    [[nodiscard]] int GetCachedCount() const noexcept;
    /// Requests waiting for a worker.
    [[nodiscard]] int GetQueuedCount() const noexcept;
    /// Requests running on a worker.
    [[nodiscard]] int GetRunningCount() const noexcept;
    /// Thumbnails generated since construction, successful or not.
    [[nodiscard]] foundation::usize GetGeneratedCount() const noexcept;
    /// Whether nothing is queued or running.
    [[nodiscard]] bool IsIdle() const noexcept;

Q_SIGNALS:
    /// The thumbnail of id was stored (or its failure recorded).
    void ThumbnailReady(const QString& id);

private:
    struct Entry final
    {
        QString Key;
        QPixmap Pixmap;
        bool HasFailed = false;
    };
    struct Results;

    void dispatch();
    void deliver();
    void accept(const ContentThumbnailResult& result);

    QCache<QString, Entry> mEntries;
    QVector<ContentThumbnailRequest> mQueue;
    QHash<QString, QString> mQueuedKeys;
    QHash<QString, QString> mRunningKeys;
    Results* mResultsOrNull = nullptr;
    QThreadPool* mPoolOrNull = nullptr;
    foundation::usize mGeneratedCount = 0;
    foundation::uint64 mGeneration = 0;
};
} // namespace ludus::editor
