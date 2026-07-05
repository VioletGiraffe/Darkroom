# Media item card & thumbnail widgets

[← Back to architecture index](../../ARCHITECTURE.md)

## `MediaItemWidget` (`src/UiComponents/MediaItemWidget.h/.cpp`)

Card used in the MainWindow grid. Constructor takes the item's **`MediaId`** (see
[data-model.md](data-model.md)), plus preview paths/label/cell size/Best state/callbacks — no folder path;
the card has no use for one (label ops are `MediaId`-anchored, and the one remaining drag interaction is a
drop target, not a source — see below). Provides: star toggle (`setOnToggleBest`), double-click to play
(`setOnDoubleClick`), middle-click to show frames (`setOnMiddleButtonClick` →
`FrameViewerWindow::showForFolder`), right-click menu (`setOnContextMenu`), and a thumbnail preview strip.

`mediaId()` exposes the card's stable identity — carried so label ops address the item directly rather
than via its path (the *caller*, `MainWindow`, still keeps the path around for thumbnails/playback/etc.;
the card itself does not).

### Card layout & frame

The card is a vertical stack — a **borderless thumbnail** on top, a **single-row footer** below — replacing
the old `[star | thumbnail]` horizontal layout. The footer fills the formerly-empty caption space and holds,
left→right: the **Best-star toggle**, the **label dots**, then the **item name** (right-aligned, elided).
The card itself (object name `mediaItemCard`) now owns the visual frame — a rounded border + hover highlight
(`Theme::Accent`/`AccentBg`), styled by `#mediaItemCard` in the central `Style.cpp` sheet (not a per-instance
stylesheet — those polished too slowly at grid scale) — and keeps a **transparent** normal background so the
grid's `item:selected` highlight shows through behind it. The thumbnail is constructed
**borderless** (matte well only; see `ThumbnailWidget`'s `framed` flag) so one frame wraps thumbnail +
footer, per the design mockup. `sizeHint()` = thumbnail size + footer height + spacing, inside the card's
border+padding margins.

The name lives in an `ElidedLabel` (a `QLabel` that elides to its width, `ElideRight`) in the footer — **not**
inside `ThumbnailWidget` anymore; `setLabel()` updates that label. The grid constructs `ThumbnailWidget`
caption-less; `ThumbnailWidget` keeps its caption mechanism for its other consumers (Compare, frame viewers).

### Label drop target

The card calls `setAcceptDrops(true)` and handles `dragEnter`/`dragMove`/`drop` for the `LabelMimeType`
(`src/UiComponents/LabelMimeType.h`) MIME a `LabelSidebar` row drag carries (see [main-window.md](main-window.md)). A
drop invokes `setOnLabelDropped(handler)` with the dropped label's id; **MainWindow** is the handler and
adds that label across `effectiveSelection(folderPath)` (drop on a card in the selection → whole selection;
else just that card), mirroring the context-menu "Labels" add path. The child `m_thumb` doesn't accept
drops, so a drop anywhere on the card bubbles up to the card. The handler **defers** the catalog mutation +
`refreshLibraryView` via a queued invoke, because that rebuild deletes the very card whose `dropEvent` is on
the stack.

### Label dots

`setLabelDots(colors, tooltip)` fills the footer's `LabelDotStrip` — a mouse-transparent widget (clicks/drags
fall through) that paints small colored dots, **one per label the item carries, including `Best`** (Best is a
dot like any other label; the star toggle exists in parallel). The strip sits in the footer's layout — no
longer an overlay in the thumbnail's corner — so it paints the dots **flat** on the card surface.

> **Pattern: "MainWindow computes, card draws."** The card is display-only here — **MainWindow** computes
> the colors from the `Catalog` (`labelById(id)->color`, unset → grey) and the tooltip (the label names),
> then hands the card a plain list of colors to paint. The card never queries `Catalog` itself. Follow this
> split for any future per-card decoration: business logic (what color, what text) in `MainWindow`; pure
> rendering in the widget.

v1 shows dots; named chips with drag-out removal are deferred polish — see
[catalog-and-labels.md](catalog-and-labels.md#deferred-polish-post-v1).

### Split-pending badge

`setSplitPending(bool)` shows/hides a small badge (an hourglass glyph on a translucent backdrop) in the
thumbnail's **top-right** corner (uncontested since the dots moved to the footer). `MainWindow::refreshMediaGrid`
sets it from whether the video is split yet — see [import.md](import.md) for the on-demand-split design
this reflects. Its position depends on the thumbnail width, so it's re-placed on every thumbnail resize, not
just when set — that's what makes the initial placement self-correcting when the card's real width isn't known
yet at construction (before the grid lays it out).

Card sizes are type-specific but share one **height** (the user's per-frame height): a **photo** card is
**square** (that height on both sides); a **video** card is a horizontal strip showing `frameCount` frames.
The video width isn't just height × count — it's chosen (`MediaItemWidget::videoCanvasWidthForTiling`) so a
video card spans exactly `frameCount` photo-card columns *including the grid gaps between them*, which makes
mixed photo/video cards align on a single column grid. Per-frame height and frame count are both
user-adjustable at runtime (see "Card preview sizing & zoom" below).

### Dragging a card out (file export)

The card *widget* originates no drag itself — `MediaItemWidget`/`ThumbnailWidget` leave a left-drag to fall
through to the view. Dragging a card is owned by the **grid view** (`MediaGrid`, see
[main-window.md](main-window.md)): it exports the selected cards' **source files** as `file://` URLs
(`CopyAction`) to Explorer or another app, drawing the grabbed card as the drag image (plus a count badge for
a multi-file drag). The **label drop target** above is the card's only *incoming* drag. (An earlier
card-owned "move video between collections" drag was retired when that became "drag a label onto a card"
instead.)

---

## `ThumbnailWidget` — sizing, rendering, drag (`src/UiComponents/ThumbnailWidget.h/.cpp`)

The shared image-rendering widget under cards/frames. Owns drag-start directly.

### Sizing model

Both constructors take a target image-area size (single-frame: `int` → square; composite: `QSize`) as a
**max bound, not exact**. The loader (see *Loading* below) best-fits each source frame (`KeepAspectRatio`) and
composes a canvas sized to the *actual tight content* (Σ fitted widths + gaps × tallest fitted height) — so the
image can be smaller than the bound. Gaps left transparent.

`paintEvent` blits the loaded image **1:1 centered** when it fits the content area, falling back to a fast
rescale only transiently (mid-resize). Deliberate: a calc bug shows as clipping, not a silent rescale.

`sizeHint()` has two modes via `setDynamicSizeHint(bool)` (default **on**): dynamic returns the tight
bounding box post-load (layout consumers re-query on `updateGeometry()` and reflow); **fixed** always
returns the max-bound. **MainWindow's grid uses fixed by design** — every card of a given media type is one
fixed size, so the grid computes the card size hint **once per type** (a video's strip and a photo's square
differ in width) and reuses it for every item of that type, activating at most two card layouts per rebuild
instead of one per card. Because the two types differ in width, `setUniformItemSizes(true)` (which forces a
single size on all items) **can't** be used; instead each item carries an explicit fixed hint, so per-item
layout stays a cached-value lookup rather than a widget layout activation — essentially the same populate cost.
Tight per-card sizing is deliberately **not** done: the image loads async, so the tight size isn't known when
the item's hint is set, and re-querying per card would reintroduce the per-card relayout the grid build avoids.

**Framing**: the composite constructor takes a `framed` flag (default true). `framed=false` — used by the
grid card — drops the border, hover and padding, leaving just the recessed matte well, because
`MediaItemWidget` draws the card frame around thumbnail + footer instead (see "Card layout & frame" above).
The grid also constructs it caption-less (the name lives in the footer), so its caption strip collapses to
zero. Framed, captioned thumbnails (single-frame viewers, Compare) are unchanged.

### Loading

The render is **not** started at construction. The first `paintEvent` arms a short dwell timer and renders only
if the card is still visible when it fires — so a grid (which paints only its visible cards) never loads
off-screen ones, and a fast scroll loads nothing it doesn't come to rest on.

A load runs in two stages: the **file read** on one process-wide I/O thread (`Core/IoThreadPool`, a hidden
single-thread pool) so a spinning disk isn't seek-thrashed by parallel reads, then the **decode** on the shared
CPU pool so it fans across cores without blocking the next read. The decode reads straight to the target size
(`QImageReader::setScaledSize` — a reduced-scale libjpeg decode for JPEG), far cheaper than a full decode plus
downscale and free of the aliasing that a single large downscale caused; it also applies EXIF orientation.

Cards are built parentless and reparented later, so the device-pixel-ratio isn't reliable until the widget is on
its real screen — another reason the render lives in `paintEvent`, which captures the correct DPR and re-renders
if an earlier one was stale.

### Drag

Drag is intrinsic to `ThumbnailWidget` (overrides `mousePressEvent`/`mouseMoveEvent`, holds a
`DragGestureHelper`, see below): the **single-frame constructor** defaults to dragging a `file://` URL with
`Qt::CopyAction` (used by `FrameViewerWindow`/`CompareWindow`'s individual frame thumbnails); the
**composite constructor** (used by `MediaItemWidget`'s grid cards) sets no drag payload at all and has no
way to acquire one — composite thumbnails are never drag sources. So `MediaItemWidget` and
`FrameViewerWindow` contain zero drag-mechanics code of their own either way; it all lives here.

## `DragGestureHelper` (`src/UiComponents/DragGestureHelper.h/.cpp`)

Tiny reusable helper: records the press, then starts a `QDrag` once the pointer moves past the drag threshold,
given a MIME-data factory and a drop action. The drag image is a supplied pixmap or, by default, a size-capped
grab of the widget. Callers: `ThumbnailWidget` (single-frame thumbnails' default file drag, grabs itself) and
`LabelSidebar` (label-assign drag, passes a grab of just the dragged row — see [main-window.md](main-window.md)).

## Card preview sizing & zoom (Ctrl+wheel)

Mechanism/policy split: `ThumbnailWidget` detects Ctrl+wheel and fires a zoom callback (consuming the event),
while plain wheel falls through so the view scrolls; `MediaItemWidget` forwards it. **Owners apply the same
shape**: bound the new size, persist it to `QSettings`, and start a single-shot debounce timer that rebuilds
once the wheel settles — `MainWindow::zoomCards` (card height) → `refreshMediaGrid()`, and
`FrameViewerWindow::zoomThumbnails` (frame-viewer thumbnail size). Preview frame count (a separate header
combobox) is persisted the same way.

The two owners' bound/persist/debounce logic is intentionally **not** de-duplicated yet (each is a few lines)
— revisit at a 4th consumer. See [backlog](../../ARCHITECTURE.md#improvement-backlog).
