// Multiple cursors (CE06, design section 5): secondary cursors live beside the
// view's single Qt cursor, every text operation is distributed across the whole
// set as one CodeEditSet (one undo step), movement moves every cursor, and the
// mapping stays one pass over the text regardless of the cursor count. These run
// offscreen against the real CodeFileDocument, CodeDocumentController and
// CodeMultiCursor; a key event is sent so the production keyPressEvent path runs.

#include "internal/code_document.h"
#include "internal/code_document_controller.h"
#include "internal/code_editor.h"
#include "internal/code_file_document.h"
#include "internal/code_multi_cursor.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <QTextCursor>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
[[nodiscard]] QString WriteFile(const QTemporaryDir& directory, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(QStringLiteral("multi.txt"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

[[nodiscard]] CodeFileDocument OpenHost(const QTemporaryDir& directory, const QByteArray& bytes)
{
    return CodeFileDocument(LoadCodeFile(WriteFile(directory, bytes)), 1);
}

[[nodiscard]] QByteArray DraftOf(const CodeFileDocument& host)
{
    const CodeDocumentSnapshot snapshot = host.GetSnapshot();
    return QByteArray(snapshot.Text.GetDataOrNull(), static_cast<qsizetype>(snapshot.Text.GetSize()));
}

// An editor bound to a real file host through a controller, as a code tab binds
// its own. The controller is deleted first because its destructor disconnects
// from the editor and clears the multi-cursor's controller pointer.
struct BoundView final
{
    explicit BoundView(CodeFileDocument& inoutHost)
        : Editor(new CodeEditor()), Controller(new CodeDocumentController(*Editor))
    {
        Controller->Bind(&inoutHost);
    }
    ~BoundView()
    {
        delete Controller;
        delete Editor;
    }
    BoundView(const BoundView&) = delete;
    BoundView& operator=(const BoundView&) = delete;

    CodeEditor* Editor;
    CodeDocumentController* Controller;
};

// Place the primary caret at a document position with no selection.
void PutCaret(CodeEditor& editor, int32 position)
{
    QTextCursor cursor(editor.document());
    cursor.setPosition(position);
    editor.setTextCursor(cursor);
}

// Add a collapsed secondary cursor at a position through the public overlay.
void AddSecondaryCaret(CodeEditor& editor, int32 position)
{
    QList<CodeSecondaryCursor> cursors = editor.GetMultiCursor().GetSecondaryCursors();
    cursors.append(CodeSecondaryCursor{ .Anchor = position, .Caret = position });
    editor.GetMultiCursor().AdoptSecondaryCursors(cursors);
}

// Send one key press through the production keyPressEvent path.
void SendKey(CodeEditor& editor, int key, Qt::KeyboardModifiers modifiers, const QString& text)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(&editor, &event);
}

constexpr qint64 LINEAR_WORK_CEILING_MS = 2000;

[[nodiscard]] QByteArray RepeatLine(const QByteArray& line, qsizetype count)
{
    QByteArray text;
    text.reserve(line.size() * count);
    for (qsizetype index = 0; index < count; ++index)
    {
        text.append(line);
    }
    return text;
}
} // namespace

TEST_CASE("Typing is distributed across every cursor as one undo step", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("ab\ncd\nef\n"));
    BoundView view(host);

    // A caret at the start of each of the three lines (positions 0, 3, 6).
    PutCaret(*view.Editor, 0);
    AddSecondaryCaret(*view.Editor, 3);
    AddSecondaryCaret(*view.Editor, 6);
    REQUIRE(view.Editor->GetMultiCursor().GetCursorCount() == 3);

    SendKey(*view.Editor, Qt::Key_X, Qt::NoModifier, QStringLiteral("X"));
    CHECK(DraftOf(host) == QByteArray("Xab\nXcd\nXef\n"));
    // Two secondary carets remain; each advanced past its inserted "X".
    CHECK(view.Editor->GetMultiCursor().GetCursorCount() == 3);

    // The whole distribution is one undo step restoring all three lines at once.
    QList<CodeTextSelection> undone;
    REQUIRE(host.Undo(undone) == true);
    CHECK(DraftOf(host) == QByteArray("ab\ncd\nef\n"));
    CHECK(host.CanUndo() == false);
    REQUIRE(undone.size() == 3);
}

TEST_CASE("Backspace at every caret removes the preceding character", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("aXb\ncXd\n"));
    BoundView view(host);

    // A caret just after each 'X' (positions 2 and 6).
    PutCaret(*view.Editor, 2);
    AddSecondaryCaret(*view.Editor, 6);
    SendKey(*view.Editor, Qt::Key_Backspace, Qt::NoModifier, QString{});
    CHECK(DraftOf(host) == QByteArray("ab\ncd\n"));
}

TEST_CASE("A selection at every cursor is replaced by typed text", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("one one\n"));
    BoundView view(host);

    // Select the first "one" as the primary and the second as a secondary.
    QTextCursor primary(view.Editor->document());
    primary.setPosition(0);
    primary.setPosition(3, QTextCursor::KeepAnchor);
    view.Editor->setTextCursor(primary);
    QList<CodeSecondaryCursor> cursors;
    cursors.append(CodeSecondaryCursor{ .Anchor = 4, .Caret = 7 });
    view.Editor->GetMultiCursor().AdoptSecondaryCursors(cursors);

    SendKey(*view.Editor, Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    CHECK(DraftOf(host) == QByteArray("z z\n"));
}

TEST_CASE("Add next occurrence selects the word, then each following match", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("foo bar foo baz foo\n"));
    BoundView view(host);
    PutCaret(*view.Editor, 1); // Inside the first "foo".

    view.Editor->AddNextOccurrence(); // Selects the word under the caret.
    CHECK(view.Editor->GetMultiCursor().GetCursorCount() == 1);
    CHECK(view.Editor->textCursor().selectedText() == QStringLiteral("foo"));

    view.Editor->AddNextOccurrence(); // Adds the second "foo".
    CHECK(view.Editor->GetMultiCursor().GetCursorCount() == 2);
    view.Editor->AddNextOccurrence(); // Adds the third "foo".
    CHECK(view.Editor->GetMultiCursor().GetCursorCount() == 3);

    // Typing replaces every selected "foo" in one step.
    SendKey(*view.Editor, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    CHECK(DraftOf(host) == QByteArray("q bar q baz q\n"));
}

TEST_CASE("Add all occurrences puts a cursor on every match in one pass", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("x x x x\n"));
    BoundView view(host);
    // Select the first "x" as the primary.
    QTextCursor primary(view.Editor->document());
    primary.setPosition(0);
    primary.setPosition(1, QTextCursor::KeepAnchor);
    view.Editor->setTextCursor(primary);

    view.Editor->AddAllOccurrences();
    CHECK(view.Editor->GetMultiCursor().GetCursorCount() == 4);

    SendKey(*view.Editor, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    CHECK(DraftOf(host) == QByteArray("y y y y\n"));
}

TEST_CASE("A cursor added below types on both lines at the same column", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("abcd\nefgh\n"));
    BoundView view(host);
    PutCaret(*view.Editor, 2); // Column 2 of the first line.

    view.Editor->AddCursorBelow();
    REQUIRE(view.Editor->GetMultiCursor().GetCursorCount() == 2);
    SendKey(*view.Editor, Qt::Key_Bar, Qt::NoModifier, QStringLiteral("|"));
    CHECK(DraftOf(host) == QByteArray("ab|cd\nef|gh\n"));
}

TEST_CASE("Escape collapses the cursor set to the primary", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("ab\ncd\n"));
    BoundView view(host);
    PutCaret(*view.Editor, 0);
    AddSecondaryCaret(*view.Editor, 3);
    REQUIRE(view.Editor->GetMultiCursor().HasSecondaryCursors() == true);

    SendKey(*view.Editor, Qt::Key_Escape, Qt::NoModifier, QString{});
    CHECK(view.Editor->GetMultiCursor().HasSecondaryCursors() == false);
    CHECK(view.Editor->GetMultiCursor().GetCursorCount() == 1);
}

TEST_CASE("Paste gives one clipboard line to each cursor when the counts match", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("a:\nb:\n"));
    BoundView view(host);
    // A caret after each colon (positions 2 and 5).
    PutCaret(*view.Editor, 2);
    AddSecondaryCaret(*view.Editor, 5);

    // The view routes a paste with secondary cursors to the overlay (see
    // insertFromMimeData); the distribution itself is exercised directly here so
    // the test does not depend on the offscreen platform clipboard.
    REQUIRE(view.Editor->GetMultiCursor().HandlePaste(QStringLiteral("one\ntwo")) == true);
    CHECK(DraftOf(host) == QByteArray("a:one\nb:two\n"));
}

TEST_CASE("Paste gives the whole clipboard to each cursor when the counts differ", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CodeFileDocument host = OpenHost(directory, QByteArray("a:\nb:\n"));
    BoundView view(host);
    PutCaret(*view.Editor, 2);
    AddSecondaryCaret(*view.Editor, 5);

    // Three lines, two cursors: each cursor gets the whole three-line text.
    REQUIRE(view.Editor->GetMultiCursor().HandlePaste(QStringLiteral("x\ny\nz")) == true);
    CHECK(DraftOf(host) == QByteArray("a:x\ny\nz\nb:x\ny\nz\n"));
}

TEST_CASE("Multi-cursor typing maps correctly across CRLF and non-ASCII text", "[code-editor][multi-cursor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    // A four-byte emoji and a two-byte scalar precede the carets, and the file
    // uses CRLF, so editor positions and byte offsets diverge.
    const QByteArray original("\xF0\x9F\x98\x80 a\r\n\xC3\xA9 b\r\n");
    CodeFileDocument host = OpenHost(directory, original);
    BoundView view(host);

    // Decoded, the emoji is two UTF-16 units: line 1 is "<emoji> a", caret after
    // 'a' is position 4; line 2 is "<e-acute> b", caret after 'b' is position 8.
    PutCaret(*view.Editor, 4);
    AddSecondaryCaret(*view.Editor, 8);
    SendKey(*view.Editor, Qt::Key_Exclam, Qt::NoModifier, QStringLiteral("!"));
    CHECK(DraftOf(host) == QByteArray("\xF0\x9F\x98\x80 a!\r\n\xC3\xA9 b!\r\n"));
}

TEST_CASE("Typing at tens of thousands of cursors stays linear", "[code-editor][multi-cursor]")
{
    constexpr qsizetype LINES = 40000;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QByteArray original = RepeatLine(QByteArray("value\n"), LINES);
    CodeFileDocument host = OpenHost(directory, original);
    BoundView view(host);

    // One caret at the start of every line: the primary on line 0, a secondary on
    // each of the rest. "value\n" is six bytes, so line N starts at 6*N.
    PutCaret(*view.Editor, 0);
    QList<CodeSecondaryCursor> cursors;
    cursors.reserve(static_cast<qsizetype>(LINES) - 1);
    for (qsizetype line = 1; line < LINES; ++line)
    {
        const int32 position = static_cast<int32>(line) * 6;
        cursors.append(CodeSecondaryCursor{ .Anchor = position, .Caret = position });
    }
    view.Editor->GetMultiCursor().AdoptSecondaryCursors(cursors);
    REQUIRE(view.Editor->GetMultiCursor().GetCursorCount() == static_cast<int32>(LINES));

    QElapsedTimer timer;
    timer.start();
    SendKey(*view.Editor, Qt::Key_Greater, Qt::NoModifier, QStringLiteral(">"));
    const qint64 elapsedMs = timer.elapsed();

    CHECK(DraftOf(host) == RepeatLine(QByteArray(">value\n"), LINES));
    CHECK(elapsedMs < LINEAR_WORK_CEILING_MS);

    // Still one undo step for the whole distribution.
    QList<CodeTextSelection> undone;
    REQUIRE(host.Undo(undone) == true);
    CHECK(DraftOf(host) == original);
}
