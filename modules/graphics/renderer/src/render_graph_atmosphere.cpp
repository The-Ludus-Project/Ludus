#include "internal/render_graph_atmosphere.h"

#include "../shaders/renderer_atmosphere_layout.h"
#include "internal/camera_inverse.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <new>

#if defined(LUDUS_RENDERER_ATMOSPHERE_SHADERS)
#    include "renderer_atmosphere_display.h"
#    include "renderer_atmosphere_sky.h"
#    include "renderer_atmosphere_transmittance.h"
#endif

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
constexpr float64 METRES_PER_KILOMETRE = 1000;
constexpr float64 GROUND_RADIUS_METRES = LUDUS_ATMOSPHERE_GROUND_KILOMETRES * METRES_PER_KILOMETRE;

// Work in a local frame whose origin is the anchor. Rationalizing r - R
// retains the sign of centimetre-scale altitude at the Earth's surface.
bool TryResolveObserver(const AtmosphereDescription& atmosphere,
                        math::Vector3 eyeWorld,
                        float64& outAltitudeMetres,
                        math::Vector3& outUpWorld) noexcept
{
    const math::Vector3d anchorUpWorld{static_cast<float64>(atmosphere.SurfaceUpWorld.X),
                                       static_cast<float64>(atmosphere.SurfaceUpWorld.Y),
                                       static_cast<float64>(atmosphere.SurfaceUpWorld.Z)};
    const float64 upLength = math::Length(anchorUpWorld);
    if (math::IsFinite(upLength) == false || upLength <= 0 || math::IsFinite(eyeWorld) == false ||
        math::IsFinite(atmosphere.SurfaceAnchorWorld) == false)
    {
        return false;
    }
    const math::Vector3d unitUpWorld = anchorUpWorld * (1 / upLength);
    const math::Vector3d offsetWorld{
        static_cast<float64>(eyeWorld.X) - static_cast<float64>(atmosphere.SurfaceAnchorWorld.X),
        static_cast<float64>(eyeWorld.Y) - static_cast<float64>(atmosphere.SurfaceAnchorWorld.Y),
        static_cast<float64>(eyeWorld.Z) - static_cast<float64>(atmosphere.SurfaceAnchorWorld.Z),
    };
    const math::Vector3d radiusWorld = unitUpWorld * GROUND_RADIUS_METRES + offsetWorld;
    const float64 radius = math::Length(radiusWorld);
    if (radius <= 0 || math::IsFinite(radius) == false)
    {
        return false;
    }
    const float64 altitude =
        (math::Dot(offsetWorld, offsetWorld) + 2 * GROUND_RADIUS_METRES * math::Dot(offsetWorld, unitUpWorld)) /
        (radius + GROUND_RADIUS_METRES);
    const math::Vector3d upWorld = radiusWorld * (1 / radius);
    outUpWorld = {static_cast<float32>(upWorld.X), static_cast<float32>(upWorld.Y), static_cast<float32>(upWorld.Z)};
    outAltitudeMetres = altitude;
    return true;
}
} // namespace

bool TryGetAtmosphereAltitude(const AtmosphereDescription& atmosphere,
                              math::Vector3 eyeWorld,
                              float64& outAltitudeMetres) noexcept
{
    math::Vector3 upWorld;
    return TryResolveObserver(atmosphere, eyeWorld, outAltitudeMetres, upWorld);
}

float64 GetAtmosphereTopAltitude() noexcept
{
    return LUDUS_ATMOSPHERE_HEIGHT_KILOMETRES * METRES_PER_KILOMETRE;
}

namespace internal
{
using Status = rhi::RasterStatus;
namespace
{
constexpr usize CLIP_ROWS = 4;
constexpr usize VERTEX_COUNT = 4;
constexpr usize INDEX_COUNT = 6;
constexpr uint32 SKY_WIDTH = LUDUS_ATMOSPHERE_SKY_WIDTH;
constexpr uint32 SKY_HEIGHT = 2 * LUDUS_ATMOSPHERE_SKY_HALF_HEIGHT;
constexpr uint32 UNIFORM_SLOT = 0;
constexpr uint32 TRANSMITTANCE_SLOT = 1;
constexpr uint32 SKY_SLOT = 2;
constexpr usize SKY_BINDING_COUNT = 2;
constexpr usize DISPLAY_BINDING_COUNT = SKY_BINDING_COUNT + 1;

bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
} // namespace

struct AtmosphereViewUniform final
{
    StaticArray<math::Vector4, CLIP_ROWS> ClipToEyeRows{};
    math::Vector4 RadialUpWorld{};
    math::Vector4 DirectionToSunWorld{};
    math::Vector4 SolarIlluminance{};
    math::Vector4 CameraForwardWorld{};
    math::Vector4 Output{};
};

struct AtmosphereSkyTable final
{
    rhi::TextureHandle Texture{};
    rhi::TextureViewHandle View{};
    AtmosphereSkyTable* NextOrNull = nullptr;
    float32 Altitude = 0;
    float32 SolarZenithCosine = 0;
    usize References = 1;
    bool HasView = false;
};

RenderGraphAtmosphereProgram::~RenderGraphAtmosphereProgram() noexcept
{
    for (PassResources& pass : mPasses)
    {
        static_cast<void>(rhi::Destroy(mDevice, pass.Pipeline));
        static_cast<void>(rhi::Destroy(mDevice, pass.Layout));
        static_cast<void>(rhi::Destroy(mDevice, pass.Shader));
    }
    static_cast<void>(rhi::Destroy(mDevice, mTransmittanceBindings));
    static_cast<void>(rhi::Destroy(mDevice, mTransmittanceView));
    static_cast<void>(rhi::Destroy(mDevice, mTransmittance));
    static_cast<void>(rhi::Destroy(mDevice, mVertex));
}

Status RenderGraphAtmosphereProgram::Prepare(rhi::DeviceHandle device) noexcept
{
    if (mPhase == Phase::Empty)
    {
#if defined(LUDUS_RENDERER_ATMOSPHERE_SHADERS)
        constexpr uint32 TRANSMITTANCE_WIDTH = LUDUS_ATMOSPHERE_TRANSMITTANCE_WIDTH;
        constexpr uint32 TRANSMITTANCE_HEIGHT = LUDUS_ATMOSPHERE_TRANSMITTANCE_HEIGHT;
        mDevice = device;
        const StaticArray<rhi::RasterShaderDescription, static_cast<usize>(Stage::Count)> descriptions{
            {shaders::renderer_atmosphere_transmittance::Fragment(),
             shaders::renderer_atmosphere_sky::Fragment(),
             shaders::renderer_atmosphere_display::Fragment()}};
        Status status = rhi::CreateRasterShader(device, shaders::renderer_atmosphere_transmittance::Vertex(), mVertex);
        for (usize index = 0; index < mPasses.GetSize() && IsAccepted(status) == true; ++index)
        {
            PassResources& pass = mPasses[index];
            const rhi::RasterShaderDescription& description = descriptions[index];
            status = rhi::CreateRasterShader(device, description, pass.Shader);
            if (IsAccepted(status) == true)
            {
                status = rhi::CreateBindingLayout(device, description.Bindings, pass.Layout);
            }
        }
        if (IsAccepted(status) == true)
        {
            status =
                rhi::CreateTexture(device,
                                   {TRANSMITTANCE_WIDTH, TRANSMITTANCE_HEIGHT, rhi::RasterFormat::Rgba16Float, true},
                                   {},
                                   mTransmittance);
        }
        mPhase = IsAccepted(status) == true ? Phase::Dependencies : Phase::Failed;
        mStatus = status;
#else
        static_cast<void>(device);
        mPhase = Phase::Failed;
        mStatus = Status::Unsupported;
#endif
    }
    if (mPhase == Phase::Ready || mPhase == Phase::Failed)
    {
        return mStatus;
    }
    mStatus = pollDependencies();
    if (IsAccepted(mStatus) == false)
    {
        mPhase = Phase::Failed;
    }
    return mStatus;
}

Status RenderGraphAtmosphereProgram::pollDependencies() noexcept
{
    if (mPhase == Phase::Dependencies)
    {
        Status status = rhi::GetStatus(mDevice, mVertex);
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, mTransmittance);
        }
        for (const PassResources& pass : mPasses)
        {
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(mDevice, pass.Shader);
            }
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(mDevice, pass.Layout);
            }
        }
        if (status != Status::Ready)
        {
            return status;
        }
        status = rhi::CreateTextureView(mDevice, mTransmittance, {}, mTransmittanceView);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        status = rhi::CreateBindingSet(mDevice,
                                       mPasses[static_cast<usize>(Stage::Transmittance)].Layout,
                                       {},
                                       mTransmittanceBindings);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        const StaticArray<rhi::RasterVertexStream, 1> streams{{{sizeof(math::Vector4), false}}};
        const StaticArray<rhi::RasterVertexAttribute, 1> attributes{{{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        for (usize index = 0; index < mPasses.GetSize(); ++index)
        {
            PassResources& pass = mPasses[index];
            rhi::RasterPipelineDescription description{mVertex,
                                                       pass.Shader,
                                                       pass.Layout,
                                                       streams.AsSpan(),
                                                       attributes.AsSpan()};
            description.Target = index == static_cast<usize>(Stage::Display) ? rhi::RasterTarget::Rgba8Unorm
                                                                             : rhi::RasterTarget::Rgba16Float;
            status = rhi::CreateRasterPipeline(mDevice, description, pass.Pipeline);
            if (IsAccepted(status) == false)
            {
                return status;
            }
        }
        mPhase = Phase::Pipelines;
    }
    Status status = rhi::GetStatus(mDevice, mTransmittanceView);
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(mDevice, mTransmittanceBindings);
    }
    for (const PassResources& pass : mPasses)
    {
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(mDevice, pass.Pipeline);
        }
    }
    if (status == Status::Ready)
    {
        mPhase = Phase::Ready;
    }
    return status;
}

Status RenderGraphAtmosphereProgram::draw(Stage stage,
                                          rhi::BindingSetHandle bindings,
                                          const rhi::RasterVertexSlice& vertices,
                                          rhi::BufferHandle indices) const noexcept
{
    return rhi::DrawIndexed(mDevice,
                            {mPasses[static_cast<usize>(stage)].Pipeline,
                             bindings,
                             {&vertices, 1},
                             indices,
                             0,
                             INDEX_COUNT,
                             VERTEX_COUNT,
                             1});
}

Status RenderGraphAtmosphereProgram::acquireSky(const AtmosphereViewUniform& uniform,
                                                AtmosphereSkyTable*& outTableOrNull) noexcept
{
    for (AtmosphereSkyTable* tableOrNull = mSkyTablesOrNull; tableOrNull != nullptr;
         tableOrNull = tableOrNull->NextOrNull)
    {
        if (tableOrNull->Altitude == uniform.RadialUpWorld.W &&
            tableOrNull->SolarZenithCosine == uniform.DirectionToSunWorld.W)
        {
            ++tableOrNull->References;
            outTableOrNull = tableOrNull;
            return Status::Ready;
        }
    }
    AtmosphereSkyTable* tableOrNull = new (std::nothrow) AtmosphereSkyTable;
    if (tableOrNull == nullptr)
    {
        return Status::OutOfMemory;
    }
    const Status status = rhi::CreateTexture(mDevice,
                                             {SKY_WIDTH, SKY_HEIGHT, rhi::RasterFormat::Rgba16Float, true},
                                             {},
                                             tableOrNull->Texture);
    if (IsAccepted(status) == false)
    {
        delete tableOrNull;
        return status;
    }
    tableOrNull->Altitude = uniform.RadialUpWorld.W;
    tableOrNull->SolarZenithCosine = uniform.DirectionToSunWorld.W;
    tableOrNull->NextOrNull = mSkyTablesOrNull;
    mSkyTablesOrNull = tableOrNull;
    outTableOrNull = tableOrNull;
    return status;
}

void RenderGraphAtmosphereProgram::releaseSky(AtmosphereSkyTable& inoutTable) noexcept
{
    if (--inoutTable.References != 0)
    {
        return;
    }
    AtmosphereSkyTable** link = &mSkyTablesOrNull;
    while (*link != &inoutTable)
    {
        link = &(*link)->NextOrNull;
    }
    *link = inoutTable.NextOrNull;
    static_cast<void>(rhi::Destroy(mDevice, inoutTable.View));
    static_cast<void>(rhi::Destroy(mDevice, inoutTable.Texture));
    delete &inoutTable;
}

RenderGraphAtmosphereView::~RenderGraphAtmosphereView() noexcept
{
    if (mProgramOrNull != nullptr)
    {
        const rhi::DeviceHandle device = mProgramOrNull->mDevice;
        static_cast<void>(rhi::Destroy(device, mDisplayBindings));
        static_cast<void>(rhi::Destroy(device, mSkyBindings));
        static_cast<void>(rhi::Destroy(device, mUniform));
        if (mSkyOrNull != nullptr)
        {
            mProgramOrNull->releaseSky(*mSkyOrNull);
        }
    }
}

Status RenderGraphAtmosphereView::Prepare(RenderGraphAtmosphereProgram& inoutProgram,
                                          rhi::DeviceHandle device,
                                          const ViewDescription& camera,
                                          uint32 width,
                                          uint32 height,
                                          float32 exposure,
                                          uint32 curve) noexcept
{
    using Stage = RenderGraphAtmosphereProgram::Stage;
    const AtmosphereDescription& atmosphere = camera.Atmosphere;
    if (atmosphere.IsEnabled == false)
    {
        return Status::Ready;
    }
    if (mHasUniform == false)
    {
        float64 altitudeMetres = 0;
        math::Vector3 upWorld;
        math::Vector3 sunWorld;
        const math::Vector3 illuminance = atmosphere.SolarIlluminance;
        const float32 radius = atmosphere.SolarAngularRadiusRadians;
        if (TryResolveObserver(atmosphere, camera.CameraPosition, altitudeMetres, upWorld) == false ||
            math::TryNormalize(atmosphere.DirectionToSunWorld, sunWorld) != math::MathStatus::Success ||
            math::IsFinite(illuminance) == false || illuminance.X < 0 || illuminance.Y < 0 || illuminance.Z < 0 ||
            math::IsFinite(radius) == false || radius <= 0 || radius >= math::kHalfPiF)
        {
            return Status::InvalidDescription;
        }
        if (altitudeMetres < 0 || altitudeMetres > GetAtmosphereTopAltitude())
        {
            return Status::Ready;
        }
        const Status prepared = inoutProgram.Prepare(device);
        if (prepared != Status::Ready)
        {
            return prepared;
        }
        AtmosphereViewUniform uniform;
        math::Matrix4 clipToEye;
        if (TryMakeCameraClipToEye(camera, clipToEye) == false)
        {
            return Status::InvalidDescription;
        }
        const math::Matrix4 rows = math::Transpose(clipToEye);
        for (usize row = 0; row < CLIP_ROWS; ++row)
        {
            uniform.ClipToEyeRows[row] = rows.Columns[row];
        }
        uniform.RadialUpWorld = {upWorld.X,
                                 upWorld.Y,
                                 upWorld.Z,
                                 static_cast<float32>(altitudeMetres / METRES_PER_KILOMETRE)};
        uniform.DirectionToSunWorld = {sunWorld.X,
                                       sunWorld.Y,
                                       sunWorld.Z,
                                       math::Clamp(math::Dot(upWorld, sunWorld), -1.0F, 1.0F)};
        uniform.SolarIlluminance = {illuminance.X, illuminance.Y, illuminance.Z, 2 * math::Sin(radius * 0.5F)};
        math::Vector3 forwardWorld{};
        if (camera.OrthographicCamera == true &&
            math::TryNormalize(camera.CameraForward, forwardWorld) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        const float32 sine = math::Sin(radius);
        const float32 diskScale = 1 / (math::kPiF * sine * sine);
        if (math::IsFinite(diskScale) == false)
        {
            return Status::InvalidDescription;
        }
        uniform.CameraForwardWorld = {forwardWorld.X, forwardWorld.Y, forwardWorld.Z, diskScale};
        uniform.Output = {static_cast<float32>(width),
                          static_cast<float32>(height),
                          exposure,
                          static_cast<float32>(curve)};
        mProgramOrNull = &inoutProgram;
        Status status = inoutProgram.acquireSky(uniform, mSkyOrNull);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Uniform, sizeof(uniform)},
                                   {reinterpret_cast<const uint8*>(&uniform), sizeof(uniform)},
                                   mUniform);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        mHasUniform = true;
    }
    Status status = rhi::GetStatus(device, mUniform);
    if (status != Status::Ready)
    {
        return status;
    }
    AtmosphereSkyTable& sky = *mSkyOrNull;
    if (sky.HasView == false)
    {
        status = rhi::GetStatus(device, sky.Texture);
        if (status != Status::Ready)
        {
            return status;
        }
        status = rhi::CreateTextureView(device, sky.Texture, {}, sky.View);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        sky.HasView = true;
    }
    status = rhi::GetStatus(device, sky.View);
    if (status != Status::Ready)
    {
        return status;
    }
    const StaticArray<rhi::RasterBindingResource, DISPLAY_BINDING_COUNT> resources{
        {{ .Binding = UNIFORM_SLOT, .Buffer = mUniform, .Size = sizeof(AtmosphereViewUniform) },
         { .Binding = TRANSMITTANCE_SLOT, .Texture = inoutProgram.mTransmittanceView },
         { .Binding = SKY_SLOT, .Texture = sky.View }}};
    if (mHasSkyBindings == false)
    {
        status = rhi::CreateBindingSet(device,
                                       inoutProgram.mPasses[static_cast<usize>(Stage::Sky)].Layout,
                                       {resources.GetData(), SKY_BINDING_COUNT},
                                       mSkyBindings);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        mHasSkyBindings = true;
    }
    if (mHasDisplayBindings == false)
    {
        status = rhi::CreateBindingSet(device,
                                       inoutProgram.mPasses[static_cast<usize>(Stage::Display)].Layout,
                                       resources.AsSpan(),
                                       mDisplayBindings);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        mHasDisplayBindings = true;
    }
    status = rhi::GetStatus(device, mSkyBindings);
    return status == Status::Ready ? rhi::GetStatus(device, mDisplayBindings) : status;
}

Status RenderGraphAtmosphereView::RecordTables(const rhi::RasterVertexSlice& vertices,
                                               rhi::BufferHandle indices) const noexcept
{
    using Stage = RenderGraphAtmosphereProgram::Stage;
    if (mSkyOrNull == nullptr)
    {
        return Status::Ready;
    }
    const RenderGraphAtmosphereProgram& program = *mProgramOrNull;
    // A cancelled frame never makes an undefined table reusable. RHI publishes
    // ColorDefined with the accepted queue submission and retains its dependencies.
    const auto drawTable = [&program, &vertices, indices](Stage stage,
                                                          rhi::TextureHandle texture,
                                                          rhi::BindingSetHandle bindings) noexcept {
        rhi::RasterTextureState state;
        Status status = rhi::GetTextureState(program.mDevice, texture, state);
        if (status != Status::Ready || state.ColorDefined == true)
        {
            return status;
        }
        rhi::RasterPassDescription pass;
        pass.Color = texture;
        pass.DepthLoad = rhi::RasterLoad::Discard;
        status = rhi::BeginRasterPass(program.mDevice, pass);
        return status == Status::Ready ? program.draw(stage, bindings, vertices, indices) : status;
    };
    const Status transmittance =
        drawTable(Stage::Transmittance, program.mTransmittance, program.mTransmittanceBindings);
    return transmittance == Status::Ready ? drawTable(Stage::Sky, mSkyOrNull->Texture, mSkyBindings) : transmittance;
}

Status RenderGraphAtmosphereView::RecordSky(const rhi::RasterVertexSlice& vertices,
                                            rhi::BufferHandle indices) const noexcept
{
    using Stage = RenderGraphAtmosphereProgram::Stage;
    return mSkyOrNull == nullptr ? Status::Ready
                                 : mProgramOrNull->draw(Stage::Display, mDisplayBindings, vertices, indices);
}
} // namespace internal
} // namespace ludus::graphics::renderer
