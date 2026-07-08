# Settings & theme

[← Back to architecture index](../../ARCHITECTURE.md)

## Settings pattern

All shared `QSettings` keys live in `src/Settings.h`, two namespaces: `Settings::Foo` (string key) and
`Defaults::Foo` (compile-time default). Read everywhere as
`QSettings{}.value(Settings::Foo, Defaults::Foo).toFoo()` — covering the root folder, ffmpeg path, output
format/quality, playback, and color scheme.

Convenience getters live in **`Utils.h`** when app-wide (`rootFolder()`, `ffmpegPath()`) or as file-local
inline helpers in the owning `.cpp` when narrower. A few keys are deliberately **kept out of Settings.h**
because they're local to one UI/class (preview frame count, card zoom, frame-viewer thumbnail size), read
directly via inline `QSettings{}`. The `MetadataStore`'s `catalog.json` (see [data-model.md](data-model.md))
is separate from `QSettings` entirely — that's per-item data, not app configuration.

## `SettingsDialog` (`src/Windows/SettingsDialog.h/.cpp`)

Uses `CSettingsDialog`/`CSettingsPage` from `qtutils` (`settingsui/`). Caveat:
`CSettingsDialog::accept()` runs every page's `acceptSettings()` (returns void) then accepts — so the
required-root-folder validation only **warns** and skips saving, but the dialog still closes. Color-scheme
radios apply immediately via `setColorScheme()` (and again at startup in `main.cpp`, before `MainWindow`).

## Theme (`src/Theme/Theme.h/.cpp`)

`Theme::ThemeColors` (a struct of hex-string fields) + `Theme::current()`, which picks the `Dark` or `Light`
palette from the system color scheme. Usage: `Theme::current().Field` everywhere; invariant colors
(`StarActive`, gold) are namespace constants. **`current()` is read at widget construction**, so most custom
stylesheets don't update on a live theme change (intentional: "works after restart"; Qt's own widgets do
update live). The exceptions both re-apply on `colorSchemeChanged`: the app-wide `Style` sheet (below), and
any widget-local sheet that opts into it via `Style::applyThemedSheet` (also below).
Adding a color means adding the field to **both** `Dark` and `Light` — the designated-initializer definitions
make the compiler catch a missing one.

Each `ThemeColors` field is tuned to a specific CSS variable in the design mockup; the per-field comments in
`Theme.h` name the source variable so a retune can go back to it instead of guessing. Two splits are worth
knowing when picking a color at a new call site: borders come in three strengths — **`BorderSubtle`** (faint
hairlines — card borders, dividers, panel edges), **`BorderMedium`** (floating-surface / container outlines —
menus, tooltips, combo popups, list frames), and **`BorderStrong`** (the resting edge of inline interactive
controls — buttons, inputs, combo field, slider handle, checkbox, segmented toggle; tuned toward `TextPrimary`
so they don't read as disabled). The emphasis hue is **`AccentBorder`** (accent borders/fills/dots) vs
**`AccentText`** (text on an `AccentBg`-tinted surface). Both exist because the mockup uses different strengths for what could look like one
color — pick the wrong half and it'll look subtly off. Distinct from both, **`SelectionHighlight`**/**`SelectedText`** are the
text-selection pair (the `QLineEdit`/text-edit selection and the app-wide `Highlight`/`HighlightedText` roles): `SelectionHighlight`
is `AccentBorder` pushed away from the *un-inverted* selected text so it stays legible — darkened but still vivid in dark, kept bright
in light — while `SelectedText` equals `TextPrimary` today but is its own token so a future theme can diverge. Two colors deliberately sit *off* the neutral ramp
(the mockup's hue-shift retune, adopted 2026-07): **`BackgroundSecondary`** (the raised/selected-row surface,
e.g. the sidebar's active row) and **`MutedText`** — sage-tinted in light, wine-tinted in dark — so the
selected-row fill and the least-prominent text have a character of their own rather than being more grey
steps. The accent family stays **blue** on purpose where the mockup's `info` tokens are green: a settled
divergence, not drift (the mockup's own selected-row bar uses this blue).

The app-wide `QPalette` (set in `Style::install()` alongside the stylesheet, re-applied on
`colorSchemeChanged`) carries the theme's background/text/accent colors into the standard `QPalette` roles.
This is what lets stock controls and native fallbacks follow the themed ramps too, instead of staying on the
OS-default grey palette. The selection roles `Highlight`/`HighlightedText` carry the `SelectionHighlight`/`SelectedText`
pair, keeping any genuinely stock (unstyled) palette-driven selection consistent with the `QLineEdit` selection the
sheet styles with the same pair. (This does *not* cover the `QComboBox` popup: its view is QSS-styled, so
`QStyleSheetStyle` draws that selection and ignores the palette role — it needs an explicit `::item:selected` rule.)
The Disabled color group gets an explicit
`MutedText` override for the text roles — without it, `setColor(role, ...)` fills every group with the
full-strength colors and disabled controls would not dim at all.

## App-wide styling (`src/Theme/Style.h/.cpp`)

`Style::install()` (called once in `main.cpp`, after the colour scheme is set, before the window shows)
builds one `Theme`-driven stylesheet and applies it to `qApp`: the shared non-stock vocabulary (rounded
corners, hairline borders, roomy padding, soft hover/focus) for stock controls — `QPushButton`, `QLineEdit`,
`QComboBox`, `QMenu`, `QMenuBar` (transparent bar, `QMenu`-matching hover pills on its items), `QScrollBar`,
`QSlider` (horizontal only; note a QSS-styled slider renders no tick marks — a QStyleSheetStyle limitation),
`QCheckBox` (rounded indicator with an accent fill + white SVG check mark; this is the app's one checkbox
look, used e.g. by the sort popover's "Favorites first"), `QGroupBox` (hairline card, title straddling the
top border), `QSplitter` (handle invisible until hovered/dragged, then `AccentBg`; needs `SplitterHandleHoverEnabler`
setting `WA_Hover` on each handle, or the hover rule never matches - see the quirks doc) and `QToolTip` (square on
purpose — rounding a tooltip would need ComboPopupRounder-style hand-painting). `QScrollArea` is deliberately
**not** styled centrally: its two uses want different frames (FrameViewerWindow sets `NoFrame` — full-window
content with nothing to frame off; IntegrityCheckDialog opts into the list-style hairline locally). The sheet is
assembled from per-concern `constexpr` sections (buttons, text inputs, combos, menus, ...) written against
named `%Token%` placeholders — spelled exactly like the `Theme::ThemeColors` field or `Theme::` constant they
resolve to — and `styleSheetString()` concatenates the sections and resolves all tokens in one replace pass. It also carries the **grid card, star and thumbnail** styles (by object
name, e.g. `#mediaItemCard`), moved here from per-instance `setStyleSheet` because those polished too slowly when
the grid and frame viewer build hundreds at once — and, being in this sheet, they now follow a live theme
switch too. Every corner radius used anywhere in the app is a named constant in `Theme.h` rather than a
literal, so the relationships between them (which are coincidentally equal vs. conceptually the same) live in
one place instead of being rediscovered. `QComboBox` also gets a themed SVG down-arrow and its drop-down popup
is rounded by `ComboPopupRounder`, an app-wide event filter that hand-paints the popup surface — QSS cannot
round that popup itself. See
[qt-styling-system-quirks.md](../tips/qt-styling-system-quirks.md) for the underlying Qt/QSS limitations this
worked around (the arrow-via-border-hack dead end, `QProxyStyle` not being reachable once a subcontrol is
styled, why the popup container can't be rounded with plain QSS); read it *before* attempting any further
`QComboBox`/QSS customization to avoid re-discovering the same dead ends. `Style::install()` also sets an app-wide `QProxyStyle`
(`FocusFrameStyle`) *before* the sheet, widening the `QPushButton` keyboard-focus rect off the label — reachable
precisely because no `QPushButton:focus` rule means QSS leaves that primitive delegated to the base style (the
reachable converse of the subcontrol limitation above). Every `QListWidget` also gets a
blanket rule here — the same hairline
border as the other stock controls, but `background: transparent` instead of a filled surface: per the
mockup, plain lists blend into whatever surface hosts them (sidebar panel, dialog body) rather than standing
out as their own "input" surface, matching the window/parent background instead of the native
`QPalette::Base` fill a scroll area defaults to. That's the *only* thing styled globally for lists — it
deliberately does **not** reach into item geometry or per-item coloring (that would disturb the grid's sized
cards); selection/hover colors stay local to each list since they vary by use (see
`MainWindow`'s grid, `LabelRowDelegate`). Because of this, each list's own
`setFrameShape(QFrame::NoFrame)` calls were removed as redundant once this rule landed. It **re-applies on
`QStyleHints::colorSchemeChanged`**, so the globally-styled chrome follows a live light/dark switch — unlike
the per-widget construction-time stylesheets elsewhere. For the few widget-local sheets that *do* need to
survive a live switch, **`Style::applyThemedSheet(widget, makeSheet)`** is the sanctioned escape hatch: it
applies `makeSheet()` now and re-runs it (reading the fresh `Theme`) on each `colorSchemeChanged`, bound to
the widget's lifetime — so the styling stays next to the widget while still tracking the theme, rather than
being hoisted into the central sheet. Used by `MainWindow`'s grid-selection tint, `FrameViewerWindow`'s
instruction label, and `SettingsDialog`'s JPEG-quality hint (that last one matters most: the theme toggle
lives in that dialog, so a snapshot color would visibly lag the switch in front of the user). Reach for it
instead of a bare `setStyleSheet` whenever a `Theme`-derived local sheet outlives a possible switch; a
transient dialog that is rebuilt per-open (e.g. `IntegrityCheckDialog`) doesn't need it.

This is the "central sheet + custom widgets" approach for matching the design mockup
(`docs/mockups/main-window-sidebar.html`): the sheet covers what QSS can express app-wide; widgets needing
looks QSS can't (per-row colours, segmented controls) add custom painting — `LabelRowDelegate` (see
[main-window.md](main-window.md)) and `SegmentedToggle` below.

### `SegmentedToggle` (`src/UiComponents/SegmentedToggle.h/.cpp`)

A reusable segmented control: mutually-exclusive segments in one rounded pill, the selected one filled with
`AccentBg` + `AccentText` text, hairline separators, hover feedback. API mirrors a checkable control —
`currentIndex()`, `setCurrentIndex()` (**silent**, for restoring persisted state without re-triggering work),
and `currentChanged(int)` (emitted **only** on user clicks). Drives the sidebar's OR/AND combine mode; built
reusable for the planned sort popover's field/direction toggles.

### Icons (`src/Theme/Icons.h/.cpp`)

The app's chrome glyphs (the mockup's Tabler-style icons). Assets are **monochrome SVGs** under `res/UI/`
(`icon_stack`, `icon_plus`, `icon_search`, `icon_columns`, `icon_sort` — plus the older `combobox_down_arrow`
and `checkbox_check` consumed by QSS `url()` rather than this helper). They're **original hand-authored
glyphs**, not copied from Tabler, so the repo carries no third-party asset license. `Theme::tintedPixmap`
renders an SVG through `QSvgRenderer` and recolors it with `CompositionMode_SourceIn` (keeps the glyph's
antialiased alpha, replaces its RGB) — so one asset serves any theme color, and the SVG's own stroke color is
irrelevant. `Theme::tintedIcon` wraps that in a **`QIconEngine` that renders on demand** at exactly the size
and device pixel ratio each request carries, so the glyph stays crisp at fractional display scaling
(125/150/175%, the Windows norm) instead of being pre-rasterized at a fixed size and bitmap-scaled — and the
consumer's own icon size drives it (no size argument; `SortControl` uses `setIconSize(20,15)` for its
non-square padded box, whose SVG viewBox is a matching 32×24 so the render doesn't distort). Call sites:
`LabelRowDelegate` (the "All" row's stack, drawn straight from `tintedPixmap` at the widget DPR + cached by
tint/DPR), the sidebar "Create label" button, `MainWindow`'s name-filter search icon + preview-count combo,
and `SortControl`'s chip. The tint is **named, not captured**: `tintedIcon` takes a pointer to the
`ThemeColors` field to use (`&ThemeColors::MutedText`), which the engine resolves against `Theme::current()`
on **every** render — so, unlike the per-widget construction stylesheets, these icons **do** follow a live
light/dark switch. The mechanism is not a signal connection (a `QIconEngine` isn't a `QObject`): the
scheme-change handler already repaints every widget, that repaint re-invokes the engine (QIcon doesn't cache
a custom engine's output), and the engine reads the now-current theme. A **disabled** control's icon uses the
palette's muted disabled tone (also live) faded to 50% alpha, combining both dim cues. Best keeps its `★`
glyph and labels keep their colour dots, per the mockup.
