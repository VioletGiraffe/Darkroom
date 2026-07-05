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
update live). The exception is the app-wide `Style` sheet (below), which re-applies on `colorSchemeChanged`.
Adding a color means adding the field to **both** `Dark` and `Light` — the designated-initializer definitions
make the compiler catch a missing one.

Each `ThemeColors` field is tuned to a specific CSS variable in the design mockup; the per-field comments in
`Theme.h` name the source variable so a retune can go back to it instead of guessing. Two splits are worth
knowing when picking a color at a new call site: hairlines are **`BorderSubtle`** (passive separators — card
borders, dividers, panel edges) vs **`BorderControl`** (interactive outlines — buttons, inputs, popovers), and
the emphasis hue is **`AccentBorder`** (accent borders/fills/dots) vs **`AccentText`** (text on an
`AccentBg`-tinted surface). Both exist because the mockup uses different strengths for what could look like one
color — pick the wrong half and it'll look subtly off.

The app-wide `QPalette` (set in `Style::install()` alongside the stylesheet, re-applied on
`colorSchemeChanged`) carries the theme's background/text/accent colors into the standard `QPalette` roles.
This is what lets stock controls and native fallbacks (e.g. the combo drop-down's item selection) follow the
themed ramps too, instead of staying on the OS-default grey palette. The Disabled color group gets an explicit
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
`QComboBox`/QSS customization to avoid re-discovering the same dead ends. Every `QListWidget` also gets a
blanket rule here — the same hairline
border as the other stock controls, but `background: transparent` instead of a filled surface: per the
mockup, plain lists blend into whatever surface hosts them (sidebar panel, dialog body) rather than standing
out as their own "input" surface, matching the window/parent background instead of the native
`QPalette::Base` fill a scroll area defaults to. That's the *only* thing styled globally for lists — it
deliberately does **not** reach into item geometry or per-item coloring (that would disturb the grid's sized
cards); selection/hover colors stay local to each list since they vary by use (see
`FindUntrackedFilesDialog`, `MainWindow`'s grid, `LabelRowDelegate`). Because of this, each list's own
`setFrameShape(QFrame::NoFrame)` calls were removed as redundant once this rule landed. It **re-applies on
`QStyleHints::colorSchemeChanged`**, so the globally-styled chrome follows a live light/dark switch — unlike
the per-widget construction-time stylesheets elsewhere.

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
