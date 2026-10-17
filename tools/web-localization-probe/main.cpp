#include <ludus/localization/catalog.hpp>

#include "fixture.hpp"

using namespace ludus::localization;

// Run the production cooker/reader/binder through wasm32. Exit status is the
// machine-readable result; this corpus makes no browser text-rendering claim.
int main()
{
    Catalog source;
    Diagnostic diagnostic;
    const auto& domain = ludus::foundation::GetSystemAllocationDomain();
    if (PrepareCatalog(kSourceCatalog, domain, source, diagnostic) != Status::Ok)
    {
        return 1;
    }
    Catalog retained = source;
    MessageBinding binding;
    ResolvedText text;
    if (source.BindMessage(localization_fixture::kUnicode, binding) != Status::Ok ||
        source.ResolveStatic(binding, text) != Status::Ok || text.Text != "العربية 日本語 😀 é")
    {
        return 2;
    }
    if (PrepareCatalog(kFrenchCatalog, domain, source, diagnostic) != Status::Ok ||
        source.ResolveStatic(binding, text) != Status::WrongCatalog ||
        retained.ResolveStatic(binding, text) != Status::Ok || text.Text != "العربية 日本語 😀 é")
    {
        return 3;
    }
    if (source.BindMessage(localization_fixture::kMenuPlay, binding) != Status::Ok ||
        source.ResolveStatic(binding, text) != Status::Ok || text.Text != "Jouer" ||
        text.Provenance != Origin::Translation)
    {
        return 4;
    }
    if (PrepareCatalog(kFallbackCatalog, domain, source, diagnostic) != Status::Ok ||
        source.BindMessage(localization_fixture::kMenuPlay, binding) != Status::Ok ||
        source.ResolveStatic(binding, text) != Status::Ok || text.Provenance != Origin::SourceFallback)
    {
        return 5;
    }
    for (usize size = 0; size < sizeof(kSourceCatalog); ++size)
    {
        if (PrepareCatalog(std::span{kSourceCatalog}.first(size), domain, source, diagnostic) == Status::Ok)
        {
            return 6;
        }
    }
    // Package version 2 (ludus-icu-mf1-v1): ICU-decoded static text, baked
    // fallback providers and dynamic records that require L2 formatting.
    Catalog release;
    if (PrepareCatalog(kReleaseFrCa, domain, release, diagnostic) != Status::Ok ||
        release.BindMessage(localization_release_fixture::kMenuStart, binding) != Status::Ok ||
        release.ResolveStatic(binding, text) != Status::Ok || text.Text != "Démarrer" ||
        text.Provenance != Origin::Fallback || text.ProviderLocale != "fr-FR")
    {
        return 7;
    }
    if (release.BindMessage(localization_release_fixture::kEscapeUnterminated, binding) != Status::Ok ||
        release.ResolveStatic(binding, text) != Status::Ok || text.Text != "Ouvrir {jusqu'à la fin")
    {
        return 8;
    }
    if (release.BindMessage(localization_release_fixture::kHudLives, binding) != Status::Ok ||
        release.ResolveStatic(binding, text) != Status::RequiresArguments ||
        release.BindMessage(localization_release_fixture::kHudLives.Key, binding) != Status::SchemaMismatch)
    {
        return 9;
    }
    for (usize size = 0; size < sizeof(kReleaseFrCa); ++size)
    {
        if (PrepareCatalog(std::span{kReleaseFrCa}.first(size), domain, release, diagnostic) == Status::Ok)
        {
            return 10;
        }
    }
    return 0;
}
