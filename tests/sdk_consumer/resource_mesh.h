#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/runtime/resources/mesh.h>

#include "l0_scene.h"

namespace ludus::qa
{
// Public-only SDK handoff: CPU definitions retire before any renderer/GPU use.
inline bool CreateResourceMeshes(graphics::renderer::Renderer& renderer,
                                 graphics::renderer::Mesh& quad,
                                 graphics::renderer::Mesh& triangle) noexcept
{
    namespace resources = ludus::resources;
    resources::MeshSource sources[2];
    sources[0].Resource.Type = sources[1].Resource.Type = content::Kind::Mesh;
    if (sources[0].Resource.Id.Set("mesh/quad") == false || sources[0].Resource.Path.Set("quad.mesh") == false ||
        sources[1].Resource.Id.Set("mesh/triangle") == false || sources[1].Resource.Path.Set("triangle.mesh") == false)
    {
        return false;
    }
    sources[0].Geometry = {L0_QUAD, 4, L0_INDICES, 6};
    sources[1].Geometry = {L0_TRIANGLE, 3, L0_INDICES, 3};
    resources::MeshDefinitionAdapter adapter;
    content::Catalog catalog;
    if (adapter.Initialize(sources, 2) != resources::Status::Ok ||
        catalog.Put(sources[0].Resource) != content::Status::Ok ||
        catalog.Put(sources[1].Resource) != content::Status::Ok)
    {
        return false;
    }
    auto descriptor = adapter.Descriptor();
    resources::Manager manager;
    if (manager.Initialize(catalog, &descriptor, 1) != resources::Status::Ok)
    {
        return false;
    }
    struct Cleanup final
    {
        resources::Manager& Manager;
        ~Cleanup() noexcept
        {
            // CPU-only bounded slots, no worker or GPU acknowledgment dependency.
            while (Manager.Shutdown() != resources::Status::Ok)
            {
                Manager.Service(64);
            }
        }
    } cleanup{manager};
    resources::AssetHandle roots[2];
    for (foundation::usize i = 0; i < 2; ++i)
    {
        if (manager.Resolve(sources[i].Resource.Id, content::Kind::Mesh, roots[i]) != resources::Status::Ok)
        {
            return false;
        }
    }
    resources::TransactionHandle transaction;
    resources::DependencyGraph graph;
    graph.Revision = 2;
    if (manager.BeginReplacement(catalog, &descriptor, 1, graph, roots, 2, transaction) != resources::Status::Ok)
    {
        return false;
    }
    // Prepare both CPU definitions privately, then activate the whole binding set.
    for (foundation::usize turn = 0; turn < 4; ++turn)
    {
        manager.Service(64);
    }
    if (manager.Commit(transaction) != resources::Status::Ok ||
        manager.EndTransaction(transaction) != resources::Status::Ok)
    {
        return false;
    }
    resources::Lease leases[2];
    resources::ScopeHandle scope;
    content::ResourceId name;
    if (name.Set("sdk/renderer") == false || manager.OpenScope(name, scope) != resources::Status::Ok)
    {
        return false;
    }
    graphics::renderer::Mesh* outputs[]{&quad, &triangle};
    for (foundation::usize i = 0; i < 2; ++i)
    {
        resources::RequestHandle request;
        if (manager.BeginAcquire(scope, roots[i], request) != resources::Status::Ok)
        {
            return false;
        }
        // Setup-only in-memory input, not an event-loop wait for asynchronous I/O/GPU.
        for (foundation::usize turn = 0; turn < 4; ++turn)
        {
            manager.Service(64);
        }
        if (manager.TakeLease(request, leases[i]) != resources::Status::Ok)
        {
            return false;
        }
        const void* payload = nullptr;
        if (manager.Borrow(leases[i].Token(), content::Kind::Mesh, payload) != resources::Status::Ok)
        {
            return false;
        }
        const auto& mesh = *static_cast<const resources::MeshDefinition*>(payload);
        const auto status = renderer.CreateMesh(
            {mesh.Positions, mesh.VertexCount, mesh.Indices, mesh.IndexCount, mesh.Texcoords, mesh.Normals},
            *outputs[i]);
        if (status != graphics::rhi::RasterStatus::Ready && status != graphics::rhi::RasterStatus::Pending)
        {
            return false;
        }
    }
    return true;
}
} // namespace ludus::qa
