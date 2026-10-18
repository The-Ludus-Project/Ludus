// Thanks to Robert Bridson, Fluid Simulation for Computer Graphics, second
// edition (2015), chapter 3 and sections 12.2-12.3, pp. 180-183, and his UBC
// "Shallow water" lecture (2005), for semi-Lagrangian tracing, staggered
// velocities and height-driven pressure:
// https://www.cs.ubc.ca/~rbridson/courses/533d-fall-2005/nov17-cs533d-slides.pdf
// Ludus assumes a flat bed; its drainage limiter, substep factor, caps and foam
// heuristic are local choices, not source recipes. Reviewed sources and the
// departure from incompressible projection: docs/architecture/fluid-field-reference-review.md.
#include <ludus/physics/fluid/field.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <new>

namespace ludus::physics::fluid
{
namespace math = foundation::math;
using foundation::core::Array;
namespace
{
using Cell = CellState;
struct GridSize final
{
    usize Width;
    usize Height;
};
// Face/scalar bilinear interpolation. Solid faces are already zeroed.
[[nodiscard]] float64 Interpolate(const float64* values, GridSize size, Point coordinate) noexcept
{
    const auto width = size.Width;
    const auto height = size.Height;
    auto x = coordinate.X;
    auto y = coordinate.Y;
    x = math::Clamp(x, 0.0, static_cast<float64>(width - 1));
    y = math::Clamp(y, 0.0, static_cast<float64>(height - 1));
    const auto ix = static_cast<usize>(x);
    const auto iy = static_cast<usize>(y);
    const auto nx = math::Min(ix + 1, width - 1);
    const auto ny = math::Min(iy + 1, height - 1);
    const float64 fx = x - static_cast<float64>(ix);
    const float64 fy = y - static_cast<float64>(iy);
    return math::Lerp(math::Lerp(values[iy * width + ix], values[iy * width + nx], fx),
                      math::Lerp(values[ny * width + ix], values[ny * width + nx], fx),
                      fy);
}
} // namespace

struct Field::Storage final
{
    Config Settings;
    usize Width = 0;
    usize Height = 0;
    usize CellCount = 0;
    usize UFaceCount = 0;
    usize VFaceCount = 0;
    Array<float64> U, V, NextU, NextV, Pressure, DrainScale;
    Array<Cell> Cells, NextCells;
    Array<uint8> Solid;
    Point HalfExtent;
    float64 Dx = 1.0;
    float64 Dy = 1.0;
    bool Active = false;

    [[nodiscard]] usize CellIndex(usize x, usize y) const noexcept
    {
        return y * Width + x;
    }
    [[nodiscard]] bool Fluid(usize x, usize y) const noexcept
    {
        return x < Width && y < Height && Solid[CellIndex(x, y)] == 0;
    }
    [[nodiscard]] bool OpenU(usize x, usize y) const noexcept
    {
        return x > 0 && x < Width && Fluid(x - 1, y) && Fluid(x, y);
    }
    [[nodiscard]] bool OpenV(usize x, usize y) const noexcept
    {
        return y > 0 && y < Height && Fluid(x, y - 1) && Fluid(x, y);
    }
    [[nodiscard]] bool Wet(Point p) const noexcept
    {
        if (math::IsFinite(p.X) == false || math::IsFinite(p.Y) == false || math::Abs(p.X) >= HalfExtent.X ||
            math::Abs(p.Y) >= HalfExtent.Y)
        {
            return false;
        }
        return Fluid(static_cast<usize>((p.X + HalfExtent.X) / Dx), static_cast<usize>((p.Y + HalfExtent.Y) / Dy));
    }
    [[nodiscard]] bool ClearSegment(Point start, Point end) const noexcept
    {
        if (Wet(start) == false || Wet(end) == false)
        {
            return false;
        }
        const Point a{(start.X + HalfExtent.X) / Dx, (start.Y + HalfExtent.Y) / Dy};
        const Point b{(end.X + HalfExtent.X) / Dx, (end.Y + HalfExtent.Y) / Dy};
        auto x = static_cast<foundation::isize>(a.X);
        auto y = static_cast<foundation::isize>(a.Y);
        const auto endX = static_cast<foundation::isize>(b.X);
        const auto endY = static_cast<foundation::isize>(b.Y);
        const Point delta{b.X - a.X, b.Y - a.Y};
        const foundation::isize sx = delta.X < 0.0 ? -1 : 1;
        const foundation::isize sy = delta.Y < 0.0 ? -1 : 1;
        const auto fluid = [&](foundation::isize cx, foundation::isize cy) noexcept {
            return cx >= 0 && cy >= 0 && Fluid(static_cast<usize>(cx), static_cast<usize>(cy));
        };
        // Exact cell traversal, including both neighbors at a corner crossing.
        // Endpoint sampling alone can miss arbitrarily short solid intersections.
        while (x != endX || y != endY)
        {
            const float64 tx = x == endX ? 2.0 : (static_cast<float64>(x + (sx > 0 ? 1 : 0)) - a.X) / delta.X;
            const float64 ty = y == endY ? 2.0 : (static_cast<float64>(y + (sy > 0 ? 1 : 0)) - a.Y) / delta.Y;
            if (math::Abs(tx - ty) <= 1e-12)
            {
                if (fluid(x + sx, y) == false || fluid(x, y + sy) == false)
                {
                    return false;
                }
                x += sx;
                y += sy;
            }
            else if (tx < ty)
            {
                x += sx;
            }
            else
            {
                y += sy;
            }
            if (fluid(x, y) == false)
            {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] Point Velocity(Point p) const noexcept
    {
        const float64 x = (p.X + HalfExtent.X) / Dx;
        const float64 y = (p.Y + HalfExtent.Y) / Dy;
        return {Interpolate(U.GetData(), {Width + 1, Height}, {x, y - 0.5}),
                Interpolate(V.GetData(), {Width, Height + 1}, {x - 0.5, y})};
    }
    [[nodiscard]] Cell Material(Point p) const noexcept
    {
        const float64 x = math::Clamp((p.X + HalfExtent.X) / Dx - 0.5, 0.0, static_cast<float64>(Width - 1));
        const float64 y = math::Clamp((p.Y + HalfExtent.Y) / Dy - 0.5, 0.0, static_cast<float64>(Height - 1));
        const auto ix = static_cast<usize>(x);
        const auto iy = static_cast<usize>(y);
        const auto nx = math::Min(ix + 1, Width - 1);
        const auto ny = math::Min(iy + 1, Height - 1);
        const float64 fx = x - static_cast<float64>(ix), fy = y - static_cast<float64>(iy);
        // Renormalize fluid weights so rock values cannot leak into water.
        Cell result;
        float64 total = 0.0;
        for (usize j = 0; j < 2; ++j)
        {
            for (usize i = 0; i < 2; ++i)
            {
                const usize index = CellIndex(i == 0 ? ix : nx, j == 0 ? iy : ny);
                const float64 weight = (i == 0 ? 1.0 - fx : fx) * (j == 0 ? 1.0 - fy : fy);
                if (Solid[index] == 0)
                {
                    total += weight;
                    result.Height += Cells[index].Height * weight;
                    result.Foam += Cells[index].Foam * weight;
                    result.DisplacementX += Cells[index].DisplacementX * weight;
                    result.DisplacementY += Cells[index].DisplacementY * weight;
                }
            }
        }
        if (total > 0.0)
        {
            result.Height /= total;
            result.Foam /= total;
            result.DisplacementX /= total;
            result.DisplacementY /= total;
        }
        return result;
    }
    [[nodiscard]] Point Departure(Point p, float64 dt) const noexcept
    {
        const Point velocity = Velocity(p);
        const Point middle{p.X - velocity.X * dt * 0.5, p.Y - velocity.Y * dt * 0.5};
        const Point midVelocity = Velocity(middle);
        const Point back{p.X - midVelocity.X * dt, p.Y - midVelocity.Y * dt};
        // CFL limits traces to less than a cell; reject solid-crossing traces.
        return ClearSegment(p, middle) && ClearSegment(middle, back) ? back : p;
    }
    [[nodiscard]] float64 Divergence(usize x, usize y) const noexcept
    {
        return (U[y * (Width + 1) + x + 1] - U[y * (Width + 1) + x]) / Dx +
               (V[(y + 1) * Width + x] - V[y * Width + x]) / Dy;
    }
    [[nodiscard]] float64 Curl(Point p) const noexcept
    {
        return (Velocity({p.X + Dx * 0.5, p.Y}).Y - Velocity({p.X - Dx * 0.5, p.Y}).Y) / Dx -
               (Velocity({p.X, p.Y + Dy * 0.5}).X - Velocity({p.X, p.Y - Dy * 0.5}).X) / Dy;
    }
    void Step(float64 dt) noexcept
    {
        const float64 damping = math::Exp(-Settings.MomentumDamping * dt);
        // Semi-Lagrangian self-advection of staggered momentum, then free-surface
        // pressure. No incompressible projection: divergence must drive height.
        for (usize y = 0; y < Height; ++y)
        {
            for (usize x = 0; x <= Width; ++x)
            {
                const usize index = y * (Width + 1) + x;
                NextU[index] = 0.0;
                if (OpenU(x, y))
                {
                    const Point p{static_cast<float64>(x) * Dx - HalfExtent.X,
                                  (static_cast<float64>(y) + 0.5) * Dy - HalfExtent.Y};
                    const Point back = Departure(p, dt);
                    const float64 advected = Velocity(back).X;
                    const usize right = CellIndex(x, y), left = CellIndex(x - 1, y);
                    const float64 gradient = (Cells[right].Height - Cells[left].Height +
                                              (Pressure[right] - Pressure[left]) / Settings.Gravity) /
                                             Dx;
                    NextU[index] = math::Clamp((advected - Settings.Gravity * gradient * dt) * damping,
                                               -Settings.MaxSpeed,
                                               Settings.MaxSpeed);
                }
            }
        }
        for (usize y = 0; y <= Height; ++y)
        {
            for (usize x = 0; x < Width; ++x)
            {
                const usize index = y * Width + x;
                NextV[index] = 0.0;
                if (OpenV(x, y))
                {
                    const Point p{(static_cast<float64>(x) + 0.5) * Dx - HalfExtent.X,
                                  static_cast<float64>(y) * Dy - HalfExtent.Y};
                    const Point back = Departure(p, dt);
                    const float64 advected = Velocity(back).Y;
                    const usize top = CellIndex(x, y), bottom = CellIndex(x, y - 1);
                    const float64 gradient = (Cells[top].Height - Cells[bottom].Height +
                                              (Pressure[top] - Pressure[bottom]) / Settings.Gravity) /
                                             Dy;
                    NextV[index] = math::Clamp((advected - Settings.Gravity * gradient * dt) * damping,
                                               -Settings.MaxSpeed,
                                               Settings.MaxSpeed);
                }
            }
        }
        // Passive material and foam are transported by the old velocity, before
        // replacing faces. Displacement stores the backtraced material map.
        for (usize y = 0; y < Height; ++y)
        {
            for (usize x = 0; x < Width; ++x)
            {
                const usize index = CellIndex(x, y);
                NextCells[index] = {};
                if (Solid[index] == 0)
                {
                    const Point p{(static_cast<float64>(x) + 0.5) * Dx - HalfExtent.X,
                                  (static_cast<float64>(y) + 0.5) * Dy - HalfExtent.Y};
                    const Point back = Departure(p, dt);
                    const Cell material = Material(back);
                    auto& next = NextCells[index];
                    next.DisplacementX =
                        math::Clamp((material.DisplacementX + p.X - back.X) * math::Exp(-Settings.MaterialDecay * dt),
                                    -12.0,
                                    12.0);
                    next.DisplacementY =
                        math::Clamp((material.DisplacementY + p.Y - back.Y) * math::Exp(-Settings.MaterialDecay * dt),
                                    -12.0,
                                    12.0);
                    const float64 hx = (Cells[CellIndex(math::Min(x + 1, Width - 1), y)].Height -
                                        Cells[CellIndex(x > 0 ? x - 1 : x, y)].Height) /
                                       (2.0 * Dx);
                    const float64 hy = (Cells[CellIndex(x, math::Min(y + 1, Height - 1))].Height -
                                        Cells[CellIndex(x, y > 0 ? y - 1 : y)].Height) /
                                       (2.0 * Dy);
                    const float64 breaking = math::Max(math::Sqrt(hx * hx + hy * hy) - 0.25, 0.0) * 2.0;
                    const float64 source = math::Max(-Divergence(x, y) - 0.08, 0.0) * 0.3 + breaking;
                    next.Foam =
                        math::Clamp(material.Foam * math::Exp(-Settings.FoamDecay * dt) + source * dt, 0.0, 1.0);
                }
            }
        }
        for (usize i = 0; i < UFaceCount; ++i)
        {
            U[i] = NextU[i];
        }
        for (usize i = 0; i < VFaceCount; ++i)
        {
            V[i] = NextV[i];
        }
        for (usize y = 0; y < Height; ++y)
        {
            for (usize x = 0; x <= Width; ++x)
            {
                const usize i = y * (Width + 1) + x;
                NextU[i] =
                    OpenU(x, y) ? U[i] * (Settings.Depth + Cells[CellIndex(U[i] > 0.0 ? x - 1 : x, y)].Height) : 0.0;
            }
        }
        for (usize y = 0; y <= Height; ++y)
        {
            for (usize x = 0; x < Width; ++x)
            {
                const usize i = y * Width + x;
                NextV[i] =
                    OpenV(x, y) ? V[i] * (Settings.Depth + Cells[CellIndex(x, V[i] > 0.0 ? y - 1 : y)].Height) : 0.0;
            }
        }
        for (usize y = 0; y < Height; ++y)
        {
            for (usize x = 0; x < Width; ++x)
            {
                const usize i = CellIndex(x, y);
                const float64 outgoing =
                    (math::Max(-NextU[y * (Width + 1) + x], 0.0) + math::Max(NextU[y * (Width + 1) + x + 1], 0.0)) /
                        Dx +
                    (math::Max(-NextV[y * Width + x], 0.0) + math::Max(NextV[(y + 1) * Width + x], 0.0)) / Dy;
                const float64 available = math::Max(Settings.Depth + Cells[i].Height - Settings.MinimumDepth, 0.0);
                DrainScale[i] = outgoing > 0.0 ? math::Min(1.0, available / (dt * outgoing)) : 1.0;
            }
        }
        for (usize y = 0; y < Height; ++y)
        {
            for (usize x = 1; x < Width; ++x)
            {
                const usize i = y * (Width + 1) + x;
                NextU[i] *= DrainScale[CellIndex(NextU[i] > 0.0 ? x - 1 : x, y)];
            }
        }
        for (usize y = 1; y < Height; ++y)
        {
            for (usize x = 0; x < Width; ++x)
            {
                const usize i = y * Width + x;
                NextV[i] *= DrainScale[CellIndex(x, NextV[i] > 0.0 ? y - 1 : y)];
            }
        }
        for (usize y = 0; y < Height; ++y)
        {
            for (usize x = 0; x < Width; ++x)
            {
                const usize i = CellIndex(x, y);
                if (Solid[i] == 0)
                {
                    NextCells[i].Height =
                        Cells[i].Height - dt * ((NextU[y * (Width + 1) + x + 1] - NextU[y * (Width + 1) + x]) / Dx +
                                                (NextV[(y + 1) * Width + x] - NextV[y * Width + x]) / Dy);
                }
            }
        }
        for (usize i = 0; i < CellCount; ++i)
        {
            Cells[i] = NextCells[i];
        }
    }
};

namespace
{
[[nodiscard]] bool Valid(const Config& c) noexcept
{
    return c.Width >= 4 && c.Width <= 256 && c.Height >= 4 && c.Height <= 256 && math::IsFinite(c.HalfExtent.X) &&
           math::IsFinite(c.HalfExtent.Y) && c.HalfExtent.X >= 0.01 && c.HalfExtent.X <= 1000.0 &&
           c.HalfExtent.Y >= 0.01 && c.HalfExtent.Y <= 1000.0 && math::IsFinite(c.Depth) && c.Depth >= 0.01 &&
           c.Depth <= 1000.0 && math::IsFinite(c.Gravity) && c.Gravity > 0.0 && c.Gravity <= 1000.0 &&
           math::IsFinite(c.MaxSpeed) && c.MaxSpeed > 0.0 && c.MaxSpeed <= 1000.0 && math::IsFinite(c.MaxPressure) &&
           c.MaxPressure > 0.0 && c.MaxPressure <= 1e6 && math::IsFinite(c.MinimumDepth) && c.MinimumDepth > 0.0 &&
           c.MinimumDepth <= c.Depth && math::IsFinite(c.MomentumDamping) && c.MomentumDamping >= 0.0 &&
           c.MomentumDamping <= 1000.0 && math::IsFinite(c.FoamDecay) && c.FoamDecay >= 0.0 && c.FoamDecay <= 1000.0 &&
           math::IsFinite(c.MaterialDecay) && c.MaterialDecay >= 0.0 && c.MaterialDecay <= 1000.0 &&
           c.MaxSubsteps > 0 && c.MaxSubsteps <= 4096;
}
} // namespace

Field::~Field() noexcept
{
    delete mStorage;
}
Field::Field(Field&& other) noexcept : mStorage(other.mStorage)
{
    other.mStorage = nullptr;
}
Field& Field::operator=(Field&& other) noexcept
{
    if (this != &other)
    {
        delete mStorage;
        mStorage = other.mStorage;
        other.mStorage = nullptr;
    }
    return *this;
}
bool Field::IsReady() const noexcept
{
    return mStorage != nullptr;
}
bool Field::IsActive() const noexcept
{
    return mStorage != nullptr && mStorage->Active;
}
Config Field::GetConfig() const noexcept
{
    return mStorage != nullptr ? mStorage->Settings : Config{};
}

Status Field::TryInitialize(const Config& config) noexcept
{
    if (Valid(config) == false)
    {
        return Status::InvalidArgument;
    }
    if (mStorage == nullptr || mStorage->Width != config.Width || mStorage->Height != config.Height)
    {
        auto* candidate = new (std::nothrow) Storage;
        if (candidate == nullptr)
        {
            return Status::AllocationFailure;
        }
        candidate->Width = config.Width;
        candidate->Height = config.Height;
        candidate->CellCount = config.Width * config.Height;
        candidate->UFaceCount = (config.Width + 1) * config.Height;
        candidate->VFaceCount = config.Width * (config.Height + 1);
        if (candidate->U.TryResize(candidate->UFaceCount) == false ||
            candidate->NextU.TryResize(candidate->UFaceCount) == false ||
            candidate->V.TryResize(candidate->VFaceCount) == false ||
            candidate->NextV.TryResize(candidate->VFaceCount) == false ||
            candidate->Cells.TryResize(candidate->CellCount) == false ||
            candidate->NextCells.TryResize(candidate->CellCount) == false ||
            candidate->Solid.TryResize(candidate->CellCount) == false ||
            candidate->Pressure.TryResize(candidate->CellCount) == false ||
            candidate->DrainScale.TryResize(candidate->CellCount) == false)
        {
            delete candidate;
            return Status::AllocationFailure;
        }
        delete mStorage;
        mStorage = candidate;
    }
    auto& s = *mStorage;
    s.Settings = config;
    s.HalfExtent = config.HalfExtent;
    s.Dx = config.HalfExtent.X * 2.0 / static_cast<float64>(config.Width);
    s.Dy = config.HalfExtent.Y * 2.0 / static_cast<float64>(config.Height);
    return TryReset();
}

Status Field::TryReset(std::span<const uint8> solids, std::span<const CellState> initial) noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    auto& s = *mStorage;
    if ((solids.empty() == false && solids.size() != s.CellCount) ||
        (initial.empty() == false && initial.size() != s.CellCount))
    {
        return Status::InvalidArgument;
    }
    for (const auto& c : initial)
    {
        if (math::IsFinite(c.Height) == false || c.Height + s.Settings.Depth < s.Settings.MinimumDepth ||
            math::Abs(c.Height) > s.Settings.Depth * 16.0 || math::IsFinite(c.Foam) == false || c.Foam < 0.0 ||
            c.Foam > 1.0 || math::IsFinite(c.DisplacementX) == false || math::Abs(c.DisplacementX) > 12.0 ||
            math::IsFinite(c.DisplacementY) == false || math::Abs(c.DisplacementY) > 12.0)
        {
            return Status::InvalidArgument;
        }
    }
    s.Active = false;
    for (usize i = 0; i < s.UFaceCount; ++i)
    {
        s.U[i] = 0.0;
    }
    for (usize i = 0; i < s.VFaceCount; ++i)
    {
        s.V[i] = 0.0;
    }
    for (usize i = 0; i < s.CellCount; ++i)
    {
        s.Solid[i] = solids.empty() ? 0 : solids[i];
        s.Cells[i] = initial.empty() || s.Solid[i] != 0 ? CellState{} : initial[i];
        s.Pressure[i] = 0.0;
        const auto& c = s.Cells[i];
        s.Active |= c.Height != 0.0 || c.Foam != 0.0 || c.DisplacementX != 0.0 || c.DisplacementY != 0.0;
    }
    return Status::Success;
}

Status Field::TrySetVelocity(std::span<const float64> u, std::span<const float64> v) noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    auto& s = *mStorage;
    if (u.size() != s.UFaceCount || v.size() != s.VFaceCount)
    {
        return Status::InvalidArgument;
    }
    for (const auto values : {u, v})
    {
        for (const auto value : values)
        {
            if (math::IsFinite(value) == false || math::Abs(value) > s.Settings.MaxSpeed)
            {
                return Status::InvalidArgument;
            }
        }
    }
    for (usize y = 0; y < s.Height; ++y)
    {
        for (usize x = 0; x <= s.Width; ++x)
        {
            const usize i = y * (s.Width + 1) + x;
            s.U[i] = s.OpenU(x, y) ? u[i] : 0.0;
            s.Active |= s.U[i] != 0.0;
        }
    }
    for (usize y = 0; y <= s.Height; ++y)
    {
        for (usize x = 0; x < s.Width; ++x)
        {
            const usize i = y * s.Width + x;
            s.V[i] = s.OpenV(x, y) ? v[i] : 0.0;
            s.Active |= s.V[i] != 0.0;
        }
    }
    return Status::Success;
}

Status Field::TrySetPressure(std::span<const float64> pressure) noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    auto& s = *mStorage;
    if (pressure.empty() == false && pressure.size() != s.CellCount)
    {
        return Status::InvalidArgument;
    }
    for (const auto value : pressure)
    {
        if (math::IsFinite(value) == false || math::Abs(value) > s.Settings.MaxPressure)
        {
            return Status::InvalidArgument;
        }
    }
    for (usize i = 0; i < s.CellCount; ++i)
    {
        s.Pressure[i] = pressure.empty() || s.Solid[i] != 0 ? 0.0 : pressure[i];
        s.Active |= s.Pressure[i] != 0.0;
    }
    return Status::Success;
}

Status Field::TryAddStroke(const Stroke& stroke) noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    auto& s = *mStorage;
    if (math::IsFinite(stroke.Radius) == false || stroke.Radius < 1e-6 || stroke.Radius > 2000.0 ||
        math::IsFinite(stroke.Spacing) == false || stroke.Spacing < 1e-6 || stroke.Spacing > 2000.0 ||
        math::IsFinite(stroke.Strength) == false || math::Abs(stroke.Strength) > 1000.0 ||
        math::IsFinite(stroke.Start.X) == false || math::IsFinite(stroke.Start.Y) == false ||
        math::IsFinite(stroke.End.X) == false || math::IsFinite(stroke.End.Y) == false)
    {
        return Status::InvalidArgument;
    }
    if (math::Abs(stroke.Start.X) >= s.HalfExtent.X || math::Abs(stroke.Start.Y) >= s.HalfExtent.Y ||
        math::Abs(stroke.End.X) >= s.HalfExtent.X || math::Abs(stroke.End.Y) >= s.HalfExtent.Y)
    {
        return Status::OutsideDomain;
    }
    if (s.Wet(stroke.Start) == false || s.Wet(stroke.End) == false)
    {
        return Status::SolidCell;
    }
    const Point segment{stroke.End.X - stroke.Start.X, stroke.End.Y - stroke.Start.Y};
    const float64 distance = math::Sqrt(segment.X * segment.X + segment.Y * segment.Y);
    if (distance == 0.0)
    {
        return Status::InvalidArgument;
    }
    const float64 brushCount = math::Ceil(distance / stroke.Spacing);
    if (brushCount > 8192.0)
    {
        return Status::WorkLimit;
    }
    if (s.ClearSegment(stroke.Start, stroke.End) == false)
    {
        return Status::SolidCell;
    }
    const auto steps = static_cast<usize>(brushCount);
    const Point impulse{segment.X * stroke.Strength / static_cast<float64>(steps),
                        segment.Y * stroke.Strength / static_cast<float64>(steps)};
    for (usize i = 0; i < steps; ++i)
    {
        const float64 t = (static_cast<float64>(i) + 0.5) / static_cast<float64>(steps);
        const Point p{stroke.Start.X + segment.X * t, stroke.Start.Y + segment.Y * t};
        const auto weight = [&](float64 x, float64 y) noexcept {
            const float64 q = ((x - p.X) * (x - p.X) + (y - p.Y) * (y - p.Y)) / (stroke.Radius * stroke.Radius);
            return q < 1.0 ? (1.0 - q) * (1.0 - q) : 0.0;
        };
        const auto lower = [](float64 coordinate, float64 spacing, usize limit) noexcept {
            return static_cast<usize>(
                math::Clamp(math::Floor(coordinate / spacing) - 1.0, 0.0, static_cast<float64>(limit)));
        };
        const auto upper = [](float64 coordinate, float64 spacing, usize limit) noexcept {
            return static_cast<usize>(
                math::Clamp(math::Ceil(coordinate / spacing) + 1.0, 0.0, static_cast<float64>(limit)));
        };
        const auto x0 = lower(p.X + s.HalfExtent.X - stroke.Radius, s.Dx, s.Width);
        const auto y0 = lower(p.Y + s.HalfExtent.Y - stroke.Radius, s.Dy, s.Height);
        const auto x1 = upper(p.X + s.HalfExtent.X + stroke.Radius, s.Dx, s.Width);
        const auto y1 = upper(p.Y + s.HalfExtent.Y + stroke.Radius, s.Dy, s.Height);
        for (usize y = y0; y < y1; ++y)
        {
            for (usize x = math::Max(x0, usize{1}); x < x1; ++x)
            {
                if (s.OpenU(x, y))
                {
                    auto& u = s.U[y * (s.Width + 1) + x];
                    u = math::Clamp(u + impulse.X * weight(static_cast<float64>(x) * s.Dx - s.HalfExtent.X,
                                                           (static_cast<float64>(y) + 0.5) * s.Dy - s.HalfExtent.Y),
                                    -s.Settings.MaxSpeed,
                                    s.Settings.MaxSpeed);
                }
            }
        }
        for (usize y = math::Max(y0, usize{1}); y < y1; ++y)
        {
            for (usize x = x0; x < x1; ++x)
            {
                if (s.OpenV(x, y))
                {
                    auto& v = s.V[y * s.Width + x];
                    v = math::Clamp(v + impulse.Y * weight((static_cast<float64>(x) + 0.5) * s.Dx - s.HalfExtent.X,
                                                           static_cast<float64>(y) * s.Dy - s.HalfExtent.Y),
                                    -s.Settings.MaxSpeed,
                                    s.Settings.MaxSpeed);
                }
            }
        }
    }
    s.Active |= stroke.Strength != 0.0;
    return Status::Success;
}

Status Field::TryGetStepLimit(float64& seconds) const noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    const auto& s = *mStorage;
    float64 depth = s.Settings.Depth;
    for (usize i = 0; i < s.CellCount; ++i)
    {
        depth = math::Max(depth, s.Settings.Depth + s.Cells[i].Height);
    }
    seconds = 0.3 * math::Min(s.Dx, s.Dy) / (s.Settings.MaxSpeed + math::Sqrt(s.Settings.Gravity * depth));
    return Status::Success;
}
Status Field::TryAdvance(float64 seconds, StepInfo& info) noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    if (math::IsFinite(seconds) == false || seconds < 0.0 || seconds > 0.25)
    {
        return Status::InvalidArgument;
    }
    info = {};
    if (IsActive() == false)
    {
        info.AdvancedSeconds = seconds;
        return Status::Success;
    }
    while (info.AdvancedSeconds < seconds)
    {
        if (info.Substeps == mStorage->Settings.MaxSubsteps)
        {
            return Status::WorkLimit;
        }
        float64 limit = 0.0;
        (void)TryGetStepLimit(limit);
        const float64 remaining = seconds - info.AdvancedSeconds;
        const float64 dt = math::Min(remaining, limit);
        mStorage->Step(dt);
        ++info.Substeps;
        info.AdvancedSeconds = dt == remaining ? seconds : info.AdvancedSeconds + dt;
    }
    return Status::Success;
}

Status Field::TrySample(Point world, Sample& output) const noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    if (math::IsFinite(world.X) == false || math::IsFinite(world.Y) == false)
    {
        return Status::InvalidArgument;
    }
    const auto& s = *mStorage;
    if (math::Abs(world.X) >= s.HalfExtent.X || math::Abs(world.Y) >= s.HalfExtent.Y)
    {
        return Status::OutsideDomain;
    }
    if (s.Wet(world) == false)
    {
        return Status::SolidCell;
    }
    const auto velocity = s.Velocity(world);
    const auto cell = s.Material(world);
    const auto x = static_cast<usize>((world.X + s.HalfExtent.X) / s.Dx);
    const auto y = static_cast<usize>((world.Y + s.HalfExtent.Y) / s.Dy);
    output = {velocity.X,
              velocity.Y,
              cell.Height,
              cell.Foam,
              cell.DisplacementX,
              cell.DisplacementY,
              s.Curl(world),
              s.Divergence(x, y)};
    return Status::Success;
}
bool Field::IsFluid(usize x, usize y) const noexcept
{
    return IsReady() && mStorage->Fluid(x, y);
}
CellState Field::GetCell(usize x, usize y) const noexcept
{
    return IsFluid(x, y) ? mStorage->Cells[mStorage->CellIndex(x, y)] : CellState{};
}
Diagnostics Field::GetDiagnostics() const noexcept
{
    Diagnostics result;
    if (IsReady() == false)
    {
        return result;
    }
    const auto& s = *mStorage;
    result.MinDepth = s.Settings.Depth * 17.0 * static_cast<float64>(s.CellCount);
    for (usize y = 0; y < s.Height; ++y)
    {
        for (usize x = 0; x < s.Width; ++x)
        {
            if (s.Fluid(x, y) == false)
            {
                continue;
            }
            const Point p{(static_cast<float64>(x) + 0.5) * s.Dx - s.HalfExtent.X,
                          (static_cast<float64>(y) + 0.5) * s.Dy - s.HalfExtent.Y};
            const auto velocity = s.Velocity(p);
            const float64 height = s.Cells[s.CellIndex(x, y)].Height;
            result.Energy += (s.Settings.Depth * (velocity.X * velocity.X + velocity.Y * velocity.Y) +
                              s.Settings.Gravity * height * height) *
                             0.5 * s.Dx * s.Dy;
            result.MaxCurl = math::Max(result.MaxCurl, math::Abs(s.Curl(p)));
            result.MaxDivergence = math::Max(result.MaxDivergence, math::Abs(s.Divergence(x, y)));
            result.MaxHeight = math::Max(result.MaxHeight, math::Abs(height));
            result.IntegratedHeight += height * s.Dx * s.Dy;
            result.MinDepth = math::Min(result.MinDepth, s.Settings.Depth + height);
        }
    }
    if (result.MinDepth == s.Settings.Depth * 17.0 * static_cast<float64>(s.CellCount))
    {
        result.MinDepth = 0.0;
    }
    return result;
}
} // namespace ludus::physics::fluid
