#pragma once

#include "internal/render_graph_document.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <catch2/catch_test_macros.hpp>

namespace ludus::editor::tests
{
inline void SetTemplateAuthoringProfiles(RenderGraphDocument& inoutDocument)
{
#if defined(Q_OS_WIN)
    // These tests edit source packages. Use a declared Vulkan/WGSL target
    // where the imported ray template is admitted, without executing it.
    const QString manifestFile = QStringLiteral("main.lushader.json");
    QJsonObject manifest = QJsonDocument::fromJson(inoutDocument.GetDraft(manifestFile)).object();
    REQUIRE(manifest.isEmpty() == false);
    manifest.insert(QStringLiteral("profiles"), QJsonArray{QStringLiteral("spirv"), QStringLiteral("wgsl")});
    REQUIRE(inoutDocument.SetText(manifestFile, EncodeCanonicalJson(manifest)) == RenderGraphDocumentStatus::Ok);
#else
    static_cast<void>(inoutDocument);
#endif
}
} // namespace ludus::editor::tests
