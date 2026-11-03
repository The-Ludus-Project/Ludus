#pragma once

#include <ludus/content/render_graph_format/detail/light_transformer_encoder_layout.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer::internal
{
struct EncoderGrid final
{
    foundation::uint32 X = 0;
    foundation::uint32 Y = 0;
    foundation::uint32 Z = 0;
};
struct EncoderLevel final
{
    foundation::Array<foundation::math::Vector3> PositionsModel;
    foundation::Array<EncoderGrid> CoordinatesGrid;
    foundation::Array<foundation::uint32> ParentToChild;
    foundation::Array<foundation::uint32> Children;
    foundation::Array<foundation::uint32> ChildStarts;
    foundation::uint32 Depth = 0;
};
[[nodiscard]] rhi::RasterStatus
PrepareEncoderFps(foundation::BorrowedView<const foundation::math::Vector3> positionsModel,
                  foundation::Array<foundation::uint32>& outIndices,
                  foundation::Array<foundation::math::Vector3>& outPositionsModel) noexcept;
/// Borrowed source/query positions and the query-to-source center mapping for one neighbor search.
struct EncoderNeighborSources final
{
    foundation::BorrowedView<const foundation::math::Vector3> PositionsModel;
    foundation::BorrowedView<const foundation::math::Vector3> QueriesModel;
    foundation::BorrowedView<const foundation::uint32> Centers;
};
[[nodiscard]] rhi::RasterStatus PrepareEncoderNeighbors(const EncoderNeighborSources& sources,
                                                        foundation::Array<foundation::uint32>& outNeighbors) noexcept;
[[nodiscard]] rhi::RasterStatus
PrepareEncoderLevels(foundation::BorrowedView<const foundation::math::Vector3> anchorsModel,
                     foundation::StaticArray<EncoderLevel, LUDUS_LIGHT_ENCODER_LEVELS>& outLevels) noexcept;
[[nodiscard]] rhi::RasterStatus PrepareEncoderSparse(const EncoderLevel& level,
                                                     foundation::uint32 kernel,
                                                     foundation::Array<foundation::uint32>& outStarts,
                                                     foundation::Array<foundation::uint32>& outPairs) noexcept;
[[nodiscard]] rhi::RasterStatus PrepareEncoderPatches(const EncoderLevel& level,
                                                      foundation::uint32 order,
                                                      foundation::Array<foundation::uint32>& outPatches) noexcept;
} // namespace ludus::graphics::renderer::internal
