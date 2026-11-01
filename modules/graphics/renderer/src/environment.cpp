#include <ludus/graphics/renderer/environment.hpp>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <cstring>
#include <new>

#include "internal/environment_data.h"

// Thanks to Romain Guy and Mathias Agopian, Physically Based Rendering in
// Filament, sections 5.3 and 9.6, https://google.github.io/filament/main/filament.html:
// diffuse SH convolution and deterministic GGX prefiltering. Latitude/longitude
// storage replaces cubemaps; this single-scatter profile has no energy compensation.
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering,
// 4th ed., Infinite Area Lights, https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights:
// luminance/solid-angle importance sampling. We use exact per-row solid angles and
// piecewise-constant source radiance, with one flat CDF rather than their hierarchy.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr usize HALF_PIXEL_BYTES = 4 * sizeof(uint16);
constexpr uint32 MAX_ENVIRONMENT_MIPS = 10;
constexpr float32 HALF_MAXIMUM = 65504;
math::Vector3 MakeDirectionWorld(math::Vector2 longitudeCosine) noexcept
{
    const float32 radius = math::Sqrt(math::Max(0.0F, 1 - longitudeCosine.Y * longitudeCosine.Y));
    const float32 longitude = math::kTauF * longitudeCosine.X;
    return {radius * math::Cos(longitude), longitudeCosine.Y, radius * math::Sin(longitude)};
}
math::Vector2 ComputeEnvironmentUv(math::Vector3 direction) noexcept
{
    float32 u = math::Atan2(direction.Z, direction.X) / math::kTauF;
    if (u < 0)
    {
        u += 1;
    }
    const float32 sine = math::Sqrt(math::Max(0.0F, 1 - direction.Y * direction.Y));
    return {u, math::Atan2(sine, direction.Y) / math::kPiF};
}
usize FindPixel(const internal::EnvironmentData& data, math::Vector3 direction) noexcept
{
    const math::Vector2 uv = ComputeEnvironmentUv(direction);
    const uint32 x = math::Min(static_cast<uint32>(uv.X * static_cast<float32>(data.Width)), data.Width - 1);
    const uint32 y = math::Min(static_cast<uint32>(uv.Y * static_cast<float32>(data.Height)), data.Height - 1);
    return usize(y) * data.Width + x;
}
float64 ComputeSolidAngle(const internal::EnvironmentData& data, usize pixel) noexcept
{
    const usize row = pixel / data.Width;
    const float64 top = math::kPi * static_cast<float64>(row) / data.Height;
    const float64 bottom = top + math::kPi / data.Height;
    return math::kTau / data.Width * (math::Cos(top) - math::Cos(bottom));
}
math::Vector3 ReadColor(math::Vector4 value) noexcept
{
    return {value.X, value.Y, value.Z};
}
// Finite nonnegative binary32 -> binary16, round-to-nearest/ties-to-even.
// This private upload codec never receives infinities, NaNs or values above 65504.
uint16 EncodeHalf(float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32 exponent = (bits >> 23U) & 255U;
    const uint32 mantissa = bits & 0x7fffffU;
    if (exponent < 103U)
    {
        return 0;
    }
    if (exponent < 113U)
    {
        const uint32 shift = 126U - exponent;
        const uint32 significand = mantissa | 0x800000U;
        const uint32 halfway = uint32{1} << (shift - 1U);
        const uint32 rounded = (significand + halfway - 1U + ((significand >> shift) & 1U)) >> shift;
        return static_cast<uint16>(rounded);
    }
    const uint32 rounded = mantissa + 0xfffU + ((mantissa >> 13U) & 1U);
    return static_cast<uint16>(((exponent - 112U) << 10U) + (rounded >> 13U));
}
float32 ComputeRadicalInverse(uint32 bits) noexcept
{
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xaaaaaaaaU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xccccccccU) >> 2U);
    bits = ((bits & 0x0f0f0f0fU) << 4U) | ((bits & 0xf0f0f0f0U) >> 4U);
    bits = ((bits & 0x00ff00ffU) << 8U) | ((bits & 0xff00ff00U) >> 8U);
    return static_cast<float32>(static_cast<float64>(bits) / 4294967296.0);
}
math::Vector3
Filter(const internal::EnvironmentData& data, float32 roughness, math::Vector3 normal, uint32 samples) noexcept
{
    if (roughness == 0)
    {
        return internal::EvaluateEnvironment(data, normal);
    }
    const math::Vector3 axis = math::Abs(normal.Y) < .99F ? math::Vector3{0, 1, 0} : math::Vector3{1, 0, 0};
    math::Vector3 tangent;
    static_cast<void>(math::TryNormalize(math::Cross(axis, normal), tangent));
    const math::Vector3 bitangent = math::Cross(normal, tangent);
    const float32 alpha = roughness * roughness;
    const float32 alphaSquare = alpha * alpha;
    math::Vector3 sum{};
    float32 weight = 0;
    for (uint32 i = 0; i < samples; ++i)
    {
        const float32 selector = (static_cast<float32>(i) + .5F) / static_cast<float32>(samples);
        const float32 cosine = math::Sqrt((1 - selector) / (1 + (alphaSquare - 1) * selector));
        const math::Vector3 local = MakeDirectionWorld({ComputeRadicalInverse(i), cosine});
        const math::Vector3 halfway = tangent * local.X + normal * local.Y + bitangent * local.Z;
        const math::Vector3 direction = halfway * (2 * math::Dot(normal, halfway)) - normal;
        const float32 cosineLight = math::Max(0.0F, math::Dot(normal, direction));
        sum = sum + internal::EvaluateEnvironment(data, direction) * cosineLight;
        weight += cosineLight;
    }
    return weight > 0 ? sum / weight : math::Vector3{};
}
} // namespace
namespace internal
{
EnvironmentData::~EnvironmentData() noexcept
{
    if (IsCpuOnly == false)
    {
        static_cast<void>(rhi::Destroy(Device, Sampler));
        static_cast<void>(rhi::Destroy(Device, Texture));
    }
}
rhi::RasterStatus BuildEnvironmentDistribution(EnvironmentData& inoutData) noexcept
{
    if (inoutData.Distribution.TryResize(inoutData.Pixels.GetSize()) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    inoutData.Weight = 0;
    for (usize i = 0; i < inoutData.Pixels.GetSize(); ++i)
    {
        const math::Vector3 radiance = ReadColor(inoutData.Pixels[i]);
        const float64 solidAngle = ComputeSolidAngle(inoutData, i);
        inoutData.Weight +=
            static_cast<float64>(math::Dot(radiance, math::Vector3{.2126F, .7152F, .0722F})) * solidAngle;
        inoutData.Distribution[i] = inoutData.Weight;
    }
    return rhi::RasterStatus::Ready;
}
void RetainEnvironment(EnvironmentData* dataOrNull) noexcept
{
    if (dataOrNull != nullptr)
    {
        ++dataOrNull->References;
    }
}
void ReleaseEnvironment(EnvironmentData* dataOrNull) noexcept
{
    if (dataOrNull != nullptr && --dataOrNull->References == 0)
    {
        delete dataOrNull;
    }
}
void EvaluateEnvironmentBasis(math::Vector3 direction, float32* outBasis) noexcept
{
    outBasis[0] = .2820947918F;
    outBasis[1] = .4886025119F * direction.Y;
    outBasis[2] = .4886025119F * direction.Z;
    outBasis[3] = .4886025119F * direction.X;
    outBasis[4] = 1.0925484306F * direction.X * direction.Y;
    outBasis[5] = 1.0925484306F * direction.Y * direction.Z;
    outBasis[6] = .3153915653F * (3 * direction.Z * direction.Z - 1);
    outBasis[7] = 1.0925484306F * direction.X * direction.Z;
    outBasis[8] = .5462742153F * (direction.X * direction.X - direction.Y * direction.Y);
}
math::Vector3 EvaluateEnvironment(const EnvironmentData& data, math::Vector3 direction) noexcept
{
    return ReadColor(data.Pixels[FindPixel(data, direction)]);
}
float32 EvaluateEnvironmentPdf(const EnvironmentData& data, math::Vector3 direction) noexcept
{
    if (data.Weight == 0)
    {
        return 1 / (4 * math::kPiF);
    }
    const usize pixel = FindPixel(data, direction);
    const float64 previous = pixel == 0 ? 0 : data.Distribution[pixel - 1];
    return static_cast<float32>((data.Distribution[pixel] - previous) / (data.Weight * ComputeSolidAngle(data, pixel)));
}
EnvironmentSample SampleEnvironment(const EnvironmentData& data, float32 selector, float32 latitude) noexcept
{
    EnvironmentSample result;
    if (data.Weight == 0)
    {
        result.DirectionWorld = MakeDirectionWorld({selector, 1 - 2 * latitude});
        result.Radiance = {};
        result.Pdf = 1 / (4 * math::kPiF);
        return result;
    }
    const float64 target = static_cast<float64>(selector) * data.Weight;
    usize low = 0;
    usize high = data.Distribution.GetSize() - 1;
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (data.Distribution[middle] <= target)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    const float64 previous = low == 0 ? 0 : data.Distribution[low - 1];
    const float64 fraction = (target - previous) / (data.Distribution[low] - previous);
    const usize row = low / data.Width;
    const float64 top = math::kPi * static_cast<float64>(row) / data.Height;
    const float64 bottom = top + math::kPi / data.Height;
    const float32 cosine = static_cast<float32>(math::Cos(top) * (1 - static_cast<float64>(latitude)) +
                                                math::Cos(bottom) * static_cast<float64>(latitude));
    result.DirectionWorld = MakeDirectionWorld(
        {static_cast<float32>((static_cast<float64>(low % data.Width) + fraction) / data.Width), cosine});
    result.Radiance = ReadColor(data.Pixels[low]);
    result.Pdf =
        static_cast<float32>((data.Distribution[low] - previous) / (data.Weight * ComputeSolidAngle(data, low)));
    return result;
}

} // namespace internal
EnvironmentMap::~EnvironmentMap() noexcept
{
    Reset();
}
EnvironmentMap::EnvironmentMap(EnvironmentMap&& other) noexcept : mDataOrNull(other.mDataOrNull)
{
    other.mDataOrNull = nullptr;
}
EnvironmentMap& EnvironmentMap::operator=(EnvironmentMap&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        mDataOrNull = other.mDataOrNull;
        other.mDataOrNull = nullptr;
    }
    return *this;
}
void EnvironmentMap::Reset() noexcept
{
    internal::ReleaseEnvironment(mDataOrNull);
    mDataOrNull = nullptr;
}
rhi::RasterStatus EnvironmentMap::GetStatus() const noexcept
{
    if (mDataOrNull == nullptr)
    {
        return rhi::RasterStatus::InvalidHandle;
    }
    const rhi::RasterStatus texture = rhi::GetStatus(mDataOrNull->Device, mDataOrNull->Texture);
    return texture == rhi::RasterStatus::Ready ? rhi::GetStatus(mDataOrNull->Device, mDataOrNull->Sampler) : texture;
}
rhi::RasterStatus EnvironmentMap::Initialize(rhi::DeviceHandle device,
                                             const EnvironmentDescription& description) noexcept
{
    using Status = rhi::RasterStatus;
    if (mDataOrNull != nullptr)
    {
        return Status::InvalidState;
    }
    const uint32 width = description.FilterWidth;
    if (description.PixelsOrNull == nullptr || description.Width == 0 || description.Height == 0 ||
        description.Width > 4096 || description.Height > 4096 || width < 16 || width > 512 ||
        (width & (width - 1)) != 0 || description.FilterSamples < 16 || description.FilterSamples > 1024)
    {
        return Status::InvalidDescription;
    }
    const usize count = usize(description.Width) * description.Height;
    const usize sourceEncodingBytes = count * 3 * sizeof(float32);
    const usize uploadBytes = usize(width) * (width / 2) * HALF_PIXEL_BYTES * 2;
    const usize cpuBytes = sizeof(internal::EnvironmentData) + count * (sizeof(math::Vector4) + sizeof(float64));
    if (cpuBytes > description.CpuByteBudget ||
        math::Max(uploadBytes, sourceEncodingBytes) > description.CpuByteBudget - cpuBytes)
    {
        return Status::CapacityExceeded;
    }
    for (usize i = 0; i < count; ++i)
    {
        const math::Vector4 pixel = description.PixelsOrNull[i];
        if (math::IsFinite(pixel.X) == false || math::IsFinite(pixel.Y) == false || math::IsFinite(pixel.Z) == false ||
            pixel.X < 0 || pixel.Y < 0 || pixel.Z < 0 || pixel.X > MAX_ENVIRONMENT_RADIANCE ||
            pixel.Y > MAX_ENVIRONMENT_RADIANCE || pixel.Z > MAX_ENVIRONMENT_RADIANCE)
        {
            return Status::InvalidDescription;
        }
    }
    internal::EnvironmentData* dataOrNull = new (std::nothrow) internal::EnvironmentData;
    if (dataOrNull == nullptr)
    {
        return Status::OutOfMemory;
    }
    dataOrNull->Device = device;
    dataOrNull->Width = description.Width;
    dataOrNull->Height = description.Height;
    dataOrNull->CpuBytes = cpuBytes;
    if (dataOrNull->Pixels.TryAddRange({description.PixelsOrNull, count}) == false ||
        dataOrNull->Distribution.TryResize(count) == false)
    {
        delete dataOrNull;
        return Status::OutOfMemory;
    }
    Array<uint8> canonical;
    if (canonical.TryResize(sourceEncodingBytes) == false)
    {
        delete dataOrNull;
        return Status::OutOfMemory;
    }
    for (usize pixel = 0; pixel < count; ++pixel)
    {
        const math::Vector4 value = dataOrNull->Pixels[pixel];
        dataOrNull->Pixels[pixel].W = 1;
        const float32 channels[]{value.X, value.Y, value.Z};
        for (usize channel = 0; channel < 3; ++channel)
        {
            uint32 bits = 0;
            std::memcpy(&bits, &channels[channel], sizeof(bits));
            static_cast<void>(
                TryWriteLittleEndian(bits,
                                     {canonical.GetData() + (pixel * 3 + channel) * sizeof(uint32), sizeof(uint32)}));
            dataOrNull->RadianceScale = math::Max(dataOrNull->RadianceScale, channels[channel] / HALF_MAXIMUM);
        }
    }
    const Fingerprint128 fingerprint = StableFingerprint128({canonical.GetData(), canonical.GetSize()});
    dataOrNull->FingerprintLow = fingerprint.Low;
    dataOrNull->FingerprintHigh = fingerprint.High;
    canonical = {};
    const Status distributionStatus = internal::BuildEnvironmentDistribution(*dataOrNull);
    if (distributionStatus != Status::Ready)
    {
        delete dataOrNull;
        return distributionStatus;
    }
    float64 irradiance[internal::ENVIRONMENT_SH_COUNT][3]{};
    for (usize i = 0; i < count; ++i)
    {
        const math::Vector3 radiance = ReadColor(dataOrNull->Pixels[i]);
        const float64 solidAngle = ComputeSolidAngle(*dataOrNull, i);
        const float32 u = (static_cast<float32>(i % dataOrNull->Width) + .5F) / static_cast<float32>(dataOrNull->Width);
        const usize row = i / dataOrNull->Width;
        const float32 v = (static_cast<float32>(row) + .5F) / static_cast<float32>(dataOrNull->Height);
        float32 basis[internal::ENVIRONMENT_SH_COUNT]{};
        internal::EvaluateEnvironmentBasis(MakeDirectionWorld({u, math::Cos(math::kPiF * v)}), basis);
        for (usize coefficient = 0; coefficient < internal::ENVIRONMENT_SH_COUNT; ++coefficient)
        {
            const float32 convolution = coefficient == 0  ? math::kPiF
                                        : coefficient < 4 ? 2 * math::kPiF / 3
                                                          : math::kPiF / 4;
            const float64 scale =
                static_cast<float64>(basis[coefficient]) * solidAngle * static_cast<float64>(convolution);
            irradiance[coefficient][0] += static_cast<float64>(radiance.X) * scale;
            irradiance[coefficient][1] += static_cast<float64>(radiance.Y) * scale;
            irradiance[coefficient][2] += static_cast<float64>(radiance.Z) * scale;
        }
    }
    for (usize coefficient = 0; coefficient < internal::ENVIRONMENT_SH_COUNT; ++coefficient)
    {
        dataOrNull->Irradiance[coefficient] = {static_cast<float32>(irradiance[coefficient][0]),
                                               static_cast<float32>(irradiance[coefficient][1]),
                                               static_cast<float32>(irradiance[coefficient][2]),
                                               0};
    }
    dataOrNull->Irradiance[0].W = dataOrNull->RadianceScale;
    Array<uint8> bytes;
    if (bytes.TryResize(uploadBytes) == false)
    {
        delete dataOrNull;
        return Status::OutOfMemory;
    }
    rhi::TextureMipUpload mips[MAX_ENVIRONMENT_MIPS]{};
    uint32 mipWidth = width;
    uint32 mipHeight = width / 2;
    while (mipWidth > 1)
    {
        ++dataOrNull->MipLevels;
        mipWidth /= 2;
    }
    ++dataOrNull->MipLevels;
    mipWidth = width;
    usize offset = 0;
    for (uint32 mip = 0; mip < dataOrNull->MipLevels; ++mip)
    {
        mips[mip] = {offset, usize(mipWidth) * HALF_PIXEL_BYTES};
        const float32 roughness = static_cast<float32>(mip) / static_cast<float32>(dataOrNull->MipLevels - 1);
        for (uint32 y = 0; y < mipHeight; ++y)
        {
            for (uint32 x = 0; x < mipWidth; ++x)
            {
                const math::Vector3 direction = MakeDirectionWorld(
                    {(static_cast<float32>(x) + .5F) / static_cast<float32>(mipWidth),
                     math::Cos(math::kPiF * (static_cast<float32>(y) + .5F) / static_cast<float32>(mipHeight))});
                const math::Vector3 radiance =
                    Filter(*dataOrNull, roughness, direction, description.FilterSamples) / dataOrNull->RadianceScale;
                const float32 channels[]{radiance.X, radiance.Y, radiance.Z, 1};
                for (usize channel = 0; channel < 4; ++channel)
                {
                    static_cast<void>(TryWriteLittleEndian(EncodeHalf(math::Min(channels[channel], HALF_MAXIMUM)),
                                                           {bytes.GetData() + offset, sizeof(uint16)}));
                    offset += sizeof(uint16);
                }
            }
        }
        mipWidth = math::Max(1U, mipWidth / 2);
        mipHeight = math::Max(1U, mipHeight / 2);
    }
    rhi::TextureDescription texture{width, width / 2, rhi::RasterFormat::Rgba16Float};
    texture.MipLevels = dataOrNull->MipLevels;
    Status status = rhi::CreateTexture(device,
                                       texture,
                                       {{bytes.GetData(), offset}, 0, {mips, dataOrNull->MipLevels}},
                                       dataOrNull->Texture);
    if (status == Status::Ready || status == Status::Pending)
    {
        rhi::SamplerDescription sampler;
        sampler.Filter = rhi::RasterFilter::Linear;
        sampler.U = rhi::RasterAddress::Repeat;
        sampler.V = rhi::RasterAddress::Clamp;
        sampler.MipFilter = rhi::RasterMipFilter::Linear;
        sampler.MaxLod = static_cast<float32>(dataOrNull->MipLevels - 1);
        status = rhi::CreateSampler(device, sampler, dataOrNull->Sampler);
    }
    if (status != Status::Ready && status != Status::Pending)
    {
        delete dataOrNull;
        return status;
    }
    mDataOrNull = dataOrNull;
    status = GetStatus();
    if (status != Status::Ready && status != Status::Pending)
    {
        Reset();
    }
    return status;
}
} // namespace ludus::graphics::renderer
