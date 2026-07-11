# Darkroom — Coding conventions

Darkroom-specific coding rules. Check these before writing new code. General cross-project working
principles live in the global `~/.claude/CLAUDE.md` and are not repeated here.

## Assertions — use `assert/advanced_assert.h`, never `<cassert>`

Include `"assert/advanced_assert.h"` (from cpputils, already on INCLUDEPATH). `<cassert>` aborts the process
in debug and is compiled out in release; the `assert_*_r` macros log the failure and keep running, so
invariant violations are caught in shipped builds without crashing.

- `assert_r(expr)` — log on failure, do not abort.
- `assert_message_r(expr, msg)` — same, with a custom message.
- `assert_unconditional_r(msg)` — always-fail, for unreachable paths.
- `assert_and_return_r(expr, retval)` — assert + early return on failure (also `assert_and_return_message_r`,
  `assert_and_return_unconditional_r`). These double as early-return guards.
- `assert_debug_only(expr)` — debug-only; use only where the check is too expensive for release.

Default to `assert_r`. Reach for `assert_debug_only` only for expensive checks.

## Containers — prefer `std::vector` over `QList` (settled 2026-07-03)

Default to `std::vector` for any container. In Qt 6 `QList` is contiguous, so the real differences are its
copy-on-write (hidden detach costs on every non-const access — hence the `std::as_const` ranged-for idiom)
and its hard requirement that elements be copy-constructible (move-only types like `std::unique_ptr` holders
don't compile). `std::vector` has neither problem. Mirrors the stdint-over-Qt-typedefs preference: standard
C++ by default, Qt types at Qt seams.

Reach for `QList` only with a specific reason: the data crosses a Qt API boundary that speaks
`QList`/`QStringList` (a conversion would be pure noise), the immediately surrounding code already handles the
same data as `QList`, or CoW cheap-copy is genuinely wanted. (`QHash`/`QSet` are out of scope — the rule is
about `QList`.)

**Corollary:** on a Qt container, prefer the STL-compatible method name over the Qt-flavored duplicate —
`empty()` over `isEmpty()`, `push_back()`/`emplace_back()` over `append()`, `size()` over `count()`,
`front()`/`back()` over `first()`/`last()`. Keeps code uniform with adjacent std containers and makes a later
container-type swap a smaller diff.

## Identity — carry `MediaId`, not a path, across boundaries

When referring to a *tracked* media item across a call or time boundary, carry its `MediaId` (the stable
name+size identity, `Core/MediaId.h`) rather than a filesystem path — and never re-derive identity with
`MediaId::fromFile(path)` at a point where the file may have moved. `fromFile()` stats the file for its size;
if the source was relocated in between (e.g. the Import dialog's "Move source file to:" relocation), it
returns an invalid id (`size == -1`) that matches nothing. The catalog API (`folderForMediaItem`, `addLabel`,
…) is already id-based — match it.

Paths are still correct where you genuinely need the file on disk (open, copy, relocate, import). For
string-only needs without touching disk, `MediaId::name()` yields the original filename and
`MediaId::fromNameAndSize()` reconstructs an id.

## Dialogs — flush caller-visible state from `done(int)`, not the destructor

For a `QDialog` subclass that accumulates state during its lifetime and needs the caller to see the result of
applying it right after `exec()` returns, do the flush from an overridden `done(int result)` (before
delegating to `QDialog::done(result)`), not the destructor. `done()` is the single funnel every
dialog-finishing path runs through (`accept()`, `reject()`, Escape, the window-close button) and runs *while
`exec()`'s internal event loop is still active*, so the postcondition holds the instant `exec()` returns —
regardless of whether the dialog is stack- or heap-allocated or kept as a member. A destructor-based flush
only works if the caller destroys the object before doing anything that depends on the flush, which the
dialog cannot guarantee from the inside. Reserve the destructor for state nobody else waits on (e.g.
persisting `QSettings` geometry). See [architecture/import.md](architecture/import.md) for the `ImportDialog`
case that established this.

## Sorting — natural sort for strings that may contain numbers

Most user-visible strings (file and media names) contain numbers, so sort them with natural comparison, not
plain lexicographic `<` / `QString::compare`:

```cpp
#include "utils/naturalsorting/cnaturalsorterqcollator.h"
// NaturalSort::lessCaseSensitive / NaturalSort::lessCaseInsensitive
```

(from the qtutils submodule).

## i18n — route user-facing strings through `tr()`

All user-facing strings in `app/src` are wrapped for translation-readiness (no `.ts` file / `QTranslator`
wired up yet). Match this for new UI strings:

- Member-function strings use `tr(...)`; free-function / namespace-scope strings use `QObject::tr(...)`.
- Concatenated messages use `%1` placeholders — `tr("Failed to create collection:\n%1").arg(path)` — not
  fragment-wrapping.
- Don't add `Q_OBJECT` solely for `tr()` — strings-only.
- Leave un-wrapped: `QSettings` keys, stylesheet/QSS, object names, ffmpeg args/paths, `QTime` format codes,
  JSON keys, pure separators (`", "`, `" - "`, `"<br>"`), and iconic single glyphs (★, ⏳, ↑, ↓, ×).

## Qt styling / QSS

Read [tips/qt-styling-system-quirks.md](tips/qt-styling-system-quirks.md) before any `QComboBox` or general
QSS customization. It documents the QSS / `QStyle` limitations (the border-triangle arrow hack, `image:` /
`url()` not loading in-memory data, subcontrol geometry not inheriting the base rule, `QProxyStyle` becoming
unreachable once a subcontrol is QSS-styled, and the popup container being unroundable by QSS) that were
rediscovered the slow, token-expensive way once already — and the `ComboPopupRounder` solution in
`Theme/Style.cpp`.
