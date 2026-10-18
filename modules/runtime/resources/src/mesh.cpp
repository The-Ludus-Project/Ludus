#include <ludus/runtime/resources/mesh.h>

#if defined(LUDUS_RESOURCE_MESH_TEST_ALLOCATIONS)
#    include "internal/mesh_test_hooks.hpp"
#endif

#include <atomic>
#include <new>

namespace ludus::resources
{
// Thanks to James Boer, "A Generic Resource Manager", Game Programming Gems,
// section 1.7, pp. 80-87, for separating generic lifetime from type preparation.
// This original CPU adapter uses immutable copied versions and explicit leases;
// Renderer uploads have a separate owner; no chapter implementation is copied.
// Review:
// https://github.com/The-Ludus-Project/Ludus/blob/main/docs/architecture/resource-management-reference-review.md
namespace
{
std::atomic<uint64> NextEpoch{1};
#if defined(LUDUS_RESOURCE_MESH_TEST_ALLOCATIONS)
int32 FailArrayAfter = -1;
#endif
template <typename T>
T* AllocateArray(usize count) noexcept
{
#if defined(LUDUS_RESOURCE_MESH_TEST_ALLOCATIONS)
    if (FailArrayAfter == 0)
    {
        FailArrayAfter = -1;
        return nullptr;
    }
    if (FailArrayAfter > 0)
    {
        --FailArrayAfter;
    }
#endif
    return new (std::nothrow) T[count];
}
uint64 ReserveEpoch() noexcept
{
    auto next = NextEpoch.load(std::memory_order_relaxed);
    while (next != 0)
    {
        const uint64 following = next == ~uint64{0} ? 0 : next + 1;
        if (NextEpoch.compare_exchange_weak(next, following, std::memory_order_relaxed))
        {
            return next;
        }
    }
    return 0;
}
bool Valid(const MeshDefinition& mesh) noexcept
{
    if (mesh.Positions == nullptr || mesh.Indices == nullptr || mesh.VertexCount == 0 || mesh.VertexCount > 4096 ||
        mesh.IndexCount == 0 || mesh.IndexCount > 12288 || mesh.IndexCount % 3 != 0)
    {
        return false;
    }
    for (usize i = 0; i < mesh.VertexCount; ++i)
    {
        if (foundation::math::IsFinite(mesh.Positions[i]) == false ||
            (mesh.Texcoords != nullptr && foundation::math::IsFinite(mesh.Texcoords[i]) == false) ||
            (mesh.Normals != nullptr && (foundation::math::IsFinite(mesh.Normals[i]) == false ||
                                         (mesh.Normals[i].X == 0 && mesh.Normals[i].Y == 0 && mesh.Normals[i].Z == 0))))
        {
            return false;
        }
    }
    for (usize i = 0; i < mesh.IndexCount; ++i)
    {
        if (mesh.Indices[i] >= mesh.VertexCount)
        {
            return false;
        }
    }
    return true;
}
uint64 Bytes(const MeshDefinition& mesh) noexcept
{
    return mesh.VertexCount * (sizeof(foundation::math::Vector3) +
                               (mesh.Texcoords != nullptr ? sizeof(foundation::math::Vector2) : 0) +
                               (mesh.Normals != nullptr ? sizeof(foundation::math::Vector3) : 0)) +
           mesh.IndexCount * sizeof(uint32);
}
} // namespace
#if defined(LUDUS_RESOURCE_MESH_TEST_ALLOCATIONS)
void internal::FailMeshArrayAllocationAfter(int32 count) noexcept
{
    FailArrayAfter = count;
}
#endif
struct MeshDefinitionAdapter::State final
{
    struct Job final
    {
        bool Used = false, Cancelled = false;
        const MeshDefinition* Source = nullptr;
        MeshDefinition Payload;
        foundation::math::Vector3* Positions = nullptr;
        foundation::math::Vector2* Texcoords = nullptr;
        foundation::math::Vector3* Normals = nullptr;
        uint32* Indices = nullptr;
        usize Vertices = 0, IndicesCopied = 0;
        void Clear() noexcept
        {
            delete[] Positions;
            delete[] Texcoords;
            delete[] Normals;
            delete[] Indices;
            *this = {};
        }
    } Jobs[16];
    MeshSource Sources[16];
    usize Count = 0;
    uint64 Epoch = 0, Peak = 0;
    Job* Find(void* candidate) noexcept
    {
        for (auto& job : Jobs)
        {
            if (candidate == &job && job.Used)
            {
                return &job;
            }
        }
        return nullptr;
    }
};
MeshDefinitionAdapter::~MeshDefinitionAdapter() noexcept
{
    if (mState != nullptr)
    {
        for (const auto& job : mState->Jobs)
        {
            LUDUS_REQUIRE(job.Used == false);
        }
    }
    delete mState;
}
Status MeshDefinitionAdapter::Initialize(const MeshSource* sources, usize count) noexcept
{
    if (mState != nullptr)
    {
        for (const auto& job : mState->Jobs)
        {
            if (job.Used)
            {
                return Status::InvalidState;
            }
        }
    }
    if (sources == nullptr || count == 0 || count > 16)
    {
        return Status::Failed;
    }
    content::Catalog validation;
    uint64 peak = 0;
    for (usize i = 0; i < count; ++i)
    {
        if (sources[i].Resource.Type != content::Kind::Mesh || Valid(sources[i].Geometry) == false)
        {
            return Status::Failed;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (sources[i].Resource.Id.View() == sources[j].Resource.Id.View())
            {
                return Status::Failed;
            }
        }
        const auto status = validation.Put(sources[i].Resource);
        if (status != content::Status::Ok)
        {
            return status == content::Status::OutOfMemory ? Status::OutOfMemory : Status::Failed;
        }
        const auto bytes = Bytes(sources[i].Geometry);
        if (bytes > peak)
        {
            peak = bytes;
        }
    }
    auto* next = new (std::nothrow) State();
    if (next == nullptr)
    {
        return Status::OutOfMemory;
    }
    next->Epoch = ReserveEpoch();
    if (next->Epoch == 0)
    {
        delete next;
        return Status::QueueFull;
    }
    next->Peak = peak;
    next->Count = count;
    for (usize i = 0; i < count; ++i)
    {
        next->Sources[i] = sources[i];
    }
    delete mState;
    mState = next;
    return Status::Ok;
}
uint64 MeshDefinitionAdapter::Epoch(void* context) noexcept
{
    const auto& self = *static_cast<MeshDefinitionAdapter*>(context);
    return self.mState != nullptr ? self.mState->Epoch : 0;
}
Status MeshDefinitionAdapter::Begin(void* context, const content::Resource& resource, void*& candidate) noexcept
{
    auto& self = *static_cast<MeshDefinitionAdapter*>(context);
    if (self.mState == nullptr || candidate != nullptr)
    {
        return Status::InvalidState;
    }
    const MeshDefinition* source = nullptr;
    for (usize i = 0; i < self.mState->Count; ++i)
    {
        const auto& entry = self.mState->Sources[i];
        if (entry.Resource.Id.View() == resource.Id.View() && entry.Resource.Path.View() == resource.Path.View() &&
            resource.Type == content::Kind::Mesh)
        {
            source = &entry.Geometry;
        }
    }
    if (source == nullptr)
    {
        return Status::NotFound;
    }
    for (auto& job : self.mState->Jobs)
    {
        if (job.Used)
        {
            continue;
        }
        job.Used = true;
        job.Source = source;
        candidate = &job; // Any partial allocation remains owned until Retire.
        job.Positions = AllocateArray<foundation::math::Vector3>(source->VertexCount);
        job.Indices = AllocateArray<uint32>(source->IndexCount);
        if (source->Texcoords != nullptr)
        {
            job.Texcoords = AllocateArray<foundation::math::Vector2>(source->VertexCount);
        }
        if (source->Normals != nullptr)
        {
            job.Normals = AllocateArray<foundation::math::Vector3>(source->VertexCount);
        }
        if (job.Positions == nullptr || job.Indices == nullptr ||
            (source->Texcoords != nullptr && job.Texcoords == nullptr) ||
            (source->Normals != nullptr && job.Normals == nullptr))
        {
            return Status::OutOfMemory;
        }
        job.Payload = {job.Positions, source->VertexCount, job.Indices, source->IndexCount, job.Texcoords, job.Normals};
        return Status::Pending;
    }
    return Status::QueueFull;
}
Status MeshDefinitionAdapter::Poll(void* context, void* candidate, const void*& payload) noexcept
{
    if (context == candidate)
    {
        return Status::InvalidHandle;
    }
    auto& self = *static_cast<MeshDefinitionAdapter*>(context);
    auto* job = self.mState != nullptr ? self.mState->Find(candidate) : nullptr;
    if (job == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (job->Cancelled)
    {
        return Status::Cancelled;
    }
    const auto& source = *job->Source;
    if (job->Payload.Positions == nullptr)
    {
        return Status::OutOfMemory;
    }
    for (usize work = 0; work < 128 && job->Vertices < source.VertexCount; ++work, ++job->Vertices)
    {
        const auto i = job->Vertices;
        job->Positions[i] = source.Positions[i];
        if (job->Texcoords != nullptr)
        {
            job->Texcoords[i] = source.Texcoords[i];
        }
        if (job->Normals != nullptr)
        {
            job->Normals[i] = source.Normals[i];
        }
    }
    for (usize work = 0; work < 384 && job->IndicesCopied < source.IndexCount; ++work, ++job->IndicesCopied)
    {
        job->Indices[job->IndicesCopied] = source.Indices[job->IndicesCopied];
    }
    if (job->Vertices != source.VertexCount || job->IndicesCopied != source.IndexCount)
    {
        return Status::Pending;
    }
    payload = &job->Payload;
    return Status::Ok;
}
void MeshDefinitionAdapter::Cancel(void* context, void* candidate) noexcept
{
    if (context == candidate)
    {
        return;
    }
    auto& self = *static_cast<MeshDefinitionAdapter*>(context);
    if (self.mState != nullptr)
    {
        if (auto* job = self.mState->Find(candidate))
        {
            job->Cancelled = true;
        }
    }
}
Status MeshDefinitionAdapter::Retire(void* context, void* candidate) noexcept
{
    if (context == candidate)
    {
        return Status::InvalidHandle;
    }
    auto& self = *static_cast<MeshDefinitionAdapter*>(context);
    if (self.mState != nullptr)
    {
        for (auto& job : self.mState->Jobs)
        {
            if (candidate == &job)
            {
                if (job.Used)
                {
                    job.Clear();
                }
                return Status::Ok;
            }
        }
    }
    return Status::InvalidHandle;
}
Status MeshDefinitionAdapter::Validate(void* context, const void* payload) noexcept
{
    if (context == payload)
    {
        return Status::InvalidHandle;
    }
    const auto& self = *static_cast<MeshDefinitionAdapter*>(context);
    if (self.mState != nullptr)
    {
        for (const auto& job : self.mState->Jobs)
        {
            if (job.Used && job.Cancelled == false && payload == &job.Payload && job.Source != nullptr &&
                job.Vertices == job.Source->VertexCount && job.IndicesCopied == job.Source->IndexCount)
            {
                return Status::Ok;
            }
        }
    }
    return Status::InvalidHandle;
}
Adapter MeshDefinitionAdapter::Descriptor() noexcept
{
    Adapter result;
    result.Type = content::Kind::Mesh;
    result.Context = this;
    result.Peak.ResidentBytes = mState != nullptr ? mState->Peak : 0;
    result.Epoch = &Epoch;
    result.Begin = &Begin;
    result.Poll = &Poll;
    result.Cancel = &Cancel;
    result.Retire = &Retire;
    result.Validate = &Validate;
    return result;
}
usize MeshDefinitionAdapter::ControlBytes() const noexcept
{
    return mState != nullptr ? sizeof(State) : 0;
}
} // namespace ludus::resources
