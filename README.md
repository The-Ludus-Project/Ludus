# Ludus

Ludus is a C++23 engine and toolset in active development, with modular runtime
systems, an installed SDK and CLI, and an optional Linux editor. A bounded editor
preview also runs in the browser.

[Read Ludus Wiki](https://the-ludus-project.github.io/Ludus/) or open
[the same documentation locally](docs/README.md). The Markdown, local assets and
checked HTML are also available as an [offline bundle](docs/development/wiki.md#offline-reading).

## Build the engine

The native reference host is Ubuntu 24.04 with Clang/LLVM 23 (from apt.llvm.org). From a clone:

```bash
./init.sh --cli linux-clang-development --preset-only --locked --with-tests
./scripts/build linux-clang-development
./scripts/test linux-clang-development
```

[Building Ludus](docs/development/building.md) owns setup flags, Linux/headless
and container instructions, macOS support, presets, IDE setup and validation.
Initial dependency setup needs internet; prepared builds use the local cache.

## Choose a task

- [Install and launch the desktop editor](docs/wiki/getting-started/install.md)
- [Try the browser editor](docs/wiki/guides/browser-editor.md)
- [Create your first game project](docs/wiki/getting-started/first-project.md)
- [Install host tools and share SDKs](docs/development/project-sdk-workflow.md)
- [Build, run and debug a game](docs/wiki/guides/build-and-debug.md)
- [Edit native gameplay live](docs/development/project-live-reload.md)
- [Package and publish a game](docs/wiki/guides/releases.md)
- [Review current capabilities](docs/wiki/getting-started/status.md)
- [Explore architecture and module ownership](docs/wiki/architecture/index.md)

## Contribute

Read [AGENTS.md](AGENTS.md) and [.kiro/steering](.kiro/steering/coding-standards.md)
before changing code. Use the [contributor guide](docs/wiki/contribute/index.md)
for validation and review requirements, and [documentation development](docs/development/wiki.md)
for editing, previewing and publishing the canonical sources.
