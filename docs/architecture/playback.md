# Frame viewing, playback, and photo comparison

[← Back to architecture index](../../ARCHITECTURE.md)

## FrameViewerWindow

`FrameViewerWindow` is a persistent, reusable top-level thumbnail viewer owned by MainWindow. MainWindow updates or
clears it when browser workflows rename, remove, or replace its current media item. It uses `CFlowLayout` because
its thumbnail flow is non-selectable; the main media grid instead requires the native selection and input behavior
documented in [main-window.md](main-window.md#media-grid--multi-select).

## VideoPlayerWindow

`VideoPlayerWindow` coordinates Qt media playback, controls, saved-loop persistence, optional oscillating
presentation, and single-frame extraction. Open players are tracked app-wide so library replacement can close them
synchronously before the stable `Library&` begins resolving a different store.

The player exposes one logical position and play-state seam. Controls therefore refer to the frame actually presented
whether `QMediaPlayer` or `OscillatingPlayback` currently owns presentation.

Media errors are observed before assigning the source. Fatal source failures are reported once and close an unusable
window. A backend format error remains nonfatal when a playable video track exists because an unsupported secondary
stream, commonly audio, need not prevent video playback.

### Oscillating playback

`OscillatingPlayback` prepares an in-memory JPEG cache for the active A-B range, or the whole video when no valid
range exists, then presents it forward and backward without seeking at turnarounds. One asynchronous ffmpeg process
streams multipart MJPEG through stdout; the controller owns parsing and process lifetime. There are no temporary
files or Darkroom worker threads.

Admission bounds range duration, output resolution, and frame count to prevent runaway caches. Presentation is driven
by elapsed time on the GUI thread, dropping obsolete frames after a late tick rather than slowing the motion. JPEGs
decode transiently and are submitted to the existing `QVideoSink` through `QVideoFrame(QImage)`, with explicit
format conversion only when Qt has no direct mapping.

Motion curves map one normalized phase while the speed control retains comparable meaning across curves. Switching
curve preserves cycle phase. The selected curve is global and persisted; activation and cached frames are
window-local and transient.

The paused Qt player remains attached while cached frames are submitted. Audio is silent during preparation and
oscillation. Any operation that invalidates the active range or presentation position drops the cache. Returning to
normal playback resumes at the last displayed cached timestamp and preserves the logical playing/paused state.

### Single-frame extraction

`SingleFrameExtraction` owns configured ffmpeg extraction, failure reporting, destination persistence, and optional
library import. It can extract to a chosen folder or import the result as an owned photo under the configured
Extracted label.

Library-bound extraction writes a temporary file under the library root so the subsequent Move import remains a
same-volume rename. It reuses the normal photo-import collision and deduplication rules. Extracted photos never enter
the video's regenerable frame folder, which full re-extraction may replace wholesale. Successful import reaches the
browser through normal Catalog notification.

### Saved loops and playback speed

Saved A-B loops are stored per `MediaId` in `MetadataStore`, including their name and captured playback speed.
Activating a loop restores its speed without overwriting the video's independently remembered speed.

Playback speed is also persisted per video. The default 1x value removes the field, distinguishing untouched/default
state without redundant records. Applying a restored or loop-specific speed can bypass persistence. Players resolve
the current store only at read/write points; MainWindow closes them before a library switch can redirect those
operations.

`MarkerSlider` adds paint-only A/B markers to `QSlider` using the slider's real style metrics. Retaining the native
slider preserves seeking, keyboard input, signals, and styling.

## PhotoCompareWindow

`PhotoCompareWindow` compares multiple photos through two transform layers:

1. A shared view transform supplies zoom and pan in equal-sized pane coordinates.
2. A per-photo similarity transform maps each image into subject space, defined by the current reference photo.

Separating these layers lets every pane show the same subject position while compensating independently for scale,
rotation, and offset. The reference transform is folded into the shared view whenever the reference changes, keeping
the reference pixels stationary while subject space rebases.

### Alignment

Default alignment normalizes image height to the reference and centers it. Alignment can then be changed in three
ways:

- **Automatic alignment** uses the `magic-alignment` library to estimate each photo against the reference, starting
  from its current transform. A failed fit preserves that transform. Rotation can be constrained out of the fit, and
  an optional subject-space region can restrict evidence when the scene has no useful global alignment.
- **Two-point calibration** uses corresponding feature pairs to solve the full similarity transform and supports
  rotations beyond automatic alignment's intended small-angle range.
- **Manual adjustment** changes an individual photo's scale and offset.

Flicker, difference, and single-photo full-view modes all consume the same aligned image set and shared view; they do
not maintain independent transforms.

### Image loading and worker ownership

Photo loading follows the app-wide two-stage rule: tagged reads run on `Core::IoThreadPool`, then a window-local
compute pool decodes images and builds halving mip chains. The completed ordered batch is published to the GUI thread
at once. Only one load batch is admitted per window; closing aborts pending decode publication and retires tagged I/O
before the compute pool joins.

The mip chain avoids live one-pass minification artifacts during continuous zoom. The same compute pool runs
automatic alignment, parallelizing across photos and inside each fit. Nested work is safe because the calling worker
participates in each parallel range rather than blocking for a saturated child queue. Alignment remains deterministic
because fits perform no cross-thread floating-point reduction.

The compute pool is window-local because loading and alignment share the window's lifetime and no broader owner needs
the resource. Alignment state is transient. Window geometry, rotation-constraint preference, and the optional
alignment region persist; the region is stored relative to the reference dimensions so it restores across
resolutions.
