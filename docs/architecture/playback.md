# Frame viewer & video player

[← Back to architecture index](../../ARCHITECTURE.md)

## `FrameViewerWindow` (`src/Windows/FrameViewerWindow.h/.cpp`)

Persistent per-folder thumbnail popup (top-level `QWidget`, reused not destroyed on close). `MainWindow` drives
it — updates its folder on rename via `currentFolder()` (see [main-window.md](main-window.md)). Uses `CFlowLayout`
(`qtutils`) — its last consumer, since it needs a plain non-selectable flow grid, unlike `MainWindow`'s grid
which needs the native multi-select `CFlowLayout` lacks (see
[main-window.md](main-window.md#media-grid--multi-select)).

---

## `VideoPlayerWindow` (`src/Windows/VideoPlayerWindow.h/.cpp`) + `MarkerSlider`

Built-in player (`QMediaPlayer` + `QVideoWidget` + `QAudioOutput`) for the double-click-to-play path. Keeps a
static list of open instances (app-wide restart/close) and auto-tiles each window into screen thirds once the
video size is known. Offers an A–B loop plus multiple saved loops per video. The player has one logical
position/play-state seam so controls keep referring to the frame actually shown when the optional oscillating
mode temporarily takes presentation over from `QMediaPlayer`.

### Oscillating playback

For a valid A–B interval, **Oscillate** prepares an in-memory JPEG cache and then presents it forward and
backward without seeking at either turnaround. Preparation is one asynchronous ffmpeg process owned by the
player window; its constant-rate, at-most-60-fps MJPEG stream is parsed by multipart `Content-length` directly
from stdout. There are no temporary files and no Darkroom worker thread. The coarse admission limits are 30
seconds and a 1920×1080 output envelope (smaller sources are not upscaled); the resulting frame-count cap is
the runaway guard, deliberately without compressed-byte accounting.

Presentation uses an approximately 60 Hz GUI-thread clock. It selects the frame implied by elapsed time, so a
late tick or 2× playback drops obsolete frames instead of slowing the motion. Linear, cosine, smoothstep, and
smootherstep curves normalize the speed selector as their approximate maximum source speed; switching curve
keeps raw cycle phase and cuts to the new mapping on the next tick. JPEG decode is inline and transient:
the decoded `QImage` is submitted to the existing `QVideoSink` through Qt 6.8's `QVideoFrame(QImage)`.
Formats with no direct Qt video-frame mapping receive one explicit `RGBA8888` conversion.

The paused `QMediaPlayer` remains attached to the video widget while the manual frames are submitted. Audio is
effectively muted during both preparation and oscillation, independently of the persisted user mute choice.
Changing/clearing A or B, activating a saved loop, seeking, restart-all, or turning the toggle off cancels and
drops the cache. Normal playback resumes at the discrete timestamp of the last displayed cached frame and
preserves whether the oscillation was playing or paused. The selected motion curve is global/persisted; the
toggle and cache are per-window/transient.

### Frame extraction

Right-clicking the video (`Ffmpeg::extractFrame`) extracts the frame at the clicked moment (left click stays
play/pause) to one of three destinations: the library, a picked folder, or a repeat of whichever ran last
(persisted). The library path lands it as an **owned photo** under the configurable "Extracted" label via
`Import::importPhoto(Move)`, reusing photo import's dedup/collision handling; extraction goes to a temp dir
under the library root (not system temp) so that move is a same-drive rename. Frames deliberately never go
into the video's frame folder — a regenerable artifact a re-split wipes wholesale. Untracked (staging-preview)
videos work too; the main window does not yet refresh when a frame lands.

### Saved loops

Persisted via `MetadataStore` (see [data-model.md](data-model.md)) under the `"intervals"` field — a list of
`{start, end, name}` objects keyed by the played video's `MediaId`; (de)serialization owned here per the
field-owns-its-format convention. Each player borrows the stable `Library&` and resolves the store at the
read/write point; after a root switch `MainWindow` closes every player synchronously before returning to the
event loop, so an old video's controls cannot write the new library's state.

## `MarkerSlider` (`src/UiComponents/MarkerSlider.h/.cpp`)

A `QSlider` subclass (paint-only) that draws up to two vertical markers, positioned from the real slider style
metrics so they align with the handle. Chosen over an overlay widget or a `QGraphicsVideoItem` route because
it keeps all of `QSlider`'s free behavior (mouse/keyboard seeking, styling, seek signals) and adds only marker
painting.

---

## `PhotoCompareWindow` (`src/Windows/PhotoCompareWindow.h/.cpp`)

N-way photo comparison in a near-square grid of equally sized panes, maximized by default. `MainWindow` offers
it for any all-photo selection of at least two; `showForFiles` filters missing paths and caps the comparison at
50 (the same cap applies to photos added later by dropping image files or whole folders, each default-aligned
against the current reference). Also openable empty from Tools → "Compare photos...". A pane's context menu can
make its photo the reference (the reference pane is outlined in yellow). Its core mechanism is **two separate
transform layers**:

- **One shared view** (zoom + pan, in widget coordinates — equal pane sizes are what make the same widget
  position show the same subject point in every pane): wheel zooms all panes around the cursor, drag pans all.
- **A per-photo alignment transform** (a similarity: uniform scale + rotation + offset) mapping each image
  into the shared "subject" space, defined as the **reference photo's** pixel coordinates (photo 0 by
  default; re-elected by two-point calibration or the pane context menu). This is the zoom/crop/rotation-difference compensation. The
  default normalizes each photo's height to the reference's and centers — so pure resolution differences
  align with no user action. The view itself stays rotation-free (the reference, by the fold convention
  below, always has rotation 0).

Three ways to set the alignment:

- **Auto-align (`A`)** — one click estimates every photo's transform against the reference via the
  `magic-alignment` submodule (a Qt-only static library; a deterministic coarse-to-fine correspondence search
  with a robust similarity fit and an accept-or-fail verdict — the full pipeline is documented on `alignImages`
  in `MagicAlignment.h`), feeding the current alignment in as the initial guess. A photo the library cannot align reliably *keeps* its current alignment rather than receiving a
  plausible-but-wrong one. Rotation is corrected along with scale and offset, but its capture range is
  small-angle only (a few degrees — horizon-correction grade; a larger real rotation fails honestly); a
  notable corrected angle is surfaced in the hint bar, being the one component with no manual-adjustment
  gesture. The bottom bar's **"Ignore rotation"** checkbox removes the rotation degree of freedom from the
  fit — scale and offset are re-derived by the constrained least squares rather than by zeroing a
  jointly-fitted angle — for pairs whose apparent rotation is spurious (e.g. depth parallax reading as a
  slight tilt).
  An optional **align region** (Shift+drag; one subject-space rect drawn dashed in every pane; Shift+click
  clears; persists across aligns and reference folds) restricts the alignment evidence to that region — the
  tool for scenes where no global alignment exists (depth parallax between focus-stack slices, locally moved
  subjects): align what matters and let the rest fall where it falls.
  `I` toggles a diagnostic overlay (off by default) that draws the last run's patch evidence as
  true-footprint squares — accent = used for the fit, orange = outlier, red = no match.
- **Two-point calibration (`Shift+A`)** — click the same two features in every photo; the photo receiving the
  session's first point becomes the reference; the two point pairs determine the full similarity exactly
  (scale from the distance ratio, rotation from the segment angles, offset from the midpoints) — so manual
  calibration handles arbitrary angles, beyond auto-align's range.
- **Manually** — Ctrl+wheel / Ctrl+drag adjusts the hovered photo's scale / offset alone (rotation has no
  manual gesture).

Both auto-align and calibration first **fold the reference's transform into the view** (the view pan/zoom
absorb it, the reference's own transform becomes identity): the reference stays pixel-frozen on screen while
subject space rebases to its pixel coordinates, and only the other photos move to meet it.

Comparison modes over the aligned set:

- **Flicker** — hold `1`..`N` (capped at 9) to render that photo in every pane under the shared view; with
  aligned photos, the fastest way to spot differences.
- **Difference** (`D`) — every pane except the reference's renders as the per-channel |photo − reference|, so
  regions that match go dark and only real differences stand out.
- **Full view** — the bottom slider (`Left`/`Right` steps it) shows one photo full-size across the whole grid,
  so scrubbing it is a full-size flicker; `Esc` returns to the grid.

Implementation notes:
- The pane widget (`PhotoComparePane`) is defined **in the .cpp** — it's a pure viewport; all state (images,
  mip chains, alignment, view, calibration points) lives in the window, reached via friendship. No `Q_OBJECT`
  on either class (direct calls, no signals).
- **Minification quality**: panes paint through a halving mip chain rather than scaling the source in one live
  pass — the same minification-aliasing lesson as `ThumbnailWidget`'s pre-resample fix, adapted for continuous
  zoom.
- **Loading is asynchronous and two-stage**, per the app-wide I/O rule (see
  [ARCHITECTURE.md](../../ARCHITECTURE.md)): the reads run on `Core/IoThreadPool` as one tagged task, each
  handing its bytes to a decode on the window's own `CWorkerThreadPool` (`_workerPool`, sized to leave the GUI
  thread a core); the decode that completes the batch posts `applyLoadedPhotoBatch`, so the ordered batch lands
  on the GUI thread in one go. **One batch at a time** — `_loadBatch` is non-null while a load is in flight and
  `dragEnterEvent` denies drops meanwhile (chosen over a pool busy-query API). Closing mid-load aborts the batch
  (pending decodes become no-ops) and retires the read task by tag, so the read loop is provably gone before the
  pool member joins.
- The same `_workerPool` also backs **auto-align**, parallel both across photos and within each `alignImages`
  call (the library takes a pool parameter and owns no threads of its own). The nesting is deadlock-free because
  `parallelFor`'s caller drains the range too, and it self-balances: a two-photo compare gives the inner fit
  every core, while many photos saturate the outer loop and each inner call runs inline. The result is
  bit-identical to a serial run — no cross-thread floating-point reductions — so alignment stays idempotent. The
  pool is **window-local on purpose**, living and dying with the window; promote it to an app-wide pool only if
  a third consumer appears.
- The alignment is transient, but three things persist across sessions: window geometry, the "Ignore
  rotation" option, and the align region. The latter two use window-local `QSettings` keys defined in the
  `.cpp` (deliberately not in `Settings.h`); the region is stored as fractions of the reference frame, so it
  restores at any resolution and re-anchors to the current reference on open.
- **`R`** resets every photo to its default alignment and drops the align region and all comparison modes,
  keeping only the current reference and the persisted "Ignore rotation" option.
