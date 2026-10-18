#include <ludus/ui/context.h>

int ExerciseInstalledUi() noexcept
{
    ludus::ui::Context context;
    if (context.TryInitialize(1) != ludus::ui::Status::Ok)
    {
        return 10;
    }
    ludus::ui::Element button;
    button.Id = 42;
    button.SemanticRole = ludus::ui::Role::Button;
    button.Placement.Width = {1, ludus::ui::Unit::Fraction};
    button.Placement.Height = {1, ludus::ui::Unit::Fraction};
    if (context.TrySetDocument({&button, 1}, {{0, 0, 100, 100}}) != ludus::ui::Status::Ok)
    {
        return 10;
    }
    if (context.TryFocus(42) == false || context.Route({ludus::ui::EventKind::Activate}).Activated != 42)
    {
        return 10;
    }
    return 0;
}
