# Quickroom architecture

Quickroom is a fast image browser/viewer: a filesystem browser showing folders and media as a thumbnail grid,
with Darkroom's image viewer and video player for opening items. It has no library, catalog, or labels - items
are files, identified by path. It ships from this repo as a second exe built on Darkroom's components, and is
distributed with Darkroom: the same Windows installer, the same macOS disk image.

Read [DARKROOM.md](DARKROOM.md) first for the repository layout and the coding conventions, and
[build.md](build.md) for how `quickroom/quickroom.pro` compiles a hand-listed subset of `app/src` and the link trap
that comes with it.

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
- `FileOperations` - file operations with Quickroom's confirmation policy, shared by the browser and the
  standalone viewer.

Image tiles are `ThumbnailWidget`s (single-file constructor - the FrameViewerWindow pattern), so lazy
dwell-loading, async decode, and Ctrl+wheel zoom are shared behavior, and `MediaGrid`'s card factory
materializes tiles on demand. The grid drives drag and drop - a multi-selection exports all selected paths as
file URLs - so the image tiles' own single-file drag is disabled. Tiles are opaque, so cells are laid out
larger than the tile: the margin is where the view paints the selection background.

Activation: double-click or Enter. Folders navigate in place (queued: the rebuild would delete the tile whose
handler is still running); images open `ImageViewerWindow` browsing the folder's images, with the browse
position reflected back into the grid selection; videos open `VideoPlayerWindow`. Both windows take a null
`Library`, which leaves out their library-bound features (see [playback.md](architecture/playback.md)).

Deletion: Del moves to Trash, Shift+Del deletes permanently, in the grid (the whole selection, folders with
their contents) and in the image viewer (the current image). Only a permanent deletion or one including a folder
asks first. The filesystem step is the shared `PathDeletion` module; the viewer deletes through a handler
Quickroom installs, and the browser removes the deleted entries in place instead of relisting.

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

See [quickroom-backlog.md](quickroom-backlog.md).
