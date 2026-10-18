# Glossary

| Term | Meaning in Ludus |
| --- | --- |
| Ludus | Latin for play, a game and a school; the engine's name and the source of its [identity](../../architecture/engine-identity.md): serious play with dependable, teachable tools |
| Engine checkout | The source repository used to develop Ludus and run its trusted tools |
| Build host | Machine running the Editor/compiler/build tools; it can differ from the game target |
| Compiled target | OS, CPU and data model selected by the compiler for a translation unit; see [platform targets](../guides/platform-targets.md) |
| Runtime capability | Confirmed feature/limit of a live subsystem session, not a conclusion from its compiled target |
| SDK | Installed engine headers, libraries and build metadata for a compatible target/flavor/toolchain |
| Project descriptor | `ludus.project.json`, the game's saved engine/build/launch intent |
| Release lock | Exact release-oriented selection; local development may use a separate ignored override |
| Local settings | Machine paths and owned preset data, kept outside committed portable intent |
| CMake preset | Named configure/build/test settings; selectable presets are the ones a user/tool can actually invoke |
| Build profile/flavor | Debug, Development, Profile or Release policy and compatible build/SDK inputs |
| Draft | Edited state that has not yet been successfully saved/published |
| Staging | A private temporary result verified before publication to its destination |
| Source document | Persistent authored data such as an audio definition or preferences layer |
| Runtime snapshot | Copied state from a running host/session, independent of a source-document save |
| Logical resource ID | Stable content identity resolved through a catalog, rather than a transient runtime handle |
| Borrowed string view | Counted bytes without retained ownership; the source must remain alive and its storage valid |
| Shared string | Immutable bytes whose allocation is retained by copied `SharedString` handles |
| Interned name | Exact spelling deduplicated within one table; `NameId` includes its process-local table token and index |
| String index | An index meaningful only with its enclosing table or dictionary owner |
| Fingerprint | A hash of specified bytes; equality does not establish unique identity or authenticate content |
| RHI | Rendering Hardware Interface, the engine's graphics backend boundary |
| GameHost | Runtime host for supported game-module and Play workflows |
| S1 / S2 | Editor milestone names: workspace shell, then document interactions/undo |

Continue with [architecture](architecture.md), [project setup](../../development/project-sdk-workflow.md)
or [the editor workspace](../../development/editor-workspace.md).

For ownership, byte/text boundaries and hash policies, see [strings](../guides/strings.md)
and [hashing](../guides/hashing.md).
