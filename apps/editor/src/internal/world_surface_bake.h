#pragma once

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include <QByteArray>
#include <QSharedPointer>
#include <QString>
#include <QVector>

namespace ludus::editor
{
struct RenderScene;
class StaticLightingJob;

struct WorldSurfaceChart final
{
    QByteArray Pixels;
    foundation::math::Vector3 Scale{};
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
};

/// Immutable CPU chart data, in the exact object order of the retained World extraction.
struct WorldSurfaceBake final
{
    QByteArray LightingKey;
    QVector<WorldSurfaceChart> Charts;
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
    void Poll(const RenderScene& scene);
    [[nodiscard]] bool Save();
    [[nodiscard]] QSharedPointer<const WorldSurfaceBake> GetReady() const;
    [[nodiscard]] QString GetFailure() const;

private:
    void resetJob() noexcept;
    QString mPath;
    QString mFailure;
    QByteArray mLightingKey;
    QSharedPointer<const WorldSurfaceBake> mReady;
    QSharedPointer<const WorldSurfaceBake> mSaved;
    foundation::threading::JobSystem mJobs;
    foundation::UniquePtr<StaticLightingJob> mJob;
};
} // namespace ludus::editor
