#include "internal/access_path.hpp"

namespace ludus::tooling::shader_language::internal
{
bool MayOverlap(const AccessPath& left, const AccessPath& right) noexcept
{
    if (left.Root != right.Root || left.Root == NO_SYMBOL)
    {
        return false;
    }
    const foundation::usize count = left.StepCount < right.StepCount ? left.StepCount : right.StepCount;
    for (foundation::usize i = 0; i < count; ++i)
    {
        const AccessStep& a = left.Steps[i];
        const AccessStep& b = right.Steps[i];
        if (a.Kind == AccessStepKind::DynamicIndex || b.Kind == AccessStepKind::DynamicIndex)
        {
            continue;
        }
        if (a.Kind != b.Kind)
        {
            // Different step kinds on one type cannot occur; stay conservative.
            continue;
        }
        if (a.Kind == AccessStepKind::Swizzle)
        {
            return (a.Value & b.Value) != 0;
        }
        if (a.Value != b.Value)
        {
            return false;
        }
    }
    return true;
}
} // namespace ludus::tooling::shader_language::internal
