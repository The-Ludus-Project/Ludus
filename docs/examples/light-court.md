# Edit the lighting in Light Court

Open a small courtyard, rotate its Sun and watch the shadow move. Light Court
is a standalone authoring sample for the [desktop editor](../wiki/getting-started/install.md)
with [rendered World preview enabled](../development/editor-workspace.md#enable-the-rendered-authoring-surface).

![Light Court rendered by Ludus. The tall concrete wall casts a diagonal shadow across the stepped platform beside a blue-green panel, a copper-colored plate and a graphite sphere.](images/light-court.png)

The image is a 1440 × 960 capture of the saved start view through the engine
default raster graph on macOS Metal. The capture uses the authored Sun and Cool
Fill, a 36° vertical field of view, and the renderer's normal display output
with the editor grid excluded. The editor build was `3b78b5239` with the World
undo correction from this sample. Light Court is original Ludus artwork using
the Editor's embedded cube and sphere meshes.

## Open the sample

1. Copy the [Light Court folder](../../examples/light-court/) to a writable
   location. The project uses the editor's built-in World schema and meshes.
2. Open the copy's `ludus.project.json` from **File → Open Project**.
3. In **Project Explorer → Worlds**, open **light-court**. The viewport opens at
   the saved perspective. If you navigated away, choose **View → Go to start view**
   in the viewport toolbar. Select **Lit** if the toolbar shows **Solid**.

The sample is an editor authoring project. Its CMake target lists the sample
data for tools that read the project.

## Rotate the Sun

1. Select **Sun** in the World hierarchy. In **Properties → Transform**, change
   **Rotation Y** from `128` to `75` degrees. Watch the shadow swing across the
   platform as the viewport previews the valid draft.
2. Choose **Apply Changes**, then **Edit → Undo**. The Sun and shadow return to
   their starting positions.
3. Choose **Edit → Redo** and **File → Save**. Reopen the World and select **Sun**
   again. The saved **Rotation Y** is `75` degrees.

See the [World authoring guide](../development/editor-workspace.md#try-a-3d-authoring-workflow)
for camera controls, preview modes and save behavior.
