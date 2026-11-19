#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::input
{
/// Signed relative pointer movement in native motion units, independent of cursor position and backing scale.
struct RelativePointerMotion final
{
    /// Horizontal motion, positive to the right.
    foundation::float32 Horizontal = 0;
    /// Vertical motion, positive downward.
    foundation::float32 Vertical = 0;
};
} // namespace ludus::input
