// Optional CPU microbenchmark. Real screens/GPU budgets require separate captures.
#include <ludus/ui/context.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <span>
int main()
{
    using namespace ludus::ui;
    for (usize count : {16, 128, 1024})
    {
        Context ui;
        if (ui.TryInitialize(count) != Status::Ok)
        {
            return 1;
        }
        Element storage[1024];
        std::span<Element> elements{storage, count};
        for (usize i = 0; i < count; ++i)
        {
            auto& e = elements[i];
            e.Id = i + 1;
            e.Placement.Width = {80, Unit::Pixels};
            e.Placement.Height = {24, Unit::Pixels};
            e.Fill = {0.1f, 0.2f, 0.3f, 1};
            e.SemanticRole = Role::Button;
            e.Placement.OffsetX = static_cast<float32>((i % 16) * 80);
            e.Placement.OffsetY = static_cast<float32>((i / 16) * 24);
        }
        ludus::foundation::float64 samples[2000];
        usize sample = 0;
        for (usize j = 0; j < 2100; ++j)
        {
            const auto start = std::chrono::steady_clock::now();
            if (ui.TrySetDocument(elements, {{0, 0, 1280, 1536}}) != Status::Ok)
            {
                return 2;
            }
            auto r = ui.Route({EventKind::FocusNext});
            r = ui.Route({EventKind::Activate});
            r = ui.Route({EventKind::PointerDown, 40, 12});
            r = ui.Route({EventKind::PointerUp, 40, 12});
            if (r.Consumed == false)
            {
                return 3;
            }
            auto elapsed =
                std::chrono::duration<ludus::foundation::float64, std::micro>(std::chrono::steady_clock::now() - start)
                    .count();
            if (j >= 100)
            {
                samples[sample++] = elapsed;
            }
        }
        std::sort(samples, samples + 2000);
        std::printf("%zu elements: median %.2f us, p95 %.2f us (resolve + 4 routed events)\n",
                    count,
                    samples[1000],
                    samples[1900]);
    }
}
