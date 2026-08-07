# Media cards and thumbnails

[← Back to architecture index](../../ARCHITECTURE.md)

## MediaItemWidget

`MediaItemWidget` is the rich card attached to a materialized browser or staging row. It carries `MediaId`, not a
storage path. `MediaBrowserWidget` retains paths and Catalog knowledge and supplies plain display state to the card:
preview inputs, label colors and tooltip, duration, Best state, and full-frame readiness. The governing split is
**browser computes, card draws**.

The card is a label-drop target. It receives a stable label-id payload and invokes a browser-owned handler that applies
the label to the effective card selection. Catalog mutation is queued until the drop event unwinds because the
resulting structural refresh may delete the receiving card.

Cards never originate source-file drags; the owning `MediaGrid` exports the current view selection. Keeping outgoing
drag at the view preserves native multi-selection behavior while the card remains responsible only for incoming label
drops.

Photo and video cards share a configured image height but have different fixed widths. Photos are square; a video's
strip spans the selected number of photo columns including grid gaps. This makes mixed media align on one column grid.
Each row therefore carries an explicit media-type-specific size hint; the grid cannot use uniform item sizes.

## ThumbnailWidget

`ThumbnailWidget` is the shared image renderer beneath media cards and standalone frame thumbnails.

### Sizing and rendering

Target image-area dimensions are maximum bounds. A loaded image that fits is painted one-to-one and centered; a live
resize may temporarily scale it. This makes incorrect layout calculations visible as clipping instead of silently
hiding them behind permanent resampling.

An empty composite input is a valid final state. The widget reserves its configured canvas and paints a no-preview
placeholder without scheduling any load, so broken or preview-less items still have reachable cards.

Dynamic size hints return the loaded image's tight bounds. Browser and staging grids use fixed hints because row
geometry must be known before asynchronous loading and because photo/video widths differ. Optional framing and
film-strip treatment change presentation without changing the shared load path.

Thumbnail pixels are explicitly clipped to the rounded matte; stylesheet border radius alone cannot clip child
painting.

### Loading

Composite thumbnails schedule no I/O at construction. First paint starts a short dwell and loading proceeds only if
the widget remains visible, preventing off-screen materialization and fast scrolling from reading images that never
need presentation.

For video cards, preview-directory enumeration and fallback to the full frame folder occur before asynchronous image
loading and currently run on the GUI thread. Lazy card materialization bounds this work to nearby rows, but slow or
unavailable storage can still delay card creation; investigations must not assume the complete thumbnail path is
background-only.

Actual loading has two stages:

1. File reads go through `Core::IoThreadPool`, which routes fast storage to bounded parallel workers and slow,
   external, network, or unknown storage to a shared serial worker.
2. CPU decoding runs on the shared compute pool.

`QImageReader` decodes toward the target size and applies orientation metadata, avoiding a full-resolution decode
and the aliasing produced by a single large post-decode reduction.

Cards are initially parentless, so their real screen and device-pixel ratio may not yet be known. Scheduling captures
the current ratio; painting rechecks it after attachment and reschedules rendering when the context changed.

### Dragging standalone frames

The standalone-frame constructor supplies a source-file URL drag by default. The composite constructor supplies no
drag payload. `ThumbnailWidget` delegates gesture recognition to `DragGestureHelper`, keeping frame viewers and
cards free of duplicated pointer-threshold mechanics.

## Drag helpers

`DragGestureHelper` starts a drag after the platform movement threshold and asks a caller-supplied factory for MIME
data; a null result vetoes the drag.

`ListRowDragFilter` provides the equivalent complete gesture for `QListWidget` rows. LabelSidebar and
ImportDialog use it to produce label-id payloads rather than implementing separate drag state machines.

## Preview sizing

Ctrl+wheel over a thumbnail requests a presentation-size change while ordinary wheel input remains available to the
containing view. Each owner bounds and persists its setting, then debounces card reconstruction:

- The main browser preserves rows and recreates attached cards.
- ImportDialog rebuilds staged cards while preserving pending state.
- FrameViewerWindow rebuilds its thumbnail flow.

These three small owner-specific persistence/debounce paths remain intentionally separate; see the
[backlog](../../ARCHITECTURE.md#improvement-backlog).
