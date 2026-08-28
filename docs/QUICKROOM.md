# Quickroom architecture

Quickroom is a fast image browser/viewer: a filesystem browser showing folders and media as a thumbnail grid,
with Darkroom's image viewer and video player for opening items. It has no library, catalog, or labels - items
are files, identified by path. It ships from this repo as a second exe built on Darkroom's components.

Read [DARKROOM.md](DARKROOM.md) first for the shared build layout, the hand-listed source sharing in
`quickroom/quickroom.pro` (and its link trap), and the coding conventions.

## Structure

`quickroom/src/` holds the Quickroom-only code:

- `main.cpp` - Darkroom's bootstrap shape minus the library, plus the startup routing below. Its own
  application identity ("Quickroom") keeps its QSettings separate from Darkroom's.
- `BrowserWindow` - the main window: navigation toolbar (back/forward/up + editable path field), `MediaGrid`
  of tiles, status-bar counts. Owns the navigation history and the folder listing: folders first in natural
  name order, unsupported files not shown. Self-deleting, created through `showForFolder`, which resolves the
  folder to open and can select one entry in it.
- `IconTileWidget` - tile for entries without an image preview (folders, videos): native file icon + caption,
  styled via the shared `framedThumbnail` QSS rule.

Image tiles are `ThumbnailWidget`s (single-file constructor - the FrameViewerWindow pattern), so lazy
dwell-loading, async decode, and Ctrl+wheel zoom are shared behavior, and `MediaGrid`'s card factory
materializes tiles on demand. The grid drives drag and drop - a multi-selection exports all selected paths as
file URLs - so the image tiles' own single-file drag is disabled.

Activation: double-click or Enter. Folders navigate in place (queued: the rebuild would delete the tile whose
handler is still running); images open `ImageViewerWindow` browsing the folder's images, with the browse
position reflected back into the grid selection; videos open `VideoPlayerWindow`. Both windows take a null
`Library`, which leaves out their library-bound features (see [playback.md](architecture/playback.md)).

Browser state (last folder, tile size, window geometry) persists under `browser/*` settings keys.

## Startup

The first command-line path decides which window opens; further paths are ignored.

- **Folder** - the browser, listing it.
- **Image** - the viewer alone, browsing the images beside it in that folder. No browser is created: building
  one lists a directory and overwrites the remembered folder, which opening a single file must not do.
- **Video** - the player alone.
- **Missing path** - reported, then the browser.
- **Any other file** - the browser on its folder, where that file is not listed.
- **No path** - the browser, on the remembered folder, else Pictures, else home.

With a single file the viewer is the only window, so Esc closes it and the app exits. Leaving fullscreen hands
off to the browser instead: the viewer's exit-fullscreen handler opens it on the image currently shown, then
closes the viewer. The browser must open first, because closing the only window would quit the app.

## Improvement backlog

- Leaving fullscreen closes the viewer; a setting could keep it open behind the browser instead. The
  exit-fullscreen handler's return value already selects between the two.
- The player has no such hand-off: a video opened from the command line never reaches the browser.
- Drive/computer view above filesystem roots; today the path field is the way across drives.
- Opening several files at once starts one process each - no single-instance handover.
- Name filter over the current folder.
- Video thumbnails: needs per-file ffmpeg extraction and a cache; tiles show the shell icon today.
- Own app icon: Darkroom's icon is reused.
- Windows deployment: CI builds the exe but stages only Darkroom into the installer.
- Deduplicate the logging/message-handler block shared verbatim by both apps' `main.cpp`.
