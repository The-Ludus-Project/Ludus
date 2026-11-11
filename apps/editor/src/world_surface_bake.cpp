#include "internal/world_surface_bake.h"

#include "internal/preview_lighting.h"
#include "internal/render_scene_limits.h"
#include "internal/static_lighting_job.h"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#if !defined(Q_OS_WASM)
#    include <QThread>
#    include <QtConcurrent/QtConcurrentRun>
#endif

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/threading/jobs.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace renderer = graphics::renderer;
namespace format = content::scene_format;
namespace
{
constexpr uint32 BAKE_MAGIC = 0x43424c57;
constexpr uint32 BAKE_VERSION = 1;
constexpr qsizetype LIGHTING_KEY_BYTES = 32;
// Bound the on-disk chart payload independently of the worker's peak scratch budget.
constexpr qsizetype MAX_CHART_FILE_BYTES = qsizetype{128} * 1024 * 1024;
constexpr float32 MAX_IRRADIANCE_SCALE = 10000;

bool HasBakeInputs(const RenderScene& scene) noexcept
{
    return scene.Lit == true && scene.Definition != nullptr && scene.Objects.isEmpty() == false &&
           scene.AlwaysOnLightIds.isEmpty() == false;
}

void ConfigureStream(QDataStream& inoutStream)
{
    inoutStream.setByteOrder(QDataStream::LittleEndian);
    inoutStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
}

bool IsChartValid(const WorldSurfaceChart& chart) noexcept
{
    return chart.Width > 0 && chart.Width <= format::MAX_BAKE_CHART_DIMENSION && chart.Height > 0 &&
           chart.Height <= format::MAX_BAKE_CHART_DIMENSION && math::IsFinite(chart.Scale) == true &&
           chart.Scale.X >= 0 && chart.Scale.X <= MAX_IRRADIANCE_SCALE && chart.Scale.Y >= 0 &&
           chart.Scale.Y <= MAX_IRRADIANCE_SCALE && chart.Scale.Z >= 0 && chart.Scale.Z <= MAX_IRRADIANCE_SCALE;
}

qsizetype GetChartByteCount(const WorldSurfaceChart& chart) noexcept
{
    return static_cast<qsizetype>(chart.Width) * chart.Height * format::TEXEL_BYTES;
}

QSharedPointer<const WorldSurfaceBake> ReadSurfaceBake(const QString& path)
{
    QFile file(path);
    if (path.isEmpty() == true || QFileInfo(path).isSymLink() == true || file.open(QIODevice::ReadOnly) == false ||
        file.size() > MAX_CHART_FILE_BYTES)
    {
        return {};
    }
    QDataStream stream(&file);
    ConfigureStream(stream);
    uint32 magic = 0;
    uint32 version = 0;
    uint32 count = 0;
    stream >> magic >> version >> count;
    if (magic != BAKE_MAGIC || version != BAKE_VERSION || count == 0 || count > MAX_PREVIEW_OBJECTS)
    {
        return {};
    }
    const QSharedPointer<WorldSurfaceBake> bake = QSharedPointer<WorldSurfaceBake>::create();
    bake->LightingKey.resize(LIGHTING_KEY_BYTES);
    if (stream.readRawData(bake->LightingKey.data(), bake->LightingKey.size()) != bake->LightingKey.size())
    {
        return {};
    }
    bake->Charts.reserve(count);
    for (uint32 index = 0; index < count; ++index)
    {
        WorldSurfaceChart chart;
        stream >> chart.Width >> chart.Height >> chart.Scale.X >> chart.Scale.Y >> chart.Scale.Z;
        if (stream.status() != QDataStream::Ok || IsChartValid(chart) == false)
        {
            return {};
        }
        const qsizetype bytes = GetChartByteCount(chart);
        if (bytes > file.size() - file.pos())
        {
            return {};
        }
        chart.Pixels.resize(bytes);
        if (stream.readRawData(chart.Pixels.data(), bytes) != bytes)
        {
            return {};
        }
        bake->Charts.append(chart);
    }
    return file.atEnd() == true ? bake : QSharedPointer<const WorldSurfaceBake>{};
}
} // namespace

WorldSurfaceBakeState::WorldSurfaceBakeState() noexcept = default;
WorldSurfaceBakeState::~WorldSurfaceBakeState() noexcept
{
#if !defined(Q_OS_WASM)
    mBakeJob.cancel();
#endif
}

void WorldSurfaceBakeState::SetWorldPath(const QString& path)
{
    const QString file = path.isEmpty() == true ? QString{} : path + QStringLiteral(".ludus-lighting");
    if (mPath != file)
    {
        mPath = file;
        mSaved = ReadSurfaceBake(mPath);
    }
}

void WorldSurfaceBakeState::SetScene(const RenderScene& scene)
{
    const QByteArray key = HasBakeInputs(scene) == true ? preview_lighting::MakeKey(scene) : QByteArray{};
    if (key.isEmpty() == true || key != mLightingKey)
    {
        invalidateBake();
        mLightingKey = key;
    }
}

void WorldSurfaceBakeState::InvalidateProbeGrid() noexcept
{
    invalidateBake();
    mLightingKey.clear();
}

QSharedPointer<const WorldSurfaceBake> WorldSurfaceBakeState::GetReady() const
{
    if (mReady.isNull() == false)
    {
        return mReady;
    }
    return mSaved.isNull() == false && mSaved->LightingKey == mLightingKey ? mSaved
                                                                           : QSharedPointer<const WorldSurfaceBake>{};
}

QString WorldSurfaceBakeState::GetFailure() const
{
    return mFailure;
}

void WorldSurfaceBakeState::invalidateBake() noexcept
{
#if !defined(Q_OS_WASM)
    if (mBakeJobState == BakeJobState::Current)
    {
        mBakeJobState = BakeJobState::Superseded;
        mBakeJob.cancel();
    }
#endif
    mReady.clear();
    mFailure.clear();
}

void WorldSurfaceBakeState::Poll(const RenderScene& scene)
{
#if !defined(Q_OS_WASM)
    if (mBakeJobState != BakeJobState::Idle)
    {
        if (mBakeJob.isFinished() == false)
        {
            return;
        }
        if (mBakeJobState == BakeJobState::Current)
        {
            const BakeResult completed = mBakeJob.result();
            mLightingKey = completed.LightingKey;
            mReady = completed.Bake;
            mFailure = completed.Failure;
        }
        mBakeJob = {};
        mBakeJobState = BakeJobState::Idle;
    }
    const bool hasProbeGrid = scene.ProbeGrid.Counts[0] != 0;
    const QSharedPointer<const WorldSurfaceBake> ready = GetReady();
    if (HasBakeInputs(scene) == false || mFailure.isEmpty() == false ||
        (ready.isNull() == false && (hasProbeGrid == false || ready->ProbeAtlas.Pixels.isEmpty() == false)))
    {
        return;
    }
    // The worker owns the frozen extraction through hashing, tracing and encoding. Edits only
    // cancel this request. Poll starts the latest one once the old job has finished releasing its inputs.
    mBakeJobState = BakeJobState::Current;
    mBakeJob = QtConcurrent::run([scene, key = mLightingKey, saved = mSaved](QPromise<BakeResult>& inoutPromise) {
        BakeResult result = bakeScene(scene, key, saved, inoutPromise);
        if (inoutPromise.isCanceled() == false)
        {
            static_cast<void>(inoutPromise.addResult(foundation::Move(result)));
        }
    });
#else
    static_cast<void>(scene);
#endif
}

#if !defined(Q_OS_WASM)
WorldSurfaceBakeState::BakeResult WorldSurfaceBakeState::bakeScene(const RenderScene& scene,
                                                                   QByteArray key,
                                                                   const QSharedPointer<const WorldSurfaceBake>& saved,
                                                                   const QPromise<BakeResult>& promise)
{
    if (key.isEmpty() == true)
    {
        key = preview_lighting::MakeKey(scene);
    }
    if (promise.isCanceled() == true)
    {
        return {};
    }
    const bool hasProbeGrid = scene.ProbeGrid.Counts[0] != 0;
    if (hasProbeGrid == false && saved.isNull() == false && saved->LightingKey == key)
    {
        return {key, {}, {}};
    }
    const auto makeFailure = [&key](rhi::RasterStatus status) -> BakeResult {
        return {key,
                {},
                QStringLiteral("Always-on bake failed (%1). Using realtime lighting. "
                               "Simplify the World geometry or clear the bake option in Properties.")
                    .arg(static_cast<uint32>(status))};
    };
    preview_lighting::Inputs inputs;
    const rhi::RasterStatus prepared =
        preview_lighting::BuildInputs(scene, key, inputs, preview_lighting::InputsTarget::Surfaces);
    if (prepared != rhi::RasterStatus::Ready)
    {
        return makeFailure(prepared);
    }
    threading::JobSystem jobs;
    if (jobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) != threading::JobStatus::Ok)
    {
        return makeFailure(rhi::RasterStatus::Failed);
    }
    renderer::StaticLightingBakeDescription description;
    description.SceneRevision = inputs.SceneRevision;
    description.DependencyRevision = inputs.DependencyRevision;
    description.ProbeGrid = scene.ProbeGrid;
    description.Surfaces = {inputs.Surfaces.GetData(), inputs.Surfaces.GetSize()};
    description.AnalyticLightIds = {scene.AlwaysOnLightIds.constData(),
                                    static_cast<usize>(scene.AlwaysOnLightIds.size())};
    StaticLightingJob job(scene, description);
    rhi::RasterStatus status = job.Start(jobs);
    while (status == rhi::RasterStatus::Pending)
    {
        if (promise.isCanceled() == true)
        {
            job.RequestInterruption();
            return {};
        }
        QThread::msleep(1);
        status = job.Poll();
    }
    if (status != rhi::RasterStatus::Ready)
    {
        return makeFailure(status);
    }
    if (promise.isCanceled() == true)
    {
        return {};
    }
    const UniquePtr<renderer::StaticLightingBake> completed = job.TakeBake();
    const QSharedPointer<WorldSurfaceBake> bake = QSharedPointer<WorldSurfaceBake>::create();
    bake->LightingKey = key;
    if (hasProbeGrid == true)
    {
        renderer::StaticGiProbeAtlas source;
        const rhi::RasterStatus atlasStatus = completed->GetProbeAtlas(source);
        if (atlasStatus != rhi::RasterStatus::Ready)
        {
            return makeFailure(atlasStatus);
        }
        bake->ProbeAtlas.Pixels = QByteArray(reinterpret_cast<const char*>(source.Pixels.GetDataOrNull()),
                                             static_cast<qsizetype>(source.Pixels.GetSize()));
        bake->ProbeAtlas.IrradianceScale = source.IrradianceScale;
        bake->ProbeAtlas.Width = source.Width;
        bake->ProbeAtlas.Height = source.Height;
    }
    bake->Charts.reserve(scene.Objects.size());
    for (qsizetype index = 0; index < scene.Objects.size(); ++index)
    {
        renderer::StaticLightingChart source;
        if (completed->GetChart(static_cast<usize>(index), source) != rhi::RasterStatus::Ready)
        {
            return makeFailure(rhi::RasterStatus::Failed);
        }
        WorldSurfaceChart chart;
        chart.Width = source.Width;
        chart.Height = source.Height;
        chart.Scale = source.Scale;
        if (IsChartValid(chart) == false)
        {
            return makeFailure(rhi::RasterStatus::InvalidDescription);
        }
        chart.Pixels = QByteArray(reinterpret_cast<const char*>(source.Pixels.GetDataOrNull()),
                                  static_cast<qsizetype>(source.Pixels.GetSize()));
        bake->Charts.append(chart);
    }
    return {key, bake, {}};
}
#endif

bool WorldSurfaceBakeState::Save()
{
    if (mReady.isNull() == true || mReady == mSaved || mPath.isEmpty() == true)
    {
        return true;
    }
    QSaveFile file(mPath);
    if (QFileInfo(mPath).isSymLink() == true || file.open(QIODevice::WriteOnly) == false)
    {
        return false;
    }
    QDataStream stream(&file);
    ConfigureStream(stream);
    stream << BAKE_MAGIC << BAKE_VERSION << static_cast<uint32>(mReady->Charts.size());
    if (stream.writeRawData(mReady->LightingKey.constData(), mReady->LightingKey.size()) != mReady->LightingKey.size())
    {
        return false;
    }
    for (const WorldSurfaceChart& chart : mReady->Charts)
    {
        stream << chart.Width << chart.Height << chart.Scale.X << chart.Scale.Y << chart.Scale.Z;
        if (chart.Pixels.size() > MAX_CHART_FILE_BYTES - file.pos() ||
            stream.writeRawData(chart.Pixels.constData(), chart.Pixels.size()) != chart.Pixels.size())
        {
            return false;
        }
    }
    if (stream.status() != QDataStream::Ok || file.commit() == false)
    {
        return false;
    }
    mSaved = mReady;
    return true;
}
} // namespace ludus::editor
