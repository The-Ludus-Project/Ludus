#include "internal/window_size.h"

#include <QtGlobal>

namespace ludus::editor
{
namespace
{
/// Percent denominator.
constexpr int PERCENT = 100;

int ComputeLength(int availableLength, int comfortableLength) noexcept
{
    const int share = availableLength * DEFAULT_WINDOW_SCREEN_PERCENT / PERCENT;
    return qMin(availableLength, qMax(share, comfortableLength));
}
} // namespace

QSize ComputeDefaultWindowSize(QSize availableSize) noexcept
{
    if (availableSize.isValid() == false || availableSize.isEmpty() == true)
    {
        return FALLBACK_WINDOW_SIZE;
    }
    return {ComputeLength(availableSize.width(), COMFORTABLE_WINDOW_WIDTH),
            ComputeLength(availableSize.height(), COMFORTABLE_WINDOW_HEIGHT)};
}
} // namespace ludus::editor
