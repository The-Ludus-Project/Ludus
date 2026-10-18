# Ludus identity and direction

Status: identity adopted October 18, 2026. This page owns what Ludus is, the
five commitments every part of the engine shares, and how those commitments
decide creative, art, technical and user-experience direction. Owner pages
apply it: the [editor art direction](editor-art-direction.md) owns the mark,
palette and theme tokens, the [interaction design](editor-interaction-design.md)
owns editor behaviour, [AGENTS.md](https://github.com/The-Ludus-Project/Ludus/blob/main/AGENTS.md)
and [engineering quality](engineering-quality.md) own the engineering rules.
When an owner page and this page disagree, fix the owner page or change the
identity here with the project owner's approval; do not let two directions
coexist.

## The name

*Ludus* is Latin for play, a game and a school: the same word names the
sport, the public games and the place where a skill is practised until it is
reliable. Thanks to Charlton T. Lewis and Charles Short, *A Latin Dictionary*
(Clarendon Press, 1879), entry *ludus*, available through the
[Perseus Digital Library](http://www.perseus.tufts.edu/hopper/text?doc=Perseus:text:1999.04.0059:entry=ludus).

That double meaning is the identity. Ludus treats making games as serious
play: a place where creative work is practised with dependable tools, where
every system can be understood, and where the engine teaches as much as it
runs. The wiki's first line, *Build understanding. Then build your game.*,
is this identity in one sentence.

## Identity statement

Ludus is a C++23 engine and toolset for people who want to understand the
machine they build on. It makes creative work predictable by owning routine
setup, showing what is happening, bounding every system explicitly and
proving its claims with evidence. It is quiet in use, precise in construction
and open about where its ideas come from.

Ludus is not a content-first engine, a marketplace, a hosted service or a
house art style. It gives a creator a trustworthy instrument and a way to
learn it, then stays out of the way of the game.

## Five commitments

Every direction below is derived from these five commitments. They are the
test for a design, a feature, a default, a sentence of documentation or a
pixel of the editor. A change that satisfies its local goal but breaks one
of these does not fit Ludus.

| Commitment | Meaning | The question to ask |
| --- | --- | --- |
| **Truthful** | Status, progress, rendered images, documentation and performance numbers report what was verified, never what is hoped. Failure, skip, unavailable and incomplete are distinct outcomes. | What evidence supports this claim, and what does the user see when it is false? |
| **Bounded** | Capacities, budgets, limits, formats and lifetimes are named, owned and checked. Behaviour is deterministic where the design allows it and explicit about where it does not. | Where is the limit named, who owns it, and what happens at the boundary? |
| **Owned** | Ludus owns its vocabulary: types, containers, formats, conversions, memory and errors have an engine owner with a stated contract. Borrowed storage states who keeps it alive. | Which module owns this, and does anything outside it decide its behaviour? |
| **Quiet** | Attention belongs to the creative work. Complexity is available when it serves a task and absent when it does not. Branding, chrome, decoration and noise stay small. | Does this earn the attention it takes, and can it be reached later when needed? |
| **Studied** | Designs come from consulted sources that are credited where they apply. Ludus explains its reasoning so a reader can learn it, disagree with it and extend it. | Which source informed this, how was it adapted, and where is that explained? |

The commitments are ordered deliberately. When two conflict, the earlier one
wins: a quieter interface may not hide a failure, and a bounded default may
not be relaxed to make a demo look better.

## Creative direction

Creative direction decides what Ludus is for, how it speaks and what it
chooses to show.

- **Audience.** Ludus serves creators and engineers who want to own their
  stack: students, small teams, researchers and tool builders. It does not
  compete on the size of an asset store or the breadth of a template catalogue.
- **Voice.** Plain, concrete and verb-first, in the editor, the CLI, logs and
  documentation alike. Say what happens and what it costs: *Create Project*,
  *Build and Play*, *Save Project Settings*. No superlatives, no marketing
  adjectives, no claims of readiness ahead of evidence. Where a limit exists,
  name it in the same sentence as the feature.
- **Naming.** Names say what a thing is. Modules, types and commands are
  spelled out, not abbreviated; milestone labels (S1, L3, W6) describe scope
  and never become product names. User projects carry their own names and
  artwork; Ludus never brands a game made with it.
- **What we show.** Samples and references are classics with known answers:
  the Cornell box, the staircase reference scene, a measured benchmark suite.
  A sample demonstrates one verifiable workflow, with screenshots of the
  actual build and an honest statement of what it does not yet do. There are
  no invented lights, placeholder art presented as finished, or showcase
  scenes that the public SDK cannot reproduce.
- **Learning is a feature.** The wiki pairs each workflow with the reasoning
  behind it and a reading path to its sources. Documentation is published
  once, online and offline, with the same content. A feature without its page
  and its credited references is not finished.
- **Games are not the brand.** The identity governs the engine, editor,
  tools, documentation and website. It never prescribes a genre, an art style
  or a tone for games built with Ludus. The
  [Drift sandbox](drift-game-integration.md) is an example consumer, not a
  flagship aesthetic.

## Art direction

Art direction decides how Ludus looks and what its images stand for.

- **Mark.** The Ludus Block: a mint, extruded capital **L** on a rounded
  charcoal tile, substantial, approachable and precise. The
  [editor art direction](editor-art-direction.md) owns the approved artwork,
  the brand colours, clear-space rules and export validation. One mark, one
  palette, in both light and dark appearance.
- **Visual language: Quiet Studio.** Cool charcoal and clean light surfaces,
  mint for actions and focus, readable neutral text, modest rounding and a
  small spacing scale. Dimension, bevels and shading belong to the mark;
  controls, panels and documentation stay flat and easy to scan. No
  decorative animation, glow, glass or external fonts. The same semantic
  token layer styles the native editor, the browser preview and the wiki.
- **Colour means something.** Accent identifies selection, focus and primary
  actions only. Warning, error and success carry words or symbols and their
  own semantic treatment; brand colour never implies completion. Contrast is
  verified in rendered components, not assumed from a palette table.
- **Rendered images are evidence.** The renderer's visual direction is
  physical plausibility and measurability before spectacle: a direct-draw
  reference path kept for debugging, a progressive reference mode to compare
  against, and optimisations accepted with evidence. A capture states which
  path, profile and revision produced it. Default project content is neutral
  and honest; a missing light renders dark rather than being quietly replaced.
- **Documentation and site.** System fonts, local assets, the mark used small,
  and the same Markdown rendered online and in the offline bundle. Screenshots
  record whether they show a prototype, a component study or the shipped
  editor.

## Technical direction

Technical direction turns the commitments into engineering rules. The rules
themselves live in [AGENTS.md](https://github.com/The-Ludus-Project/Ludus/blob/main/AGENTS.md),
the [coding standards](https://github.com/The-Ludus-Project/Ludus/blob/main/.kiro/steering/coding-standards.md)
and the decision records; this section states why they exist.

- **Explicit over implicit.** C++23 with no exceptions, explicit error
  results, fixed-width types and spelled-out variable types. Failure is part
  of a function's signature. See
  [ADR 0015](../decisions/0015-primitive-types-and-numeric-boundaries.md).
- **Owned over borrowed.** Ludus-owned containers, strings, smart owners,
  parsing and formatting instead of standard-library runtime facilities; a
  missing operation is added to its owning Foundation module with tests.
  See [ADR 0003](../decisions/0003-standard-library-usage-policy.md) and
  [ADR 0008](../decisions/0008-memory-management.md).
- **Bounded over unbounded.** Named capacities, admission limits with one
  owner, build-time budgets, bounded task graphs and parsing, deterministic
  randomness and cooked bytes. See [ADR 0005](../decisions/0005-build-time-budgets.md),
  [ADR 0020](../decisions/0020-bounded-task-graphs.md) and
  [ADR 0022](../decisions/0022-bounded-parsing-foundation.md).
- **Layered and directional.** Foundation never depends upward; public headers
  are light and self-sufficient; shared data formats are owned by neither the
  tool that writes them nor the runtime that reads them. See
  [ADR 0007](../decisions/0007-foundational-core-header.md) and the
  [module include boundary](foundational-headers.md).
- **Reproducible.** One pinned reference toolchain, locked dependencies, one
  implementation of each check invoked by contributors, editor tasks and CI
  alike, and evidence records that name the tested revision and environment.
  See [building](../development/building.md) and
  [engineering quality](engineering-quality.md).
- **Portable by qualification, not by assumption.** Linux is the native
  reference; macOS and the browser (WebGPU with WebGL 2 fallback) are distinct,
  separately qualified capabilities. Recognising a platform never establishes
  support for it. See [platform porting](../development/platform-porting.md).
- **Truthful infrastructure.** Logging, profiling, assertions and status never
  crash the engine or convert a failure into success; an evidence writer
  observes, it does not decide.

## User-experience direction

User-experience direction applies the commitments wherever a person meets
Ludus: the editor, the CLI, the browser preview, diagnostics and the wiki.

- **Product promise.** *Make creative work predictable.* Ludus owns routine
  setup, shows what is happening and protects the user's work. People should
  understand a command's outcome before invoking it and recover from a failed
  operation without reconstructing their workspace.
- **One command catalogue.** Menus, toolbar strips, shortcuts, search, the CLI
  and automation share one command and capability policy and one project
  backend. An unavailable command explains why; hiding it does not invent
  support.
- **Explicit transitions.** Opening and checking are read-only. Creation,
  repair, builds and publication are explicit operations with visible progress
  and cancellation that reports *requested* until the work acknowledges it.
  Save, Discard and Cancel are distinct; a failed save keeps the draft.
- **Draft, session and artifact stay distinct.** A source document, a running
  session and an imported artifact are different owners. Applying a live value
  is not a disk save; a heartbeat never steals focus or rewrites a form.
- **Quiet workspace, discoverable depth.** The current authoring work has first
  claim on space. Secondary panels collapse until useful, advanced options are
  disclosed without hiding the next step, and shortcuts and search remain
  available without being the only path.
- **Accessible by contract.** Keyboard paths, visible focus distinct from
  selection, names and roles on controls, light and dark appearance, density
  and scale options, reduced motion respected, and state conveyed by more
  than colour.
- **Diagnostics are part of the interface.** Errors identify the operation,
  the result and the next action, stay selectable and never claim readiness
  before configure, build and tests have completed. The same words appear in
  the editor, the CLI and the documentation.

The [interaction design](editor-interaction-design.md) and
[GUI systems](editor-gui-systems.md) pages own the concrete layouts, command
behaviour and acceptance gates; the [workspace guide](../development/editor-workspace.md)
documents what is implemented today.

## Applying the identity

Use this checklist when proposing, reviewing or documenting a change:

1. **Truthful:** every claim in the change (status text, screenshot, number,
   documentation sentence) names its evidence or its limit.
2. **Bounded:** new capacities, budgets and limits have one named owner, and
   boundary behaviour is tested.
3. **Owned:** the change lives in its owning module, reuses engine vocabulary
   and states who keeps borrowed storage alive.
4. **Quiet:** new interface, output or branding takes only the attention its
   task needs and can be reached again later.
5. **Studied:** consulted sources are credited near the code, the design page
   explains the adaptation, and the wiki links the reasoning.

A change that cannot pass one of these is reported with the conflict stated,
not shipped with the conflict hidden.

## Changing the identity

The identity changes rarely and deliberately. A proposal states which
commitment or direction changes, why the current one fails a real user or
contributor, and which owner pages must follow. The project owner approves
it; the change updates this page first and the owner pages in the same change,
and records the date in the status line above. Milestones, demos and schedule
pressure are not reasons to relax a commitment; they are reasons to state a
limit truthfully.
