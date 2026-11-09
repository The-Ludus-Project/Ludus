#include "internal/browser_viewport_backend.h"

#include <QUrlQuery>

namespace ludus::editor
{
BrowserViewportBackend ParseBrowserViewportBackend(const QString& query)
{
    const QString value = QUrlQuery(query.startsWith(QLatin1Char('?')) == true ? query.mid(1) : query)
                              .queryItemValue(QStringLiteral("viewport-backend"))
                              .toLower();
    return value == QStringLiteral("webgpu")   ? BrowserViewportBackend::WebGpu
           : value == QStringLiteral("webgl2") ? BrowserViewportBackend::WebGl2
                                               : BrowserViewportBackend::Automatic;
}

BrowserViewportApi GetFirstBrowserViewportApi(BrowserViewportBackend requested) noexcept
{
    return requested == BrowserViewportBackend::WebGl2 ? BrowserViewportApi::WebGl2 : BrowserViewportApi::WebGpu;
}

bool ShouldFallBackToWebGl2(BrowserViewportBackend requested, BrowserViewportApi attempted, bool hasStarted) noexcept
{
    return requested == BrowserViewportBackend::Automatic && attempted == BrowserViewportApi::WebGpu &&
           hasStarted == false;
}

QString GetBrowserViewportApiName(BrowserViewportApi api)
{
    return api == BrowserViewportApi::WebGl2 ? QStringLiteral("WebGL 2") : QStringLiteral("WebGPU");
}

QString DescribeBrowserViewportUnavailable(BrowserViewportBackend requested,
                                           const QString& webGpuReason,
                                           const QString& webGl2Reason)
{
    const auto describe = [](BrowserViewportApi api, const QString& reason) {
        return reason.isEmpty() == true ? GetBrowserViewportApiName(api)
                                        : QStringLiteral("%1 (%2)").arg(GetBrowserViewportApiName(api), reason);
    };
    if (requested == BrowserViewportBackend::Automatic)
    {
        return QStringLiteral("The 3D viewport is unavailable: neither %1 nor %2 could start in this browser. World "
                              "edits remain available. Turn on hardware acceleration in the browser's settings, use "
                              "a browser with WebGPU or WebGL 2, or use the desktop editor.")
            .arg(describe(BrowserViewportApi::WebGpu, webGpuReason),
                 describe(BrowserViewportApi::WebGl2, webGl2Reason));
    }
    const BrowserViewportApi forced = GetFirstBrowserViewportApi(requested);
    return QStringLiteral("The 3D viewport is unavailable: %1 could not start, and the page address forces it with "
                          "viewport-backend=%2. World edits remain available. Remove viewport-backend from the "
                          "address to let the editor choose WebGPU or WebGL 2.")
        .arg(describe(forced, forced == BrowserViewportApi::WebGl2 ? webGl2Reason : webGpuReason),
             forced == BrowserViewportApi::WebGl2 ? QStringLiteral("webgl2") : QStringLiteral("webgpu"));
}

QString DescribeBrowserViewportApi(BrowserViewportApi active, const QString& webGpuReason)
{
    if (active == BrowserViewportApi::WebGpu || webGpuReason.isEmpty() == true)
    {
        return GetBrowserViewportApiName(active);
    }
    return QStringLiteral("WebGL 2 (WebGPU unavailable: %1)").arg(webGpuReason);
}
} // namespace ludus::editor
