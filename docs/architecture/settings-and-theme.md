# Settings & theme

[← Back to architecture index](../../ARCHITECTURE.md)

## Settings pattern

All shared `QSettings` keys live in `src/Settings.h`, two namespaces: `Settings::Foo` (string key) and
`Defaults::Foo` (compile-time default). Read everywhere as
`QSettings{}.value(Settings::Foo, Defaults::Foo).toFoo()`.

Convenience getters live in **`Utils.h`** when truly app-wide (`ffmpegPath()`) or as file-local
inline helpers in the owning `.cpp` when narrower. A few keys are deliberately **kept out of Settings.h**
because they're local to one UI/class, read directly via inline `QSettings{}`. The `MetadataStore`'s `catalog.json` (see
[data-model.md](data-model.md))
is separate from `QSettings` entirely — that's per-item data, not app configuration. Runtime root paths come
from an explicitly owned/borrowed `Library`, never from `QSettings`; `MainWindow`'s library-opening workflow
alone reads and writes the last successfully opened root.

## `SettingsDialog` (`src/Windows/SettingsDialog.h/.cpp`)

Library selection belongs to the Library menu rather than application preferences. Color-scheme applies
immediately (unlike other settings which persist on accept) — via `setColorScheme()` at dialog time, and
again at startup in `main.cpp`, before `MainWindow`.

## Theme (`src/Theme/Theme.h/.cpp`)

`Theme::ThemeColors` (a struct of hex-string fields) + `Theme::current()`, which picks the `Dark` or `Light`
palette from the system color scheme. Usage: `Theme::current().Field` everywhere; invariant colors
(`StarActive`, gold) are namespace constants. **`current()` is read at widget construction**, so most custom
stylesheets don't update on a live theme change (intentional: "works after restart"; Qt's own widgets do
update live). The exceptions both re-apply on `colorSchemeChanged`: the app-wide `Style` sheet (below), and
any widget-local sheet that opts into it via `Style::applyThemedSheet` (also below).

Each `ThemeColors` field is tuned to a specific CSS variable in the design mockup; the per-field comments in
`Theme.h` name the source variable so a retune can go back to it instead of guessing. Two splits are worth
knowing when picking a color at a new call site: borders come in three strengths — **`BorderSubtle`** (hairlines),
**`BorderMedium`** (floating-surface / container outlines), and **`BorderStrong`** (resting edge of inline interactive
controls; tuned toward `TextPrimary` so they don't read as disabled). The emphasis hue is **`AccentBorder`** (accent
borders/fills/dots) vs
**`AccentText`** (text on an `AccentBg`-tinted surface). Both exist because the mockup uses different strengths for what
could look like one
color — pick the wrong half and it'll look subtly off. Distinct from both, **`SelectionHighlight`**/**`SelectedText`**
are the text-selection pair: `SelectionHighlight`
is a legibility-adjusted accent variant (readable against selected text in both themes), while `SelectedText` is its own
token so a future theme can diverge. Two colors deliberately sit *off* the neutral ramp (the mockup's hue-shift retune,
adopted 2026-07): **`BackgroundSecondary`** and **`MutedText`** — so the raised-row fill and the least-prominent text
have a character of their own rather than being more grey steps. The accent family stays **blue** on purpose where the
mockup's `info` tokens are green: a settled
divergence, not drift (the mockup's own selected-row bar uses this blue).

The app-wide `QPalette` (set in `Style::install()` alongside the stylesheet, re-applied on
`colorSchemeChanged`) carries the theme's background/text/accent colors into the standard `QPalette` roles.
This is what lets stock controls and native fallbacks follow the themed ramps too, instead of staying on the
OS-default grey palette. The selection roles `Highlight`/`HighlightedText` carry the `SelectionHighlight`/`SelectedText`
pair, keeping any genuinely stock (unstyled) palette-driven selection consistent with the `QLineEdit` selection the
sheet styles with the same pair. The `QComboBox` popup is an exception: its view is QSS-styled, so
`QStyleSheetStyle` ignores the palette role — it needs an explicit `::item:selected` rule. The Disabled color
group gets an explicit `MutedText` override — without it, disabled controls would not dim at all.

## App-wide styling (`src/Theme/Style.h/.cpp`)

`Style::install()` (called once in `main.cpp`, after the colour scheme is set, before the window shows)
builds one `Theme`-driven stylesheet and applies it to `qApp`, providing the shared non-stock vocabulary
(rounded corners, hairline borders, roomy padding, soft hover/focus) for stock controls. `QScrollArea` is
deliberately **not** styled centrally: its uses want different frames. The sheet also carries **grid card,
star, and thumbnail** styles (by object name, e.g. `#mediaItemCard`), kept here rather than per-instance
`setStyleSheet` — that polishes too slowly when the grid and frame viewer build hundreds at once, and a
central sheet also follows a live theme switch. Every corner radius used anywhere in the app is a named
constant in `Theme.h` rather than a literal, so the relationships between them live in one place instead of
being rediscovered. `QComboBox`'s drop-down popup is rounded by `ComboPopupRounder`, an app-wide event filter
that hand-paints the popup surface — QSS cannot round that popup itself. See
[qt-styling-system-quirks.md](../tips/qt-styling-system-quirks.md) for the underlying Qt/QSS limitations this
worked around; read it *before* attempting any further `QComboBox`/QSS customization to avoid
re-discovering the same dead ends.

Every `QListWidget` gets a blanket `background: transparent` rule — per the mockup, plain lists blend into
whatever surface hosts them rather than standing out as their own "input" surface. The rule deliberately does
**not** reach into item geometry or per-item coloring (that would disturb the grid's sized cards);
selection/hover colors stay local to each list since they vary by use.

The sheet **re-applies on `QStyleHints::colorSchemeChanged`**, so the globally-styled chrome follows a live
light/dark switch — unlike the per-widget construction-time stylesheets elsewhere. For the few widget-local
sheets that *do* need to survive a live switch, **`Style::applyThemedSheet(widget, makeSheet)`** is the
sanctioned escape hatch: it applies `makeSheet()` now and re-runs it (reading the fresh `Theme`) on each
`colorSchemeChanged`, bound to the widget's lifetime — so the styling stays next to the widget while still
tracking the theme, rather than being hoisted into the central sheet. This matters most for `SettingsDialog`:
the theme toggle lives there, so a snapshot color would visibly lag the switch in front of the user. Reach
for it instead of a bare `setStyleSheet` whenever a `Theme`-derived local sheet outlives a possible switch; a
transient dialog that is rebuilt per-open doesn't need it.

This is the "central sheet + custom widgets" approach for matching the design mockup
(`docs/mockups/main-window-sidebar.html`): the sheet covers what QSS can express app-wide; widgets needing
looks QSS can't (per-row colours, segmented controls) add custom painting — `LabelRowDelegate` (see
[main-window.md](main-window.md)) and `SegmentedToggle` below.

### `SegmentedToggle` (`src/UiComponents/SegmentedToggle.h/.cpp`)

A reusable segmented control: mutually-exclusive segments in one rounded pill. `setCurrentIndex()` is
**silent** (for restoring persisted state without re-triggering work); `currentChanged(int)` emits **only**
on user clicks. Drives the sidebar's OR/AND combine mode and the `SortControl` popover's field/direction
toggles (see [main-window.md](main-window.md)).

### Icons (`src/Theme/Icons.h/.cpp`)

The app's chrome glyphs: **monochrome SVGs** under `res/UI/`. They're **original hand-authored glyphs**, not
copied from Tabler, so the repo carries no third-party asset license. Some SVGs (`combobox_down_arrow`,
`checkbox_check`) are consumed by QSS `url()` rather than the icon helper. `Theme::tintedPixmap` recolors an
SVG so one asset serves any theme color (the SVG's own stroke color is irrelevant). `Theme::tintedIcon`
wraps that in a **`QIconEngine` that renders on demand** at exactly the size and device pixel ratio each
request carries, so the glyph stays crisp at fractional display scaling (125/150/175%, the Windows norm)
instead of being pre-rasterized at a fixed size and bitmap-scaled. The tint is **named, not captured**: the
engine resolves against `Theme::current()` on **every** render — so, unlike the per-widget construction
stylesheets, these icons **do** follow a live light/dark switch.
