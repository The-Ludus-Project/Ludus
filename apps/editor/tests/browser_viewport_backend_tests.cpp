#include "internal/browser_viewport_backend.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using namespace ludus::editor;

TEST_CASE("the viewport-backend query selects the browser viewport policy", "[editor][browser-viewport]")
{
    CHECK(ParseBrowserViewportBackend({}) == BrowserViewportBackend::Automatic);
    CHECK(ParseBrowserViewportBackend(QStringLiteral("?project=sample")) == BrowserViewportBackend::Automatic);
    CHECK(ParseBrowserViewportBackend(QStringLiteral("?viewport-backend=webgl2")) == BrowserViewportBackend::WebGl2);
    CHECK(ParseBrowserViewportBackend(QStringLiteral("?project=x&viewport-backend=WebGPU")) ==
          BrowserViewportBackend::WebGpu);
    CHECK(ParseBrowserViewportBackend(QStringLiteral("viewport-backend=webgl2")) == BrowserViewportBackend::WebGl2);
    CHECK(ParseBrowserViewportBackend(QStringLiteral("?viewport-backend=auto")) == BrowserViewportBackend::Automatic);
    // Unknown values never force an API.
    CHECK(ParseBrowserViewportBackend(QStringLiteral("?viewport-backend=vulkan")) == BrowserViewportBackend::Automatic);
    CHECK(ParseBrowserViewportBackend(QStringLiteral("?viewport-backend=")) == BrowserViewportBackend::Automatic);
}

TEST_CASE("only an automatic WebGPU startup failure falls back to WebGL 2 once", "[editor][browser-viewport]")
{
    CHECK(GetFirstBrowserViewportApi(BrowserViewportBackend::Automatic) == BrowserViewportApi::WebGpu);
    CHECK(GetFirstBrowserViewportApi(BrowserViewportBackend::WebGpu) == BrowserViewportApi::WebGpu);
    CHECK(GetFirstBrowserViewportApi(BrowserViewportBackend::WebGl2) == BrowserViewportApi::WebGl2);

    CHECK(ShouldFallBackToWebGl2(BrowserViewportBackend::Automatic, BrowserViewportApi::WebGpu, false) == true);
    // A device that started and then failed is reported, not replaced.
    CHECK(ShouldFallBackToWebGl2(BrowserViewportBackend::Automatic, BrowserViewportApi::WebGpu, true) == false);
    // WebGL 2 is the last attempt.
    CHECK(ShouldFallBackToWebGl2(BrowserViewportBackend::Automatic, BrowserViewportApi::WebGl2, false) == false);
    // A forced API never switches.
    CHECK(ShouldFallBackToWebGl2(BrowserViewportBackend::WebGpu, BrowserViewportApi::WebGpu, false) == false);
    CHECK(ShouldFallBackToWebGl2(BrowserViewportBackend::WebGl2, BrowserViewportApi::WebGl2, false) == false);
}

TEST_CASE("browser viewport messages name each API, its reason and the next step", "[editor][browser-viewport]")
{
    const QString automatic = DescribeBrowserViewportUnavailable(BrowserViewportBackend::Automatic,
                                                                 QStringLiteral("no compatible GPU adapter"),
                                                                 QStringLiteral("not provided by this browser"));
    CHECK(automatic.contains(QStringLiteral("WebGPU (no compatible GPU adapter)")) == true);
    CHECK(automatic.contains(QStringLiteral("WebGL 2 (not provided by this browser)")) == true);
    CHECK(automatic.contains(QStringLiteral("World edits remain available")) == true);
    CHECK(automatic.contains(QStringLiteral("hardware acceleration")) == true);

    const QString forced =
        DescribeBrowserViewportUnavailable(BrowserViewportBackend::WebGl2, {}, QStringLiteral("startup failed"));
    CHECK(forced.contains(QStringLiteral("WebGL 2 (startup failed)")) == true);
    CHECK(forced.contains(QStringLiteral("viewport-backend=webgl2")) == true);
    CHECK(forced.contains(QStringLiteral("Remove viewport-backend")) == true);
    // A reason that is unknown is omitted rather than shown as empty parentheses.
    CHECK(DescribeBrowserViewportUnavailable(BrowserViewportBackend::WebGpu, {}, {}).contains(QStringLiteral("()")) ==
          false);

    CHECK(DescribeBrowserViewportApi(BrowserViewportApi::WebGpu, {}) == QStringLiteral("WebGPU"));
    CHECK(DescribeBrowserViewportApi(BrowserViewportApi::WebGl2, {}) == QStringLiteral("WebGL 2"));
    CHECK(DescribeBrowserViewportApi(BrowserViewportApi::WebGl2, QStringLiteral("no compatible GPU adapter")) ==
          QStringLiteral("WebGL 2 (WebGPU unavailable: no compatible GPU adapter)"));
}
