# Settings and theme

[← Back to architecture index](../../ARCHITECTURE.md)

## Settings ownership

Shared `QSettings` keys and defaults live in `Settings.h` as matching `Settings::Foo` and `Defaults::Foo`
constants. Narrow settings used by one implementation remain local to that owner. App-wide derived accessors may
live in `Utils.h`. Theming preferences are the exception: qtutils' `CThemeController` owns and persists them under
its own keys.

`QSettings` holds application preferences, not library or per-item state. Per-item fields belong in
`MetadataStore`; runtime library paths come from an owned or borrowed `Library`. MainWindow's library workflow is
the sole owner of the last-successful-root and recent-library settings.

`SettingsDialog` excludes library selection, which belongs to the Library menu. Most changes persist on acceptance.
The theming choices - color scheme and one theme per polarity - apply and persist immediately as a live preview;
rejecting the dialog restores all three.

## Themes

`Theme::Theme` (`Theme/Theme.h`) is pure data: name and polarity, a `CBasePalette`, `Metrics`, the app-specific
colors (star and label-UI tones, `instructionText`, `thumbnailMatte`, `readyGreen`), and an optional QSS fragment
appended after the app sheet so its rules win equal-specificity ties. The selectable themes are one table in
`Theme.cpp`; `Theme::current()` returns a copy of the resolved one.

`Metrics` holds every radius, thickness, and indicator size the stylesheet and custom painting use, so a theme
reshapes by changing numbers; the fragment covers structure metrics cannot express.

The app-specific colors resolve the way the palette's derived fields do: `selectActiveTheme()` fills any left unset
from the palette and the polarity, so a theme authors its palette and nothing else unless it needs to differ. It also
supplies its own `accentBg` default ahead of `resolvedPalette`, blended to a target contrast against the window: the
media grid fills a whole card with that colour, where the library's flat tint is sized for a text highlight.

qtutils provides the app-agnostic parts: `CThemeController` (persists the choices, drives
`QStyleHints::setColorScheme()` so the platform follows, emits `themeChanged()` when the effective theme changes),
`CBasePalette` with its derivations (nine authored colors, the rest derivable), the tinted icon engine, the
`themeicon:/` handler, and the styling fixups. The app defines its themes and its stylesheet. Polarity is always
read from Qt. Colors are `QColor`, stringified only during sheet assembly.

## Application styling

`Style::install` runs once at startup: it installs the `themeicon:/` handler, the focus-frame proxy style, and the
Qt-fixup event filters, then applies the active theme - `qtPaletteFor()` publishes the palette for stock controls,
and one theme-generated stylesheet goes to `qApp`. The same apply runs on every `themeChanged()`.

The fixups (`CComboPopupRounder`, `CSplitterHandleHoverEnabler`, `CFocusFrameStyle`) resolve their colors and
metrics through provider callbacks at each use, so they follow a switch without reinstalling. Their rationale is in
[qtutils' styling-quirks page](../../qtutils/docs/qt-styling-quirks.md).

The app sheet is where a visual lives by default; a widget-local sheet is the exception. Widgets the sheet targets
individually carry an object name (`mediaItemCard`, `framedThumbnail`, `cardStar`), so a widget's own source may set
none of its visuals.

Visuals QSS cannot express use custom painting or delegates, which read `Theme::current()` at paint time and need
no subscription. `Style::applyThemedSheet(widget, makeSheet)` covers widget-local sheets that must survive a
switch; a short-lived widget recreated after each switch can use a construction-time sheet instead.

## Icons

Icons are monochrome SVGs with tintable strokes and fills marked `currentColor`; there are no per-theme image
variants.

- Code-consumed icons: `tintedSvgIcon()` renders per requested size and DPR, resolving its color per render
  through a callback. `tintedSvgPixmap()` serves delegates that paint directly.
- QSS-consumed glyphs (checkbox check, combo arrow): `themeicon:/name-rrggbb.svg` URLs built by `themeIconUrl()`
  during sheet assembly, served by `CThemeIconHandler` patching the source SVG in memory. The tint is part of the
  URL, so no invalidation is needed.

The Best label's persisted color is deliberately not themed: `Catalog` seeds it from its own constant, keeping
Core free of the theme runtime.

## Custom controls

`SegmentedToggle` provides mutually exclusive segments in one control. Programmatic `setCurrentIndex` is silent,
allowing state restoration without triggering work; `currentChanged` represents user activation only.

Per-row label visuals use `LabelRowDelegate` because active, hover, and label-specific state cannot be expressed by
one blanket list rule.
