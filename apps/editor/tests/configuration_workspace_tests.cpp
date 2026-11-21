#include "internal/configuration_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QFile>
#include <QTableWidget>
#include <QTemporaryDir>

using namespace ludus::editor;
using namespace ludus::foundation::config;
TEST_CASE("Configuration workspace shares runtime schema and saves sparse preferences", "[editor][configuration]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    ConfigurationWorkspace workspace;
    auto* table = workspace.findChild<QTableWidget*>(QStringLiteral("configurationSettings"));
    REQUIRE(table != nullptr);
    CHECK(table->rowCount() == 2);
    REQUIRE(workspace.Edit("host.max_frames", QStringLiteral("18446744073709551615")));
    CHECK(workspace.Dirty());
    CHECK_FALSE(workspace.Edit("host.max_frames", QStringLiteral("18446744073709551616")));
    CHECK_FALSE(workspace.Edit("host.headless", QStringLiteral("1")));
    const auto path = directory.filePath(QStringLiteral("preferences.json"));
    REQUIRE(workspace.SavePreferences(path));
    CHECK_FALSE(workspace.Dirty());
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    file.close();
    CHECK(bytes.contains("18446744073709551615"));
    CHECK_FALSE(bytes.contains("host.headless"));
    ConfigurationWorkspace loaded;
    REQUIRE(loaded.Load(path, Layer::Preference));
    REQUIRE(loaded.Edit("host.max_frames", {}, true));
    REQUIRE(loaded.SavePreferences(path));
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll().contains("\"values\":[]"));
    file.close();
}
TEST_CASE("Configuration workspace rejects malformed files and external preference changes", "[editor][configuration]")
{
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("preferences.json"));
    ConfigurationWorkspace workspace;
    REQUIRE(workspace.Edit("host.headless", QStringLiteral("true")));
    REQUIRE(workspace.SavePreferences(path));
    REQUIRE(workspace.Edit("host.headless", QStringLiteral("false")));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write("changed") == 7);
    file.close();
    CHECK_FALSE(workspace.SavePreferences(path));
    CHECK(workspace.Dirty());
    CHECK_FALSE(workspace.Load(path, Layer::Preference));
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == QByteArrayLiteral("changed"));
}
TEST_CASE("Configuration workspace selects engine schemas explicitly and from loaded bundles",
          "[editor][configuration]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    ConfigurationWorkspace workspace;
    auto* table = workspace.findChild<QTableWidget*>(QStringLiteral("configurationSettings"));
    QComboBox* selector = workspace.findChild<QComboBox*>(QStringLiteral("configurationSchema"));
    REQUIRE(table != nullptr);
    REQUIRE(selector != nullptr);
    CHECK(workspace.GetSchemaIdentity() == "ludus.host.v1");
    CHECK(selector->count() >= 2);

    selector->setCurrentIndex(selector->findData(QStringLiteral("ludus.audio.v1")));
    CHECK(workspace.GetSchemaIdentity() == "ludus.audio.v1");
    CHECK(table->rowCount() == 4);
    CHECK_FALSE(workspace.Edit("host.headless", QStringLiteral("true")));
    REQUIRE(workspace.Edit("audio.volume.music", QStringLiteral("0.5")));
    CHECK_FALSE(workspace.Edit("audio.volume.music", QStringLiteral("2")));
    const QString audio = directory.filePath(QStringLiteral("audio.json"));
    REQUIRE(workspace.SavePreferences(audio));
    QFile file(audio);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    file.close();
    CHECK(bytes.contains("\"schema\":\"ludus.audio.v1\""));
    CHECK(bytes.contains("audio.volume.music"));
    CHECK_FALSE(bytes.contains("audio.volume.master"));
    CHECK_FALSE(workspace.SelectSchema("ludus.audio.v0"));
    CHECK(workspace.GetSchemaIdentity() == "ludus.audio.v1");

    // A clean host workspace switches to the schema an audio bundle names.
    ConfigurationWorkspace loaded;
    REQUIRE(loaded.Load(audio, Layer::Preference));
    CHECK(loaded.GetSchemaIdentity() == "ludus.audio.v1");
    QComboBox* loadedSelector = loaded.findChild<QComboBox*>(QStringLiteral("configurationSchema"));
    REQUIRE(loadedSelector != nullptr);
    CHECK(loadedSelector->currentData().toString() == QStringLiteral("ludus.audio.v1"));
    Binding binding;
    Explanation explanation;
    REQUIRE(loaded.Preview()->Bind("audio.volume.music", binding) == Status::Ok);
    REQUIRE(loaded.Preview()->Explain(binding, explanation) == Status::Ok);
    CHECK(explanation.Requested.Real == 0.5);
    REQUIRE(loaded.SelectSchema("ludus.host.v1"));
    CHECK(loaded.findChild<QTableWidget*>(QStringLiteral("configurationSettings"))->rowCount() == 2);
}
