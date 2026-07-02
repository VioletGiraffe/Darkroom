# Frame viewer & video player

[← Back to architecture index](../../ARCHITECTURE.md)

## `FrameViewerWindow` (`src/Windows/FrameViewerWindow.h/.cpp`)

Separate top-level `QWidget` (`Qt::Window`), persistent — reused, not destroyed on close.
- `showForFolder(path)` — sets folder, refreshes thumbnails, only `show()/raise()/activateWindow()` when
  path is non-empty (guards against popping an empty window).
- `currentFolder()` — used by `MainWindow` to update it on rename (see [main-window.md](main-window.md)).
- Ctrl+scroll resizes thumbnails (persisted, see [media-widgets.md](media-widgets.md#card-preview-sizing--zoom-ctrlwheel));
  Esc closes (`QShortcut`).
- Context menu (open in explorer, copy path) is local; thumbnail drag is intrinsic to `ThumbnailWidget` (see
  [media-widgets.md](media-widgets.md)).
- Uses `CFlowLayout` (`qtutils`) for its thumbnail grid — the one remaining consumer of that layout, since
  it needs a plain non-selectable flow grid (unlike `MainWindow`'s grid, which needs the native multi-select
  that `CFlowLayout` doesn't provide — see [main-window.md](main-window.md#media-grid--multi-select)).

---

## `VideoPlayerWindow` (`src/Windows/VideoPlayerWindow.h/.cpp`) + `MarkerSlider`

Built-in player (`QMediaPlayer` + `QVideoWidget`), used for the double-click-to-play path. Keeps a static list
of open instances (for app-wide restart/close) and auto-tiles each window into screen thirds once the video
size is known.

### Seek behavior

Mouse drag and keyboard arrow seeks take different paths: arrows don't fire the slider's press/release
signals, so they're handled on value-change (gated so only a real keyboard seek, not slider tracking, acts).
Position updates from the player block the slider's signals while the user isn't dragging, to avoid a
seek/update feedback loop.

### A–B loop

A live loop is held as a start/end pair (ms, unset = -1); playback seeks back to the start once it passes the
end, active only when both are set and the range is forward (an inverted/partial range just shows its markers,
inert). It coexists with whole-file looping. Loop controls live on their **own row** to keep the seek row
uncluttered: **A / B / Clear** set/clear the endpoints at the current position (A/B checkable for "is set"
feedback); keyboard `[` / `]` mirror set-A/set-B.

### Saved loops

Multiple per video, one active at a time (combo + **Save**/**Delete**), persisted via `MetadataStore` (see
[data-model.md](data-model.md)) under the `"intervals"` field — a list of `{start, end, name}` objects keyed by
the played video's `MediaId`. Programmatic combo resets (Clear, Delete) block the selection signal so Delete
removes only the saved entry and leaves the live loop intact. Interval (de)serialization is owned here, per
`MetadataStore`'s field-owns-its-format convention.

## `MarkerSlider` (`src/UiComponents/MarkerSlider.h/.cpp`)

A `QSlider` subclass (paint-only) that draws up to two vertical markers, positioned from the real slider style
metrics so they align with the handle. Chosen over an overlay widget or a `QGraphicsVideoItem` route because
it keeps all of `QSlider`'s free behavior (mouse/keyboard seeking, styling, seek signals) and adds only marker
painting.
