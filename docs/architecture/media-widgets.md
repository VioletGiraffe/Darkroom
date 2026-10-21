# Media cards and thumbnails

[← Back to architecture index](../DARKROOM.md)

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
drops. The grid also mirrors each row's selection onto its card as a `selected` dynamic property, which the central
stylesheet styles; cards hold no selection logic. Only the card is repolished on a change, so those rules style the
card itself, never its descendants.

Photo and video cards share a configured image height but have different fixed widths. Photos are square; a video's
strip spans the selected number of photo columns including grid gaps. This makes mixed media align on one column grid.
Each row therefore carries an explicit media-type-specific size hint; the grid cannot use uniform item sizes.

## ThumbnailWidget

`ThumbnailWidget` presents an image in a matte beneath media cards, standalone frame thumbnails and Quickroom's tiles,
with optional frame, caption and film-strip treatment. A source produces the image, so the widget is independent of
where the pixels come from.

### Image sources

- The widget holds its source by value in `ThumbnailWidget::Source`, a variant of value types sharing the members listed
  in `ImageSource.h`.
- `renderNow` answers on the GUI thread when the image is cheap to produce there. Otherwise the widget waits out the
  load dwell and calls `renderAsync`, so cheap sources skip the dwell without the widget telling sources apart.
- Asynchronous results arrive on the GUI thread through a `Delivery`. The returned `Request` cancels on destruction or
  replacement, and pending stages skip their work once cancelled.
- A source states whether its image gets the matte's rounded corners.
- A result carries the file's type: detected from the data for decoded images, given by the caller otherwise. An
  optional caller-supplied lookup maps it to an icon at the right end of the caption band, drawn at one of the icon's
  native sizes on whole device pixels: scaling would blur hand-tuned pixels.
- `ImageFilesSource` tiles image files side by side. `IconSource` (Quickroom's folders and videos) draws a file icon,
  synchronously and without rounded corners.

### Sizing and rendering

Target image-area dimensions are maximum bounds. A loaded image that fits is painted one-to-one and centered; a live
resize may temporarily scale it. This makes incorrect layout calculations visible as clipping instead of silently
hiding them behind permanent resampling.

The image area is a configured canvas (cards) or follows the widget's size. A size or pixel-ratio change re-renders,
keeping the old image until the new one arrives.

A source with nothing to show is a valid final state: `ImageFilesSource` without paths answers `renderNow` with an empty
result and the widget paints a no-preview placeholder, so broken or preview-less items still have reachable cards.

Dynamic size hints return the loaded image's tight bounds. Browser and staging grids use fixed hints because row
geometry must be known before asynchronous loading and because photo/video widths differ. Layout-sized widgets use
fixed hints too: a hint following the image would resize the widget, which re-renders the image. Framing and the film
strip do not involve the source; the film strip only shrinks the requested box by its bands.

Thumbnail pixels are explicitly clipped to the rounded matte; stylesheet border radius alone cannot clip child
painting.

### Loading

Nothing renders at construction. Rendering is decided at paint time, so only shown widgets load. For asynchronous
sources, first paint starts a short dwell and loading proceeds only if the widget remains visible, preventing
off-screen materialization and fast scrolling from reading images that never need presentation.

For video cards, preview-directory enumeration and fallback to the full frame folder occur before asynchronous image
loading and currently run on the GUI thread. Lazy card materialization bounds this work to nearby rows, but slow or
unavailable storage can still delay card creation; investigations must not assume the complete thumbnail path is
background-only.

`ImageFilesSource` loads in two stages:

1. File reads go through `Core::IoThreadPool`, which routes fast storage to bounded parallel workers and slow,
   external, network, or unknown storage to a shared serial worker. On fast storage a card's files are read in parallel.
2. CPU decoding runs on `cpuThreadPool()`, a card's frames in parallel.

`QImageReader` decodes at full resolution; `ImageProcessing::resize` then scales to the target size, its slices also on
`cpuThreadPool()`. Orientation metadata is applied after the resize, at thumbnail size.

With `PROFILING_ENABLED` defined, each batch of overlapping loads logs its wall time and the read and decode totals:
the time to fill a screenful of thumbnails.

Cards are initially parentless, so their real screen and device-pixel ratio may not yet be known. Deciding at paint
time renders for the ratio of the screen the widget is shown on.

### Dragging standalone frames

A source-file URL drag is opt-in through `setDragFilePath`: frame viewers and the compare window offer one, grid cards
do not. `ThumbnailWidget` delegates gesture recognition to `DragGestureHelper`, keeping frame viewers and cards free of
duplicated pointer-threshold mechanics.

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
[backlog](../darkroom-backlog.md).
