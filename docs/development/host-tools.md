# ludus-tools

The Ludus host tooling: the single project/SDK operation backend shared by the
installed `ludus` CLI and the optional Editor. It installs independently of a
Ludus engine source checkout and has **no Qt dependency** (requirements
P07/P11), using only the Python standard library.

## Install

From the Ludus checkout root:

```bash
python3 -m venv out/ludus-cli-venv
out/ludus-cli-venv/bin/python -m pip install ./scripts/python
out/ludus-cli-venv/bin/ludus --help
```

See [independent game projects](project-sdk-workflow.md) for the
full installation, SDK override/refresh, project creation, migration, direct
CMake and recovery documentation.

## Render graph contracts

The installed CLI includes `render-graph new`, `render-graph validate` and
`render-graph dump` for RG0 saved/imported graph contracts and serial plans.
See [render graph authoring](render-graph-authoring.md) for explicit configurations,
camera output routes, the JSON format and validation limits. This offline stage
does not compile shaders or execute GPU rendering.

## Native release packaging

The installed CLI supports optional `project create --release`, native
`project package`, `project package verify`, and offline `project publish plan`.
See [the release guide](game-packaging-publishing.md).
The wheel ships the standalone CMake File API resolver; these operations need
no Qt or engine checkout. Explicit uploads use `project publish upload`; see the release setup guide below.

Existing-project Editor setup, browser packaging and CI uploading: see
[Editor-managed game releases](editor-game-releases.md).

RG1 shader/schema packages use the same installed backend. See [build sources,
pinned SDK helper and immutable publication](render-graph-authoring.md#build-explicit-shader-packages-rg1).
RG3 adds `render-graph cook-runtime`, `schema-diff` and `migrate`, and JSON
failures carry an authored `location`. See [edit and reload graphs](render-graph-authoring.md#edit-and-reload-graphs-rg3).
`render-graph ray-path-capture` compiles the editor-only diagnostic capture
variant of a published generation; see
[capture ray paths for inspection](render-graph-authoring.md#capture-ray-paths-for-inspection).
