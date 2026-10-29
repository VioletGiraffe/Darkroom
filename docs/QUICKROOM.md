# Quickroom architecture

Quickroom is a fast image browser/viewer: a filesystem browser showing folders and media as a thumbnail grid,
with Darkroom's image viewer and video player for opening items. It has no library, catalog, or labels - items
are files, identified by path. It ships from this repo as a second exe built on Darkroom's components, and is
distributed with Darkroom: the same Windows installer, the same macOS disk image.

Read [DARKROOM.md](DARKROOM.md) first for the repository layout, including the `Shared` library both apps link, and
the coding conventions.

## Structure

`Quickroom/src/` holds the Quickroom-only code:

- `main.cpp` - Shared's `Application` startup, plus the startup routing below. Its own
  application identity ("Quickroom") keeps its QSettings separate from Darkroom's; see
  [settings-and-theme.md](architecture/settings-and-theme.md) for the store both apps share.
- `BrowserWindow` - the main window: navigation header (back/forward/up, path field, Browse button, name filter,
  `SortControl`), `MediaGrid` of tiles, status-bar counts, main menu. Owns the Back/Forward history. The path field's
  dropdown is a separate, saved history of the folders opened from the field itself, Browse, or a folder on the command
  line. Self-deleting, created through `showForFolder`, which resolves the folder to open and can select one entry in
  it. The name filter stays set across navigation.
- `FolderListing` - the folder's entries, listed through thin_io: folders and supported media files, without hidden
  entries. Also the browser's order: by name, modification time or extension, folders first when ascending and last
  when descending. The viewer and player opened from the command line browse in that order too.
- `SettingsDialog` - Quickroom's own: ffmpeg path, theming, camera raw rendering, color management, the video player's frame extraction format.
- `FileOperations` - file operations with Quickroom's confirmation policy, shared by the browser and the
  standalone viewer: deletion, the file clipboard, and copy/move through the `file-operations` engine.

Every tile is a `ThumbnailWidget`: images through a single-file `ImageFilesSource` (the FrameViewerWindow pattern),
folders and videos through `IconSource` with the native file icon. Lazy dwell-loading, async decode, and Ctrl+wheel
zoom are shared behavior, and `MediaGrid`'s card factory materializes tiles on demand. The grid drives drag and drop -
a multi-selection exports all selected paths as file URLs - so tiles offer no single-file drag of their own. Each tile
is its row's card and fills the cell; the view paints no cell background, and the tile shows selection itself through
the `selected` property the grid keeps on every card.

All tiles share one size, as the grid's uniform item sizes require. Zoom sets the width; the height gives the image area
the aspect of the folder's first image in sort order, within 1:2 to 2:1, read from its headers when the folder is listed.
Without an image the tiles are square. Re-sorting, filtering and refreshing keep the size.

A file tile's caption is its name without the extension, followed by the file type's icon from `FileTypeIcons`: for
images the format detected from the decoded data, so a misnamed file shows its real type; for camera raws and videos the
suffix.

Activation: the grid's (double-click, or Cmd+O on macOS) and Enter on every platform. Folders navigate in place
(queued: the rebuild would delete the row whose activation is still being handled); images open `ImageViewerWindow`
browsing the images shown, and videos open `VideoPlayerWindow` browsing the videos shown, each with the browse
position reflected back into the grid selection.
Neither window gets Darkroom's library features: no Best action, no persisted speed or saved loops, no frame target
(see [playback.md](architecture/playback.md)).

Deletion: Del moves to Trash, Shift+Del deletes permanently, in the grid (the whole selection, folders with
their contents) and in the image viewer and video player (the current item). Only a permanent deletion or one
including a folder asks first. The filesystem step is the shared `PathDeletion` module; the viewer and player delete
through the removal actions `FileOperations` installs, and the browser removes the deleted entries in place instead
of relisting.

Photo comparison is Darkroom's `PhotoCompareWindow` (see [playback.md](architecture/playback.md#photocomparewindow)),
opened from the context menu of a selection of two or more images, or empty from Tools > Compare photos to take dropped
files. It is parentless, for a taskbar button of its own. Its pane menu moves the photo to Trash unasked and removes its
entry from the grid.

Copy and move:

- Ctrl+C / Ctrl+X put the selection on the clipboard as file URLs, a cut with the marker file managers use for
  "move on paste" (Explorer's on Windows, KDE's and GNOME's elsewhere). Tiles of cut files are faded, whichever app cut
  them.
- Ctrl+V pastes into the folder shown; a folder tile's menu pastes into that folder. Everything on the clipboard is
  pasted, folders and unsupported files included. A cut is moved and taken off the clipboard.
- A drop lands in the folder tile under the cursor, else in the folder shown. It follows file managers: a move within
  one volume, a copy across volumes, unless the copy or move key is held. Dragging out offers copy and move; the target
  performs a move itself.
- The `file-operations` engine runs each transfer in its own progress dialog, which asks about collisions. Before a
  move, open players and viewers release the files moved. The folder watch shows the result.

On Windows a tile's menu ends with "Explorer context menu": Explorer's own (classic) menu for the same files, opened beside
the item like a submenu, on hover or click, while Quickroom's menu stays open behind it. The shell side is cpputils'
`system/shellcontextmenu`.

The browser watches the folder it shows and refreshes it shortly after a change, so changes made by other programs
appear without F5. A refresh compares the listing with the grid: unchanged entries keep their tiles, and a file
rewritten in place gets a new one. When the shown folder disappears, the browser moves to its nearest existing
ancestor.

Browser state (last folder, tile width, sort order, window geometry) persists under `browser/*` settings keys.

## Startup

The first command-line path decides which window opens; further paths are ignored.

- **Folder** - the browser, listing it.
- **Image** - the viewer alone, browsing the images beside it in that folder. No browser is created: building
  one lists a directory and overwrites the remembered folder, which opening a single file must not do.
- **Video** - the player alone, browsing the videos beside it in that folder.
- **Missing path** - reported, then the browser.
- **Any other file** - the browser on its folder, where that file is not listed.
- **No path** - the browser, on the remembered folder, else Pictures, else home.

With a single file the viewer is the only window, so Esc closes it and the app exits. Leaving fullscreen hands
off to the browser instead: the viewer's exit-fullscreen handler opens it on the image currently shown, then
closes the viewer. The browser must open first, because closing the only window would quit the app.

## Improvement backlog

See [quickroom-backlog.md](quickroom-backlog.md).
