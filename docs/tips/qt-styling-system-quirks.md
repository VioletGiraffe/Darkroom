# Qt styling-system quirks

Known QSS and `QStyle` traps with established remedies. The central implementations live in
[`Theme/Style.cpp`](../../app/src/Theme/Style.cpp).

## QSS shape and image limits

- The browser CSS border-triangle trick does not work: Qt paints border edges as rectangular bands. Supply an image
  or paint the glyph.
- Paint anti-aliased shapes with `QPainter` when QSS cannot express them. Inset a 1 px outline by half a pixel for a
  crisp stroke.
- One-pixel dashed/dotted borders read as dots because the dash period follows border width; use at least 2 px when
  the dash must remain visible.
- QSS `image:` and `url()` accept files or Qt resources, not data URIs, `QByteArray`, or `QPixmap`. Runtime
  resources must be registered `.rcc` data rather than a raw image blob.
- SVG QSS images require the qsvg plugin through `QT += svg`; missing support fails silently.
- Truly in-memory pixmaps require painting or a style API. A proxy style cannot override a subcontrol already owned
  by QSS.

## QComboBox

- Styling `QComboBox::drop-down` suppresses the native arrow. Also provide a `::down-arrow` image.
- The arrow image scales to the subcontrol dimensions. A larger source scaled down remains crisper on high-DPI
  screens.
- Hover subcontrol rules do not inherit base geometry. Repeat width, height, and position or omit the hover variant;
  otherwise Qt may paint a second natural-size arrow in the default position.
- `QProxyStyle::drawPrimitive(PE_IndicatorArrowDown)` is not reached once `QStyleSheetStyle` owns the styled
  subcontrol.
- Styling the popup item view removes native row margins and changes behavior between Qt versions. Set explicit
  `QComboBox QAbstractItemView::item` padding.
- A QSS-styled item view does not take selected-row colors from `QPalette::Highlight` and
  `HighlightedText`. Style `::item:selected` explicitly.
- A fully styled combo receives no reliable style-drawn focus ring. Add an explicit `QComboBox:focus` rule.

## Combo popup window

The popup is a private top-level `QFrame` containing a list view. Its window, view, and viewport are separate paint
layers:

- Rounding the view does not hide the square popup window behind it.
- Type selectors are unreliable on the private container; an unscoped stylesheet applies, but cascades to children.
- A child stylesheet outranks that cascade and can restore view-specific styling.
- A translucent top-level does not paint its own QSS background.
- `QAbstractScrollArea` radius does not clip its opaque viewport, and its QSS background is routed to that viewport.

`ComboPopupRounder` is the working recipe. An application event filter recognizes the private popup:

1. On Show, make the container translucent and the view plus viewport transparent.
2. On Paint, draw the rounded surface and border with `QPainter`, then consume the container paint.
3. Let item painting proceed normally above that surface.

Gate the global filter before private-type inspection: reject unrelated event types and non-window objects first.

## Hover state

QSS `:hover` follows `QStyle::State_MouseOver`, derived from `QWidget::underMouse()` and
`Qt::WA_UnderMouse`. Enter/Leave events merely notify widgets; Qt's internal dispatch updates the attribute
separately.

Mouse grabs and widget replacement can leave the attribute stale. A context menu may prevent the spawning widget from
receiving a real leave, while a new widget created beneath a stationary cursor may never receive enter.

Sending a synthetic Leave does not repair the state. Clear the attribute and repaint:

```cpp
widget->setAttribute(Qt::WA_UnderMouse, false);
widget->update();
```

[`clearStuckHoverIfCursorLeft()`](../../app/src/Utils.h) performs this resynchronization after context menus while
preserving a legitimate hover when the cursor did not leave. If a menu action may delete the spawning widget, retain
it through `QPointer` before the post-`exec()` check. Styled plain widgets also need `WA_Hover` so real hover
changes trigger repaint; this is independent of stale-state repair.

`QSplitterHandle` does not enable `WA_Hover`, so `QSplitter::handle:hover` is otherwise inert.
`SplitterHandleHoverEnabler` sets the attribute when internally created handles are polished.

## Palette and selection

- `palette(role)` follows a palette change automatically. QSS values generated from Theme colors require rebuilding
  the stylesheet when the scheme changes.
- Set `selection-background-color` and `selection-color` together. Once QSS owns selection painting, omitting the
  foreground does not reliably fall back to `QPalette::HighlightedText`; it may retain the normal text color and
  become unreadable.
