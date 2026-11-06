// Native-platform (Cocoa, Windows, xcb) editor tests run unattended, so no
// first-use modal may wait for a person. Offscreen runs already skip them.
#include "internal/code_keymap.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <QGuiApplication>
#include <QString>

namespace
{
/// Before each native test case, store the default Ludus keymap when no profile
/// is stored, as a returning user's choice would. Otherwise showing a MainWindow
/// opens the modal first-use keymap chooser, which blocks the test forever
/// (batch 42087632706 hung the macOS release journey for six hours).
class NativeSessionListener final : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& testInfo) override
    {
        static_cast<void>(testInfo);
        if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        {
            return;
        }
        ludus::editor::CodeKeymap keymap;
        if (keymap.HasStoredProfile() == false)
        {
            keymap.SetActiveProfile(ludus::editor::CodeKeymapProfile::Ludus);
        }
    }
};
} // namespace

CATCH_REGISTER_LISTENER(NativeSessionListener)
