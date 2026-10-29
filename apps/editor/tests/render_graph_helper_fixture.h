#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <catch2/catch_test_macros.hpp>

/// @file
/// A substitute SDK shader-helper directory for render-graph builds. CMake stages the installed
/// helper set (cmake/LudusRenderGraph.cmake, _ludus_stage_render_graph_helpers) into
/// LUDUS_EDITOR_TEST_RENDER_GRAPH_HELPER, so the copy always matches what the SDK installs.
namespace ludus::editor::tests
{
/// Copy the staged render-graph build helpers into directory, creating it when absent.
inline void CopyRenderGraphBuildHelpers(const QString& directory)
{
    const QDir helper(directory);
    REQUIRE(QDir().mkpath(helper.absolutePath()) == true);
    const QDir staged(QStringLiteral(LUDUS_EDITOR_TEST_RENDER_GRAPH_HELPER));
    const QStringList files = staged.entryList(QDir::Files);
    REQUIRE(files.isEmpty() == false);
    for (const QString& file : files)
    {
        QFile source(staged.filePath(file));
        REQUIRE(source.open(QIODevice::ReadOnly) == true);
        const QByteArray bytes = source.readAll();
        QFile destination(helper.filePath(file));
        REQUIRE(destination.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
        REQUIRE(destination.write(bytes) == bytes.size());
    }
}
} // namespace ludus::editor::tests
