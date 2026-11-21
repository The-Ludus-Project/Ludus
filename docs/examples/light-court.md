# Edit the lighting in Light Court

Open a small courtyard, rotate its Sun and watch the shadow move. Light Court
is a standalone authoring sample for the [desktop editor](../wiki/getting-started/install.md)
with [rendered World preview enabled](../development/editor-workspace.md#enable-the-rendered-authoring-surface).

![Light Court rendered by RT1. The graphite sphere has a soft highlight, a diagonal shadow crosses the concrete steps, and a warm reflection from the copper plate lights the wall beside the blue-green panel.](images/light-court.png)

The image is a 1440 × 960 [RT1 reference capture](../development/editor-workspace.md#reference-capture-of-the-viewport)
from the saved start view on macOS Metal. GPU compute traced 1,024 samples per
pixel to depth eight with seed 1. The PNG uses exposure 1 and Khronos PBR Neutral
with sRGB encoding. The editor build was `3b78b5239` with the World undo
correction from this sample.

## Open the sample

1. Copy the [Light Court folder](../../examples/light-court/) to a writable
   location. Keep `art/`, `assets/`, and `authoring/` together. The project
   includes the imported meshes and material slots.
2. Open the copy's `ludus.project.json` from **File → Open Project**.
3. In **Project Explorer → Worlds**, open **light-court**. The viewport opens at
   the saved perspective. If you navigated away, choose **View → Go to start view**
   in the viewport toolbar. Select **Lit** if the toolbar shows **Solid**.

The sample is an editor authoring project. Its CMake target lists the sample
data for tools that read the project.

## Inspect the materials

The [Light Court glTF](../../examples/light-court/art/light-court.gltf) contains
the original geometry and five metallic-roughness materials. In the World's
**Materials** tab, inspect the imported slots. Concrete uses roughness 0.86, the
blue-green enamel uses 0.22, and satin copper uses metallic 1 and roughness 0.34.
The graphite ceramic sphere uses smooth vertex normals, a tangent-space normal
map for fine grain, and a roughness map that varies the sheen across the glaze.
The mesh, materials and maps are original to Light Court and ship under the
[sample license](../../examples/light-court/art/LICENSE.txt). The catalog and
blobs under `assets/` are the Editor import of this source. See [inspect imported
materials](../development/editor-workspace.md#browse-a-worlds-material-resources) for how
to find the surfaces that use each slot.

## Rotate the Sun

The interactive viewport uses raster rendering so the shadow responds as you
edit. The [raster capture](images/light-court-raster.png) shows the same start
view at 1440 × 960 with the default editor graph on Metal and the grid hidden.
To capture bounce light and material reflections at the same camera, open
**Reference…** in the viewport toolbar and select **RT1 reference** as the **Source**.

1. Select **Sun** in the World hierarchy. In **Properties → Transform**, change
   **Rotation Y** from `128` to `75` degrees. Watch the shadow swing across the
   platform as the viewport previews the valid draft.
2. Choose **Apply Changes**, then **Edit → Undo**. The Sun and shadow return to
   their starting positions.
3. Choose **Edit → Redo** and **File → Save**. Reopen the World and select **Sun**
   again. The saved **Rotation Y** is `75` degrees.

See the [World authoring guide](../development/editor-workspace.md#try-a-3d-authoring-workflow)
for camera controls, preview modes and save behavior.
