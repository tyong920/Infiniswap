# Git

Project git hygiene for this repository.

## Ignore rules

Root [`.gitignore`](../../.gitignore) covers:

- CMake output
- Kernel-module and userspace build artifacts
- Local indexes (`.codegraph/`) and editor/OS files

Do not commit `.ko`, `.o`, generated CMake files, or daemon binaries.

## Line endings

[`.gitattributes`](../../.gitattributes) normalizes text to LF. Binaries stay binary.

## Enable commit hooks

Hooks live in [`.githooks/`](../../.githooks/) (tracked). Enable once per clone:

```bash
git config core.hooksPath .githooks
```

What they do:

- **pre-commit** — blocks staged build/generated artifacts that should stay untracked
- **commit-msg** — requires a non-empty subject ≤ 72 characters, no trailing period

## Commit message style

Follow the recent history tone: short imperative / descriptive subject, optional body.

Examples from this repo:

- `docs: record production modernization decisions`
- `fix: avoid kernel panic when cloning IO request`

Prefer `area: summary` when it helps (`bd:`, `daemon:`, `setup:`, `docs:`).
