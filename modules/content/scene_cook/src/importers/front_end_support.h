#pragma once

// Small helpers shared by the glTF and OBJ front-ends: cancellation polling,
// bounded diagnostic text, source-file bookkeeping and material emission.

#include "import_diagnostics.h"
#include "scene_ir.h"

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/containers/static_array.hpp>

#include <string_view>

namespace ludus::content::scene_cook::importers
{
/// True when the optional hook asks the import to stop.
[[nodiscard]] bool IsCancelled(const Cancellation* cancelOrNull) noexcept;

/// Rec. 709 luminance of linear RGB.
[[nodiscard]] float64 ComputeRec709Luminance(const float64 (&rgb)[3]) noexcept;

/// Fixed-capacity text for diagnostic messages; overlong text is truncated.
class MessageBuilder final
{
public:
    MessageBuilder& Append(std::string_view text) noexcept;
    MessageBuilder& AppendUnsigned(foundation::core::uint64 value) noexcept;
    [[nodiscard]] LUDUS_INLINE std::string_view GetView() const noexcept
    {
        return std::string_view(mText.GetData(), mLength);
    }

private:
    static constexpr usize CAPACITY = 192;
    foundation::StaticArray<char, CAPACITY> mText{};
    usize mLength = 0;
};

/// Append a root-relative source file with the digest of the bytes read.
/// @return false when memory is exhausted.
[[nodiscard]] bool AddSourceFile(SceneIr& inoutScene, std::string_view relative, const Digest& digest) noexcept;

/// Append a root-relative binary data file with the digest of the bytes read.
[[nodiscard]] bool AddDataFile(SceneIr& inoutScene, std::string_view relative, const Digest& digest) noexcept;

/// Make shape emit the material's optional emission (zero leaves it a non-emitter).
/// EmissionScale carries the luminance so the planner's luminance normalisation keeps
/// the authored magnitude.
void ApplyMaterialEmission(const IrMaterial& material, bool isTwoSided, IrShape& inoutShape) noexcept;
} // namespace ludus::content::scene_cook::importers
