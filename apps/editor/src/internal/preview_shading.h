#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// Editor visualization mode; does not modify authored materials or lights.
enum class PreviewShading : foundation::uint8
{
    Solid,
    Albedo,
    Lit,
    /// Triangle edges over a dim fill, independent of materials and lighting.
    Wireframe,
};
} // namespace ludus::editor
