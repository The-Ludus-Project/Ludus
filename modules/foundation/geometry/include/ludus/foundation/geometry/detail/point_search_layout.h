#pragma once

// Maximum result count shared by CPU queries and GPU kernels.
#define LUDUS_MAX_POINT_NEIGHBORS 64
// Balanced uint32 point-tree traversal stack entries.
#define LUDUS_POINT_SEARCH_STACK_SIZE 32
// Largest absolute coordinate in world units; squared float32 distances remain finite.
#define LUDUS_MAX_POINT_SEARCH_COORDINATE 1000000
// Packed sixteen-byte words per point-tree node.
#define LUDUS_POINT_SEARCH_NODE_WORDS 2
// Packed bytes per point-tree node, independently of host alignment.
#define LUDUS_POINT_SEARCH_NODE_BYTES 32
// Missing node/source-index sentinel, reserved from the valid index range.
#define LUDUS_ABSENT_POINT_NODE 0xffffffffu

// Threads per portable point-query workgroup.
#define LUDUS_POINT_SEARCH_GROUP_SIZE 64
