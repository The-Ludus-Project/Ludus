#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::content
{
/// Inclusive encoded JSON byte ceiling for Content definitions and audio catalogs.
inline constexpr foundation::usize MAX_DOCUMENT_BYTES = foundation::usize{1024} * 1024;
/// Inclusive number of records in a Content catalog.
inline constexpr foundation::usize MAX_RESOURCES = 4096;
/// Inclusive byte length of a Content filesystem root path.
inline constexpr foundation::usize MAX_ROOT_BYTES = 4096;
/// Inclusive byte ceiling for SaveFile inputs and existing files checked for replacement.
/// This persistence budget is independent of JSON document and scene payload admission.
inline constexpr foundation::usize MAX_SAVE_BYTES = foundation::usize{32} * 1024 * 1024;
} // namespace ludus::content
