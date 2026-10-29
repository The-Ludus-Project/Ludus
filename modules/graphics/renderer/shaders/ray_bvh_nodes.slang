#pragma once
// Reader for the renderer's packed binned-SAH BVH (PackGpuScene, gpu_preview_services.h), shared by
// the ray walk in ray_bvh_traversal.slang and the line walk in ray_line_visibility.slang, so the node
// layout, the leaf order and the traversal stack bound have one owner.
//
// The including kernel supplies SceneValue(uint) over the packed scene words and the Parameters
// fields Offsets (x: nodes, y: leaf order) and Counts (y: node count).
#include "renderer_gpu_preview_layout.h"
#include "renderer_reference_layout.h"

/// Packed float4 words per node: lower bound and Left, upper bound and Right, then the triangle range.
static const uint BVH_NODE_WORDS = 3;
/// Most entries a walk may hold before pushing an inner node's two children into its
/// LUDUS_REFERENCE_STACK_SIZE-entry stack. BuildReferenceBvh bounds the depth so that a walk keeping
/// one deferred sibling per level stays below it; exceeding it means a malformed pack.
static const uint BVH_MAXIMUM_PENDING = LUDUS_REFERENCE_STACK_SIZE - 2;

/// One decoded ReferenceBvhNode.
struct BvhNode
{
    float3 MinimumWorld;
    float3 MaximumWorld;
    /// Child holding the lower centroids along SplitAxis; inner nodes only.
    uint Left;
    /// The other child; inner nodes only.
    uint Right;
    /// First leaf-order slot of a leaf.
    uint First;
    /// Triangles in a leaf; zero for an inner node.
    uint Count;
    /// Axis (0-2) that separated Left from Right; zero for leaves.
    uint SplitAxis;
};

/// Decode node `index` of the packed array at Offsets.x. The caller keeps index below Counts.y.
BvhNode LoadBvhNode(uint index)
{
    uint word = Offsets.x + index * BVH_NODE_WORDS;
    float4 lower = SceneValue(word);
    float4 upper = SceneValue(word + 1);
    uint3 range = asuint(SceneValue(word + 2).xyz);
    BvhNode node;
    node.MinimumWorld = lower.xyz;
    node.MaximumWorld = upper.xyz;
    node.Left = asuint(lower.w);
    node.Right = asuint(upper.w);
    node.First = range.x;
    node.Count = range.y;
    node.SplitAxis = range.z;
    return node;
}

/// Packed triangle at leaf-order slot `slot`. Order word Z keeps the BVH leaf order even when word X
/// holds the native opacity partition (PackGpuPreviewRayOrder), so leaf ranges address it in every plan.
uint GetBvhLeafTriangle(uint slot)
{
    return asuint(SceneValue(Offsets.y + slot).z);
}
