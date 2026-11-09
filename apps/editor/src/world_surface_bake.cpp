#include "internal/world_surface_bake.h"

#include "internal/preview_lighting.h"
#include "internal/render_scene_limits.h"
#include "internal/static_lighting_job.h"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <new>

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
WorldSurfaceBakeState::~WorldSurfaceBakeState() noexcept = default;

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
    const bool hasBakeInputs = scene.Lit == true && scene.Definition != nullptr && scene.Objects.isEmpty() == false &&
                               scene.AlwaysOnLightIds.isEmpty() == false;
    const QByteArray key = hasBakeInputs == true ? preview_lighting::MakeKey(scene) : QByteArray{};
    if (key != mLightingKey)
    {
        resetJob();
        mLightingKey = key;
        mReady.clear();
        mFailure.clear();
    }
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

void WorldSurfaceBakeState::resetJob() noexcept
{
    mJob.Reset();
    static_cast<void>(mJobs.Shutdown());
}

void WorldSurfaceBakeState::Poll(const RenderScene& scene)
{
    if (mLightingKey.isEmpty() == true || GetReady().isNull() == false || mFailure.isEmpty() == false)
    {
        return;
    }
    const auto recordFailure = [this](rhi::RasterStatus status) {
        resetJob();
        mFailure = QStringLiteral("Always-on bake failed (%1). Using realtime lighting. "
                                  "Simplify the World geometry or clear the bake option in Properties.")
                       .arg(static_cast<uint32>(status));
    };
    if (mJob.Get() == nullptr)
    {
        preview_lighting::Inputs inputs;
        const rhi::RasterStatus prepared =
            preview_lighting::BuildInputs(scene, mLightingKey, inputs, preview_lighting::InputsTarget::Surfaces);
        if (prepared != rhi::RasterStatus::Ready)
        {
            recordFailure(prepared);
            return;
        }
        if (mJobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) != threading::JobStatus::Ok)
        {
            recordFailure(rhi::RasterStatus::Failed);
            return;
        }
        renderer::StaticLightingBakeDescription description;
        description.SceneRevision = inputs.SceneRevision;
        description.DependencyRevision = inputs.DependencyRevision;
        description.Surfaces = {inputs.Surfaces.GetData(), inputs.Surfaces.GetSize()};
        description.AnalyticLightIds = {scene.AlwaysOnLightIds.constData(),
                                        static_cast<usize>(scene.AlwaysOnLightIds.size())};
        mJob.Reset(new (std::nothrow) StaticLightingJob(scene, description));
        const rhi::RasterStatus started = mJob.Get() == nullptr ? rhi::RasterStatus::OutOfMemory : mJob->Start(mJobs);
        if (started != rhi::RasterStatus::Pending && started != rhi::RasterStatus::Ready)
        {
            recordFailure(started);
        }
        return;
    }
    const rhi::RasterStatus status = mJob->Poll();
    if (status == rhi::RasterStatus::Pending)
    {
        return;
    }
    if (status != rhi::RasterStatus::Ready)
    {
        recordFailure(status);
        return;
    }
    const UniquePtr<renderer::StaticLightingBake> completed = mJob->TakeBake();
    resetJob();
    const QSharedPointer<WorldSurfaceBake> bake = QSharedPointer<WorldSurfaceBake>::create();
    bake->LightingKey = mLightingKey;
    bake->Charts.reserve(scene.Objects.size());
    for (qsizetype index = 0; index < scene.Objects.size(); ++index)
    {
        renderer::StaticLightingChart source;
        if (completed->GetChart(static_cast<usize>(index), source) != rhi::RasterStatus::Ready)
        {
            recordFailure(rhi::RasterStatus::Failed);
            return;
        }
        WorldSurfaceChart chart;
        chart.Width = source.Width;
        chart.Height = source.Height;
        chart.Scale = source.Scale;
        if (IsChartValid(chart) == false)
        {
            recordFailure(rhi::RasterStatus::InvalidDescription);
            return;
        }
        chart.Pixels = QByteArray(reinterpret_cast<const char*>(source.Pixels.GetDataOrNull()),
                                  static_cast<qsizetype>(source.Pixels.GetSize()));
        bake->Charts.append(chart);
    }
    mReady = bake;
}

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
