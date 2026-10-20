#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::editor
{
/// Source version captured by a validation or build job.
enum class RenderGraphSource : foundation::uint8
{
    Draft, ///< Current undoable edits, including unsaved files.
    Saved, ///< Last loaded or saved bytes; excludes unsaved additions/removals.
};
} // namespace ludus::editor
