#include <catch2/catch_session.hpp>

#include <QApplication>
#include <QTemporaryDir>

int main(int argc, char* argv[])
{
    // Never read or change the user's editor history in offscreen tests.
    QTemporaryDir config;
    if (config.isValid() == false)
    {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication application(argc, argv);
    return Catch::Session().run(argc, argv);
}
