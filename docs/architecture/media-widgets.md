# Media item card & thumbnail widgets

[← Back to architecture index](../../ARCHITECTURE.md)

## `MediaItemWidget` (`src/UiComponents/MediaItemWidget.h/.cpp`)

Card used in the MainWindow grid. Takes a **`MediaId`** (see [data-model.md](data-model.md)) rather than a
folder path — label ops are `MediaId`-anchored, and the one remaining drag interaction is a drop target, not
a source (see below). `MediaBrowserWidget` retains paths for thumbnails/playback; the card does not.

### Card layout & frame

The card frame (rounded border + hover highlight, styled via `#mediaItemCard` in the central `Style.cpp`
sheet rather than per-instance stylesheets — those repaint too slowly at grid scale) wraps a borderless
thumbnail above a footer row. The card keeps a **transparent** normal background so the grid's
`item:selected` highlight shows through. The thumbnail is constructed with `framed=false` (see
`ThumbnailWidget`) so one border surrounds thumbnail + footer together.

### Label drop target

The card is a drop target for `LabelMimeType` (`src/UiComponents/LabelMimeType.h`) MIME carried by
`LabelSidebar` row drags. A drop invokes a `setOnLabelDropped` handler in **`MediaBrowserWidget`**, which adds the
label across `effectiveSelection` (drop on a selected card → whole selection; else just that card),
mirroring the context-menu path. The handler **defers** the catalog mutation + view rebuild via a queued
invoke — the rebuild deletes the card whose `dropEvent` is still on the stack.

### Label dots

`LabelDotStrip` in the footer paints one dot per label the item carries; **`Best` is a dot like any other
label**, with the star toggle existing in parallel.

> **Pattern: "browser computes, card draws."** The card is display-only — **`MediaBrowserWidget`** computes
> the colors from the `Catalog` (`labelById(id)->color`, unset → grey) and the tooltip, then hands the
> card a plain list of colors and a tooltip string. The card never queries `Catalog` itself. Follow this
> split for any future per-card decoration: feature logic in the browser; pure rendering in the card.

v1 shows dots; named chips with drag-out removal are deferred polish.

### Frames-extracted badge

A green contact-sheet grid icon marks a video whose full frame set has been extracted. It is a *positive*
marker of the ready state — a "frames-done" glyph reads immediately, where an abstract "not-done" one
wouldn't. **`MediaBrowserWidget` gates it to videos** —
a photo never shows it even though `isSplitIntoFrames()` returns `true` (photos have no frames; see
[import.md](import.md)). Its position depends on the thumbnail width, so it is re-placed on every thumbnail
resize — the self-correcting placement that handles the real widget size not being known at construction.

Card sizes are type-specific but share one **height** (the user's per-frame height): a **photo** card is
**square**; a **video** card is a horizontal strip. The video width is chosen so a video card spans exactly
`frameCount` photo-card columns *including the grid gaps*, keeping mixed photo/video cards on a single
column grid. Per-frame height and frame count are both user-adjustable at runtime (see
[Card preview sizing & zoom](#card-preview-sizing--zoom-ctrlwheel) below).

`PreviewFrameCountCombo` supplies the identical 1-10-frame selector used by the main browser and Import staging;
each owner persists and applies its own selected count.

### Duration badge

A small play-triangle overlay marks a card as video at a glance. **`MediaBrowserWidget`** supplies the duration from
the `Catalog`, matching the "browser computes, card draws" split; the card never queries the catalog
itself.

### Film-strip treatment (video cards)

A video card renders with sprocket-perforated bands top and bottom — distinct from a photo's plain square
at a glance. The `filmStrip` flag passed into `ThumbnailWidget` reserves the bands. The frames keep their
transparent inter-frame gaps; the dark film base behind them makes those gaps read as **black divider lines
for free**. `ThumbnailWidget::filmStripBandHeight` is public so the card can offset the corner badges
inward past the perforations.

### Dragging a card out (file export)

The card originates no drag itself. Drag is owned by the **grid view** (`MediaGrid`, see
[main-window.md](main-window.md)): it exports selected cards' source files as `file://` URLs
(`CopyAction`) to Explorer or another app. The **label drop target** above is the card's only *incoming*
drag.

---

## `ThumbnailWidget` — sizing, rendering, drag (`src/UiComponents/ThumbnailWidget.h/.cpp`)

The shared image-rendering widget under cards/frames. Owns drag-start directly.

### Sizing model

Both constructors take a target image-area size as a **max bound, not exact** — the loaded image can be
smaller.

`paintEvent` blits the loaded image **1:1 centered** when it fits, falling back to a fast rescale only
transiently (mid-resize). Deliberate: a calc bug shows as clipping, not a silent rescale.

The image is clipped to an antialiased rounded path at `ThumbnailMatteRadius` — the well/frame is rounded
but QSS `border-radius` can't clip child painting, so a flush image's square corners would otherwise
overpaint the well's rounded corners.

With nothing to render — an **empty** composite path list — the widget still claims its full canvas and
paints a "No preview" text placeholder, arming no load at all. That is a legitimate final state, not a
transient one: a video whose `preview/` and frame folder are both empty still gets a card (see
[import.md](import.md)), as does a staged import card whose preview extraction failed.

`sizeHint()` has two modes via `setDynamicSizeHint(bool)` (default **on**): dynamic returns the tight
bounding box post-load; **fixed** always returns the max-bound. **The main browser grid uses fixed by design**
— every card of a given media type is one fixed size, so the grid computes the hint once per type and
reuses it for every item of that type. Because the two types differ in width, `setUniformItemSizes(true)`
**can't** be used; each item carries an explicit fixed hint instead. Tight per-card sizing is deliberately
**not** done: the image loads async, so the tight size isn't known when the item's hint is set.

**Framing**: the composite constructor takes a `framed` flag (default true). `framed=false` — used by the
grid card — drops the border, hover, and padding, because `MediaItemWidget` draws the card frame around
thumbnail + footer instead. The grid also constructs it caption-less; `ThumbnailWidget` keeps its caption
mechanism for its other consumers (Compare, frame viewers).

The composite constructor also takes a `filmStrip` flag (default false, set only by the grid's video
cards). The rest of the render/paint path is shared.

### Loading

Newly constructed composite thumbnails follow one loading rule at startup and during later scrolling: their
first paint arms a short dwell, and rendering starts only if they remain visible when it expires. Construction
alone therefore performs no image I/O; off-screen cards and cards merely passed during a fast scroll read
nothing.

For a browser video card, choosing the preview inputs is an earlier, separate step: card materialization
enumerates `preview/`, then falls back to the full frame folder. That directory discovery is currently
**synchronous on the GUI thread**; only the subsequent image reads and decoding are asynchronous. Deferring
cards bounds this cost to rows near the viewport, but a slow, unavailable, or network-backed library can
still delay card appearance. Do not treat the complete thumbnail path as background work when investigating
UI latency.

A load runs in two stages: the **file read** through `Core/IoThreadPool`, which routes by storage medium —
a fast random-access volume gets a small parallel pool; a slow, external, network, or unclassifiable one
shares a single worker so a spinning disk isn't seek-thrashed by parallel reads — then the **decode** on
the shared CPU pool so it fans across cores without blocking the next read.

The decode reads straight to the target size (`QImageReader::setScaledSize` — a reduced-scale libjpeg
decode for JPEG), far cheaper than a full decode plus downscale and free of the aliasing that a single
large downscale caused; it also applies EXIF orientation.

Cards are built parentless and reparented later, so the device-pixel-ratio isn't reliable until the widget
is on its real screen. Render scheduling captures the current DPR, and painting rechecks it after realization
and reschedules when necessary.

### Drag

The **single-frame constructor** defaults to dragging a `file://` URL (`CopyAction`), used by
`FrameViewerWindow`/`CompareWindow`'s individual thumbnails via `DragGestureHelper`. The **composite
constructor** sets no drag payload — composite thumbnails are never drag sources. `MediaItemWidget` and
`FrameViewerWindow` contain zero drag-mechanics code of their own; it all lives here.

## `DragGestureHelper` (`src/UiComponents/DragGestureHelper.h/.cpp`)

Reusable helper that starts a `QDrag` once the pointer moves past the drag threshold, given a MIME-data
factory (returning null vetoes the drag). `ThumbnailWidget` uses it directly for single-frame file drags.

The same file also provides **`ListRowDragFilter`**, a self-installing event filter that packages the
complete drag-out gesture for a `QListWidget`'s rows, parameterized by one
`QListWidgetItem → QMimeData*` factory. `LabelSidebar` and `ImportDialog`'s label panel both use it for
their label-assign drags — neither hand-rolls the gesture (see [main-window.md](main-window.md)).

## Card preview sizing & zoom (Ctrl+wheel)

`ThumbnailWidget` consumes Ctrl+wheel as a zoom request; plain wheel input continues to the surrounding
view. All three owners bound and persist the new size, then debounce the visual rebuild. The main browser keeps
its rows and recreates only cards because zoom changes presentation, not item membership; the Import dialog does
the same for its staged cards while preserving their pending state, and the frame viewer rebuilds its thumbnail view.

The three owners' bound/persist/debounce logic is intentionally **not** de-duplicated yet (each is a few
lines) — revisit at a 4th consumer. See [backlog](../../ARCHITECTURE.md#improvement-backlog).
