# Main window

[← Back to architecture index](../../ARCHITECTURE.md)

`MainWindow.h/.cpp` owns a stable `Library` member, a `MediaBrowserWidget`, and a persistent `FrameViewerWindow`
(reused, not recreated). `MediaBrowserWidget` is the main-window content composite: it owns the `LabelSidebar`,
browser toolbar, card grid (`MediaGrid`), browser-local settings, filtering/sorting, selection/view-state preservation,
card construction, activation, context menu, Best/label mutations, and item rename/remove/delete workflows. It borrows
the stable `Library&`. Its remaining signals cross a real ownership boundary: `MainWindow` updates its Edit-menu state
from browser selection, asks `FrameExtraction` to satisfy an on-demand split before presenting its persistent frame
viewer, and keeps that viewer coherent when a browser operation renames or deletes its current folder.

The **constructor** runs `loadInitialLibrary()` before building anything — loading first is not stylistic: `setupUI()`
constructs the browser with a `Library&`, which its sidebar also keeps for life, so there is no window to build without
one. On the **first run**
(`Settings::RootFolder` unset), `chooseFirstRunLibraryFolder()` prompts before feeding the same recovery loop.
Cancelling leaves the window unbuilt and `main()` drops it after asking `isLibraryLoaded()`; the destructor returns
early for the same reason. The library loads through
`Library::setRoot()` — the same call a later switch uses (see [data-model.md](data-model.md)), so startup has no
parallel load path of its own.

Library > Open library and Library > Create new library share `pickAndSwitchLibrary()`. `Library::setRoot()` validates
and fully loads the requested root; `setRoot()` first flushes the current library, so a persistent save failure blocks
replacement. On success it synchronously destroys player windows, clears the persistent frame viewer, and asks the
browser to clear its grid and library-specific label filter before returning to the event loop. The browser and sidebar
borrow the stable `Library&`, so they need no replacement/rebinding. Library switching and Settings are refused while
`_isProcessing`: re-export pumps events while holding a catalog batch, and settings changes partway through could give
one batch mixed encoding behavior. Import has its own application-modal workspace, which prevents interaction with
these shell commands while `ImportExecution` pumps events.

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

The **All / Videos / Photos** switch is ANDed with the other filters. It is a *structural* filter: changing it
changes which grid rows exist, unlike the name filter's cheap hide/show.

**Photo cards** use the decoded photo file directly as the image strip — no preview cache; an unloadable path (e.g. a
referenced photo on an unmounted drive) renders a blank card. Videos read `preview/`, falling back to the real frame
folder and then to a "No preview" placeholder (see [import.md](import.md)). **Every catalog item admitted by the
structural filters gets a row**, whatever shape its backing is in; its card is built only once the row nears the viewport
(see *Grid population* below). Actions reached through a card need it on screen anyway; selection-based actions read
rows, so off-screen items stay reachable.
Per-type gates: double-click opens the system image viewer instead of the built-in player; middle-click (frame viewer)
is not wired; the green "frames extracted" badge is gated to videos; the context menu adapts per type.

## Name filter

The toolbar's item-name substring filter is ANDed with the sidebar's label filter. It is a **view-level
hide/show**, not a structural rebuild: it hides existing rows and renumbers the visible captions without
recreating cards or decoding thumbnails again. The label filter, by contrast, changes which rows exist.

## Sort control

A single **`SortControl`** chip widget bundles the three ordering options (sort field, ascending/descending, "Favorites
first"). It owns its persistence and reports one aggregate change notification; the browser then reorders the existing
rows together with any cards already attached to them.

## Grid population

The grid separates **lightweight rows** from rich `MediaItemWidget` cards. Every item admitted by the
structural filters has a row, preserving native selection, keyboard navigation, sort order, and scroll
geometry without constructing the entire library's widget tree. A card is created only when its row
approaches the viewport.

Changes stop at the narrowest layer that can represent them:

1. A library, label-filter, or media-type change replaces the affected row set and its cards.
2. Card geometry or preview-frame-count changes keep the rows but recreate the cards.
3. Sorting reorders rows together with any cards already attached to them.
4. Name filtering only hides or reveals rows; existing cards remain attached.

Cards are retained once created and are not evicted by scrolling. This avoids reloading thumbnails when the
user revisits an area, at the deliberate cost that a session which traverses the entire library eventually
materializes every card. The grid owns *when* materialization happens; the browser supplies *what* to build
and remains the sole owner of catalog semantics. An attached row widget is the only materialization record —
there is no parallel card registry to synchronize.

The split has three load-bearing invariants:

1. Every row needs its fixed, media-type-specific size before insertion, because layout must work before a
   card exists. Photo and video cards differ in width, so the view cannot assume one uniform item size.
2. A card may be created at any later scroll position. Everything it displays must therefore be derivable
   from the current `Catalog` plus row state; even the visible caption number belongs to the row first.
3. Qt defers icon-view layout, and the persisted startup anchor becomes reliable only after the post-show
   resize turn. Startup therefore restores filters immediately but defers row construction until that turn;
   it then applies the persisted anchor before materializing the first cards. Ordinary in-session rebuilds
   already have stable geometry and preserve the current view state around the row replacement.

Grid rebuilding queries the already-current in-memory `Catalog`; it never re-derives the model from
persistence or disk. View state is preserved by `MediaId` identity rather than row index or raw scroll
offset, both of which can change as rows are inserted, removed, filtered, or sorted. Scroll position also
persists across restarts; selection is per-session.

## Browser refresh and item-management boundaries

Browser-visible catalog mutations and library replacement arrive through the stable
`Library::catalogChanged` signal. The browser queues and coalesces them into one structural refresh, so a
mutation stack never synchronously destroys its own cards and a compound operation rebuilds only once.
View-only changes use the narrower paths described above. Direct refreshes remain only where files can
change without a corresponding catalog mutation, such as incomplete physical deletion or preview
regeneration.

The context menu and label-drop path share the same multi-selection-aware target calculation. **Remove from
library** drops those records from the catalog only; because the catalog is never recreated from a disk
walk, leftover frame folders remain untracked until an integrity scan or re-import finds them.

Physical Delete and catalog-only Remove confirmations live in **`MediaItemManagement`**
(`src/Windows/MediaItemManagement.h/.cpp`). Catalog record removal reaches the browser through `catalogChanged`; the
delete result carries only effects the blanket model notification cannot express: whether a failed operation may have
changed storage while retaining its catalog record, and which video frame folders may have changed. The browser emits
the latter to synchronize MainWindow's persistent frame viewer.

## Label assignment

Two paths share the same multi-selection-aware target calculation:

1. The context menu exposes a label checklist.
2. An ordinary label row (never "All" or `Best`) can be dragged from the sidebar onto a card. The card is the
   drop target; the label id is the payload (see [media-widgets.md](media-widgets.md)). Applying the drop is
   deferred until the drop event unwinds, because the resulting structural refresh may delete that card.

## LabelSidebar structure (`src/UiComponents/LabelSidebar.h/.cpp`)

A flat **`QListWidget`** (single column) painted by a custom **`LabelRowDelegate`** — also installed on `ImportDialog`'s
label list. The per-row visual state (active tint, spine, hover) cannot be expressed in plain QSS, which is why it's a
delegate. The list is `NoSelection` — filter toggling is handled manually on click.

Row order: **All** → **Best** → a **divider** row → the ordinary labels.

**Auto-hug width**: `ContentWidthListWidget` (a tiny `QListWidget` subclass) overrides `sizeHint` to report the widest
row via the delegate.

## Sidebar label management

Right-clicking an ordinary label row opens a **Rename / Set color / Delete** menu; `LabelSidebar` emits the
corresponding signal carrying the label id. `MediaBrowserWidget` handles these local requests through
**`LabelManagement`** (`src/Windows/LabelManagement.h/.cpp`); successful Catalog mutations trigger the blanket
library notification, so the workflow module returns no refresh status and `MainWindow` is not involved. The module
owns the create/rename/color/delete prompts, confirmations, and error reporting; the `Catalog` API owns all name/path
rules, backing-folder creation, relocation, persistence, and change reporting (see
[catalog-and-labels.md](catalog-and-labels.md)). `ImportDialog` materializes provisional labels through the same module.

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
(`CopyAction`) to Explorer or another app. Catalog access stays out of `MediaGrid` — `MediaBrowserWidget` sets the URL
provider (`dragUrlsForItems`), following the same "browser computes, card draws" split. `MainWindow::dragEnterEvent`
asks the browser whether the event source is its grid, guarding against the export dropping back onto the import handler.

**Empty state**: `MediaGrid` paints a caller-set message whenever **no item is visible** — whether none were built or
the name filter hid them all; the paint checks live visibility, so it needs no hooks in `applyNameFilter`.

---

## Renaming a media item

**`MediaRename`** (`src/Windows/MediaRename.h/.cpp`) is the module — free functions with entry
`MediaRename::renameItemInteractive(id, dialogParent)`, dispatching by media type. `MediaBrowserWidget` calls it from
both the Edit-menu entry point and the card context menu. Catalog notification refreshes the browser on success; the
result retains only the old/new frame-folder payload needed when MainWindow's persistent frame viewer may need
repointing.

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
