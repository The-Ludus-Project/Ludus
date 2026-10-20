// Adapter preference for hosted windows. The Vulkan backend orders its enumerated
// adapters with OrderAdaptersByPreference() before it tries them, so these cases
// pin the selection order a multi-GPU host sees without needing two GPUs.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

#include "internal/hosted_adapter.h"

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::StaticArray;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::graphics::rhi::internal::AreAdaptersEqual;
using ludus::graphics::rhi::internal::OrderAdaptersByPreference;
using ludus::platform::HOSTED_ADAPTER_UUID_BYTES;
using ludus::platform::HostedAdapterIdentity;

namespace
{
constexpr usize ADAPTER_COUNT = 4;
/// Integrated GPU first, as Linux loaders often enumerate it, then the discrete GPU on two drivers.
constexpr usize INTEGRATED = 0;
constexpr usize DISCRETE = 1;
constexpr usize DISCRETE_OTHER_DRIVER = 2;
constexpr usize SOFTWARE = 3;

HostedAdapterIdentity MakeAdapter(uint8 device, uint8 driver)
{
    HostedAdapterIdentity adapter;
    adapter.DeviceUuid.Fill(device);
    adapter.DriverUuid.Fill(driver);
    return adapter;
}

StaticArray<HostedAdapterIdentity, ADAPTER_COUNT> MakeHost()
{
    StaticArray<HostedAdapterIdentity, ADAPTER_COUNT> adapters{};
    adapters[INTEGRATED] = MakeAdapter(0x10, 0x01);
    adapters[DISCRETE] = MakeAdapter(0x20, 0x02);
    adapters[DISCRETE_OTHER_DRIVER] = MakeAdapter(0x20, 0x03);
    adapters[SOFTWARE] = MakeAdapter(0x30, 0x04);
    return adapters;
}

usize Order(const StaticArray<HostedAdapterIdentity, ADAPTER_COUNT>& adapters,
            const HostedAdapterIdentity* preferredOrNull,
            StaticArray<usize, ADAPTER_COUNT>& outOrder)
{
    return OrderAdaptersByPreference({adapters.GetData(), adapters.GetSize()},
                                     preferredOrNull,
                                     {outOrder.GetData(), outOrder.GetSize()});
}
} // namespace

TEST_CASE("Adapters match only when both the device and driver UUIDs are equal", "[rhi][hosted]")
{
    const StaticArray<HostedAdapterIdentity, ADAPTER_COUNT> adapters = MakeHost();
    CHECK(AreAdaptersEqual(adapters[DISCRETE], MakeAdapter(0x20, 0x02)));
    CHECK_FALSE(AreAdaptersEqual(adapters[DISCRETE], adapters[DISCRETE_OTHER_DRIVER]));
    CHECK_FALSE(AreAdaptersEqual(adapters[DISCRETE], adapters[INTEGRATED]));
    HostedAdapterIdentity lastByte = adapters[DISCRETE];
    lastByte.DriverUuid[HOSTED_ADAPTER_UUID_BYTES - 1] = 0x7F;
    CHECK_FALSE(AreAdaptersEqual(adapters[DISCRETE], lastByte));
}

TEST_CASE("A hosted window tries the editor's adapter before the others", "[rhi][hosted]")
{
    const StaticArray<HostedAdapterIdentity, ADAPTER_COUNT> adapters = MakeHost();
    StaticArray<usize, ADAPTER_COUNT> order{};

    // The editor ranks the discrete GPU highest; the game would otherwise take the integrated one.
    const HostedAdapterIdentity editor = adapters[DISCRETE];
    CHECK(Order(adapters, &editor, order) == 1);
    CHECK(order[0] == DISCRETE);
    CHECK(order[1] == INTEGRATED);
    CHECK(order[2] == DISCRETE_OTHER_DRIVER);
    CHECK(order[3] == SOFTWARE);

    // The same GPU on another driver cannot import the images, so it is not preferred.
    const HostedAdapterIdentity otherDriver = adapters[DISCRETE_OTHER_DRIVER];
    CHECK(Order(adapters, &otherDriver, order) == 1);
    CHECK(order[0] == DISCRETE_OTHER_DRIVER);
    CHECK(order[1] == INTEGRATED);
    CHECK(order[2] == DISCRETE);
}

TEST_CASE("Without a matching adapter the hosted order is the enumeration order", "[rhi][hosted]")
{
    const StaticArray<HostedAdapterIdentity, ADAPTER_COUNT> adapters = MakeHost();
    StaticArray<usize, ADAPTER_COUNT> order{};

    // No announcement: Metal editors, or a consumer without an adapter identity.
    CHECK(Order(adapters, nullptr, order) == 0);
    for (usize i = 0; i < ADAPTER_COUNT; ++i)
    {
        CHECK(order[i] == i);
    }

    // An adapter this host does not have: selection proceeds and the consumer refuses the images.
    const HostedAdapterIdentity absent = MakeAdapter(0x99, 0x09);
    CHECK(Order(adapters, &absent, order) == 0);
    for (usize i = 0; i < ADAPTER_COUNT; ++i)
    {
        CHECK(order[i] == i);
    }
}

TEST_CASE("Equal adapters keep their enumeration order", "[rhi][hosted]")
{
    StaticArray<HostedAdapterIdentity, ADAPTER_COUNT> adapters = MakeHost();
    // A loader can list one device twice, for example through two ICD manifests.
    adapters[SOFTWARE] = adapters[DISCRETE];
    StaticArray<usize, ADAPTER_COUNT> order{};
    const HostedAdapterIdentity editor = adapters[DISCRETE];
    CHECK(Order(adapters, &editor, order) == 2);
    CHECK(order[0] == DISCRETE);
    CHECK(order[1] == SOFTWARE);
    CHECK(order[2] == INTEGRATED);
    CHECK(order[3] == DISCRETE_OTHER_DRIVER);
}
