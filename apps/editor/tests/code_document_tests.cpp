#include "internal/code_assistance.h"
#include "internal/code_document_controller.h"
#include "internal/code_edit_scope.h"
#include "internal/code_editor_panel.h"
#include "internal/code_formatter.h"
#include "internal/code_text.h"
#include "internal/document_history.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QInputMethodEvent>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QUrl>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
#if !defined(Q_OS_WASM)
#    if defined(Q_OS_MACOS)
constexpr Qt::KeyboardModifier ASSISTANCE_MODIFIER = Qt::MetaModifier;
#    else
constexpr Qt::KeyboardModifier ASSISTANCE_MODIFIER = Qt::ControlModifier;
#    endif
#endif
struct MemoryTextState final
{
    QByteArray Text;
    CodeTextSelection Selection;
    bool operator==(const MemoryTextState& other) const
    {
        return Text == other.Text;
    }
};

// Independent single-file document owner: the shared editor has no dependency
// on the render graph's schema, history payload, files or shader compiler.
class MemoryCodeDocument final : public CodeDocumentHost
{
public:
    explicit MemoryCodeDocument(const QByteArray& text)
    {
        mState.Text = text;
    }
    CodeDocumentSnapshot GetSnapshot() const noexcept override
    {
        return {{1, Session, mHistory.Revision()},
                {mState.Text.constData(), static_cast<usize>(mState.Text.size())},
                LimitBytes,
                true};
    }
    CodeEditStatus ApplyEdit(const CodeDocumentIdentity& expected,
                             const CodeTextEdit& edit,
                             CodeTextSelection before,
                             CodeTextSelection after,
                             bool shouldMerge) override
    {
        if (expected != GetSnapshot().Identity)
        {
            return CodeEditStatus::Conflict;
        }
        QByteArray candidate = mState.Text;
        candidate.replace(static_cast<isize>(edit.StartBytes),
                          static_cast<isize>(edit.RemovedBytes),
                          edit.Inserted.GetDataOrNull(),
                          static_cast<isize>(edit.Inserted.GetSize()));
        if (static_cast<usize>(candidate.size()) > LimitBytes)
        {
            return CodeEditStatus::Capacity;
        }
        if (candidate == mState.Text)
        {
            return CodeEditStatus::Ok;
        }
        MemoryTextState previous = mState;
        previous.Selection = before;
        const MemoryTextState next{candidate, after};
        if (mHistory.Commit(previous, next, shouldMerge) == false)
        {
            return CodeEditStatus::Conflict;
        }
        mState = next;
        return CodeEditStatus::Ok;
    }
    void BreakUndoGroup() noexcept override
    {
        mHistory.BreakGroup();
    }
    bool CanUndo() const noexcept override
    {
        return mHistory.CanUndo();
    }
    bool CanRedo() const noexcept override
    {
        return mHistory.CanRedo();
    }
    bool Undo()
    {
        return mHistory.Undo(mState);
    }
    bool Redo()
    {
        return mHistory.Redo(mState);
    }
    const MemoryTextState& GetState() const
    {
        return mState;
    }
    usize LimitBytes = 4096;
    uint64 Session = 1;

private:
    MemoryTextState mState;
    DocumentHistory<MemoryTextState> mHistory;
};

} // namespace

TEST_CASE("owned text history coalesces typing and routes undo without a Qt stack", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("prefix "));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QObject::connect(&editor, &CodeEditor::UndoRequested, &controller, [&]() {
        if (document.Undo() == true)
        {
            controller.Synchronize();
            controller.RestoreSelection(document.GetState().Selection);
        }
    });
    QObject::connect(&editor, &CodeEditor::RedoRequested, &controller, [&]() {
        if (document.Redo() == true)
        {
            controller.Synchronize();
            controller.RestoreSelection(document.GetState().Selection);
        }
    });
    editor.moveCursor(QTextCursor::End);
    QTest::keyClicks(&editor, QStringLiteral("word"));
    CHECK(document.GetState().Text == QByteArrayLiteral("prefix word"));
    CHECK(document.GetSnapshot().Identity.Revision == 4);
    CHECK(editor.document()->isUndoRedoEnabled() == false);
    CHECK(editor.document()->isUndoAvailable() == false);
    CHECK(editor.CanUndo() == true);
    editor.Undo();
    CHECK(document.GetState().Text == QByteArrayLiteral("prefix "));
    CHECK(editor.toPlainText() == QStringLiteral("prefix "));
    CHECK(editor.textCursor().position() == 7);
    CHECK(editor.CanRedo() == true);
    editor.Redo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix word"));
    CHECK(editor.textCursor().position() == 11);
    QTest::keyClicks(&editor, QStringLiteral(" next"));
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix word "));
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix word"));
    editor.Undo();
    CHECK(editor.toPlainText() == QStringLiteral("prefix "));
}

TEST_CASE("multi-line commands are one authoritative history entry", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("one\ntwo\nthree"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    editor.selectAll();
    editor.IndentSelection();
    CHECK(document.GetState().Text == QByteArrayLiteral("    one\n    two\n    three"));
    CHECK(document.GetSnapshot().Identity.Revision == 1);
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(editor.toPlainText() == QStringLiteral("one\ntwo\nthree"));
    CHECK(document.CanUndo() == false);
}

TEST_CASE("Unicode input and CRLF survive document history and formatting", "[editor][text][history]")
{
    CodeEditor editor;
    const QByteArray original = QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("文字😀\r\nnext").toUtf8();
    MemoryCodeDocument document(original);
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QSignalSpy errors(&controller, &CodeDocumentController::Failed);
    CHECK(editor.toPlainText().contains(QString::fromUtf8("文字😀\nnext")) == true);
    editor.GoToLine(2);
    QInputMethodEvent preedit(QStringLiteral("候"), {});
    QApplication::sendEvent(&editor, &preedit);
    CHECK(document.GetSnapshot().Identity.Revision == 0);
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("新😀"));
    QApplication::sendEvent(&editor, &commit);
    CHECK(document.GetState().Text == QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("文字😀\r\n新😀next").toUtf8());
    CHECK(errors.count() == 0);
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == original);
    editor.GoToLine(1);
    editor.moveCursor(QTextCursor::EndOfBlock);
    QTest::keyClick(&editor, Qt::Key_Return);
    CHECK(document.GetState().Text == QByteArray("\xEF\xBB\xBF") + QString::fromUtf8("文字😀\r\n\r\nnext").toUtf8());
}

TEST_CASE("rejected text input preserves owner and view without an undo entry", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("ab"));
    document.LimitBytes = 3;
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    QSignalSpy errors(&controller, &CodeDocumentController::Failed);
    editor.selectAll();
    QApplication::clipboard()->setText(QStringLiteral("oversized"));
    editor.paste();
    CHECK(document.GetState().Text == QByteArrayLiteral("ab"));
    CHECK(editor.toPlainText() == QStringLiteral("ab"));
    CHECK(document.CanUndo() == false);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("ab"));
    CHECK(errors.count() == 1);
}

TEST_CASE("stale formatting cannot replace current text or cross a binding", "[editor][text][history]")
{
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("text"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    const CodeTextCapture captured = controller.Capture();
    QTest::keyClicks(&editor, QStringLiteral("new"));
    const QByteArray edited = document.GetState().Text;
    CHECK(controller.ApplyServiceText(captured.Identity, QByteArrayLiteral("formatted"), {}) ==
          CodeEditStatus::Conflict);
    CHECK(document.GetState().Text == edited);
    const CodeTextCapture oldBinding = controller.Capture();
    ++document.Session;
    controller.Synchronize();
    CHECK(controller.ApplyServiceText(oldBinding.Identity, QByteArrayLiteral("formatted"), {}) ==
          CodeEditStatus::Conflict);
    CHECK(document.GetState().Text == edited);
}

TEST_CASE("UTF-8 positions distinguish bytes UTF-16 surrogates BOM and CRLF", "[editor][text][positions]")
{
    const QByteArray text = QString::fromUtf8("a😀\r\n文字").toUtf8();
    usize byte = 99;
    REQUIRE(GetCodeByteOffset(text, 3, byte) == true);
    CHECK(byte == 5);
    REQUIRE(GetCodeByteOffset(text, 4, byte) == true);
    CHECK(byte == 7);
    CHECK(GetCodeByteOffset(text, 2, byte) == false);
    CHECK(byte == 7);
    int32 position = 99;
    CHECK(GetCodeCursorPosition(text, 6, position) == false);
    CHECK(position == 99);
    REQUIRE(GetCodeCursorPosition(text, 7, position) == true);
    CHECK(position == 4);
    const QByteArray before = QString::fromUtf8("a😀z").toUtf8();
    const QByteArray after = QString::fromUtf8("a😁z").toUtf8();
    const CodeTextEdit edit = MakeCodeTextEdit(before, after);
    CHECK(edit.StartBytes == 1);
    CHECK(edit.RemovedBytes == 4);
    CHECK(edit.Inserted.GetSize() == 4);
}

#if !defined(Q_OS_WASM)
TEST_CASE("formatter applies one history group and preserves Unicode selection and CRLF", "[editor][text][format]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString program = FindShaderFormatter();
    if (program.isEmpty() == true)
    {
        SKIP("Pinned native clang-format is unavailable");
    }
    QFile style(QDir(directory.path()).filePath(QStringLiteral(".clang-format")));
    REQUIRE(style.open(QIODevice::WriteOnly) == true);
    style.write("BasedOnStyle: LLVM\nIndentWidth: 2\n");
    style.close();
    CodeEditor editor;
    const QByteArray initial =
        QString::fromUtf8("// 文字😀\r\nfloat4 Shade(){return float4(1,2,3,4);}\r\n"
                          "struct Payload{float4 Value;};\r\n"
                          "T Identity<T>(T value){return value;}\r\n"
                          "RWStructuredBuffer<Payload> Output;\r\n"
                          "[shader(\"compute\")]\r\n[numthreads(1,1,1)]\r\n"
                          "void ComputeMain(uint3 dispatchThreadId:SV_DispatchThreadID){Payload payload;"
                          "payload.Value=Identity<float4>(float4(1,2,3,4));Output[dispatchThreadId.x]=payload;}\r\n")
            .toUtf8();
    MemoryCodeDocument document(initial);
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    const isize byte = initial.indexOf("Shade");
    controller.RestoreSelection({static_cast<usize>(byte + 5), static_cast<usize>(byte)});
    CodeFormatter formatter(controller);
    QSignalSpy finished(&formatter, &CodeFormatter::Finished);
    REQUIRE(formatter.Start(
                MakeShaderFormatter(program, QDir(directory.path()).filePath(QStringLiteral("test.slang")))) == true);
    CHECK(formatter.IsRunning() == true);
    REQUIRE(finished.wait(12000) == true);
    INFO(formatter.GetMessage().toStdString());
    REQUIRE(finished.at(0).at(0).toBool() == true);
    CHECK(document.GetState().Text.contains("float4 Shade() { return float4(1, 2, 3, 4); }") == true);
    CHECK(document.GetState().Text.contains("\r\n") == true);
    CHECK(document.GetState().Text.contains("[numthreads(1, 1, 1)]") == true);
    CHECK(document.GetState().Text.contains("Identity<float4>") == true);
    CHECK(editor.textCursor().selectedText() == QStringLiteral("Shade"));
    CHECK(editor.textCursor().anchor() > editor.textCursor().position());
    CHECK(document.GetSnapshot().Identity.Revision == 1);
    const QByteArray formatted = document.GetState().Text;
    REQUIRE(document.Undo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == initial);
    REQUIRE(document.Redo() == true);
    controller.Synchronize();
    CHECK(document.GetState().Text == formatted);
    REQUIRE(formatter.Start(
                MakeShaderFormatter(program, QDir(directory.path()).filePath(QStringLiteral("test.slang")))) == true);
    REQUIRE(finished.wait(12000) == true);
    CHECK(document.GetSnapshot().Identity.Revision == 3); // No-op format adds no history.
}

TEST_CASE("formatter failures cancellation and stale results preserve the authoritative source",
          "[editor][text][format]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const CodeFormatterTool profile = MakeShaderFormatter({}, {});
    const QString fixture = QDir(directory.path()).filePath(QStringLiteral("formatter-") + profile.MinimumVersion);
    REQUIRE(QFile::copy(QString::fromUtf8(LUDUS_FORMATTER_FIXTURE), fixture) == true);
    REQUIRE(QFile::setPermissions(fixture, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) == true);
    CodeEditor editor;
    MemoryCodeDocument document(QByteArrayLiteral("intx;"));
    CodeDocumentController controller(editor);
    controller.Bind(&document);
    CodeFormatter formatter(controller);
    QSignalSpy finished(&formatter, &CodeFormatter::Finished);
    CodeFormatterTool tool{fixture, {QStringLiteral("success")}, profile.MinimumVersion};
    bool shouldSucceed = false;
    SECTION("success")
    {
        shouldSucceed = true;
    }
    SECTION("move caret during formatting")
    {
        shouldSucceed = true;
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &controller, [&]() { editor.moveCursor(QTextCursor::End); });
    }
    SECTION("split Unicode scalar")
    {
        editor.setPlainText(QString::fromUtf8("😀text"));
        tool.Arguments = {QStringLiteral("split")};
    }
    SECTION("newer release of the pinned major")
    {
        shouldSucceed = true;
        tool.MinimumVersion = profile.MinimumVersion.section(QLatin1Char('.'), 0, 0) + QStringLiteral(".0.0");
    }
    SECTION("older release of the pinned major")
    {
        tool.MinimumVersion = profile.MinimumVersion.section(QLatin1Char('.'), 0, 0) + QStringLiteral(".999.0");
    }
    SECTION("wrong version")
    {
        tool.MinimumVersion = QStringLiteral("0.0.0");
    }
    SECTION("malformed version")
    {
        tool.MinimumVersion = QStringLiteral("23.1");
    }
    SECTION("missing executable")
    {
        tool.Program += QStringLiteral("-missing");
    }
    SECTION("process failure")
    {
        tool.Arguments = {QStringLiteral("failure")};
    }
    SECTION("malformed XML")
    {
        tool.Arguments = {QStringLiteral("malformed")};
    }
    SECTION("invalid range")
    {
        tool.Arguments = {QStringLiteral("range")};
    }
    SECTION("transport capacity")
    {
        tool.Arguments = {QStringLiteral("transport")};
    }
    SECTION("decoded capacity")
    {
        document.LimitBytes = 5;
    }
    SECTION("timeout")
    {
        tool.Arguments = {QStringLiteral("timeout")};
    }
    SECTION("cancel")
    {
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &formatter, &CodeFormatter::Cancel);
    }
    SECTION("edit while formatting")
    {
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &controller, [&]() { editor.insertPlainText(QStringLiteral("//new\n")); });
    }
    SECTION("replace binding while formatting")
    {
        tool.Arguments = {QStringLiteral("delayed")};
        QTimer::singleShot(20, &controller, [&]() {
            ++document.Session;
            controller.Synchronize();
        });
    }
    const QByteArray captured = document.GetState().Text;
    REQUIRE(formatter.Start(tool) == true);
    CHECK(formatter.Start(tool) == false); // One in-flight job.
    REQUIRE(finished.wait(12000) == true);
    INFO(formatter.GetMessage().toStdString());
    CHECK(finished.at(0).at(0).toBool() == shouldSucceed);
    CHECK(formatter.IsRunning() == false);
    if (shouldSucceed == true)
    {
        CHECK(document.GetState().Text == QByteArrayLiteral("int x;"));
    }
    else
    {
        CHECK(document.GetState().Text.contains(captured) == true);
    }
    CHECK(document.GetSnapshot().Identity.Revision <= 1);
    // Failure/cancellation leaves the service reusable.
    document.LimitBytes = 4096;
    tool = {fixture, {QStringLiteral("success")}, profile.MinimumVersion};
    REQUIRE(formatter.Start(tool) == true);
    REQUIRE(finished.wait(12000) == true);
    CHECK(formatter.IsRunning() == false);
}
#endif

TEST_CASE("unbound editor publishes native undo availability for its command host", "[editor][text][history]")
{
    CodeEditor editor;
    QSignalSpy availability(&editor, &CodeEditor::HistoryAvailabilityChanged);
    editor.insertPlainText(QStringLiteral("text"));
    CHECK(editor.CanUndo() == true);
    CHECK(availability.count() > 0);
    const int32 beforeUndo = static_cast<int32>(availability.count());
    editor.Undo();
    CHECK(editor.CanRedo() == true);
    CHECK(availability.count() > beforeUndo);
}

#if !defined(Q_OS_WASM)
TEST_CASE("Shader completion maps UTF-16 ranges and additional edits into one draft undo group",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host(
        QByteArray("// additional-edit \xF0\x9F\x98\x80\r\nvoid Main() {\r\n    scene.Ve\r\n}\r\n"));
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              QStringLiteral("// generated\n"),
                              QStringLiteral("\n// generated end"),
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.resize(800, 500);
    editor.show();
    editor.setFocus();
    editor.GoToLine(3, 13);
    const QByteArray original = host.GetState().Text;
    QTest::keyClick(&editor, Qt::Key_Space, ASSISTANCE_MODIFIER);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    REQUIRE(popupOrNull->count() == 1);
    REQUIRE(popupOrNull->item(0)->toolTip().contains(QStringLiteral("&lt;b&gt;untrusted markup&lt;/b&gt;")));
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text.contains("scene.Version\r\n"));
    REQUIRE(host.GetState().Text.startsWith("// added\r\n// additional-edit"));
    REQUIRE(host.GetState().Text.contains("\xF0\x9F\x98\x80"));
    REQUIRE(host.Undo());
    document.Synchronize(true);
    REQUIRE(host.GetState().Text == original);
    REQUIRE(host.CanUndo() == false);
}

TEST_CASE("Shader assistance rejects generated edits and unsolicited snippets", "[editor][text][assistance]")
{
    for (const QByteArray& marker : {QByteArray("generated-edit"), QByteArray("snippet")})
    {
        MemoryCodeDocument host("// " + marker + "\nscene.Ve");
        CodeEditor editor;
        CodeDocumentController document(editor);
        document.Bind(&host);
        editor.SetLanguage(CodeLanguage::Slang);
        CodeAssistance assistance(editor, document);
        QTemporaryDir temporary;
        QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
            assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  temporary.path(),
                                  QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  QStringLiteral("// generated\n"),
                                  {},
                                  {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
        });
        editor.show();
        editor.setFocus();
        editor.moveCursor(QTextCursor::End);
        QSignalSpy status(&assistance, &CodeAssistance::StatusChanged);
        assistance.Request(false);
        QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
        REQUIRE(popupOrNull != nullptr);
        REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
        QTest::keyClick(&editor, Qt::Key_Return);
        REQUIRE(host.CanUndo() == false);
        REQUIRE(host.GetState().Text.endsWith("scene.Ve"));
        REQUIRE(status.last().at(0).toString().isEmpty() == false);
    }
}

TEST_CASE("Shader signature help highlights the active parameter and escapes documentation",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host("EvaluateMaterial(scene, ");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    QTest::keyClick(&editor, Qt::Key_Space, ASSISTANCE_MODIFIER | Qt::ShiftModifier);
    QLabel* signatureOrNull = editor.findChild<QLabel*>(QStringLiteral("codeSignatureHelp"));
    REQUIRE(signatureOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([signatureOrNull]() { return signatureOrNull->isVisible() == true; }));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("<b><u>SurfaceHit hit</u></b>")));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("&lt;script&gt;")));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("/private/tmp/generated")) == false);
    QTest::keyClick(&editor, Qt::Key_PageDown, Qt::AltModifier);
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("uint index")));
    QTest::keyClick(&editor, Qt::Key_Escape);
    REQUIRE(signatureOrNull->isVisible() == false);
    REQUIRE(host.CanUndo() == false);
}

TEST_CASE("Shader replies are discarded after text navigation binding and context changes",
          "[editor][text][assistance]")
{
    MemoryCodeDocument host("// hold-response\nscene.Ve");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QTest::qWait(100);
    host.Session = 2;
    document.Synchronize();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false); // releases the previous binding's reply
    QTest::qWait(100);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(popupOrNull->isVisible() == false);
    assistance.ClearContext();
    editor.insertPlainText(QStringLiteral("r"));
    REQUIRE(host.GetState().Text.endsWith("scene.Ver"));
    REQUIRE(popupOrNull->isVisible() == false);
}

TEST_CASE("Slang protocol failures and crashes preserve text and allow a fresh retry", "[editor][text][assistance]")
{
    for (const QByteArray& marker : {QByteArray("malformed-server"), QByteArray("crash-server")})
    {
        MemoryCodeDocument host("// " + marker + "\nscene.Ve");
        CodeEditor editor;
        CodeDocumentController document(editor);
        document.Bind(&host);
        editor.SetLanguage(CodeLanguage::Slang);
        CodeAssistance assistance(editor, document);
        QTemporaryDir temporary;
        QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
            assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  temporary.path(),
                                  QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  {},
                                  {},
                                  {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
        });
        editor.show();
        editor.setFocus();
        editor.moveCursor(QTextCursor::End);
        QSignalSpy status(&assistance, &CodeAssistance::StatusChanged);
        assistance.Request(false);
        REQUIRE(QTest::qWaitFor([&status]() {
            return status.isEmpty() == false && status.last().at(0).toString().contains(QStringLiteral("Slang"));
        }));
        REQUIRE(host.CanUndo() == false);
        const CodeTextCapture capture = document.Capture();
        const QByteArray repaired("// repaired\nscene.Ve");
        REQUIRE(document.ApplyServiceText(capture.Identity,
                                          repaired,
                                          {static_cast<usize>(repaired.size()), static_cast<usize>(repaired.size())}) ==
                CodeEditStatus::Ok);
        assistance.Request(false);
        QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
        REQUIRE(popupOrNull != nullptr);
        REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
        QTest::keyClick(&editor, Qt::Key_Return);
        REQUIRE(host.GetState().Text.endsWith("scene.Version"));
    }
}

TEST_CASE("Escape cancels pending shader assistance before a delayed reply", "[editor][text][assistance]")
{
    MemoryCodeDocument host("// hold-response\nscene.Ve");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QTest::qWait(100);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(100);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(popupOrNull->isVisible() == false);
    REQUIRE(host.CanUndo() == false);
}

TEST_CASE("Typing while shader assistance starts retains the latest completion intent", "[editor][text][assistance]")
{
    MemoryCodeDocument host("scene.Ve");
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        QTimer::singleShot(200, &assistance, [&]() {
            assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                                  temporary.path(),
                                  QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                                  {},
                                  {},
                                  {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
        });
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    assistance.Request(false);
    QTest::keyClicks(&editor, QStringLiteral("r"));
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text == QByteArray("scene.Version"));
    REQUIRE(host.Undo());
    document.Synchronize(true);
    REQUIRE(host.GetState().Text == QByteArray("scene.Ver"));
}

TEST_CASE("Completion capacity rejection is visible beside the unchanged source", "[editor][text][assistance]")
{
    MemoryCodeDocument host("scene.Ve");
    host.LimitBytes = static_cast<usize>(host.GetState().Text.size());
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(QString::fromUtf8(LUDUS_TEST_PYTHON),
                              temporary.path(),
                              QUrl::fromLocalFile(temporary.filePath(QStringLiteral("unit.slang"))).toString(),
                              {},
                              {},
                              {QString::fromUtf8(LUDUS_SLANG_FIXTURE)});
    });
    editor.show();
    editor.setFocus();
    editor.moveCursor(QTextCursor::End);
    QSignalSpy status(&assistance, &CodeAssistance::StatusChanged);
    assistance.Request(false);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }));
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text == QByteArray("scene.Ve"));
    REQUIRE(host.CanUndo() == false);
    REQUIRE(status.last().at(0).toString().contains(QStringLiteral("byte limit")));
}

TEST_CASE("Native Slang completion and signature help share the versioned editing controller",
          "[editor][text][assistance][slang-native]")
{
    const QString server = qEnvironmentVariable("LUDUS_TEST_SLANGD");
    if (server.isEmpty() == true)
    {
        SKIP("Set LUDUS_TEST_SLANGD to exercise the installed Slang server.");
    }
    const QString prefix = QStringLiteral("struct SceneContext { uint Version; };\nfloat EvaluateMaterial(SceneContext "
                                          "scene, float value) { return value; }\n");
    const QString source = QStringLiteral("// Unicode: \U0001f600\nvoid Main(SceneContext scene) { scene.Ve; }\n");
    MemoryCodeDocument host(source.toUtf8());
    CodeEditor editor;
    CodeDocumentController document(editor);
    document.Bind(&host);
    editor.SetLanguage(CodeLanguage::Slang);
    CodeAssistance assistance(editor, document);
    QTemporaryDir temporary;
    QFile unit(temporary.filePath(QStringLiteral("unit.slang")));
    REQUIRE(unit.open(QIODevice::WriteOnly) == true);
    REQUIRE(unit.write((prefix + source).toUtf8()) > 0);
    unit.close();
    QObject::connect(&assistance, &CodeAssistance::ContextRequested, &editor, [&]() {
        assistance.SetContext(server, temporary.path(), QUrl::fromLocalFile(unit.fileName()).toString(), prefix, {});
    });
    editor.resize(900, 480);
    editor.show();
    editor.setFocus();
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(static_cast<int32>(source.indexOf(QStringLiteral("Ve"))) + 2);
    editor.setTextCursor(cursor);
    assistance.Request(false);
    QListWidget* popupOrNull = editor.findChild<QListWidget*>(QStringLiteral("codeCompletionList"));
    REQUIRE(popupOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([popupOrNull]() { return popupOrNull->isVisible() == true; }, 15000));
    REQUIRE(popupOrNull->item(0)->text().startsWith(QStringLiteral("Version")));
    const QString capture = qEnvironmentVariable("LUDUS_ASSISTANCE_CAPTURE_DIR");
    if (capture.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(capture) == true);
        REQUIRE(editor.grab().save(QDir(capture).filePath(QStringLiteral("source.png"))) == true);
        REQUIRE(popupOrNull->grab().save(QDir(capture).filePath(QStringLiteral("completion.png"))) == true);
    }
    QTest::keyClick(&editor, Qt::Key_Tab);
    REQUIRE(host.GetState().Text.contains("scene.Version;"));
    const QByteArray call("void Main(SceneContext scene) { EvaluateMaterial(scene, ); }\n");
    const usize caret = static_cast<usize>(call.indexOf(", ")) + 2;
    REQUIRE(document.ApplyServiceText(document.Capture().Identity, call, {caret, caret}) == CodeEditStatus::Ok);
    assistance.Request(true);
    QLabel* signatureOrNull = editor.findChild<QLabel*>(QStringLiteral("codeSignatureHelp"));
    REQUIRE(signatureOrNull != nullptr);
    REQUIRE(QTest::qWaitFor([signatureOrNull]() { return signatureOrNull->isVisible() == true; }, 15000));
    REQUIRE(signatureOrNull->text().contains(QStringLiteral("<b><u>float value</u></b>")));
    if (capture.isEmpty() == false)
    {
        REQUIRE(signatureOrNull->grab().save(QDir(capture).filePath(QStringLiteral("signature.png"))) == true);
    }
}

#endif
