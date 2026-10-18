#include <ludus/localization/catalog.hpp>
#include <ludus/localization/formatter.hpp>

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
    // The formatter API is installed with or without LUDUS_BUILD_UNICODE.
    FormatterContext context;
    FormatResult result;
    char output[4] = {};
    const bool isFormatterExplicit =
        PrepareFormatterContext(catalog, {}, ludus::foundation::GetSystemAllocationDomain(), context) ==
            Status::Invalid &&
        context.IsValid() == false && Format(context, binding, {}, output, result) == Status::Invalid;
    return isFormatterExplicit == true &&
           PrepareCatalog(invalid, ludus::foundation::GetSystemAllocationDomain(), catalog, diagnostic) ==
               Status::Invalid &&
           catalog.IsValid() == false && catalog.GetDomain().empty() && catalog.GetLocale().empty() &&
           catalog.BindMessage({"game/ui", "menu/play"}, binding) == Status::Invalid &&
           catalog.ResolveStatic(binding, text) == Status::Invalid;
}
