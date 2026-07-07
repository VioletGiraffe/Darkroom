# Qt styling system quirks

Hard-won gotchas about Qt Style Sheets (QSS) and `QStyle`, collected so we don't rediscover them the slow
way. Most came out of styling the `QComboBox` arrow and rounding its drop-down popup (see
`ComboPopupRounder` and the `QComboBox` rules in [`app/src/Theme/Style.cpp`](../../app/src/Theme/Style.cpp)).

## QSS can't draw shapes - it fills boxes

- **The CSS "border triangle" hack does not work in Qt.** A zero-size box with thick one-side borders makes
  a triangle in a browser (mitred corners); Qt's QSS renderer fills each border edge as a flat rectangular
  band, so you get a rectangle, not an arrow. Any arrow/chevron/checkmark glyph must come from an `image:`,
  or be hand-painted.
- **Anti-aliased rounded rectangles, borders, dashes:** when QSS can't express it cleanly, paint it yourself
  with `QPainter` (see the custom widgets `SegmentedToggle`, `SortControl`, and `ComboPopupRounder`). Inset
  the rect by half a pixel (`adjusted(0.5, 0.5, -0.5, -0.5)`) so a 1px stroke stays crisp.
- **`dashed`/`dotted` borders read as dots at 1px** - the dash period scales with border width. Use >= 2px
  for a visible dash.

## QSS images

- **`image:` / `url()` load only from a file path or the Qt resource system (`:/...`).** No `data:` URIs, no
  `QByteArray`/`QPixmap` injection. Resources are in-memory but must be in `.rcc` format
  (`QResource::registerResource`), not a raw PNG blob.
- **Rendering an SVG via QSS needs the `qsvg` image plugin** - add `QT += svg` (otherwise `url(:/x.svg)`
  silently loads nothing and you get no image). PNG needs no plugin.
- **Truly in-memory image data => bypass QSS and paint via `QStyle`/`QProxyStyle`** (takes a `QPixmap`
  directly). But see the `QProxyStyle` caveat below.

## QComboBox specifics

- **Styling `QComboBox::drop-down` suppresses the native arrow.** Once that subcontrol is styled, Qt draws no
  arrow there unless you also give `::down-arrow { image: url(...) }`.
- **`QComboBox::down-arrow` scales the image** to its `width`/`height`. Drawing the source ~2x and scaling
  down keeps it crisp on high-DPI without `@2x` asset variants.
- **Hover subcontrol rules do NOT inherit geometry from the base rule.** `QComboBox:hover::down-arrow { image:
  ... }` with no `width`/`height`/position renders the image at its natural size at a default (centred)
  position - a second, wrongly-placed arrow on top of the idle one. Repeat the geometry, or drop the hover
  variant.
- **`QProxyStyle::drawPrimitive(PE_IndicatorArrowDown)` is never reached when the subcontrol is styled in
  QSS** - `QStyleSheetStyle` owns the subcontrol and doesn't delegate. A proxy style can't customise the
  arrow while a stylesheet touches the combo.
- **Styling the drop-down view collapses its row height - pin `QComboBox QAbstractItemView::item` padding.**
  Once the view is styled (the `QComboBox QAbstractItemView { ... }` rule plus `ComboPopupRounder`'s
  `setStyleSheet` on the view), `QStyleSheetStyle` takes over item sizing and drops the native per-item margins,
  so rows collapse to about the text height. That bare default is also not stable across Qt versions - 6.9 and
  6.10 render it differently. Set an explicit `::item { padding: ... }` (`Style.cpp` uses `5px 8px`, echoing the
  `QMenu::item` vertical rhythm) so row height is both roomy and identical on every Qt version. Applies to any
  styled `QAbstractItemView`, not just combos.

## The drop-down popup is a separate top-level window

The popup is a private top-level container (`QComboBoxPrivateContainer`, a `QFrame`) hosting the list view.
Rounding it is genuinely awkward:

- **`border-radius` on the view (`QComboBox QAbstractItemView`) does not round the popup** - the square,
  opaque container window shows behind it.
- **Type selectors don't reliably match that private container.** `QComboBoxPrivateContainer { ... }` and even
  `QFrame { ... }` had no effect when set on it; only an **unscoped** stylesheet (`background: ...; border:
  ...;` with no selector) applied.
- **An unscoped stylesheet on a parent cascades to its children** (it wiped the inner view's background). A
  child's own stylesheet outranks the parent cascade, so re-assert child styling on the child directly.
- **A `WA_TranslucentBackground` top-level won't paint a QSS background** - it stays invisible. (A child
  widget paints its QSS background fine; the translucent top-level doesn't.)
- **`QAbstractItemView` (any `QAbstractScrollArea`) won't visually round via `border-radius`** - its opaque
  **viewport** child paints a square over the rounded frame. QSS `background` on a scroll area routes to the
  viewport, so the corners stay square (the "flat rectangle").

**What actually works (the `ComboPopupRounder` recipe):** an app-wide event filter that, on the container's
`Show`, sets `WA_TranslucentBackground` and makes the view + `viewport()` transparent; and on the container's
`Paint`, draws the surface itself - an anti-aliased rounded rect (`palette().base()` fill + hairline border)
with `QPainter` - returning `true` to suppress the container's own paint. The items paint on top afterward.
Painting from an event filter on `QEvent::Paint` works and produces no warnings. Gate the global filter
cheaply (bail on non-`Show`/`Paint`, require `isWindow()` before the `inherits()` check) since it sees every
event in the app.

## `:hover` is driven by the `WA_UnderMouse` attribute, not by Enter/Leave events

QSS `:hover` matches on `QStyle::State_MouseOver`, which `QStyleOption::initFrom()` derives from
`QWidget::underMouse()` - i.e. the `Qt::WA_UnderMouse` **attribute**. Crucially, that attribute is *not* toggled
by the `QEvent::Enter`/`QEvent::Leave` events. Qt's internal enter/leave dispatch
(`QApplicationPrivate::dispatchEnterLeave`) sets the attribute and *then* sends the event as a bare notification -
two separate steps. Two consequences:

- **Anything that bypasses that dispatch leaves the attribute stale, so the highlight sticks.** A popup menu's
  `exec()` runs a mouse grab: the widget that spawned the menu never gets a leave dispatched when the cursor
  slides onto the menu, so it keeps `WA_UnderMouse` set and stays highlighted after the menu closes - it reads as
  a stuck "selection". The same failure mode hits a widget deleted and recreated under a stationary cursor (e.g. a
  grid rebuild on Ctrl+wheel zoom): the fresh widget never receives an enter.
- **Fix it by clearing the attribute, not by faking an event.** Sending a synthetic `QEvent::Leave` does nothing
  to the highlight - the (usually absent) `leaveEvent` handler runs, but `WA_UnderMouse` is untouched, so the next
  repaint still sees `underMouse() == true`. Clear it directly and repaint:
  `w->setAttribute(Qt::WA_UnderMouse, false); w->update();`

The shared `clearStuckHoverIfCursorLeft()` in [`app/src/Utils.h`](../../app/src/Utils.h) does exactly that after a
context menu's `exec()` - guarded so a right-click then Esc without moving the mouse keeps a legitimate hover - and
is used by both the frame-viewer thumbnails (`#framedThumbnail`) and the grid cards (`#mediaItemCard`). Both set
`WA_Hover`, which is what makes a plain styled `QWidget` repaint on real hover changes in the first place; the
stuck-state fix is separate from that. When the menu action can delete the spawning widget mid-`exec()` (grid
cards get rebuilt by their own menu actions), guard the widget with a `QPointer` before the post-`exec()` re-sync.

- **`QSplitter::handle:hover` never matches out of the box** - `QSplitterHandle` doesn't set `WA_Hover` on
  itself (long-standing QTBUG-13768), so the handle neither repaints on enter/leave nor reports
  `State_MouseOver`, and the QSS hover rule is dead. The handles are created internally by `QSplitter` with
  no per-instance hook, so `Style.cpp`'s `SplitterHandleHoverEnabler` (an app-wide event filter, same pattern
  as `ComboPopupRounder`) sets the attribute on each handle as it gets polished.

## General QSS gotchas

- **`QString::arg` replaces every occurrence of the lowest-numbered placeholder in one call** (e.g. `%1` used
  10 times is fully filled by one `.arg(...)`). `Style.cpp` has since moved from numbered placeholders to
  named `%Token%` ones resolved via `QString::replace`, but the per-instance sheets elsewhere still use
  `.arg()` and rely on this.
- **`palette(...)` roles track the active theme automatically; hardcoded hex does not.** Anything built from
  a `Theme` hex must be rebuilt on a light/dark switch (we re-apply the whole sheet on
  `colorSchemeChanged`); `palette(base)` etc. update on their own.
- **`selection-background-color` and `selection-color` are a set - specify both or neither.** Once a QSS rule
  gives a text widget (`QLineEdit`/`QPlainTextEdit`/`QTextEdit`) a `selection-background-color`,
  `QStyleSheetStyle` owns that widget's selection painting and does *not* fall back to the palette's
  `HighlightedText` for the unset `selection-color` - the selected text keeps its **normal** foreground. On a
  dark theme that's light text on the accent fill = unreadable. The app-wide palette `Highlight`/`HighlightedText`
  still covers widgets the sheet doesn't reach (combo popup, item views), but a QSS rule that sets one selection
  color must carry its matching half itself. (`Style.cpp`'s `kTextInputs` sets both, from the `SelectionHighlight`
  / `SelectedText` theme pair.)
