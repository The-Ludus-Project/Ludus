#pragma once

#include <ludus/foundation/base/types.h>

#include <QString>

namespace ludus::editor
{
/// Graphics API the browser editor's viewport canvas uses.
enum class BrowserViewportApi : foundation::uint8
{
    WebGpu, ///< Preferred: the complete portable raster profile.
    WebGl2, ///< Fallback for browsers or devices where WebGPU cannot start.
};
/// Viewport API policy requested by the page; Automatic unless `viewport-backend` names one.
enum class BrowserViewportBackend : foundation::uint8
{
    Automatic, ///< Start WebGPU; retry once on WebGL 2 if WebGPU cannot start.
    WebGpu,    ///< WebGPU only; a startup failure is reported, never concealed.
    WebGl2,    ///< WebGL 2 only, for diagnosis and for browsers whose WebGPU misbehaves.
};

/// Read `viewport-backend=auto|webgpu|webgl2` from a page query such as `?project=x&viewport-backend=webgl2`.
/// Absent, empty or unknown values select Automatic. Matching is case-insensitive.
[[nodiscard]] BrowserViewportBackend ParseBrowserViewportBackend(const QString& query);
/// The API of the first startup attempt: WebGL 2 when forced, otherwise WebGPU.
[[nodiscard]] BrowserViewportApi GetFirstBrowserViewportApi(BrowserViewportBackend requested) noexcept;
/// Whether a failure of the attempted API is retried on WebGL 2. Only an Automatic request retries,
/// only once (after WebGPU), and only while the device has never reached Ready: a device that
/// started and then failed, or a forced API, reports its error instead.
[[nodiscard]] bool
ShouldFallBackToWebGl2(BrowserViewportBackend requested, BrowserViewportApi attempted, bool hasStarted) noexcept;
/// Display name of an API, for example "WebGL 2".
[[nodiscard]] QString GetBrowserViewportApiName(BrowserViewportApi api);
/// Final viewport message when no attempted API could start. Names each attempted API with its reason
/// and the next step; reasons may be empty when unknown. World editing remains available.
[[nodiscard]] QString DescribeBrowserViewportUnavailable(BrowserViewportBackend requested,
                                                         const QString& webGpuReason,
                                                         const QString& webGl2Reason);
/// Short note naming the active API, with the WebGPU reason when the viewport fell back to WebGL 2.
[[nodiscard]] QString DescribeBrowserViewportApi(BrowserViewportApi active, const QString& webGpuReason);
} // namespace ludus::editor
