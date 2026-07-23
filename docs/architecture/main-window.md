# Main window

[← Back to architecture index](../../ARCHITECTURE.md)

`MainWindow.h/.cpp` owns a stable `Library` member, the `LabelSidebar`, the card grid (`MediaGrid`), and a persistent
`FrameViewerWindow` (reused, not recreated).

The **constructor** runs `loadInitialLibrary()` before building anything — loading first is not stylistic: `setupUI()`
hands the sidebar a `Library&` it keeps for life, so there is no window to build without one. On the **first run**
(`Settings::RootFolder` unset), `chooseFirstRunLibraryFolder()` prompts before feeding the same recovery loop.
Cancelling leaves the window unbuilt and `main()` drops it after asking `isLibraryLoaded()`; the destructor returns
early for the same reason. The library loads through
`Library::setRoot()` — the same call a later switch uses (see [data-model.md](data-model.md)), so startup has no
parallel load path of its own.

Library > Open library and Library > Create new library share `pickAndSwitchLibrary()`. `Library::setRoot()` validates
and fully loads the requested root; `setRoot()` first flushes the current library, so a persistent save failure blocks
replacement. On success it synchronously destroys player windows, clears the persistent frame viewer, grid, and the
library-specific label filter before returning to the event loop. The sidebar borrows the stable `Library&`, so it needs
no replacement/rebinding. Library switching and Settings are refused while `_isProcessing`: import/re-export pumps
events while holding a catalog batch, and settings changes partway through could give one batch mixed encoding behavior.

## The Library menu and its recent list

The **Library** menu holds `Open library...` and `Create new library...` plus recently opened roots as quick-switch
entries. All paths converge on `switchLibraryTo()`, so a recent entry gets the identical validate-load-replace
transaction and the identical busy refusal.

`Create new library` needs no separate creation step — `setRoot()` already treats a missing root and missing JSON files
as a valid new library. It differs from `Open library` only in rejecting a folder that `Library::holdsLibrary()` reports
as taken, keeping "create" from silently adopting an existing catalog.

Two properties are deliberate and worth preserving:

- **The recent list never touches the filesystem.** Its entries are `Library::rootFolder()` values, already
  lexically normalized, so they are compared as plain case-insensitive strings and never stat-ed to check they
  still exist. A recent library may well sit on an unplugged drive or a dead network share,
  and stat-ing one of those can stall for seconds *every time the menu opens*. The cost of not checking is
  that a stale entry looks live until clicked, at which point the normal switch failure reports it; the entry
  keeps its place, so re-plugging the drive is enough to make it work again.
- **The entries are rebuilt on `aboutToShow`**, so they cannot go stale after a switch.

`recordCurrentLibrary()` is the **single writer** of `Settings::RootFolder` — it persists the root and pushes the recent
entry together; both the startup load and `switchLibraryTo()` call it, keeping the two settings from drifting apart.

`Library` also routes a failed `catalog.json` or `labels.json` flush to MainWindow. The window queues the warning and
offers Retry or Keep working; dirty state remains in memory in the latter case. Closing requires an explicit Discard
choice before closing with unsaved changes.

## Media-type switch

`_mediaTypeFilter` (a `SegmentedToggle`): **All / Videos / Photos**, ANDed with the other filters. A *structural*
filter — changing it rebuilds the grid (`refreshMediaGrid`), unlike the name filter's cheap hide/show.

**Photo cards** use the decoded photo file directly as the image strip — no preview cache. An unloadable path (e.g. a
referenced photo on an unmounted drive) renders a blank card rather than hiding the item (accepted v1 behavior).
Per-type gates: double-click opens the system image viewer instead of the built-in player; middle-click (frame viewer)
is not wired; the green "frames extracted" badge is gated to videos; the context menu adapts per type.

## Name filter

`_nameFilter` (toolbar `QLineEdit`): an item-name substring filter ANDed with the sidebar's label filter. It's a
**view-level hide/show**, not a rebuild: `textChanged` runs `applyNameFilter`, which only toggles each card's
`setHidden` — no grid rebuild and no thumbnail re-decode. (The label filter, by contrast, *does* drive which cards get
built, in `refreshMediaGrid`.)

## Sort control (`src/UiComponents/SortControl.h/.cpp`)

A single **`SortControl`** chip widget bundles the three ordering options (sort field, ascending/descending, "Favorites
first"). It **owns its own persistence** and emits a single **`changed()`** signal whenever the user adjusts any option;
`MainWindow` connects that to `resortMediaGrid()` and reads the state back through the control's getters.

## Key methods

- `refreshMediaGrid()` — clears and rebuilds the grid from `Catalog::mediaItems()` filtered by the label filter, ending
  with `applyNameFilter()`. Deliberately does **not** call `Catalog::rebuildIndex()` — see
  [catalog-and-labels.md](catalog-and-labels.md#in-memory-model) for why. A rebuild preserves the scroll position and
  selection by re-anchoring on `MediaId` identity — not scroll offset or row index, which shift as items are
  inserted/removed. Scroll also persists across restarts; the selection is per-session.
- `showMediaItemContextMenu()` — multi-select-aware right-click menu. **"Remove from library"** drops the selection from
  the catalog only — since the catalog is never re-derived from a disk walk, an untracked video's frame folder stays on
  disk, surfaced again only by the integrity tool or a re-import.
- `effectiveSelection(id)` — **shared** by the context menu, the Edit menu actions, and the label-drop handler.
- `refreshLibraryView()` — `refreshMediaGrid()` plus the sidebar refresh: the entry point for **structural** changes
  (add/delete/rename an item, create a label), where plain `refreshMediaGrid()` covers filter/sort/zoom.

## Label assignment

Two paths, both routed through `effectiveSelection`:
- **Context-menu Labels checklist** — see `showMediaItemContextMenu` above.
- **Dragging a label row from `LabelSidebar` onto a card.** The sidebar drags ordinary label rows only (not "All" or
  `Best`), carrying the label id as `LabelMimeType`; the card is the drop target (see
  [media-widgets.md](media-widgets.md)). The drop handler is **deferred via a queued `invokeMethod`** — the grid rebuild
  must not delete the card mid-drop (the rebuild happens inside the handler, which runs from the card's own
  `dropEvent`).

## LabelSidebar structure (`src/UiComponents/LabelSidebar.h/.cpp`)

A flat **`QListWidget`** (single column) painted by a custom **`LabelRowDelegate`** — also installed on `ImportDialog`'s
label list. The per-row visual state (active tint, spine, hover) cannot be expressed in plain QSS, which is why it's a
delegate. The list is `NoSelection` — filter toggling is handled manually on click.

Row order: **All** → **Best** → a **divider** row → the ordinary labels.

**Auto-hug width**: `ContentWidthListWidget` (a tiny `QListWidget` subclass) overrides `sizeHint` to report the widest
row via the delegate.

## Sidebar label management

Right-clicking an ordinary label row opens a **Rename / Set color / Delete** menu; `LabelSidebar` emits the
corresponding signal carrying the label id, and `MainWindow` handles each. `MainWindow` does not duplicate name/path
rules or create the backing folder itself — those live in the `Catalog` API (see
[catalog-and-labels.md](catalog-and-labels.md)). Import callbacks likewise resolve the destination through the Catalog's
verified path accessors.

## Card interactions

Middle-click → `FrameViewerWindow` (videos only); double-click → built-in player for a video, system image viewer for a
photo; right-click → context menu; Ctrl/Shift-click and rubber-band → multi-select; left-drag → export source files (see
**Media grid & multi-select** below). Middle-click is the deliberate choice for "show frames" — single + double click on
the same button proved unreliable.

---

## Media grid & multi-select

The grid is a `QListWidget` in **IconMode** — gives a wrapping-grid shape while keeping native `ExtendedSelection`.

**History worth knowing**: it was originally a vertical `QListWidget` (free multi-select), then migrated to
a `CFlowLayout` grid for the wrapping shape — which **silently dropped multi-select/Compare** (lost for
several sessions before being caught). The current IconMode `QListWidget` restores both.

> **Lesson encoded**: when swapping a container for shape, list what the old one gave for free (selection
> model, keyboard nav, drag-and-drop, focus handling) and check each still has an equivalent before calling
> the swap done — or say up front that it doesn't, rather than letting the regression surface later.

`CFlowLayout` (`qtutils`) is now used **only** by `FrameViewerWindow`, which needs a plain non-selectable
flow grid.

**Keeping a multi-selection through a press**: `setDragEnabled(true)` on the grid is what prevents a plain press from
collapsing the selection before a drag starts: with a drag possible, `QAbstractItemView` **defers** the "collapse to the
clicked item" to *release*, so the multi-selection survives press+move and the whole group drags together. Deliberately
not implemented: Explorer's release-without-drag collapse-to-single nuance.

**Dragging cards out (file export)**: the grid is a **`MediaGrid`** (`src/UiComponents/MediaGrid.h/.cpp`), a
`QListWidget` subclass whose `startDrag` exports the current selection's **source files** as `file://` URLs
(`CopyAction`) to Explorer or another app. Catalog access stays out of the view — `MainWindow` sets the URL provider
(`dragUrlsForItems`), following the same "MainWindow computes, card draws" split. `MainWindow::dragEnterEvent` guards
against the export dropping back onto the import handler (`event->source() == _mediaGrid` → ignored).

**Empty state**: `MediaGrid` paints a caller-set message whenever **no item is visible** — whether none were built or
the name filter hid them all; the paint checks live visibility, so it needs no hooks in `applyNameFilter`.

---

## Renaming a media item

**`MediaRename`** (`src/Windows/MediaRename.h/.cpp`) is the module — free functions with entry
`MediaRename::renameItemInteractive(id, dialogParent)`, dispatching by media type. `MainWindow::renameItemInteractive`
is a thin wrapper: on success it calls `refreshLibraryView()` and repoints the frame viewer when needed. Both the
Edit-menu **Rename** action and the card context menu go through that wrapper.

`renameVideo` is the **one place a video's frame folder moves on disk outside the label-mutation paths in `Catalog`**
(see [catalog-and-labels.md](catalog-and-labels.md)). It renames the source file, then the frame folder, then calls
`Catalog::applyRename(oldId, newId, ...)` to re-key the `MetadataStore` record (loop intervals, labels incl. Best) —
without it, that metadata would be silently orphaned (`MediaId` encodes filename + size, so a source rename changes it).
`applyRename` refuses on a new-id collision with another tracked item; `renameVideo` then reverts both disk renames,
leaving disk and catalog exactly as they were.

Photos rename differently — `renamePhoto` renames only the file's base name in place (keeping directory and extension),
then re-keys via the same `Catalog::applyRename`. A missing source file is refused.

## Logging & diagnostics

Qt's diagnostic stream is captured into an in-memory log surfaced under **Help → Show log**. `main()` installs a
`QtMessageHandler` **before** constructing `QApplication` — so platform/plugin warnings emitted during its construction
are caught too — and chains to the previous handler so normal stderr output is preserved.

The sink is qtutils' **`CLoggerInMemory`**, reached through the `loggerInstance<CLoggerInMemory>()` singleton.
Thread-safety lives in the sink — it is designed as a global multi-thread sink.

The log is **memory-only and uncapped** — no file sink and no size bound.
