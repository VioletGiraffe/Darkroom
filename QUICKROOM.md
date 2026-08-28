# Quickroom architecture

Quickroom is a fast image browser/viewer: a filesystem browser showing folders and media as a thumbnail grid,
with Darkroom's image viewer and video player for opening items. It has no library, catalog, or labels - items
are files, identified by path. It ships from this repo as a second exe built on Darkroom's components.

Read [DARKROOM.md](DARKROOM.md) first for the shared build layout, the hand-listed source sharing in
`quickroom/quickroom.pro` (and its link trap), and the coding conventions.

## Structure

`quickroom/src/` holds the Quickroom-only code:

- `main.cpp` - Darkroom's bootstrap shape minus the library. Its own application identity ("Quickroom") keeps
  its QSettings separate from Darkroom's.
- `BrowserWindow` - the main window: navigation toolbar (back/forward/up + editable path field), `MediaGrid`
  of tiles, status-bar counts. Owns the navigation history and the folder listing: folders first in natural
  name order, unsupported files not shown.
- `IconTileWidget` - tile for entries without an image preview (folders, videos): native file icon + caption,
  styled via the shared `framedThumbnail` QSS rule.

Image tiles are `ThumbnailWidget`s (single-file constructor - the FrameViewerWindow pattern), so lazy
dwell-loading, async decode, and Ctrl+wheel zoom are shared behavior, and `MediaGrid`'s card factory
materializes tiles on demand. The grid drives drag and drop - a multi-selection exports all selected paths as
file URLs - so the image tiles' own single-file drag is disabled.

Activation: double-click or Enter. Folders navigate in place (queued: the rebuild would delete the tile whose
handler is still running); images open `ImageViewerWindow` browsing the folder's images, with the browse
position reflected back into the grid selection; videos open `VideoPlayerWindow`. Both windows take a null
`Library`, which leaves out their library-bound features (see [playback.md](docs/architecture/playback.md)).

Browser state (last folder, tile size, window geometry) persists under `browser/*` settings keys.

## Improvement backlog

- Drive/computer view above filesystem roots (v2); today the path field is the way across drives.
- Name filter over the current folder.
- Video thumbnails: needs per-file ffmpeg extraction and a cache; tiles show the shell icon today.
- Own app icon: Darkroom's icon is reused.
- Windows deployment: CI builds the exe but stages only Darkroom into the installer.
- Deduplicate the logging/message-handler block shared verbatim by both apps' `main.cpp`.
