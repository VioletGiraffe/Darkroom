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

---

## `PhotoCompareWindow` (`src/Windows/PhotoCompareWindow.h/.cpp`)

N-way (2..4, gated in `MainWindow`'s context menu for all-photo selections; also openable empty from
Tools → "Compare photos...") photo comparison in a square grid of equally sized panes, maximized by default.
Photos can be added at any time by dropping image files onto the window — the pane grid is rebuilt on every
count change, and each added photo is default-aligned against the current reference. Its core mechanism is
**two separate transform layers**:

- **One shared view** (zoom + pan, in widget coordinates — equal pane sizes are what make the same widget
  position show the same subject point in every pane): wheel zooms all panes around the cursor, drag pans all.
- **A per-photo alignment transform** (uniform scale + offset — *no rotation*, a deliberate v1
  simplification) mapping each image into the shared "subject" space, defined as the **reference photo's**
  pixel coordinates (photo 0 by default; two-point calibration re-elects it). This is the zoom/crop-difference
  compensation. The default normalizes each photo's height to the reference's and centers — so pure
  resolution differences align with no user action.

Three ways to set the alignment:

- **Auto-align (`A`)** — one click estimates every photo's transform against the reference via the
  `magic-alignment` submodule (a Qt-only static library; pipeline: black-bar detection → coarse joint
  scale+offset brute force → patch correspondences refined coarse-to-fine over an image pyramid → trimmed
  least-squares similarity fit → accept-or-fail verdict), feeding the current alignment in as the initial
  guess. A photo the library cannot align reliably *keeps* its current alignment rather than receiving a
  plausible-but-wrong one. The fit measures rotation internally (it keeps patch tracking and outlier trimming
  honest on slightly rotated pairs), but the applied model stays scale + translation — a notable measured
  angle is reported in the hint bar instead, as the explanation for residual mismatch the model cannot
  correct. The per-patch evidence is drawn on the panes as true-footprint squares (accent = used in the fit,
  orange = matched well but disagrees with the fit: locally moved content, parallax, red = no match) until
  the next alignment; a per-photo outcome summary lands in the hint bar.
- **Two-point calibration (`Shift+A`)** — click the same two features in every photo; the photo receiving the
  session's first point becomes the reference; the distance ratio gives each photo's scale, the midpoint
  difference its offset.
- **Manually** — Ctrl+wheel / Ctrl+drag adjusts the hovered photo's transform alone.

Both auto-align and calibration first **fold the reference's transform into the view** (the view pan/zoom
absorb it, the reference's own transform becomes identity): the reference stays pixel-frozen on screen while
subject space rebases to its pixel coordinates, and only the other photos move to meet it.

Comparison modes over the aligned set:

- **Flicker** (hold `1`..`N`, capped at 9): every pane temporarily renders that photo under the shared view
  (accent frame + the corner caption flag the override) — with aligned photos, the fastest way to spot
  differences.
- **Difference** (`D` / the Normal—Difference toggle at the toolbar's right): every pane except the
  reference's renders as the per-channel |photo − reference| (the reference drawn first, the photo composited
  over it with `QPainter::CompositionMode_Difference`); a region only one of the two covers differences
  against the matte, i.e. reads as (nearly) unchanged.
- **Full view** (the bottom slider; `Left`/`Right` step it): a single pane covering the whole grid area shows
  the photo at the slider's position, so scrubbing the slider is a full-size flicker; held digit keys still
  override; `Esc` returns to the grid. Viewport switches keep the subject point at the center fixed once the
  user has navigated (an untouched view just re-fits).

Implementation notes:
- The pane widget (`PhotoComparePane`) is defined **in the .cpp** — it's a pure viewport; all state (images,
  mip chains, alignment, view, calibration points) lives in the window, reached via friendship. No `Q_OBJECT`
  on either class (direct calls, no signals).
- **Minification quality**: panes paint through a lazily built halving mip chain
  (`QImage::scaled(..., SmoothTransformation)` per level), picking the level that keeps the painter's live
  bilinear pass at ≤2× reduction — the same minification-aliasing lesson as `ThumbnailWidget`'s pre-resample
  fix, adapted for continuous zoom.
- **Click-vs-drag** on a pane: a press starts drag tracking, a sub-threshold release is a click — that's how
  calibration points are placed, keeping pan/zoom fully available *while* calibrating (points are stored in
  image coordinates, so navigating doesn't disturb them).
- The view re-fits on every pane resize (first show, maximize, later window resizes) **until the user first
  navigates**; after that resizes leave the view alone.
- Photos that fail to load are skipped at load time (constructor batch and dropped files alike) with a
  `qWarning` (so all downstream code can assume every pane has a valid image); alignment is transient —
  nothing here persists except window geometry.
