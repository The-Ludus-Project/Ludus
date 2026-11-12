#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting.hpp>

#include <QByteArray>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#if !defined(Q_OS_WASM)
#    include <QFuture>
#    include <QPromise>
#endif

namespace ludus::editor
{
struct RenderScene;

struct WorldSurfaceChart final
{
    QByteArray Pixels;
    foundation::math::Vector3 Scale{};
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
};

/// CPU probe payload for the matching extraction, optionally retained beside the World.
struct WorldProbeAtlas final
{
    QByteArray Pixels;
    foundation::float32 IrradianceScale = 0;
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
};

/// Immutable CPU lighting data. Charts follow the exact order of the retained World extraction.
struct WorldSurfaceBake final
{
    QByteArray LightingKey;
    QVector<WorldSurfaceChart> Charts;
    WorldProbeAtlas ProbeAtlas;
};

/// GUI-thread owner shared by the built-in preview and its project graph consumer.
/// The worker owns immutable extraction inputs. Disk I/O occurs on open and explicit save.
class WorldSurfaceBakeState final
{
public:
    WorldSurfaceBakeState() noexcept;
    ~WorldSurfaceBakeState() noexcept;
    void SetWorldPath(const QString& path);
    void SetScene(const RenderScene& scene);
    /// Invalidate the previous grid. The next poll resolves a bake for the current scene and grid.
    void SetProbeGrid(const graphics::renderer::GiProbeGrid& grid) noexcept;
    void Poll(const RenderScene& scene);
    [[nodiscard]] bool Save();
    [[nodiscard]] QSharedPointer<const WorldSurfaceBake> GetReady() const;
    [[nodiscard]] QString GetFailure() const;

private:
    void invalidateBake() noexcept;
#if !defined(Q_OS_WASM)
    struct BakeResult final
    {
        QByteArray LightingKey;
        QSharedPointer<const WorldSurfaceBake> Bake;
        QString Failure;
    };

    [[nodiscard]] static BakeResult bakeScene(const RenderScene& scene,
                                              QByteArray key,
                                              const QSharedPointer<const WorldSurfaceBake>& saved,
                                              const QPromise<BakeResult>& promise);
    enum class BakeJobState : foundation::uint8
    {
        Idle,
        Current,
        Superseded
    };
    QFuture<BakeResult> mBakeJob;
    BakeJobState mBakeJobState = BakeJobState::Idle;
#endif
    QString mPath;
    QString mFailure;
    QByteArray mLightingKey;
    graphics::renderer::GiProbeGrid mProbeGrid{};
    QSharedPointer<const WorldSurfaceBake> mReady;
    QSharedPointer<const WorldSurfaceBake> mSaved;
};
} // namespace ludus::editor
