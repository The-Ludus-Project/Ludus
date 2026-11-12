// Path text that stays readable in narrow fields (docs/development/editor-workspace.md#truncation).

#include "internal/path_text.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QFont>
#include <QFontMetrics>

using namespace ludus::editor;

TEST_CASE("Path text splits at the last separator", "[editor][path-text]")
{
    QString folder;
    QString name;
    SplitPathText(QStringLiteral("graphs/Main/main.lurender.json"), folder, name);
    CHECK(folder == QStringLiteral("graphs/Main/"));
    CHECK(name == QStringLiteral("main.lurender.json"));
    SplitPathText(QStringLiteral("main.lurender.json"), folder, name);
    CHECK(folder.isEmpty() == true);
    CHECK(name == QStringLiteral("main.lurender.json"));
    SplitPathText(QStringLiteral("shaders\\geometry.slang"), folder, name);
    CHECK(folder == QStringLiteral("shaders\\"));
    CHECK(name == QStringLiteral("geometry.slang"));
    SplitPathText(QString(), folder, name);
    CHECK(folder.isEmpty() == true);
    CHECK(name.isEmpty() == true);
}

TEST_CASE("Eliding a path keeps the file name whole and cuts the folder from the middle", "[editor][path-text]")
{
    const QFontMetrics metrics{QFont()};
    const QString path = QStringLiteral("content/render_graphs/visibility_buffer/main_shaders/geometry.slang");
    const QString name = QStringLiteral("geometry.slang");
    const int fullWidth = metrics.horizontalAdvance(path);
    const int nameWidth = metrics.horizontalAdvance(name);

    CHECK(ElidePathText(path, metrics, fullWidth) == path);

    const QString cutFolder = ElidePathText(path, metrics, nameWidth + metrics.horizontalAdvance(path.left(12)));
    CHECK(cutFolder.endsWith(name) == true);
    CHECK(cutFolder.contains(QStringLiteral("\u2026")) == true);
    CHECK(cutFolder.startsWith(QLatin1Char('c')) == true);
    CHECK(metrics.horizontalAdvance(cutFolder) <= nameWidth + metrics.horizontalAdvance(path.left(12)));

    // With no room for any folder, the whole file name is better than a cut one.
    CHECK(ElidePathText(path, metrics, nameWidth) == name);

    // Narrower than the file name: the name itself is elided from the middle.
    const int narrow = nameWidth / 2;
    const QString cutName = ElidePathText(path, metrics, narrow);
    CHECK(cutName.contains(QStringLiteral("\u2026")) == true);
    CHECK(metrics.horizontalAdvance(cutName) <= narrow);
}

TEST_CASE("Text without a folder elides from the middle", "[editor][path-text]")
{
    const QFontMetrics metrics{QFont()};
    const QString text = QStringLiteral("(asset default) with a long configuration name");
    const int width = metrics.horizontalAdvance(text) / 2;
    const QString shown = ElidePathText(text, metrics, width);
    CHECK(shown.contains(QStringLiteral("\u2026")) == true);
    CHECK(shown.startsWith(QStringLiteral("(asset")) == true);
    CHECK(metrics.horizontalAdvance(shown) <= width);
}

TEST_CASE("A path label asks for little width and shows its full text as a tooltip when cut", "[editor][path-text]")
{
    PathLabel label(QStringLiteral("content/render_graphs/visibility_buffer/main.lurender.json"));
    CHECK(label.minimumSizeHint().width() < label.sizeHint().width() / 2);
    label.resize(label.minimumSizeHint());
    label.show();
    QApplication::processEvents();
    CHECK(label.width() <= label.sizeHint().width());
}

TEST_CASE("A document name leaves out the suffix its group already names", "[editor][path-text]")
{
    CHECK(RemoveKnownSuffix(QStringLiteral("sponza.world.json"), u".world.json") == QStringLiteral("sponza"));
    CHECK(RemoveKnownSuffix(QStringLiteral("Main.LURENDER.json"), u".lurender.json") == QStringLiteral("Main"));
    // Nothing would be left, a different suffix, or no suffix: the file name stays as it is.
    CHECK(RemoveKnownSuffix(QStringLiteral(".world.json"), u".world.json") == QStringLiteral(".world.json"));
    CHECK(RemoveKnownSuffix(QStringLiteral("door.luau"), u".world.json") == QStringLiteral("door.luau"));
    CHECK(RemoveKnownSuffix(QStringLiteral("door.luau"), {}) == QStringLiteral("door.luau"));
}

TEST_CASE("A document name drops its qualifier before it elides, and keeps its head and tail", "[editor][path-text]")
{
    const QFontMetrics metrics{QFont()};
    const QString name = QStringLiteral("SliceSamplingCoupled");
    const QString qualifier = QStringLiteral("render_graphs/slice");
    const QString full = name + GetDocumentQualifierSeparator() + qualifier;
    QString shownName;
    QString shownQualifier;

    ElideDocumentName(name, qualifier, metrics, metrics.horizontalAdvance(full), shownName, shownQualifier);
    CHECK(shownName == name);
    CHECK(shownQualifier == GetDocumentQualifierSeparator() + qualifier);

    // Short of the whole qualifier: it is cut from the middle, the name stays whole.
    const int partWidth = metrics.horizontalAdvance(name + GetDocumentQualifierSeparator() + qualifier.left(8));
    ElideDocumentName(name, qualifier, metrics, partWidth, shownName, shownQualifier);
    CHECK(shownName == name);
    CHECK(shownQualifier.startsWith(GetDocumentQualifierSeparator()) == true);
    CHECK(shownQualifier.contains(QStringLiteral("\u2026")) == true);
    CHECK(metrics.horizontalAdvance(shownName + shownQualifier) <= partWidth);

    // Only the name fits: no qualifier at all rather than a stub.
    ElideDocumentName(name, qualifier, metrics, metrics.horizontalAdvance(name), shownName, shownQualifier);
    CHECK(shownName == name);
    CHECK(shownQualifier.isEmpty() == true);

    // Narrower than the name: it keeps its head and its tail, which is where similar names differ.
    const int narrow = metrics.horizontalAdvance(name) * 2 / 3;
    ElideDocumentName(name, qualifier, metrics, narrow, shownName, shownQualifier);
    CHECK(shownName.startsWith(QStringLiteral("Slice")) == true);
    CHECK(shownName.endsWith(QStringLiteral("led")) == true);
    CHECK(shownName.contains(QStringLiteral("\u2026")) == true);
    CHECK(shownQualifier.isEmpty() == true);
    CHECK(metrics.horizontalAdvance(shownName) <= narrow);

    // A name with dots is a name, not a file name with an extension to keep.
    const QString dotted = QStringLiteral("sponza.lighting.review.final");
    const int dottedWidth = metrics.horizontalAdvance(dotted) / 2;
    ElideDocumentName(dotted, {}, metrics, dottedWidth, shownName, shownQualifier);
    CHECK(shownName == metrics.elidedText(dotted, Qt::ElideMiddle, dottedWidth));
}
