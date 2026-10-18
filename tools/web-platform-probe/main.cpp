#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/platform/base/window.h>
#include <ludus/platform/browser/window.h>
#include <ludus/platform/entropy.h>
#include <ludus/platform/native_window.h>

#include <string_view>

#include <emscripten.h>

namespace
{
using namespace ludus::foundation;
using namespace ludus::platform;
UniquePtr<Window> gWindow;
WindowManager gManager;
uint64 gEvents = 0;
uint64 gKeyDownEvents = 0;

bool Verify(bool condition, std::string_view message) noexcept
{
    if (condition == false)
    {
        LUDUS_LOG_WARN(logging::LOG_TEMP, "W3 failure: {}", message);
    }
    return condition;
}

// Deterministic tests use ordinary DOM events on the real canvas in a browser,
// and a small explicit DOM fixture in Node. No private engine state is patched.
// clang-format off
EM_JS(void, Scenario, (int32 step), {
    const canvas = document.querySelector('#canvas');
    const key = (code, down) => {
        const event = new KeyboardEvent(down ? 'keydown' : 'keyup', {code, bubbles: true, cancelable: true});
        canvas.dispatchEvent(event);
        Module.lastPrevented = event.defaultPrevented;
    };
    if (step == 0) { canvas.focus(); key('KeyW', true); }
    if (step == 1) { document.querySelector('#outside').focus(); }
    if (step == 2) { canvas.style.width = '300px'; canvas.style.height = '150px'; }
    if (step == 3) { canvas.style.display = 'none'; }
    if (step == 4) { canvas.style.display = ""; canvas.focus(); key('Tab', true); }
    if (step == 5) {
        for (let i=0; i<160; ++i) key('KeyA', true);
        key('KeyA', false);
    }
    if (step == 6) {
        const event = new WheelEvent('wheel', {deltaX: 2, deltaY: 3, deltaMode: 1, cancelable: true});
        canvas.dispatchEvent(event);
    }
    if (step == 7) {
        canvas.dispatchEvent(new PointerEvent('pointerdown', {pointerId: 1, isPrimary: true,
            buttons: 1, clientX: 25, clientY: 30, cancelable: true}));
    }
    if (step == 8) { key('KeyW', true); canvas.dispatchEvent(new PointerEvent('pointercancel')); }
    if (step == 9) { key('KeyB', true); window.dispatchEvent(new Event('blur')); }
    if (step == 10) { window.dispatchEvent(new Event('focus')); }
    if (step == 11) { canvas.remove(); }
});
EM_JS(int32, WasPrevented, (), { return Module.lastPrevented ? 1 : 0; });
// Replace Web Crypto with a throwing stub (or restore it) to exercise the entropy failure path.
EM_JS(void, SetWebCryptoBroken, (int32 isBroken), {
    if (isBroken) {
        Module.savedCrypto = Object.getOwnPropertyDescriptor(globalThis, 'crypto');
        Object.defineProperty(globalThis, 'crypto', {configurable: true, writable: true,
            value: {getRandomValues: () => { throw new Error('entropy unavailable'); }}});
        return;
    }
    if (Module.savedCrypto) Object.defineProperty(globalThis, 'crypto', Module.savedCrypto);
    else delete globalThis.crypto;
});
EM_JS(void, DrawState,
      (uint32 width, uint32 height, float64 cssWidth, float64 cssHeight, float64 ratio,
       int32 focused, int32 visible, uint32 buttons, uint32 held, uint32 events, uint32 keys,
       float64 x, float64 y), {
    const status = document.getElementById('status');
    status.textContent = 'CSS ' + cssWidth + ' × ' + cssHeight + ' | framebuffer ' + width + ' × ' + height +
        ' | DPR ' + ratio + ' | focused ' + !!focused + ' | visible ' + !!visible +
        ' | held keys ' + held + ' | buttons ' + buttons + ' | events ' + events + ' | keydowns ' + keys +
        ' | pointer ' + Math.round(x) + ', ' + Math.round(y);
    const canvas = document.querySelector('#canvas');
    if (!canvas) return;
    // W3 tests DOM/input ownership, not rendering. Keep the canvas context
    // unclaimed for the later WebGPU renderer; diagnostics use DOM/CSS only.
    canvas.style.backgroundColor = focused ? '#25496a' : '#292b36';
    const pointer = document.getElementById('pointer');
    pointer.hidden = !visible || x < 0 || y < 0 || x >= cssWidth || y >= cssHeight;
    pointer.style.transform = 'translate(' + (x - 5) + 'px, ' + (y - 5) + 'px)';
});
EM_JS(void, ReportResult, (int32 passed), {
    if (typeof document != 'undefined') {
        const result = document.getElementById('result');
        if (result) result.textContent = passed ? (Module.arguments.includes('automated') ? 'W3 lifecycle checks passed' : 'W3 backend ready') : 'W3 checks failed';
    }
});
// clang-format on

void Drain() noexcept
{
    browser::InputEvent event;
    while (gWindow->PollBrowserInput(event))
    {
        ++gEvents;
        if (event.Kind == browser::EventKind::KeyDown)
        {
            ++gKeyDownEvents;
        }
    }
}

void Frame() noexcept
{
    if (gWindow.Get() == nullptr)
    {
        return;
    }
    (void)gWindow->HandleEvent({});
    Drain();
    browser::WindowState state;
    (void)gWindow->GetBrowserState(state);
    uint32 held = 0;
    for (bool key : state.Keys)
    {
        if (key)
        {
            ++held;
        }
    }
    DrawState(state.FramebufferWidth,
              state.FramebufferHeight,
              state.CssWidth,
              state.CssHeight,
              state.PixelRatio,
              state.Focused ? 1 : 0,
              state.Visible ? 1 : 0,
              state.Buttons,
              held,
              static_cast<uint32>(gEvents),
              static_cast<uint32>(gKeyDownEvents),
              state.PointerX,
              state.PointerY);
}

// The browser entropy adapter must satisfy the native contract: every request
// either fills all bytes from crypto.getRandomValues or reports failure.
bool VerifyEntropy() noexcept
{
    constexpr usize UUID_COUNT = 8;
    // Spans several 256-byte entropy requests and ends in a partial one.
    constexpr usize LARGE_REQUEST_BYTES = 1000;
    // 2048 zero bits at the tail occur by chance with negligible probability.
    constexpr usize TAIL_CHECK_BYTES = 256;
    const identity::EntropySource source = GetSystemEntropySource();
    StaticArray<identity::Uuid, UUID_COUNT> values{};
    for (usize i = 0; i < UUID_COUNT; ++i)
    {
        identity::Uuid value;
        if (identity::TryGenerateUuidV4(source, value) != identity::UuidStatus::Success ||
            identity::ValidateUuid(value, identity::UuidPolicy::RequireVersion4) != identity::UuidStatus::Success)
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (values[j] == value)
            {
                return false;
            }
        }
        values[i] = value;
    }
    // A throwing Web Crypto must be reported as unavailable, not raised through wasm.
    const identity::Uuid preserved = values[0];
    identity::Uuid unchanged = preserved;
    SetWebCryptoBroken(1);
    const identity::UuidStatus brokenStatus = identity::TryGenerateUuidV4(source, unchanged);
    SetWebCryptoBroken(0);
    if (brokenStatus != identity::UuidStatus::EntropyUnavailable || (unchanged == preserved) == false)
    {
        return false;
    }
    StaticArray<uint8, LARGE_REQUEST_BYTES> bytes{};
    if (TryFillSystemEntropy(bytes.GetData(), bytes.GetSize()) == false || TryFillSystemEntropy(nullptr, 1) == true)
    {
        return false;
    }
    for (usize i = LARGE_REQUEST_BYTES - TAIL_CHECK_BYTES; i < LARGE_REQUEST_BYTES; ++i)
    {
        if (bytes[i] != 0)
        {
            return true;
        }
    }
    return false;
}

bool Create() noexcept
{
    return gManager.CreateWindow({}, gWindow);
}

bool Automated() noexcept
{
    if (Verify(VerifyEntropy(), "entropy") == false)
    {
        return false;
    }
    if (Verify(Create(), "create") == false)
    {
        return false;
    }
    browser::WindowState state;
    (void)gWindow->GetBrowserState(state);
    const auto native = gWindow->GetNativeWindowInfo();
    if (Verify(native.System == WindowSystem::WebCanvas && native.CanvasSelector != nullptr && state.Attached &&
                   state.Visible && state.FramebufferWidth == static_cast<uint32>(state.CssWidth * state.PixelRatio) &&
                   state.FramebufferHeight == static_cast<uint32>(state.CssHeight * state.PixelRatio),
               "canvas descriptor") == false)
    {
        return false;
    }
    UniquePtr<Window> duplicate;
    if (Verify(gManager.CreateWindow({}, duplicate) == false, "exclusive canvas ownership") == false)
    {
        return false;
    }
    Drain();
    Scenario(0);
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Keys[static_cast<usize>(browser::Key::KeyW)] && state.Focused && WasPrevented(), "keydown") ==
        false)
    {
        return false;
    }
    Scenario(1);
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Keys[static_cast<usize>(browser::Key::KeyW)] == false && state.Focused == false,
               "blur clears held keys") == false)
    {
        return false;
    }
    Scenario(2);
    (void)gWindow->HandleEvent({});
    if (Verify(gWindow->SetBrowserFramebufferLimit(128), "limit update") == false)
    {
        return false;
    }
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.CssWidth == 300 && state.CssHeight == 150 && state.FramebufferWidth <= 128 &&
                   state.FramebufferHeight <= 128 && gWindow->SetBrowserFramebufferLimit(0) == false,
               "CSS/pixel size limits") == false)
    {
        return false;
    }
    Scenario(3);
    (void)gWindow->HandleEvent({});
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Visible == false && state.FramebufferWidth == 0 && state.FramebufferHeight == 0,
               "hidden canvas") == false)
    {
        return false;
    }
    Scenario(4);
    (void)gWindow->HandleEvent({});
    if (Verify(WasPrevented() == false, "Tab navigation") == false)
    {
        return false;
    }
    Drain();
    Scenario(5);
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.DroppedEvents > 0 && state.Keys[static_cast<usize>(browser::Key::KeyA)] == false,
               "overflow held state") == false)
    {
        return false;
    }
    Drain();
    Scenario(6);
    browser::InputEvent event;
    if (Verify(gWindow->PollBrowserInput(event) && event.Kind == browser::EventKind::Wheel && event.WheelX == 32 &&
                   event.WheelY == 48,
               "wheel normalization") == false)
    {
        return false;
    }
    Scenario(7);
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Buttons == 1 && state.PointerX >= 0, "pointer") == false)
    {
        return false;
    }
    Scenario(8);
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Buttons == 0 && state.Keys[static_cast<usize>(browser::Key::KeyW)],
               "pointer cancel preserves keyboard") == false)
    {
        return false;
    }
    Scenario(9);
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Focused == false && state.Keys[static_cast<usize>(browser::Key::KeyB)] == false, "window blur") ==
        false)
    {
        return false;
    }
    Scenario(10);
    (void)gWindow->HandleEvent({});
    gWindow.Reset();
    if (Verify(Create(), "restart") == false)
    {
        return false;
    }
    Drain();
    Scenario(0);
    usize keyEvents = 0;
    while (gWindow->PollBrowserInput(event))
    {
        if (event.Kind == browser::EventKind::KeyDown)
        {
            ++keyEvents;
        }
    }
    if (Verify(keyEvents == 1, "no duplicate callbacks after restart") == false)
    {
        return false;
    }
    Scenario(11);
    if (Verify(gWindow->HandleEvent({}) == false, "removed canvas") == false)
    {
        return false;
    }
    (void)gWindow->GetBrowserState(state);
    if (Verify(state.Attached == false && state.Visible == false &&
                   state.Keys[static_cast<usize>(browser::Key::KeyW)] == false,
               "removed state") == false)
    {
        return false;
    }
    gWindow.Reset();
    LUDUS_LOG_WARN(logging::LOG_TEMP, "[W3:passed]");
    return true;
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void W3Restart()
{
    gWindow.Reset();
    gKeyDownEvents = 0;
    gEvents = 0;
    ReportResult(Create() ? 1 : 0);
}

int main(int argc, char** argv)
{
    if (gManager.Initialize({}) == false)
    {
        return 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "automated")
    {
        const bool passed = Automated();
        ReportResult(passed ? 1 : 0);
        return passed ? 0 : 1;
    }
    if (Create() == false)
    {
        return 1;
    }
    // Repeated creation/destruction is checked without blocking the event loop.
    gWindow.Reset();
    if (Create() == false)
    {
        return 1;
    }
    ReportResult(1);
    emscripten_set_main_loop(Frame, 0, true);
    return 0;
}
