#include <ludus/ui/context.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <new>

// Thanks to Gero Gerber, "A Flexible User Interface Layout System for Divergent
// Environments", Game Programming Gems 8, 4.10, pp. 442-452: safe regions and
// logical dimensions inform this explicit viewport/anchor contract; no XML
// condition hierarchy or stretched virtual screen is adopted.
// Thanks to Greg Seegert, "Real-Time Input and UI in 3D Games", Game Programming
// Gems 3, 1.13, pp. 109-116: ordered events and identity independent of labels
// inform routing; platform-specific character synthesis is not adopted.
// Thanks to Hyunwoo Ki, "Optimizing a 3D UI Engine for Mobile Devices", GPU Pro 1,
// VI.4, pp. 397-411: clipping informs paint emission; translucent order is kept
// rather than globally sorting by texture. These are original implementations.
// Details and source review: docs/architecture/ui.md.
// Thanks also to Omar Cornut's "About the IMGUI paradigm" (Dear ImGui wiki),
// https://github.com/ocornut/imgui/wiki/About-the-IMGUI-paradigm, and the RmlUi
// team's "Render Interface", https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/
// interfaces/render.html: value submission with retained state and a separate
// renderer boundary inform the subsystem split, not feature or speed parity.

namespace ludus::ui
{
namespace
{
using foundation::core::Array;
using foundation::math::IsFinite;
using foundation::math::Max;
using foundation::math::Min;
constexpr usize kMaximumCapacity = 65536;

bool IsValid(Rect rect) noexcept
{
    return IsFinite(rect.X) && IsFinite(rect.Y) && IsFinite(rect.Width) && IsFinite(rect.Height) && rect.Width >= 0 &&
           rect.Height >= 0 && IsFinite(rect.X + rect.Width) && IsFinite(rect.Y + rect.Height);
}
bool IsValid(Length length) noexcept
{
    return IsFinite(length.Value) && length.Value >= 0 &&
           (length.Units == Unit::Pixels || (length.Units == Unit::Fraction && length.Value <= 1));
}
bool IsValid(Color color) noexcept
{
    const float32 channels[] = {color.R, color.G, color.B, color.A};
    for (const auto channel : channels)
    {
        if (IsFinite(channel) == false || channel < 0 || channel > 1)
        {
            return false;
        }
    }
    return true;
}
bool IsValid(const Element& element) noexcept
{
    const auto& layout = element.Placement;
    return element.Id != 0 && IsValid(layout.Width) && IsValid(layout.Height) && IsValid(element.Fill) &&
           IsFinite(layout.OffsetX) && IsFinite(layout.OffsetY) && IsFinite(layout.Padding) && layout.Padding >= 0 &&
           IsFinite(layout.Gap) && layout.Gap >= 0 && layout.Horizontal <= Align::End &&
           layout.Vertical <= Align::End && layout.Children <= Flow::Column && element.SemanticRole <= Role::Button;
}
Rect Intersect(Rect first, Rect second) noexcept
{
    const auto x = Max(first.X, second.X);
    const auto y = Max(first.Y, second.Y);
    return {x,
            y,
            Max(0.0f, Min(first.X + first.Width, second.X + second.Width) - x),
            Max(0.0f, Min(first.Y + first.Height, second.Y + second.Height) - y)};
}
bool Contains(Rect rect, const Event& event) noexcept
{
    return event.X >= rect.X && event.Y >= rect.Y && event.X < rect.X + rect.Width && event.Y < rect.Y + rect.Height;
}
float32 ResolveLength(Length length, float32 extent) noexcept
{
    return length.Units == Unit::Fraction ? length.Value * extent : length.Value;
}
float32 ResolveAlignment(Align align, float32 available) noexcept
{
    return align == Align::End ? available : (align == Align::Center ? available * 0.5f : 0.0f);
}
bool CanFocus(const LayoutBox& box) noexcept
{
    return box.Visible && box.Enabled && box.SemanticRole == Role::Button;
}
} // namespace

struct Context::State final
{
    struct Buffer final
    {
        Array<Element> Elements;
        Array<LayoutBox> Boxes;
        Array<PaintCommand> Paint;
        Array<float32> Cursors;
        usize Count = 0;
        usize PaintCount = 0;
        bool TryInitialize(usize capacity) noexcept
        {
            return Elements.TryResize(capacity) && Boxes.TryResize(capacity) && Paint.TryResize(capacity) &&
                   Cursors.TryResize(capacity);
        }
    };
    Buffer Buffers[2];
    Array<WidgetId> Ids;
    usize Capacity = 0;
    usize Current = 0;
    WidgetId Focus = 0;
    WidgetId Capture = 0;
    uint32 Pointer = 0;
    bool PointerOwned = false;

    [[nodiscard]] const LayoutBox* Find(WidgetId id) const noexcept
    {
        const auto& buffer = Buffers[Current];
        for (usize index = 0; index < buffer.Count; ++index)
        {
            if (buffer.Boxes[index].Id == id)
            {
                return &buffer.Boxes[index];
            }
        }
        return nullptr;
    }
};

Context::~Context() noexcept
{
    delete mState;
}
Status Context::TryInitialize(usize capacity) noexcept
{
    if (mState != nullptr)
    {
        return Status::InvalidState;
    }
    if (capacity == 0 || capacity > kMaximumCapacity)
    {
        return Status::InvalidDescription;
    }
    auto* state = new (std::nothrow) State;
    if (state == nullptr)
    {
        return Status::OutOfMemory;
    }
    usize tableSize = 1;
    while (tableSize < capacity * 2)
    {
        tableSize *= 2;
    }
    if (state->Buffers[0].TryInitialize(capacity) == false || state->Buffers[1].TryInitialize(capacity) == false ||
        state->Ids.TryResize(tableSize) == false)
    {
        delete state;
        return Status::OutOfMemory;
    }
    state->Capacity = capacity;
    mState = state;
    return Status::Ok;
}
Status Context::TrySetDocument(std::span<const Element> elements, Viewport viewport) noexcept
{
    if (mState == nullptr)
    {
        return Status::InvalidState;
    }
    if (elements.size() > mState->Capacity)
    {
        return Status::CapacityExceeded;
    }
    if (IsValid(viewport.Bounds) == false)
    {
        return Status::InvalidDescription;
    }
    auto& buffer = mState->Buffers[1 - mState->Current];
    for (auto& id : mState->Ids)
    {
        id = 0;
    }
    buffer.PaintCount = 0;
    for (usize index = 0; index < elements.size(); ++index)
    {
        const auto& element = elements[index];
        if (IsValid(element) == false || (element.Parent != kRoot && element.Parent >= index))
        {
            return Status::InvalidDescription;
        }
        // Multiplicative hash, bounded open addressing. IDs may be arbitrary values.
        usize slot = static_cast<usize>((element.Id ^ (element.Id >> 32)) * 11400714819323198485ULL) &
                     (mState->Ids.GetSize() - 1);
        while (mState->Ids[slot] != 0 && mState->Ids[slot] != element.Id)
        {
            slot = (slot + 1) & (mState->Ids.GetSize() - 1);
        }
        if (mState->Ids[slot] != 0)
        {
            return Status::DuplicateId;
        }
        mState->Ids[slot] = element.Id;
        buffer.Elements[index] = element;
        buffer.Cursors[index] = 0;
        Rect parent = viewport.Bounds;
        Rect clip = viewport.Bounds;
        bool visible = element.Visible;
        bool enabled = element.Enabled;
        Flow flow = Flow::Overlay;
        if (element.Parent != kRoot)
        {
            const auto& parentBox = buffer.Boxes[element.Parent];
            const auto& parentElement = buffer.Elements[element.Parent];
            parent = parentBox.Bounds;
            const auto padding = parentElement.Placement.Padding;
            parent.X += padding;
            parent.Y += padding;
            parent.Width = Max(0.0f, parent.Width - 2 * padding);
            parent.Height = Max(0.0f, parent.Height - 2 * padding);
            clip = parentElement.ClipChildren ? Intersect(parentBox.Clip, parentBox.Bounds) : parentBox.Clip;
            // Visibility propagates from the authored parent, not its clipped geometry:
            // unclipped children may remain visible outside an offscreen parent.
            visible = visible && parentElement.Visible;
            enabled = enabled && parentBox.Enabled;
            flow = parentElement.Placement.Children;
        }
        const auto& layout = element.Placement;
        Rect bounds;
        bounds.Width = ResolveLength(layout.Width, parent.Width);
        bounds.Height = ResolveLength(layout.Height, parent.Height);
        bounds.X = parent.X + layout.OffsetX + ResolveAlignment(layout.Horizontal, parent.Width - bounds.Width);
        bounds.Y = parent.Y + layout.OffsetY + ResolveAlignment(layout.Vertical, parent.Height - bounds.Height);
        if (element.Parent != kRoot && visible && flow != Flow::Overlay)
        {
            auto& cursor = buffer.Cursors[element.Parent];
            if (flow == Flow::Row)
            {
                bounds.X = parent.X + layout.OffsetX + cursor;
            }
            else
            {
                bounds.Y = parent.Y + layout.OffsetY + cursor;
            }
            cursor +=
                (flow == Flow::Row ? bounds.Width : bounds.Height) + buffer.Elements[element.Parent].Placement.Gap;
            if (IsFinite(cursor) == false)
            {
                return Status::InvalidDescription;
            }
        }
        if (IsValid(parent) == false || IsValid(bounds) == false)
        {
            return Status::InvalidDescription;
        }
        const auto painted = Intersect(bounds, clip);
        // Store inherited authored visibility for the next descendant.
        buffer.Elements[index].Visible = visible;
        visible = visible && painted.Width > 0 && painted.Height > 0;
        buffer.Boxes[index] = {element.Id, bounds, clip, element.SemanticRole, visible, enabled};
        if (visible && element.Fill.A > 0)
        {
            buffer.Paint[buffer.PaintCount++] = {element.Id, painted, element.Fill};
        }
    }
    buffer.Count = elements.size();
    mState->Current = 1 - mState->Current;
    const auto* focused = mState->Find(mState->Focus);
    if (focused == nullptr || CanFocus(*focused) == false)
    {
        mState->Focus = 0;
    }
    const auto* captured = mState->Find(mState->Capture);
    if (captured == nullptr || CanFocus(*captured) == false)
    {
        mState->Capture = 0;
    }
    // Keep ownership until release/cancel even if a captured element disappears.
    return Status::Ok;
}
std::span<const LayoutBox> Context::GetLayout() const noexcept
{
    if (mState == nullptr)
    {
        return {};
    }
    const auto& buffer = mState->Buffers[mState->Current];
    return {buffer.Boxes.GetData(), buffer.Count};
}
std::span<const PaintCommand> Context::GetPaintCommands() const noexcept
{
    if (mState == nullptr)
    {
        return {};
    }
    const auto& buffer = mState->Buffers[mState->Current];
    return {buffer.Paint.GetData(), buffer.PaintCount};
}
Stats Context::GetStats() const noexcept
{
    return {GetLayout().size(), GetPaintCommands().size(), GetLayout().size()};
}
bool Context::TryFocus(WidgetId id) noexcept
{
    if (mState == nullptr)
    {
        return false;
    }
    if (id == 0)
    {
        mState->Focus = 0;
        return true;
    }
    const auto* box = mState->Find(id);
    if (box == nullptr || CanFocus(*box) == false)
    {
        return false;
    }
    mState->Focus = id;
    return true;
}
void Context::CancelInput() noexcept
{
    if (mState == nullptr)
    {
        return;
    }
    mState->Capture = 0;
    mState->PointerOwned = false;
    mState->Focus = 0;
}
InputResult Context::Route(Event event) noexcept
{
    if (mState == nullptr)
    {
        return {};
    }
    InputResult result;
    if (event.Kind == EventKind::FocusLost)
    {
        CancelInput();
    }
    else if (event.Kind == EventKind::PointerCancel)
    {
        if (mState->PointerOwned && event.Pointer == mState->Pointer)
        {
            result.Consumed = true;
            mState->Capture = 0;
            mState->PointerOwned = false;
        }
    }
    else if ((event.Kind == EventKind::PointerDown || event.Kind == EventKind::PointerUp) && IsFinite(event.X) &&
             IsFinite(event.Y))
    {
        const auto& buffer = mState->Buffers[mState->Current];
        if (event.Kind == EventKind::PointerDown && mState->PointerOwned == false)
        {
            for (usize index = buffer.Count; index > 0; --index)
            {
                const auto& box = buffer.Boxes[index - 1];
                const auto& element = buffer.Elements[index - 1];
                if (box.Visible && Contains(Intersect(box.Bounds, box.Clip), event) &&
                    (element.BlockPointer || element.SemanticRole == Role::Button))
                {
                    result.Consumed = true;
                    mState->PointerOwned = true;
                    mState->Pointer = event.Pointer;
                    mState->Capture = CanFocus(box) ? box.Id : 0;
                    if (CanFocus(box))
                    {
                        mState->Focus = box.Id;
                    }
                    break;
                }
            }
        }
        else if (mState->PointerOwned && event.Pointer == mState->Pointer)
        {
            result.Consumed = true;
            if (event.Kind == EventKind::PointerUp)
            {
                const auto* box = mState->Find(mState->Capture);
                if (box != nullptr && CanFocus(*box) && Contains(Intersect(box->Bounds, box->Clip), event))
                {
                    result.Activated = box->Id;
                }
                mState->PointerOwned = false;
                mState->Capture = 0;
            }
        }
    }
    else if (event.Kind == EventKind::FocusNext || event.Kind == EventKind::FocusPrevious)
    {
        const auto boxes = GetLayout();
        usize current = event.Kind == EventKind::FocusNext ? boxes.size() - 1 : 0;
        for (usize index = 0; index < boxes.size(); ++index)
        {
            if (boxes[index].Id == mState->Focus)
            {
                current = index;
                break;
            }
        }
        for (usize step = 0; step < boxes.size(); ++step)
        {
            current = event.Kind == EventKind::FocusNext ? (current + 1) % boxes.size()
                                                         : (current + boxes.size() - 1) % boxes.size();
            if (CanFocus(boxes[current]))
            {
                mState->Focus = boxes[current].Id;
                result.Consumed = true;
                break;
            }
        }
    }
    else if (event.Kind == EventKind::Activate)
    {
        const auto* box = mState->Find(mState->Focus);
        if (box != nullptr && CanFocus(*box))
        {
            result.Consumed = true;
            result.Activated = box->Id;
        }
    }
    result.Focused = mState->Focus;
    return result;
}
} // namespace ludus::ui
