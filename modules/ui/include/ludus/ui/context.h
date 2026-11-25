#pragma once

#include <ludus/foundation/base/core.h>

#include <span>

namespace ludus::ui
{
using foundation::float32;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

using WidgetId = uint64; // Nonzero, application-owned identity. Never an address or display string.
inline constexpr usize kRoot = static_cast<usize>(-1);

struct Rect final
{
    float32 X = 0;
    float32 Y = 0;
    float32 Width = 0;
    float32 Height = 0;
};
struct Color final
{
    float32 R = 0;
    float32 G = 0;
    float32 B = 0;
    float32 A = 0; ///< Straight alpha, linear RGB. Backend performs attachment conversion.
};
enum class Unit : uint8
{
    Pixels,
    Fraction
};
struct Length final
{
    float32 Value = 0;
    Unit Units = Unit::Pixels;
};
enum class Align : uint8
{
    Start,
    Center,
    End
};
enum class Flow : uint8
{
    Overlay,
    Row,
    Column
};
enum class Role : uint8
{
    Decoration,
    Button
};
struct Layout final
{
    Length Width;
    Length Height;
    float32 OffsetX = 0;
    float32 OffsetY = 0;
    Align Horizontal = Align::Start;
    Align Vertical = Align::Start;
    Flow Children = Flow::Overlay;
    float32 Padding = 0;
    float32 Gap = 0;
};
struct Element final
{
    WidgetId Id = 0;
    usize Parent = kRoot; ///< Parent's index must precede this element. Submission order is paint order.
    Layout Placement;
    Color Fill;
    Role SemanticRole = Role::Decoration;
    bool Visible = true;
    bool Enabled = true;
    bool ClipChildren = true;
    bool BlockPointer = false; ///< Decorations are input-transparent unless explicitly blocking.
};
struct Viewport final
{
    Rect Bounds; ///< Logical pixels, top-left origin, +Y down; caller has removed safe-area insets.
};
struct LayoutBox final
{
    WidgetId Id = 0;
    Rect Bounds;
    Rect Clip;
    Role SemanticRole = Role::Decoration;
    bool Visible = false;
    bool Enabled = false;
};
struct PaintCommand final
{
    WidgetId Id = 0;
    Rect Bounds; ///< Already intersected with Clip. Solid rectangles only in U0.
    Color Fill;
    /// Effective ancestor and viewport clip in logical pixels, identical to the clip hit testing uses.
    /// Render adapters derive the command's scissor from it; Bounds never extends outside it.
    Rect Clip;
};
enum class Status : uint8
{
    Ok,
    InvalidState,
    InvalidDescription,
    DuplicateId,
    CapacityExceeded,
    OutOfMemory
};
enum class EventKind : uint8
{
    PointerDown,
    PointerUp,
    PointerCancel,
    FocusNext,
    FocusPrevious,
    Activate,
    FocusLost
};
struct Event final
{
    EventKind Kind = EventKind::PointerCancel;
    float32 X = 0;
    float32 Y = 0;
    uint32 Pointer = 0;
};
struct InputResult final
{
    bool Consumed = false;
    WidgetId Activated = 0; ///< Apply game commands after Route returns; no reentrant callbacks.
    WidgetId Focused = 0;
};
struct Stats final
{
    usize Elements = 0;
    usize PaintCommands = 0;
    usize LayoutVisits = 0;
};

/// @brief One main-thread context per surface.
///
/// Initialization allocates bounded storage; document resolution/input never allocate. Documents are copied, with
/// no borrows retained. Failure preserves the last committed layout and input state. Spans expire on the next
/// successful TrySetDocument or context destruction.
class Context final
{
public:
    Context() noexcept = default;
    ~Context() noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
    [[nodiscard]] Status TryInitialize(usize capacity) noexcept;
    [[nodiscard]] Status TrySetDocument(std::span<const Element> elements, Viewport viewport) noexcept;
    [[nodiscard]] std::span<const LayoutBox> GetLayout() const noexcept;
    [[nodiscard]] std::span<const PaintCommand> GetPaintCommands() const noexcept;
    [[nodiscard]] Stats GetStats() const noexcept;
    [[nodiscard]] InputResult Route(Event event) noexcept;
    [[nodiscard]] bool TryFocus(WidgetId id) noexcept;
    void CancelInput() noexcept;

private:
    struct State;
    State* mState = nullptr;
};
} // namespace ludus::ui
