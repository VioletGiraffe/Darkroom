# Settings and theme

[← Back to architecture index](../../ARCHITECTURE.md)

## Settings ownership

Shared `QSettings` keys and defaults live in `Settings.h` as matching `Settings::Foo` and `Defaults::Foo`
constants. Narrow settings used by one implementation remain local to that owner. App-wide derived accessors may
live in `Utils.h`.

`QSettings` holds application preferences, not library or per-item state. Per-item fields belong in
`MetadataStore`; runtime library paths come from an owned or borrowed `Library`. MainWindow's library workflow is
the sole owner of the last-successful-root and recent-library settings.

`SettingsDialog` excludes library selection, which belongs to the Library menu. Most changes persist on acceptance.
Color scheme applies immediately so the open dialog and application can preview it; startup applies the saved scheme
before constructing MainWindow.

## Theme colors

`Theme::current()` selects the light or dark `ThemeColors` set. Code consumes named semantic fields rather than
literal colors. Important families are:

- Border tokens distinguish subtle separators, container outlines, and interactive-control edges.
- Accent border/fill and accent text are separate because tinted surfaces need different contrast.
- Text-selection background and foreground are an explicit pair.
- Invariant colors, such as the active Best star, live outside the scheme-specific ramps.

Most widget-local styles read Theme at construction and therefore update when that widget is recreated. The app-wide
Style sheet and local sheets installed through `Style::applyThemedSheet` subscribe to color-scheme changes and
rebuild in place.

`Style::install` also publishes Theme colors through the application `QPalette`, allowing stock controls and
native fallbacks to follow the scheme. Disabled and selection roles are set explicitly. QSS-styled subcontrols may
ignore palette roles and need their own rules; see
[qt-styling-system-quirks.md](../tips/qt-styling-system-quirks.md).

## Application styling

`Style::install` applies one Theme-generated stylesheet to `qApp` before the main window appears. Shared stock
control styling and high-volume grid/thumbnail object-name rules live there. Central rules avoid repeated per-instance
polishing and automatically rebuild on a live scheme change.

Not every widget belongs in the central sheet. Different scroll areas require different frames, and visuals QSS
cannot express use custom painting or delegates. Corner radii are named Theme constants so relationships remain
consistent across central and custom rendering.

`Style::applyThemedSheet(widget, makeSheet)` is the supported local escape hatch when a Theme-derived stylesheet
must survive a live scheme switch. It applies immediately and regenerates on later changes for the widget's lifetime.
A short-lived widget recreated after each switch can use a construction-time sheet instead.

`ComboPopupRounder` and the splitter-hover enabler are application event filters for Qt behaviors QSS cannot repair
alone. Their constraints and recipes are documented in the styling-quirks page.

## Custom controls

`SegmentedToggle` provides mutually exclusive segments in one control. Programmatic `setCurrentIndex` is silent,
allowing state restoration without triggering work; `currentChanged` represents user activation only.

Per-row label visuals use `LabelRowDelegate` because active, hover, and label-specific state cannot be expressed by
one blanket list rule.

## Icons

Chrome icons are original monochrome SVG resources. `Theme::tintedPixmap` renders an SVG in a requested Theme color.
`Theme::tintedIcon` uses an on-demand `QIconEngine`, rendering at each requested size and device-pixel ratio rather
than scaling one rasterization.

Icon tints are named Theme fields rather than captured colors, so every render resolves the active scheme and follows
a live light/dark change. SVGs referenced directly from QSS remain resource URLs and follow the rules in the styling
quirks document.
