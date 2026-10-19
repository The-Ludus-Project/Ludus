#include "internal/preview_shader_build.h"
#include "internal/preview_shader_reload.h"
#include "internal/preview_shader_set.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "shader_reload_fixture.h"
#include "viewport_fixture.h"

#include <ludus/foundation/base/core.h>

#include <ludus/graphics/rhi/rhi.h>

#include <new>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace compiler = ludus::tooling::shader_compiler;
namespace rhi = ludus::graphics::rhi;

namespace
{
/// Marker that makes FakeCompiler report a Slang-style error at its line.
constexpr const char* ERROR_MARKER = "LUDUS_TEST_ERROR";
/// Marker that makes FakeCompiler return changed fragment code with the built-in interface.
constexpr const char* TINT_MARKER = "LUDUS_TEST_TINT";
/// Marker that makes FakeCompiler return changed fragment code using an undeclared binding.
constexpr const char* INTERFACE_MARKER = "LUDUS_TEST_INTERFACE";
/// SPIR-V OpNop, appended to built-in fragment code to make it differ without changing behaviour.
constexpr uint32 SPIRV_NOP = 0x00010000U;
/// Upper bound for one fake (in-process) build, including debounce and thread hops.
constexpr int32 FAKE_TIMEOUT_MILLISECONDS = 30000;
/// Upper bound for real worker builds of every viewport stage plus lit preparation on lavapipe.
constexpr int32 WORKER_TIMEOUT_MILLISECONDS = 600000;
/// Directory for PNG captures of the live shader overlay; captures are skipped when unset.
constexpr const char* SHOT_DIRECTORY_VARIABLE = "LUDUS_VIEWPORT_DEBUG_SHOTS";
/// Pause before grabbing the viewport so the canvas has painted the newest frame and overlay.
constexpr int32 SETTLE_MILLISECONDS = 500;
/// Capture size of the viewport overlay test.
constexpr int32 VIEWPORT_WIDTH = 480;
constexpr int32 VIEWPORT_HEIGHT = 320;

QByteArray ToBytes(BorrowedView<const char> view)
{
    return QByteArray(view.GetDataOrNull(), static_cast<qsizetype>(view.GetSize()));
}
bool Fill(Array<uint8>& outBytes, const char* dataOrNull, usize size)
{
    if (outBytes.TryResize(size) == false)
    {
        return false;
    }
    for (usize i = 0; i < size; ++i)
    {
        outBytes[i] = static_cast<uint8>(dataOrNull[i]);
    }
    return true;
}

/// In-process stand-in for ludus-shader-worker: returns each recipe's built-in artifact, so an
/// untouched tree rebuilds to the built-in set, and reacts to the markers above.
class FakeCompiler final : public PreviewShaderCompiler
{
public:
    explicit FakeCompiler(const PreviewShaderSources& sources)
    {
        REQUIRE(ReadPreviewShaderRecipes(sources, mRecipes, mError) == true);
    }
    compiler::Status
    Compile(const compiler::Request& request, compiler::Result& outResult, const QAtomicInt& cancel) noexcept override
    {
        static_cast<void>(cancel);
        mCalls.fetchAndAddRelaxed(1);
        outResult = {};
        outResult.Identity = request.Identity;
        const QByteArray source = ToBytes(request.Source);
        const PreviewShaderStage stage =
            request.EntryStage == compiler::Stage::Vertex ? PreviewShaderStage::Vertex : PreviewShaderStage::Fragment;
        // Like Slang, read only the root and its #include closure, so stages depend on fewer files.
        QList<bool> isRead(static_cast<qsizetype>(request.Files.GetSize()), false);
        QStringList pending{QString::fromUtf8(source)};
        static const QRegularExpression INCLUDE(QStringLiteral(R"re(^\s*#\s*include\s+"([^"]+)")re"),
                                                QRegularExpression::MultilineOption);
        while (pending.isEmpty() == false)
        {
            const QString name = pending.takeLast();
            for (usize i = 0; i < request.Files.GetSize(); ++i)
            {
                const QString path = QString::fromUtf8(ToBytes(request.Files[i].Path));
                if (isRead[static_cast<qsizetype>(i)] == true ||
                    (path != name && path.endsWith(QLatin1Char('/') + name) == false))
                {
                    continue;
                }
                isRead[static_cast<qsizetype>(i)] = true;
                QRegularExpressionMatchIterator includes =
                    INCLUDE.globalMatch(QString::fromUtf8(ToBytes(request.Files[i].Text)));
                while (includes.hasNext() == true)
                {
                    pending.append(includes.next().captured(1));
                }
                break;
            }
        }
        bool isTinted = false;
        bool isInterfaceChanged = false;
        for (usize i = 0; i < request.Files.GetSize(); ++i)
        {
            if (isRead[static_cast<qsizetype>(i)] == false)
            {
                continue;
            }
            const QByteArray text = ToBytes(request.Files[i].Text);
            const qsizetype error = text.indexOf(ERROR_MARKER);
            if (error >= 0)
            {
                const QByteArray diagnostics = ToBytes(request.Files[i].Path) + "(" +
                                               QByteArray::number(text.left(error).count('\n') + 1) +
                                               "): error 30015: undefined identifier '" + ERROR_MARKER + "'.\n    " +
                                               ERROR_MARKER + ";\n    ^~~~~~~~\n";
                static_cast<void>(Fill(outResult.Diagnostics, diagnostics.constData(), diagnostics.size()));
                outResult.Outcome = compiler::Status::CompileFailed;
                return outResult.Outcome;
            }
            isTinted = isTinted || text.contains(TINT_MARKER);
            isInterfaceChanged = isInterfaceChanged || text.contains(INTERFACE_MARKER);
        }
        rhi::RasterShaderDescription builtIn;
        for (usize program = 0; program < PREVIEW_SHADER_PROGRAM_COUNT; ++program)
        {
            const PreviewShaderRecipe& recipe = mRecipes[program];
            const QByteArray entry = stage == PreviewShaderStage::Vertex ? recipe.VertexEntry : recipe.FragmentEntry;
            if (recipe.Source.toUtf8() == source && entry == ToBytes(request.Entry))
            {
                builtIn = GetBuiltInPreviewShader(static_cast<PreviewShaderProgram>(program), stage);
                break;
            }
        }
        QList<uint32> words(builtIn.Artifact.Spirv.begin(), builtIn.Artifact.Spirv.end());
        QByteArray reflection = QByteArrayLiteral("{}");
        if (stage == PreviewShaderStage::Fragment && (isTinted == true || isInterfaceChanged == true))
        {
            words.append(SPIRV_NOP);
        }
        if (stage == PreviewShaderStage::Fragment && isInterfaceChanged == true)
        {
            reflection = QByteArrayLiteral(
                R"({"parameters":[{"name":"extraTexture","binding":{"kind":"descriptorTableSlot","index":30},)"
                R"("type":{"kind":"resource","baseShape":"texture2D","resultType":{"kind":"vector",)"
                R"("elementCount":4,"elementType":{"kind":"scalar","scalarType":"float32"}}}}],)"
                R"("nativeEntryPointResourceUsage":{"extraTexture":true}})");
        }
        if (Fill(outResult.Artifact,
                 reinterpret_cast<const char*>(words.constData()),
                 static_cast<usize>(words.size()) * sizeof(uint32)) == false ||
            Fill(outResult.Reflection, reflection.constData(), static_cast<usize>(reflection.size())) == false ||
            outResult.SourceReadMask.TryResize(request.Files.GetSize(), 0) == false)
        {
            outResult.Outcome = compiler::Status::OutOfMemory;
            return outResult.Outcome;
        }
        for (usize i = 0; i < request.Files.GetSize(); ++i)
        {
            outResult.SourceReadMask[i] = isRead[static_cast<qsizetype>(i)] == true ? 1 : 0;
        }
        outResult.Outcome = compiler::Status::Ready;
        return outResult.Outcome;
    }
    [[nodiscard]] int32 GetCalls() const noexcept
    {
        return mCalls.loadRelaxed();
    }

private:
    PreviewShaderRecipes mRecipes{};
    QString mError;
    QAtomicInt mCalls;
};

/// Wait until the controller is idle or reports a failure, after its debounce and build.
bool WaitForSettled(const PreviewShaderReload& reload, int32 timeoutMilliseconds)
{
    return QTest::qWaitFor(
        [&reload]() {
            const PreviewShaderReloadState state = reload.GetState();
            return state != PreviewShaderReloadState::Compiling && state != PreviewShaderReloadState::Applying;
        },
        timeoutMilliseconds);
}
/// Append text after the last line of path.
void AppendLine(const QString& path, const QByteArray& line)
{
    tests::WriteText(path, tests::ReadText(path) + "\n" + line + "\n");
}
} // namespace

TEST_CASE("Preview shader sets replace one stage and identify their content", "[editor][shader-reload]")
{
    PreviewShaderSet shaders;
    CHECK(shaders.HasArtifacts() == false);
    const QByteArray builtInDigest = shaders.ComputeDigest();
    const rhi::RasterShaderDescription builtIn =
        GetBuiltInPreviewShader(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment);
    REQUIRE(builtIn.Artifact.Spirv.empty() == false);
    PreviewShaderArtifact artifact;
    artifact.Spirv = QList<uint32>(builtIn.Artifact.Spirv.begin(), builtIn.Artifact.Spirv.end());
    artifact.Spirv.append(SPIRV_NOP);
    artifact.Entry = QByteArrayLiteral("FragmentMain");
    shaders.SetArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment, artifact);
    CHECK(shaders.HasArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment) == true);
    CHECK(shaders.HasArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Vertex) == false);
    const rhi::RasterShaderDescription replaced = shaders.Get(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment);
    CHECK(replaced.Artifact.Spirv.size() == builtIn.Artifact.Spirv.size() + 1);
    CHECK(replaced.Artifact.Spirv.back() == SPIRV_NOP);
    // Bindings and uniform layout always come from the built-in description.
    CHECK(replaced.Bindings.size() == builtIn.Bindings.size());
    CHECK(shaders.Get(PreviewShaderProgram::Lit, PreviewShaderStage::Vertex).Artifact.Spirv.data() ==
          GetBuiltInPreviewShader(PreviewShaderProgram::Lit, PreviewShaderStage::Vertex).Artifact.Spirv.data());
    CHECK(shaders.ComputeDigest() != builtInDigest);
    shaders.SetArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment, {});
    CHECK(shaders.HasArtifacts() == false);
    CHECK(shaders.ComputeDigest() == builtInDigest);
}

TEST_CASE("Shader diagnostics keep the first Slang error with its location", "[editor][shader-reload]")
{
    PreviewShaderSnapshot snapshot;
    snapshot.Names = {QStringLiteral("renderer_lit.slang")};
    snapshot.Paths = {QStringLiteral("/engine/shaders/renderer_lit.slang")};
    snapshot.Texts = {QByteArray()};
    const QByteArray output = "renderer_lit.slang(240): warning 30081: implicit conversion\n"
                              "renderer_lit.slang(241): error 30015: undefined identifier 'stored'.\n"
                              "            gProbe = float4(stored);\n"
                              "                            ^~~~~~\n"
                              "renderer_lit.slang(250): error 30019: second error\n";
    const PreviewShaderDiagnostic diagnostic = ParsePreviewShaderDiagnostic(output, snapshot);
    CHECK(diagnostic.Path == QStringLiteral("/engine/shaders/renderer_lit.slang"));
    CHECK(diagnostic.Line == 241);
    CHECK(diagnostic.Message == QStringLiteral("undefined identifier 'stored'."));
    REQUIRE(diagnostic.Excerpt.size() == 2);
    CHECK(diagnostic.Excerpt[0].contains(QStringLiteral("gProbe")) == true);
    const QString text = FormatPreviewShaderDiagnostic(diagnostic);
    CHECK(text.startsWith(QDir::toNativeSeparators(QStringLiteral("/engine/shaders/renderer_lit.slang")) +
                          QStringLiteral(":241\nundefined identifier")) == true);
    CHECK(text.contains(QStringLiteral("second error")) == false);
    const PreviewShaderDiagnostic plain = ParsePreviewShaderDiagnostic("\nworker crashed\n", snapshot);
    CHECK(plain.Line == 0);
    CHECK(plain.Message == QStringLiteral("worker crashed"));
}

TEST_CASE("Compiled shader entry names come from SPIR-V and Metal output", "[editor][shader-reload]")
{
    QByteArray entry;
    const rhi::RasterShaderDescription builtIn =
        GetBuiltInPreviewShader(PreviewShaderProgram::Flat, PreviewShaderStage::Vertex);
    REQUIRE(FindSpirvEntry(QList<uint32>(builtIn.Artifact.Spirv.begin(), builtIn.Artifact.Spirv.end()), entry) == true);
    CHECK(entry ==
          QByteArray(builtIn.Artifact.SpirvEntry.data(), static_cast<qsizetype>(builtIn.Artifact.SpirvEntry.size())));
    QByteArray unchanged = QByteArrayLiteral("keep");
    CHECK(FindSpirvEntry({1, 2, 3}, unchanged) == false);
    CHECK(unchanged == QByteArrayLiteral("keep"));
    const QByteArray metal = "#include <metal_stdlib>\n"
                             "[[vertex]] VertexOut VertexMain_0(VertexIn input [[stage_in]])\n{}\n"
                             "[[fragment]] FragmentOut FragmentMain_0(VertexOut input [[stage_in]])\n{}\n";
    REQUIRE(FindMetalEntry(metal, PreviewShaderStage::Vertex, entry) == true);
    CHECK(entry == QByteArrayLiteral("VertexMain_0"));
    REQUIRE(FindMetalEntry(metal, PreviewShaderStage::Fragment, entry) == true);
    CHECK(entry == QByteArrayLiteral("FragmentMain_0"));
    CHECK(FindMetalEntry(QByteArrayLiteral("float4 f();"), PreviewShaderStage::Vertex, unchanged) == false);
}

TEST_CASE("Live shader interfaces may drop resources but not add or reshape them", "[editor][shader-reload]")
{
    const rhi::RasterBinding bindings[]{{ .Binding = 0, .Kind = rhi::RasterBindingKind::UniformBuffer, .MinSize = 64 },
                                        { .Binding = 1, .Kind = rhi::RasterBindingKind::Texture2D },
                                        { .Binding = 2, .Kind = rhi::RasterBindingKind::Sampler }};
    const rhi::RasterUniformMember members[]{
        { .Binding = 0, .Name = "WorldToClip", .Offset = 0, .Shape = rhi::RasterUniformShape::Float4x4 }};
    rhi::RasterShaderDescription builtIn;
    builtIn.Bindings = bindings;
    builtIn.UniformMembers = members;
    const auto makeReflection = [](const char* parameters, const char* usage) {
        return QByteArray(R"({"parameters":[)") + parameters + R"(],"nativeEntryPointResourceUsage":{)" + usage + "}}";
    };
    const char* buffer =
        R"({"name":"frame","binding":{"kind":"descriptorTableSlot","index":0},"type":{"kind":"constantBuffer",)"
        R"("elementVarLayout":{"binding":{"kind":"uniform","offset":0,"size":64}},"elementType":{"kind":"struct",)"
        R"("fields":[{"name":"WorldToClip","binding":{"kind":"uniform","offset":0,"size":64},"type":{"kind":"matrix",)"
        R"("rowCount":4,"columnCount":4,"elementType":{"kind":"scalar","scalarType":"float32"}}}]}}})";
    const char* texture =
        R"({"name":"albedo","binding":{"kind":"descriptorTableSlot","index":1},"type":{"kind":"resource",)"
        R"("baseShape":"texture2D","resultType":{"kind":"vector","elementCount":4,)"
        R"("elementType":{"kind":"scalar","scalarType":"float32"}}}})";
    const char* moved =
        R"({"name":"albedo","binding":{"kind":"descriptorTableSlot","index":5},"type":{"kind":"resource",)"
        R"("baseShape":"texture2D","resultType":{"kind":"vector","elementCount":4,)"
        R"("elementType":{"kind":"scalar","scalarType":"float32"}}}})";
    const char* resized =
        R"({"name":"frame","binding":{"kind":"descriptorTableSlot","index":0},"type":{"kind":"constantBuffer",)"
        R"("elementVarLayout":{"binding":{"kind":"uniform","offset":0,"size":80}},"elementType":{"kind":"struct",)"
        R"("fields":[]}}})";
    QString reason;
    const QByteArray both =
        makeReflection((QByteArray(buffer) + "," + texture).constData(), R"("frame":true,"albedo":true)");
    CHECK(IsPreviewShaderInterfaceCompatible(both, PreviewShaderStage::Fragment, false, builtIn, reason) == true);
    // Dropping the texture keeps the layout: the pipeline still binds it, the code ignores it.
    const QByteArray subset =
        makeReflection((QByteArray(buffer) + "," + texture).constData(), R"("frame":true,"albedo":false)");
    CHECK(IsPreviewShaderInterfaceCompatible(subset, PreviewShaderStage::Fragment, false, builtIn, reason) == true);
    const QByteArray added = makeReflection(moved, R"("albedo":true)");
    CHECK(IsPreviewShaderInterfaceCompatible(added, PreviewShaderStage::Fragment, false, builtIn, reason) == false);
    CHECK(reason.contains(QStringLiteral("binding 5")) == true);
    const QByteArray grown = makeReflection(resized, R"("frame":true)");
    CHECK(IsPreviewShaderInterfaceCompatible(grown, PreviewShaderStage::Fragment, false, builtIn, reason) == false);
    CHECK(reason.contains(QStringLiteral("size")) == true);
    CHECK(IsPreviewShaderInterfaceCompatible("not json", PreviewShaderStage::Fragment, false, builtIn, reason) ==
          false);
}

TEST_CASE("Shader builder reuses unchanged stages and keeps the last good output", "[editor][shader-reload]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const PreviewShaderSources sources = tests::CopySources(tests::GetCheckoutSources(), directory.path());
    FakeCompiler compiler(sources);
    PreviewShaderBuilder builder(sources, compiler::Target::Spirv, QString());
    QAtomicInt cancel;
    const PreviewShaderBuildResult first = builder.Build(compiler, cancel);
    INFO(first.Message.toStdString());
    REQUIRE(first.Status == PreviewShaderBuildStatus::Ready);
    CHECK(first.Shaders.HasArtifacts() == false);
    CHECK(first.CompiledStages > 0);
    CHECK(first.WatchedPaths.contains(QDir(sources.RecipeDirectory).filePath(QStringLiteral("recipes.json"))) == true);
    const PreviewShaderBuildResult again = builder.Build(compiler, cancel);
    CHECK(again.Status == PreviewShaderBuildStatus::Ready);
    CHECK(again.CompiledStages == 0);
    const QString lit = QDir(sources.RecipeDirectory).filePath(QStringLiteral("renderer_lit.slang"));
    const QByteArray original = tests::ReadText(lit);
    AppendLine(lit, QByteArrayLiteral("// ") + TINT_MARKER);
    const PreviewShaderBuildResult tinted = builder.Build(compiler, cancel);
    REQUIRE(tinted.Status == PreviewShaderBuildStatus::Ready);
    CHECK(tinted.Shaders.HasArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment) == true);
    CHECK(tinted.Shaders.HasArtifact(PreviewShaderProgram::Flat, PreviewShaderStage::Fragment) == false);
    CHECK(tinted.CompiledStages > 0);
    CHECK(tinted.CompiledStages < first.CompiledStages);
    AppendLine(lit, ERROR_MARKER);
    const PreviewShaderBuildResult failed = builder.Build(compiler, cancel);
    REQUIRE(failed.Status == PreviewShaderBuildStatus::CompileFailed);
    CHECK(failed.Diagnostic.Path == QFileInfo(lit).absoluteFilePath());
    CHECK(failed.Diagnostic.Line == static_cast<int32>(tests::ReadText(lit).count('\n')));
    CHECK(failed.Diagnostic.Message.contains(QString::fromLatin1(ERROR_MARKER)) == true);
    tests::WriteText(lit, original);
    const PreviewShaderBuildResult recovered = builder.Build(compiler, cancel);
    REQUIRE(recovered.Status == PreviewShaderBuildStatus::Ready);
    CHECK(recovered.Shaders.HasArtifacts() == false);
    cancel.storeRelaxed(1);
    AppendLine(lit, QByteArrayLiteral("// ") + TINT_MARKER);
    CHECK(builder.Build(compiler, cancel).Status == PreviewShaderBuildStatus::Cancelled);
}

TEST_CASE("Live shaders debounce edits, report errors and recover", "[editor][shader-reload]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const PreviewShaderSources sources = tests::CopySources(tests::GetCheckoutSources(), directory.path());
    RenderPreview preview;
    FakeCompiler* fakeOrNull = new (std::nothrow) FakeCompiler(sources);
    REQUIRE(fakeOrNull != nullptr);
    FakeCompiler& fake = *fakeOrNull;
    PreviewShaderCompiler* compilerOrNull = fakeOrNull;
    PreviewShaderReload reload(preview,
                               sources,
                               UniquePtr<PreviewShaderCompiler>(compilerOrNull),
                               compiler::Target::Spirv);
    CHECK(reload.GetState() == PreviewShaderReloadState::Off);
    reload.SetEnabled(true);
    REQUIRE(WaitForSettled(reload, FAKE_TIMEOUT_MILLISECONDS) == true);
    INFO(reload.GetStatusText().toStdString());
    REQUIRE(reload.GetState() == PreviewShaderReloadState::Idle);
    CHECK(reload.GetStatusText().startsWith(QStringLiteral("Live shaders on")) == true);
    CHECK(reload.GetBuildCount() == 1);
    const QString lit = QDir(sources.RecipeDirectory).filePath(QStringLiteral("renderer_lit.slang"));
    CHECK(reload.GetWatchedPaths().contains(QFileInfo(lit).absoluteFilePath()) == true);
    CHECK(preview.GetShaders().HasArtifacts() == false);
    const QByteArray original = tests::ReadText(lit);

    // Several notifications within the quiet period share one build.
    AppendLine(lit, QByteArrayLiteral("// ") + TINT_MARKER);
    reload.NotifyChanged(lit);
    reload.NotifyChanged(lit);
    reload.NotifyChanged(lit);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload.GetBuildCount() > 1; }, FAKE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(WaitForSettled(reload, FAKE_TIMEOUT_MILLISECONDS) == true);
    QTest::qWait(1000);
    CHECK(reload.GetBuildCount() == 2);
    CHECK(reload.GetState() == PreviewShaderReloadState::Idle);
    // Without a device the set waits in the preview for its next generation.
    CHECK(preview.GetShaders().HasArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment) == true);
    const QByteArray tinted = preview.GetShaders().ComputeDigest();

    // A compile error keeps the last good set and explains itself with file:line.
    const int32 calls = fake.GetCalls();
    AppendLine(lit, ERROR_MARKER);
    reload.NotifyChanged(lit);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload.HasError() == true; }, FAKE_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload.GetState() == PreviewShaderReloadState::Failed);
    CHECK(fake.GetCalls() > calls);
    const QString expectedLocation = QStringLiteral("%1:%2")
                                         .arg(QDir::toNativeSeparators(QFileInfo(lit).absoluteFilePath()))
                                         .arg(tests::ReadText(lit).count('\n'));
    CHECK(reload.GetStatusText().contains(QStringLiteral("last good shader kept")) == true);
    CHECK(reload.GetStatusText().contains(expectedLocation) == true);
    CHECK(reload.GetStatusText().contains(QString::fromLatin1(ERROR_MARKER)) == true);
    CHECK(preview.GetShaders().ComputeDigest() == tinted);

    // Fixing the error clears the overlay.
    tests::WriteText(lit, original + "\n// " + TINT_MARKER + "\n");
    reload.NotifyChanged(lit);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload.HasError() == false; }, FAKE_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(WaitForSettled(reload, FAKE_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload.GetState() == PreviewShaderReloadState::Idle);
    CHECK(reload.GetStatusText().contains(QString::fromLatin1(ERROR_MARKER)) == false);
    CHECK(preview.GetShaders().ComputeDigest() == tinted);

    // A new binding cannot be drawn by the running pipelines: restart required, set kept.
    AppendLine(lit, QByteArrayLiteral("// ") + INTERFACE_MARKER);
    reload.NotifyChanged(lit);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload.HasError() == true; }, FAKE_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload.GetState() == PreviewShaderReloadState::RestartRequired);
    CHECK(reload.GetStatusText().contains(QStringLiteral("restart required")) == true);
    CHECK(reload.GetStatusText().contains(QStringLiteral("binding 30")) == true);
    CHECK(preview.GetShaders().ComputeDigest() == tinted);

    // Off stops watching; sources may then be retargeted.
    CHECK(reload.SetSources(sources) == false);
    reload.SetEnabled(false);
    CHECK(reload.GetState() == PreviewShaderReloadState::Off);
    CHECK(preview.GetShaders().HasArtifacts() == false);
    CHECK(reload.GetStatusText().isEmpty() == true);
    CHECK(reload.GetWatchedPaths().isEmpty() == true);
    CHECK(reload.SetSources(sources) == true);
}

TEST_CASE("Live shaders without a compiler explain how to get one", "[editor][shader-reload]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    PreviewShaderSources sources;
    sources.RecipeDirectory = directory.filePath(QStringLiteral("missing"));
    RenderPreview preview;
    PreviewShaderCompiler* compilerOrNull = new (std::nothrow) FakeCompiler(tests::GetCheckoutSources());
    REQUIRE(compilerOrNull != nullptr);
    PreviewShaderReload reload(preview,
                               sources,
                               UniquePtr<PreviewShaderCompiler>(compilerOrNull),
                               compiler::Target::Spirv);
    reload.SetEnabled(true);
    REQUIRE(WaitForSettled(reload, FAKE_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload.GetState() == PreviewShaderReloadState::Unavailable);
    CHECK(reload.HasError() == true);
    CHECK(reload.GetStatusText().contains(QStringLiteral("recipes.json")) == true);
}

TEST_CASE("Live shaders rebuild through ludus-shader-worker and swap into the viewport",
          "[editor][shader-reload][worker][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    if (QFileInfo(QString::fromUtf8(shader_reload_paths::WORKER_EXECUTABLE)).isExecutable() == false)
    {
        SKIP("Configure with -DLUDUS_BUILD_SHADER_WORKER=ON to run the live shader worker test");
    }
    const QByteArray previousViewport = qgetenv("LUDUS_EDITOR_VIEWPORT");
    const QByteArray previousSocket = qgetenv("LUDUS_SHADER_WORKER_SOCKET");
    qputenv("LUDUS_EDITOR_VIEWPORT", QByteArrayLiteral("offscreen"));
    qunsetenv("LUDUS_SHADER_WORKER_SOCKET");
    rhi::Shutdown();
    struct Guard final
    {
        QByteArray PreviousViewport;
        QByteArray PreviousSocket;
        ~Guard()
        {
            rhi::Shutdown();
            const auto restore = [](const char* name, const QByteArray& value) {
                if (value.isEmpty() == true)
                {
                    qunsetenv(name);
                }
                else
                {
                    qputenv(name, value);
                }
            };
            restore("LUDUS_EDITOR_VIEWPORT", PreviousViewport);
            restore("LUDUS_SHADER_WORKER_SOCKET", PreviousSocket);
        }
    } guard{previousViewport, previousSocket};
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const PreviewShaderSources sources =
        tests::CopySources(tests::GetCheckoutSources(), root + QStringLiteral("/shaders"));
    RenderScene scene;
    tests::MakeLitScene(root + QStringLiteral("/world"), scene);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    PreviewShaderReload* reload = viewport.GetShaderReloadOrNull();
    REQUIRE(reload != nullptr);
    REQUIRE(reload->SetSources(sources) == true);
    viewport.resize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    viewport.show();
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start() == true);
    REQUIRE(QTest::qWaitFor(
                [&viewport, &preview]() { return viewport.PresentedFrames() >= 2 || preview.Running() == false; },
                WORKER_TIMEOUT_MILLISECONDS) == true);
    if (preview.HasDevice() == false)
    {
        SKIP("No Vulkan device (install lavapipe for this smoke test)");
    }
    QWidget* canvas = viewport.findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport"));
    REQUIRE(canvas != nullptr);
    const QString shotDirectory = qEnvironmentVariable(SHOT_DIRECTORY_VARIABLE);
    const auto capture = [&canvas, &shotDirectory](const QString& name) {
        QTest::qWait(SETTLE_MILLISECONDS);
        const QImage image = canvas->grab().toImage();
        if (shotDirectory.isEmpty() == false)
        {
            REQUIRE(QDir().mkpath(shotDirectory) == true);
            CHECK(image.save(QStringLiteral("%1/%2.png").arg(shotDirectory, name)) == true);
        }
        return image;
    };
    const QImage before = capture(QStringLiteral("live-shaders-before"));

    QToolButton* toggle = viewport.findChild<QToolButton*>(QStringLiteral("viewportLiveShaders"));
    REQUIRE(toggle != nullptr);
    toggle->click();
    REQUIRE(WaitForSettled(*reload, WORKER_TIMEOUT_MILLISECONDS) == true);
    INFO(reload->GetStatusText().toStdString());
    REQUIRE(reload->GetState() == PreviewShaderReloadState::Idle);
    // The worker reproduces the build's artifacts, so turning live shaders on swaps nothing.
    CHECK(reload->GetStatusText().startsWith(QStringLiteral("Live shaders on")) == true);
    CHECK(preview.GetShaders().HasArtifacts() == false);
    const QString lit = QDir(sources.RecipeDirectory).filePath(QStringLiteral("renderer_lit.slang"));
    REQUIRE(reload->GetWatchedPaths().contains(lit) == true);
    const QByteArray original = tests::ReadText(lit);

    // Tint every lit surface red; the file watcher alone must notice the save.
    const QByteArray finalReturn = "    return float4(min(max(result, float3(0)), float3(65504)) * alpha, alpha);";
    REQUIRE(original.contains(finalReturn) == true);
    QByteArray tinted = original;
    tinted.replace(finalReturn, "    result = result * float3(1, 0.25, 0.25) + float3(0.35, 0, 0);\n" + finalReturn);
    tests::WriteText(lit, tinted);
    REQUIRE(
        QTest::qWaitFor([&reload]() { return reload->GetStatusText().startsWith(QStringLiteral("Shader reloaded")); },
                        WORKER_TIMEOUT_MILLISECONDS) == true);
    CHECK(viewport.GetShaderStatusText().startsWith(QStringLiteral("Shader reloaded")) == true);
    CHECK(preview.GetShaders().HasArtifact(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment) == true);
    const QImage after = capture(QStringLiteral("live-shaders-after-tint"));
    REQUIRE(after.size() == before.size());
    // The frame's centre shows lit cubes; tinting must make it redder.
    int64 redGain = 0;
    for (int32 y = after.height() / 4; y < after.height() * 3 / 4; ++y)
    {
        for (int32 x = after.width() / 4; x < after.width() * 3 / 4; ++x)
        {
            redGain += qRed(after.pixel(x, y)) - qRed(before.pixel(x, y));
        }
    }
    CHECK(redGain > 0);

    // A typo keeps the tinted shaders and shows the compiler's location in the overlay.
    QByteArray broken = tinted;
    broken.replace(finalReturn, "    float typo = undefinedLiveShaderName;\n" + finalReturn);
    tests::WriteText(lit, broken);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload->HasError() == true; }, WORKER_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload->GetState() == PreviewShaderReloadState::Failed);
    CHECK(viewport.GetShaderStatusText().contains(QDir::toNativeSeparators(lit) + QLatin1Char(':')) == true);
    CHECK(viewport.GetShaderStatusText().contains(QStringLiteral("undefinedLiveShaderName")) == true);
    CHECK(preview.Running() == true);
    const QImage error = capture(QStringLiteral("live-shaders-error-overlay"));
    CHECK(error.size() == before.size());

    // Fixing the typo clears the error without another swap.
    tests::WriteText(lit, tinted);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload->HasError() == false; }, WORKER_TIMEOUT_MILLISECONDS) == true);
    REQUIRE(WaitForSettled(*reload, WORKER_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload->GetState() == PreviewShaderReloadState::Idle);

    // A new resource needs new pipeline layouts: the editor asks for a restart instead.
    QByteArray extended = tinted;
    extended.replace(finalReturn, "    result += liveExtraTexture.Load(int3(0, 0, 0)).rgb * 1e-6;\n" + finalReturn);
    extended.replace("struct Input\n",
                     "LUDUS_SHADER_BINDING(30) Texture2D<float4> liveExtraTexture : register(t30);\nstruct Input\n");
    tests::WriteText(lit, extended);
    REQUIRE(QTest::qWaitFor([&reload]() { return reload->HasError() == true; }, WORKER_TIMEOUT_MILLISECONDS) == true);
    CHECK(reload->GetState() == PreviewShaderReloadState::RestartRequired);
    CHECK(viewport.GetShaderStatusText().contains(QStringLiteral("restart required")) == true);
    static_cast<void>(capture(QStringLiteral("live-shaders-restart-required")));
    toggle->click();
    CHECK(viewport.GetShaderStatusText().isEmpty() == true);
    CHECK(preview.GetShaders().HasArtifacts() == false);
    preview.Stop();
#else
    SKIP("The live shader worker test runs on Linux with the native preview");
#endif
}
