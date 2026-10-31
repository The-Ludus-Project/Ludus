// The capture profile request of the Reference window (docs/architecture/render-graph-frame-capture.md, phase 4c):
// one build per generation, stale answers ignored, failures retried, and a Ready empty path naming the runtime
// package.
#include "internal/render_graph_capture_profile.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QString>

using ludus::editor::CaptureProfileState;
using ludus::editor::RenderGraphCaptureProfile;

TEST_CASE("A capture profile request builds once per generation and ignores stale answers",
          "[editor][render-graph-capture]")
{
    RenderGraphCaptureProfile profile;
    int requested = 0;
    QByteArray requestedIdentity;
    QObject::connect(&profile, &RenderGraphCaptureProfile::BuildRequested, [&](const QByteArray& identity) {
        ++requested;
        requestedIdentity = identity;
    });
    const QByteArray first("first");
    const QByteArray second("second");
    CHECK(profile.GetState() == CaptureProfileState::Idle);
    profile.Request(first);
    CHECK(profile.GetState() == CaptureProfileState::Building);
    CHECK(requested == 1);
    CHECK(requestedIdentity == first);
    // A repeated request while it builds starts nothing.
    profile.Request(first);
    CHECK(requested == 1);
    // A newer generation supersedes the build; the first one's answer is then ignored.
    profile.Request(second);
    CHECK(requested == 2);
    profile.AcceptBuild(first, QStringLiteral("first.lurgcp"));
    CHECK(profile.GetState() == CaptureProfileState::Building);
    profile.AcceptBuild(second, QStringLiteral("second.lurgcp"));
    CHECK(profile.GetState() == CaptureProfileState::Ready);
    CHECK(profile.IsReadyFor(second) == true);
    CHECK(profile.IsReadyFor(first) == false);
    CHECK(profile.GetPackagePath() == QStringLiteral("second.lurgcp"));
    // A Ready profile of the same generation is reused.
    profile.Request(second);
    CHECK(requested == 2);
}

TEST_CASE("A failed capture profile is retried and a graph without a capture budget uses its runtime package",
          "[editor][render-graph-capture]")
{
    RenderGraphCaptureProfile profile;
    int requested = 0;
    QObject::connect(&profile, &RenderGraphCaptureProfile::BuildRequested, [&]() { ++requested; });
    const QByteArray identity("generation");
    profile.Request(identity);
    profile.RejectBuild(identity, QStringLiteral("Another render-graph job is running"));
    CHECK(profile.GetState() == CaptureProfileState::Failed);
    CHECK(profile.GetError() == QStringLiteral("Another render-graph job is running"));
    CHECK(profile.IsReadyFor(identity) == false);
    profile.Request(identity);
    CHECK(requested == 2);
    CHECK(profile.GetState() == CaptureProfileState::Building);
    CHECK(profile.GetError().isEmpty() == true);
    profile.AcceptBuild(identity, {});
    CHECK(profile.IsReadyFor(identity) == true);
    CHECK(profile.GetPackagePath().isEmpty() == true);
}
