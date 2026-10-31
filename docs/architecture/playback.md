# Frame viewing, playback, and photo comparison

[← Back to architecture index](../DARKROOM.md)

## FrameViewerWindow

`FrameViewerWindow` is a persistent, reusable top-level thumbnail viewer owned by MainWindow. MainWindow updates or
clears it when browser workflows rename, remove, or replace its current media item. Its frames are rows of a
`MediaGrid`, which supplies selection, keyboard navigation and dragging the selected frames as files (see
[main-window.md](main-window.md#media-grid--multi-select)). The grid's activation (double-click, or Cmd+O on macOS)
and Enter on every platform open ImageViewerWindow on the frame. The grid's context menu reveals the clicked frame and
copies the paths of its effective selection; on empty space it reveals the frame folder.

## CompareWindow

`CompareWindow` shows several videos' frame folders side by side: one `ThumbnailWidget` pane per folder and one
slider spanning the longest folder, so every pane steps together and a shorter folder's pane goes blank past its
end. Frame loads are debounced behind slider and resize events. Activating a pane opens ImageViewerWindow browsing
that folder from the current frame. The browser opens it from the context menu of an all-video selection (Inspect for
one item, Compare selected for several), passing frame-folder paths.

## ImageViewerWindow

`ImageViewerWindow` is a self-deleting viewer over qtutils' `CImageViewerWidget`, which owns the pan/zoom view,
the scaling, and the info overlay; the window supplies the menu bar and the browsing order.
Scaling goes through `ImageProcessing::resize`, nearest neighbor when the widget requests a fast transform, and falls
back to `CImageViewerWidget::scaleWithQt` for pixel formats the resizer rejects. Its chunks run on `cpuThreadPool()`,
which also runs work on the calling thread - the GUI thread is blocked for the duration of a scale either way.

Loading never blocks the GUI thread. `ImageViewerLoading` reads and decodes on the pools; the window hands the results to
the widget through a `CImageViewerWidget::PendingImage`, so the widget stays free of threads.
- A preview comes first where the final image takes longer, decoded in parallel with it:
  - a JPEG whose full decode is predicted at 100 ms or more - a 1/8 DCT-reduced decode, the fastest: the full decode
    soon replaces it, but on a slow machine a viewer flicking through files may move on first. The prediction is the
    megapixels times this machine's running average for full JPEG decodes.
  - a camera raw LibRaw renders - its embedded JPEG, fitted to the screen where that takes 1/2 or less, else decoded
    whole: it stays up while LibRaw renders, and above 1/2 a reduced decode is no cheaper.
  - Progressive and arithmetic-coded JPEGs are never reduced: entropy decoding dominates at every scale.
- Other formats show only the final image. Decode times at each JPEG scale and for LibRaw renders:
  [image-decode-performance.md](../image-decode-performance.md).
- The widget lays a preview out at the final image's size, so zoom, Actual pixels and the info strip already refer to the
  final image, and its arrival changes only sharpness.
- While LibRaw renders a raw, the viewer shows its progress. The fraction comes from per-stage weights in
  `LibRawDecoder.cpp`; the hidden raw samples test logs progress against time for tuning them.
- A pending image accepts nothing once superseded, and no preview after its final image. Navigating replaces the load's
  `AsyncRequest`, which stops the abandoned job between its stages and abandons a LibRaw rendering at its next stage.
- The previous image stays on screen until the new one shows; title, position and host actions switch at once. Save as
  and Copy wait for the final image. A failed load empties the view, or leaves its preview, and stays on that position;
  a notice in place of the progress bar gives the reason, with no message box.
- The preview's target is the screen size, not the view's: the first load starts before the window is shown.
- Animated files go to the widget's own reader, which plays them.

Each viewer opens fullscreen and is parentless, for a taskbar button of its own; a modal caller owns the window
instead, Qt blocking input to unparented windows under a modal dialog. Double-click and `F` switch between
fullscreen and a window sized to the image down to a minimum, and the menu bar shows only outside fullscreen, or after a lone Alt tap until its menus close. A caller that defers
creating the window behind the viewer can install an exit-fullscreen handler: it runs once when the user leaves
fullscreen and decides whether that switch still happens. A caller can also install removal actions, see below, and
add its own actions to the Image menu, which the viewer keeps current for the image shown: MediaBrowserWidget adds
Best. Alone among the windows here it persists no geometry.
Copy puts both the image and its file on the clipboard, for apps that paste pixels and for those that take files.
Save as copies the file when the target format matches the source, keeping metadata and every frame; any other
format encodes the frame shown.
The info strip shows the image's position in the list and its file name in the center.
The current image becomes the window icon after a delay, so several open viewers stay distinguishable in the
taskbar without an icon being scaled for every image browsed past.

It browses a path list captured when it opens, skipping entries that have since left the disk; moves concluded by
`followMovedMediaFiles` rebase it. Four callers build that list: MediaBrowserWidget from the photos the grid currently
shows, ImportDialog from its staged photos, FrameViewerWindow from one frame folder, and
CompareWindow from the frames of the pane that was clicked.

Browsing, host actions and removal actions receive the current path of the image shown. MediaBrowserWidget and
ImportDialog derive the item's id from it at that moment, as the player derives its own, so an item renamed while the
viewer is open keeps working; browsing moves their grid selection onto the photo on screen. The initial image is not
reported: the caller already acted on it.

## VideoPlayerWindow

`VideoPlayerWindow` coordinates Qt media playback, controls, saved-loop persistence, optional oscillating
presentation, and single-frame extraction. It does not depend on the library model: a host can supply a playback
settings store, a navigation list, and a frame target. `LibraryPlayer` opens players with a store that resolves each
path through the catalog, so only library items keep their settings, and with the Extracted-label target; Quickroom's
players have neither. Open players are tracked app-wide so library replacement can close them synchronously before
the store starts resolving against a different library.

MediaBrowserWidget and Quickroom's browser give each player they open the paths of the videos the grid currently shows
as its previous/next order, and a handler that moves the grid selection onto the item navigated to, as with the image
viewer above; Quickroom's standalone player gets the videos beside it in its folder. The initially loaded video is not
reported. A removal clears the entry, so navigation skips it like a deleted file.

Copy puts the video file on the clipboard.

The player exposes one logical position and play-state seam. Controls therefore refer to the frame actually presented
whether `QMediaPlayer` or `OscillatingPlayback` currently owns presentation.

Media errors are observed before assigning the source. Fatal source failures are reported once and close an unusable
window. A backend format error remains nonfatal when a playable video track exists because an unsupported secondary
stream, commonly audio, need not prevent video playback.

### Removal actions

The viewer and the player bind Del and Shift+Del to two actions their host supplies, each a menu text and an
operation that reports whether the item left the host's list. Without them the keys do nothing.

| Host | Del | Shift+Del |
| --- | --- | --- |
| MediaBrowserWidget | remove from library | delete to Trash |
| ImportDialog | remove from staging | delete the source file |
| Quickroom | move to Trash | delete permanently |

After a removal the window moves to the nearest remaining item, or closes when it has none. Frame viewers and the
relocation preview players install no actions.

An open file cannot be deleted or moved on Windows, so an operation that deletes or moves media first calls
`releaseMediaFilesHeldOpen`: every player showing an affected file drops its source, and every viewer showing an
affected animation closes it, stopping on its current frame. Still images are decoded whole and hold no file.

- **Deletion** (`PathDeletion`): the windows stay open without the file. The window whose own removal action failed or
  was cancelled reloads its file, a player at its previous position.
- **Darkroom's moves** (renaming an item or a label, a label removal that relocates storage, import moves and
  renames): `followMovedMediaFiles` concludes them. Players and viewers rebase the paths they list and reopen a released
  file at its new location, or where it was when its move failed. `Catalog` moves storage itself, so it reports its moves through
  storage-move handlers that MainWindow installs on `Library`.
- **Quickroom's transfers**: the moved files are released and not reopened.

### Oscillating playback

`OscillatingPlayback` prepares an in-memory JPEG cache for the active A-B range, or the whole video when no valid
range exists, then presents it forward and backward without seeking at turnarounds. One asynchronous ffmpeg process
streams multipart MJPEG through stdout; the controller owns process lifetime, and `MJpegParser` splits the stream into
frames. There are no temporary files or Darkroom worker threads.

Admission bounds range duration, output resolution, and frame count to prevent runaway caches. Presentation is driven
by elapsed time on the GUI thread, dropping obsolete frames after a late tick rather than slowing the motion. A gap
too long for a late tick is a blocked GUI thread, such as a frame extraction: the motion resumes where it stopped. JPEGs
decode transiently and are submitted to the existing `QVideoSink` through `QVideoFrame(QImage)`, with explicit
format conversion only when Qt has no direct mapping.

Motion curves map one normalized phase while the speed control retains comparable meaning across curves. Switching
curve preserves cycle phase. The selected curve is global and persisted; activation and cached frames are
window-local and transient.

The paused Qt player remains attached while cached frames are submitted. Audio is silent during preparation and
oscillation. Any operation that invalidates the active range or presentation position drops the cache. Returning to
normal playback resumes at the last displayed cached timestamp and preserves the logical playing/paused state.

### Single-frame extraction

`SingleFrameExtraction` owns configured ffmpeg extraction into a folder, failure reporting, and the remembered last
destination that `E` repeats: a chosen folder, or the host's frame target. A frame target names a folder the frame is
written into, then handles the written file. ffmpeg runs in the background, owned by the player: closing the player
abandons the extraction and removes its partial output.

`LibraryPlayer`'s target is the configured Extracted label's photo folder, and the frame is tracked there as an owned
photo. The library holds no two identical files: when a byte-identical photo is already tracked, the frame is deleted
and that photo gets the label instead. Extracted photos never enter the video's regenerable frame folder, which full
re-extraction may replace wholesale. Successful import reaches the browser through normal Catalog notification.

### Saved loops and playback speed

Saved A-B loops are stored per video, including their name and captured playback speed.
Activating a loop restores its speed without overwriting the video's independently remembered speed.

Playback speed is also persisted per video. The default 1x value removes the field, distinguishing untouched/default
state without redundant records. Applying a restored or loop-specific speed can bypass persistence. Players resolve
the current video's item only at read/write points; MainWindow closes them before a library switch can redirect those
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

The window lives in `Shared`: Quickroom opens it too. Each pane's context menu offers the host's removal action, if
one is installed. A photo the action reports gone leaves the comparison; removing the reference resets alignment around
a surviving reference.

- Darkroom's action (`LibraryPhotoCompare`) borrows the stable `Library`: a path Catalog owns is deleted through
  `MediaItemManagement`, its record removed only after the filesystem operation succeeds. Any other file goes to Trash
  after its own confirmation. ImportDialog's action also unstages the deleted source.
- Quickroom's action follows its deletion policy, see [QUICKROOM.md](../QUICKROOM.md).

### Image loading and worker ownership

Photo loading follows the app-wide two-stage rule: reads run on `Core::IoThreadPool`, decoding on `cpuThreadPool()`.
The completed ordered batch is published to the GUI thread at once. Only one load batch is admitted per window. The
background stages hold only the batch, never the window, so closing cancels the load without waiting for a read in
flight.

The mip chain avoids live one-pass minification artifacts during continuous zoom. A window-local compute pool runs
automatic alignment, parallelizing across photos and inside each fit. Nested work is safe because the calling worker
participates in each parallel range rather than blocking for a saturated child queue. Alignment remains deterministic
because fits perform no cross-thread floating-point reduction.

The alignment pool is window-local because alignment shares the window's lifetime and no broader owner needs the
resource. Alignment state is transient. Window geometry, rotation-constraint preference, and the optional
alignment region persist; the region is stored relative to the reference dimensions so it restores across
resolutions.
