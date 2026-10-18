#include "resource_adapter.h"

namespace sample
{
namespace audio = ludus::audio;
namespace content = ludus::content;
namespace resources = ludus::resources;
using namespace ludus::foundation;
SoundResourceAdapter::SoundResourceAdapter(audio::AudioSystem& system,
                                           const content::Catalog& catalog,
                                           const content::Text<4096>& root,
                                           const audio::content::Routing& routing) noexcept
    : mSystem(system), mCatalog(catalog), mRoot(root), mRouting(routing), mLoader(system)
{
}
void SoundResourceAdapter::CancelAndJoin() noexcept
{
    mLoader.Cancel();
    mPending = false;
}
resources::Adapter SoundResourceAdapter::Descriptor() noexcept
{
    resources::Adapter adapter;
    adapter.Context = this;
    adapter.MaxCandidates = 1;
    // Policy reservations for this demonstrator, not measured strict decoder or
    // driver heap bounds. Core enforcement covers these declared peaks only.
    adapter.Peak = {128ULL * 1024 * 1024, audio::RESIDENT_PCM_CAP_BYTES};
    adapter.Epoch = [](void* context) noexcept -> uint64 {
        auto& self = *static_cast<SoundResourceAdapter*>(context);
        return self.mSystem.GetState() == audio::SystemState::Ready ? self.mSystem.GetSession() : 0;
    };
    adapter.Begin = [](void* context, const content::Resource& resource, void*& candidate) noexcept {
        auto& self = *static_cast<SoundResourceAdapter*>(context);
        if (self.mUsed)
        {
            return resources::Status::QueueFull;
        }
        const auto result = self.mLoader.Begin(self.mRoot.View(), resource.Id.View(), nullptr, nullptr, &self.mCatalog);
        if (result != content::Status::Pending)
        {
            return result == content::Status::OutOfMemory ? resources::Status::OutOfMemory : resources::Status::Failed;
        }
        self.mUsed = true;
        self.mPending = true;
        self.mReleased = false;
        self.mLease = {};
        candidate = &self;
        return resources::Status::Pending;
    };
    adapter.Poll = [](void* context, void*, const void*& payload) noexcept {
        auto& self = *static_cast<SoundResourceAdapter*>(context);
        content::Diagnostic diagnostic;
        const auto result = self.mLoader.Poll(self.mRouting, self.mLease, diagnostic);
        if (result == content::Status::Pending)
        {
            return resources::Status::Pending;
        }
        // A detach error retains the native job. Retire keeps polling that job;
        // no budget or context is freed on this error path.
        self.mPending = result == content::Status::IoError;
        if (result != content::Status::Ok ||
            self.mLoader.GetSound(self.mLease, self.mPayload.Definition, self.mPayload.Descriptor) !=
                content::Status::Ok)
        {
            return resources::Status::Failed;
        }
        payload = &self.mPayload;
        return resources::Status::Ok;
    };
    adapter.Cancel = [](void* context, void*) noexcept {
        static_cast<SoundResourceAdapter*>(context)->mLoader.RequestCancel();
    };
    adapter.Retire = [](void* context, void*) noexcept {
        auto& self = *static_cast<SoundResourceAdapter*>(context);
        if (self.mPending)
        {
            self.mLoader.RequestCancel();
            content::Diagnostic diagnostic;
            const auto result = self.mLoader.Poll(self.mRouting, self.mLease, diagnostic);
            if (result == content::Status::Pending || result == content::Status::IoError)
            {
                return resources::Status::Pending;
            }
            self.mPending = false;
        }
        if (self.mReleased == false && self.mLease.Owner != 0)
        {
            if (self.mLoader.Release(self.mLease) != content::Status::Ok)
            {
                return resources::Status::Failed;
            }
            self.mReleased = true;
        }
        self.mLoader.Service();
        audio::SystemSnapshot snapshot;
        self.mSystem.GetSystemSnapshot(snapshot);
        if (snapshot.ResidentPcmBytes != 0)
        {
            return resources::Status::Pending;
        }
        self.mUsed = false;
        return resources::Status::Ok;
    };
    adapter.Validate = [](void* context, const void*) noexcept {
        auto& self = *static_cast<SoundResourceAdapter*>(context);
        audio::content::Sound definition;
        audio::app::EventDescriptor descriptor;
        return self.mLoader.GetSound(self.mLease, definition, descriptor) == content::Status::Ok
                   ? resources::Status::Ok
                   : resources::Status::DeviceLost;
    };
    return adapter;
}
} // namespace sample
