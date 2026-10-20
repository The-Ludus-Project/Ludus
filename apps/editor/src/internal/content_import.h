#pragma once

#include "internal/content_import_gate.h"

#include <ludus/content/content.h>

#include <QString>

namespace ludus::editor
{
struct ContentImportSource final
{
    QString Root;
    QString File;
    QString Id;
};

struct ContentImportResult final
{
    content::Status Status = content::Status::Invalid;
    QString Message;
    QString CandidatePath;
};

// Cold synchronous adapter; invoke on a native worker or the browser's sole
// filesystem owner after yielding to paint progress. Hooks are borrowed for
// this call and support deterministic cancellation/conflict tests. A failed
// catalog switch retains the old version and reports any unreferenced candidate.
struct ContentImportHooks final
{
    void* Context = nullptr;
    bool (*Cancelled)(void*) noexcept = nullptr;
    void (*BeforePublish)(void*) noexcept = nullptr;
};
[[nodiscard]] ContentImportResult ImportAudioSource(const ContentImportSource& source,
                                                    ContentImportGate& gate,
                                                    const content::Digest* expectedCatalog = nullptr,
                                                    ContentImportHooks hooks = {});
} // namespace ludus::editor
