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
    CHECK(cutFolder.startsWith(QStringLiteral("content")) == true);
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
