#include <ludus/localization/catalog.hpp>

// Installed header, static link closure, empty lifecycle and explicit errors.
// Cooker/runtime roundtrips are exercised by the native and wasm corpus tests.
bool ExerciseInstalledLocalization() noexcept
{
    using namespace ludus::localization;
    Catalog catalog;
    Diagnostic diagnostic;
    const uint8 invalid[] = {0};
    MessageBinding binding;
    ResolvedText text;
    return PrepareCatalog(invalid, ludus::foundation::GetSystemAllocationDomain(), catalog, diagnostic) ==
               Status::Invalid &&
           catalog.IsValid() == false && catalog.GetDomain().empty() && catalog.GetLocale().empty() &&
           catalog.BindMessage({"game/ui", "menu/play"}, binding) == Status::Invalid &&
           catalog.ResolveStatic(binding, text) == Status::Invalid;
}
