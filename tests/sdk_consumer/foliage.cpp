#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/content/foliage_pages/foliage_page_publication.h>
#include <ludus/content/foliage_pages/foliage_pages.h>

// Encode a one-page LUFOL page set with the installed encoder, then validate and
// decode it with the installed reader against the same terrain binding. Publish
// it, and check that a corrupt replacement keeps the published generation.
bool ExerciseInstalledFoliagePages() noexcept
{
    using namespace ludus::content::foliage_pages;
    using ludus::foundation::BorrowedView;
    using ludus::foundation::uint8;

    constexpr ludus::foundation::uint32 PAGE_SIZE = 4096;
    constexpr ludus::foundation::usize BUFFER_BYTES = 512;
    const FoliageLayer layers[] = {{ .Id = 1, .SlotsPerCell = 1, .CellSize = 1024 }};
    const FoliageSpecies species[] = {{ .Key = 7, .PlacementRevision = 1, .VariantCount = 1 }};
    const FoliagePlacement placements[] = {
        { .Layer = 1, .CellX = -1, .CellZ = 0, .OffsetX = 10, .OffsetZ = 20, .Scale = 4096 },
    };
    const FoliagePage pages[] = {
        {
            .PageX = -1,
            .PageZ = 0,
            .FirstPlacement = 0,
            .PlacementCount = 1,
            .Bounds = { .MinimumX = -1014, .MinimumZ = 20, .MaximumX = -1014, .MaximumZ = 20 },
        },
    };
    FoliagePageSetDescription description;
    description.Header.GeneratorVersion = 1;
    description.Header.PageSize = PAGE_SIZE;
    description.Header.DomainId = 1;
    description.Layers = BorrowedView<const FoliageLayer>(layers);
    description.Species = BorrowedView<const FoliageSpecies>(species);
    description.Pages = BorrowedView<const FoliagePage>(pages);
    description.Placements = BorrowedView<const FoliagePlacement>(placements);

    uint8 bytes[BUFFER_BYTES]{};
    ludus::foundation::usize written = 0;
    FoliagePageDiagnostic diagnostic;
    if (WriteFoliagePageSet(description, {}, BorrowedView<uint8>(bytes), written, diagnostic) != FoliagePageStatus::Ok)
    {
        return false;
    }
    FoliagePageSetView view;
    FoliagePlacement decoded;
    ludus::foundation::usize page = 1;
    const FoliagePageBinding binding = { .DomainId = 1, .TerrainRevision = 0 };
    if (ReadFoliagePageSet(BorrowedView<const uint8>(bytes, written), binding, {}, view, diagnostic) !=
            FoliagePageStatus::Ok ||
        view.TryFindPage(-1, 0, page) != FoliagePageStatus::Ok || page != 0 ||
        view.TryGetPlacement(0, decoded) != FoliagePageStatus::Ok || (decoded == placements[0]) == false)
    {
        return false;
    }

    ludus::foundation::Array<uint8> owned;
    ludus::foundation::Array<uint8> corrupt;
    if (owned.TryResize(written) == false || corrupt.TryResize(written) == false)
    {
        return false;
    }
    for (ludus::foundation::usize i = 0; i < written; ++i)
    {
        owned[i] = bytes[i];
        corrupt[i] = bytes[i];
    }
    corrupt[0] = 0;
    FoliagePageSetPublication publication;
    FoliagePageSetCandidate candidate;
    if (publication.TryStage(owned, binding, candidate, diagnostic) != FoliagePageStatus::Ok ||
        publication.TryPublish(candidate) != FoliagePublicationStatus::Ok)
    {
        return false;
    }
    return publication.TryStage(corrupt, binding, candidate, diagnostic) == FoliagePageStatus::Malformed &&
           publication.TryPublish(candidate) == FoliagePublicationStatus::NotReady &&
           publication.GetAcceptedRevision() == 1 && publication.GetAcceptedBytes().GetSize() == written &&
           publication.GetAccepted().TryGetPlacement(0, decoded) == FoliagePageStatus::Ok && decoded == placements[0];
}
