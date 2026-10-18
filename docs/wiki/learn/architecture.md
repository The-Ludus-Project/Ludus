# How Ludus fits together

Understanding ownership makes a workflow easier to predict and debug. Start with
the boundaries you use every day, then follow the source references when you
need implementation detail. The [identity and direction](../../architecture/engine-identity.md)
page explains why these boundaries exist: the commitments below are its
technical consequences.

## Four useful boundaries

| Boundary | Responsibility | Practical consequence |
| --- | --- | --- |
| Engine checkout | Engine modules, trusted tools and reference applications | Build Ludus and produce SDKs here |
| Installed SDK | Public headers, compiled libraries, dependencies and compatibility metadata | A game consumes a compatible installation rather than rebuilding engine source implicitly |
| Game project | Authored code/content, descriptor, CMake files and release intent | Commit portable intent; keep local machine paths ignored |
| Editor and CLI | Clients of shared project/build policy | Opening/checking is read-only; creation, repair and builds are explicit operations |

## Module direction

Foundation vocabulary sits below higher-level modules. Public module headers
live in `include/`; implementation headers stay private. FoundationBase must not
depend on higher layers. Qt belongs to the optional native editor, not the
installed engine SDK.

This keeps engine consumers independent of desktop UI details and makes include
cost and dependency direction enforceable. See the
[module include boundary](../../architecture/foundational-headers.md).

## Persistent data and running state

A source document describes authored intent. A runtime snapshot describes a
running session. Applying a live value is not a disk save. Saving preferences is
not a live host mutation. Explicit commands, revisions and validation connect
these owners where the workflow supports it.

Read [configuration](../guides/configuration.md) for a concrete layered example,
and [audio](../guides/audio.md) for content identity and preview ownership.

## Errors and operations

Engine code is exception-free and reports failures explicitly. Tools present
those failures with progress and recovery context. A cancellable operation owns
its child processes until cleanup is confirmed; a lost reply is not sufficient
evidence that work has stopped.

See the [editor architecture](../../architecture/editor-architecture.md)
and [module lifecycle contract](../../architecture/module-lifecycle.md).

## Where to learn next

The [engine architecture map](../architecture/index.md) provides subsystem
guides for foundations, jobs, lifecycle, rendering, worlds, content and the
GameHost/editor boundary. Each guide separates implemented behavior from future
design and links to its source, contracts and evidence.

Choose one concrete task, inspect its source and tests, then use a
[reference reading path](references.md) to compare design options. Design and
evidence documents describe their own baseline and acceptance scope; check both
before treating a proposal as a shipping feature.
