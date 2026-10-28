#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <catch2/catch_test_macros.hpp>

/// @file
/// A staged SDK shader-helper directory for render-graph builds of raster graphs. The list
/// mirrors HELPERS and NATIVE_WORKER_HELPER in scripts/python/ludus_tools/render_graph_build.py
/// and the helper copies in cmake/LudusRenderGraph.cmake; ray graphs need further renderer
/// shader files that these tests do not stage.
namespace ludus::editor::tests
{
/// Copy the checkout's render-graph build helpers into directory, creating it when absent.
inline void CopyRenderGraphBuildHelpers(const QString& directory)
{
    const QDir helper(directory);
    REQUIRE(QDir().mkpath(helper.absolutePath()) == true);
    for (const QString& file : {QStringLiteral("cmake/shaders/compile_shader.py"),
                                QStringLiteral("cmake/shaders/compile_raster.py"),
                                QStringLiteral("cmake/shaders/compile_graph_stage.py"),
                                QStringLiteral("cmake/shaders/compile_metal_ray.py"),
                                QStringLiteral("cmake/shaders/native_worker.py"),
                                QStringLiteral("config/shader_toolchain.json"),
                                QStringLiteral("config/spirv_cross_toolchain.json")})
    {
        QFile source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/") + file);
        REQUIRE(source.open(QIODevice::ReadOnly) == true);
        const QByteArray bytes = source.readAll();
        QFile destination(helper.filePath(QFileInfo(file).fileName()));
        REQUIRE(destination.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
        REQUIRE(destination.write(bytes) == bytes.size());
    }
}
} // namespace ludus::editor::tests
