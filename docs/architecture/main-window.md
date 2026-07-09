# Main window

[← Back to architecture index](../../ARCHITECTURE.md)

`MainWindow.h/.cpp`: a left **`LabelSidebar`** (label filter) and a toolbar of view controls above a
`QListWidget` grid of `MediaItemWidget` cards, in a horizontal `QSplitter`. The card set is the sidebar's
label filter ∩ the media-type switch ∩ the name-filter box.

The window owns the sidebar, the toolbar controls (name filter, media-type switch, frames-per-card density
combo, sort control), the card grid, and a persistent `FrameViewerWindow` (reused, not recreated).

## Media-type switch

`m_mediaTypeFilter` (a `SegmentedToggle`, right of the name filter): **All / Videos / Photos**, ANDed with
the other filters. A *structural* filter — changing it rebuilds the grid (`refreshMediaGrid` post-filters
the enumerated ids by `Catalog::mediaType`), unlike the name filter's cheap hide/show. Persisted as its
segment index under a MainWindow-local settings key; restored silently during setup (`setCurrentIndex`
doesn't emit).

**Photo cards** in the grid: the card's image strip is the decoded photo file itself
(`Catalog::sourcePathForMediaItem`), no preview cache — an unloadable path (e.g. a referenced photo on an
unmounted drive) renders a blank card rather than hiding the item (accepted v1 behavior). A photo's display
name comes from its id's file name (`itemInfoFor`), since no frame folder names it; its date sort uses the
same `getSourceFileDate` (file-name timestamp, then birth time). **Per-type gates**: a photo card's
double-click opens the file in the system image viewer (`openSourceInSystemApp`) instead of the built-in
player; middle-click (frame viewer) is not wired on photo cards — a photo has no frames; the green "frames
extracted" badge never shows (it's gated to videos — a photo reports `isSplitIntoFrames() == true` but has no
frames); the context menu adapts per type — see
`showMediaItemContextMenu` below.

## Name filter

`m_nameFilter` (toolbar `QLineEdit`, left of the view controls): an item-name substring filter ANDed with
the sidebar's label filter, via the `nameMatchesFilter` helper — `^` prefix anchors to the start of the
name, otherwise contains-anywhere, always case-insensitive, empty = no filtering (this syntax was carried
forward from the retired `SearchDialog`). It's a **view-level hide/show**, not a rebuild: `textChanged` runs
`applyNameFilter`, which only toggles each card's `setHidden` and renumbers the visible ones — no grid
rebuild and no thumbnail re-decode, so it stays cheap on every keystroke. (The label filter, by contrast,
*does* drive which cards get built, in `refreshMediaGrid`.) An earlier implementation post-filtered via
`folders.removeIf(...)` inside `refreshMediaGrid` itself, which meant every keystroke triggered a full grid
rebuild plus thumbnail re-decode — caught in code review and fixed by moving to the hide/show model instead
of debouncing the heavy path. **Ctrl+F** raises/activates the window and focuses + selects the box (an
app-wide `QShortcut`).

## Sort control (`src/UiComponents/SortControl.h/.cpp`)

The toolbar's three former ordering widgets (sort-by combo, ascending/descending button, "Favorites first"
checkbox) are collapsed into one **`SortControl`** — a chip-style `QPushButton` whose face shows the current
field and direction (e.g. `Date  ↓`). Clicking opens a **styled popover** (the mockup's non-stock
vocabulary): a frameless `Qt::Popup` top-level (auto-dismisses on an outside click) with a translucent
gutter around a shadowed rounded card, holding two **`SegmentedToggle`s** (field Name/Date, direction
↑Ascending/↓Descending) and a small custom-painted favorites-first checkbox. It sits **rightmost** in the
toolbar (after the frames-per-card combo), matching the mockup; the popover right-aligns under it.

`SortControl` **owns its own persistence** — it reads/writes its own settings (sort field, direction,
favorites-first), moved here out of `MainWindow`, and emits a single **`changed()`** signal whenever the user
adjusts any option. `MainWindow` connects that to `resortMediaGrid()` and reads the state back through the
control's getters. The **sort logic itself is unchanged** — the same grid ordering drives it; only the
controls that feed it moved. The popover and its favorites checkbox are file-local; the checkbox is
hand-painted because the central QSS can't render a checkmark glyph without an image — revisit if the broader
restyle needs an app-wide styled `QCheckBox`.

## Key methods

- `refreshMediaGrid()` — clears and rebuilds `m_mediaGrid`, one `GridItem` (a `QListWidgetItem` carrying the
  card's `MediaId`) per shown item. Reads `cardImageHeight()` and `previewFrameCount` each rebuild, builds
  the card set from `Catalog::mediaItems()` filtered by the sidebar's **label** filter
  (`Catalog::mediaItemsForLabel`), then ends with `applyNameFilter()` so the active name filter is honoured on the
  freshly built cards. Deliberately does **not** call `Catalog::rebuildIndex()` — see
  [catalog-and-labels.md](catalog-and-labels.md#in-memory-model) for why.
- `applyNameFilter()` — hides cards not matching the name box (`setHidden`, deselecting any it hides) and
  renumbers the visible ones; the cheap per-keystroke path. `renumberGridCaptions` numbers only visible
  cards.
- `showMediaItemContextMenu()` — multi-select-aware right-click menu. **Per-type entries**: Inspect /
  "Compare selected" (`CompareWindow` is frame-folder based) is offered only when nothing in the selection is
  a photo; "Compare photos" (`PhotoCompareWindow` — see [playback.md](playback.md)) is offered for an
  all-photo selection of 2–4, fed the source file paths (missing files filtered out, <2 remaining → warning);
  the same window also opens empty from Tools → "Compare photos..." and is populated by dropping image files;
  "Open in Explorer" (the item's folder) and "Rename media file" are videos-only — a photo's
  `folderForMediaItem` is the shared `Photos/<label>` dir, or nothing when referenced; photos get "Open
  photo" where videos get "Play source video" (both `openSourceInSystemApp`; a video's *double-click* opens
  the built-in player instead). **"Delete" is per-type** (Shift+Del): a video loses its frame folder +
  source file; a photo loses its file (never the shared label dir) — the confirmation message spells out
  exactly what applies to the selection at hand. **"Remove from library"** (Del) drops the selection from
  the catalog only (`Catalog::removeMediaItem`) — no file is touched, but the items' catalog metadata
  (labels incl. Best, saved loops) is discarded; since the catalog is never re-derived from a disk walk,
  an untracked video's frame folder simply stays on disk, surfaced again only by the integrity tool (or a
  re-import of the source). **"Rename"** (F2) renames a single video's frame folder + source file (videos
  only). All three live in the **Edit** main menu (with shortcuts) and are reused in the context menu with
  count-variant text; their shortcuts are scoped to the grid (`WidgetWithChildrenShortcut`) so they don't
  fire while typing in the name filter. Includes a **Labels** submenu: a checklist
  of every ordinary label (check state from the right-clicked card via `Catalog::mediaItemHasLabel`); toggling
  one drives the whole effective selection to that state via `Catalog::addLabel`/`removeLabel`, then
  `refreshLibraryView()`.
- `effectiveSelection(id)` — takes `std::optional<MediaId>`: with an id, returns the multi-selection if the
  id is in it, else just `{id}`; with nullopt, returns the raw grid selection (may be empty). The keyboard
  shortcut path passes nullopt, the context menu path passes the right-clicked card's id (stored in
  `m_contextMenuTarget` for the duration of `menu.exec()`). **Shared** by the context menu, the Edit menu
  actions, and the label-drop handler.
- `refreshLibraryView()` — `refreshMediaGrid()` **then** `m_labelSidebar->refresh()` (reads label counts from
  the now-current model); the order matters, the sidebar would read stale counts if reversed. **Never call
  `m_labelSidebar->refresh()` from inside `refreshMediaGrid()` itself**: the sidebar's `itemClicked` emits
  `filterChanged` → `refreshMediaGrid`, so a refresh from in there would `clear()`+rebuild the sidebar's
  `QListWidget` from within its own click-signal chain — use-after-free on the item/view that's still on the
  call stack. This is why structural changes (add/delete/rename an item, create a label) call
  `refreshLibraryView`, while filter/sort/zoom changes call plain `refreshMediaGrid`. Direct consequence:
  toggling Best leaves the sidebar's Best count stale by one until the next structural refresh — accepted,
  not a bug to "fix" by refreshing the sidebar there.

## Label assignment

Two paths, both add-only (except the context-menu checklist, which also removes) and both routed through
`effectiveSelection`:
- **Context-menu Labels checklist** — see `showMediaItemContextMenu` above.
- **Dragging a label row from `LabelSidebar` onto a card.** The sidebar is the drag source — its list
  viewport carries a `ListRowDragFilter` (the shared list-row drag gesture in `DragGestureHelper.h/.cpp`) whose
  MIME factory drags ordinary label rows only (returning null for "All" and the virtual `Best`), carrying the
  label id as `LabelMimeType` (`src/UiComponents/LabelMimeType.h`); the card is the
  drop target (see [media-widgets.md](media-widgets.md)). `addCard` wires each card's `setOnLabelDropped`
  to add the label across the effective selection, **deferred via a queued `invokeMethod`** so the grid
  rebuild doesn't delete the card mid-drop (the rebuild happens inside the handler, which runs from the
  card's own `dropEvent`).

## LabelSidebar structure (`src/UiComponents/LabelSidebar.h/.cpp`)

A flat **`QListWidget`** (single column) whose rows are painted by a custom **`LabelRowDelegate`** — the
non-stock look from the design mockup: a leading color dot, the name, a right-aligned count, and per-row
state drawn by the delegate (active rows get a `BackgroundSecondary`-filled pill — the theme's hue-shifted
raised-row surface — **plus a left accent bar tinted to that label's own colour**; hovered rows a dashed
outline in a translucent text overlay). That per-row accent colour is exactly what
plain QSS can't express, which is why it's a delegate. Row data rides on item roles; active state is a role
flag (repainted on toggle), **not** a selection (the list is `NoSelection` — we toggle the filter on click
ourselves).

Row order: **All** (no dot) → **Best** (gold dot **and** a gold `★`, via `kStarRole`) → a **divider** row
(`kDividerRole`, `NoItemFlags`, painted as a hairline, `sizeHint` width 0 so it never widens the panel,
skipped in `applyRowHighlight`/`onItemClicked`) → the ordinary labels. Best is pulled from
`Catalog::allLabels()` by `Catalog::BestLabelId` and pinned — this relies on Best being present in that list.

**Auto-hug width**: `ContentWidthListWidget` (a tiny `QListWidget` subclass) overrides
`sizeHint`/`minimumSizeHint` to report `sizeHintForColumn(0)` (the widest row, via the delegate) + frame +
scrollbar, so the splitter sizes the panel to hug its content; the row's generous breathing-room padding is
folded into the delegate's `sizeHint`. (This replaced an equivalent `QTreeWidget` subclass when the sidebar
migrated from a 2-column tree to the single-column delegate-painted list — the delegate subsumes the old
name/count column behaviour.)

**OR/AND combine mode** is a **`SegmentedToggle`** (reusable custom widget — see
[settings-and-theme.md](settings-and-theme.md)): `isAndMode()` == segment 1; `setActiveFilter` restores it
silently via `setCurrentIndex`. **"Create label"** is a `QPushButton` with object name `addLabelButton`,
styled dashed/transparent (and left-aligned, with a leading `+` glyph) by the central app sheet.

## Sidebar label management

Right-clicking an ordinary label row (not "All" / `Best`) opens a menu — **Rename / Set color / Delete** —
and `LabelSidebar` emits `renameLabelRequested` / `setLabelColorRequested` / `deleteLabelRequested` carrying
the label id. `MainWindow` handles each (mirroring `addLabelRequested → createLabelInteractive`):
`renameLabelInteractive` (`QInputDialog` → `Catalog::renameLabel`), `setLabelColorInteractive`
(`QColorDialog` → `setColor`), and `deleteLabelInteractive` (reads `deleteLabelImpact` to refuse orphaning
up front and to show relocate/untag counts in a confirm dialog, then `deleteLabel`). All finish with
`refreshLibraryView()`. See [catalog-and-labels.md](catalog-and-labels.md) for the underlying `Catalog` API.

## Card interactions

Middle-click → `FrameViewerWindow::showForFolder()` (videos only — not wired on photo cards); double-click →
built-in player for a video, system image viewer for a photo; right-click → context menu;
Ctrl/Shift-click and rubber-band → multi-select; left-drag → export the source file(s) out to Explorer/another
app (see **Media grid & multi-select** below). (Single+double click on the same button proved unreliable;
middle-click is the deliberate choice for "show frames".)

---

## Media grid & multi-select

The grid is a `QListWidget` in **IconMode** (`setFlow(LeftToRight)`, `setWrapping(true)`,
`setResizeMode(Adjust)`) — gives a wrapping-grid shape while keeping native `ExtendedSelection`.

**History worth knowing**: it was originally a vertical `QListWidget` (free multi-select), then migrated to
a `CFlowLayout` grid for the wrapping shape — which **silently dropped multi-select/Compare** (lost for
several sessions before being caught). The current IconMode `QListWidget` restores both.

> **Lesson encoded**: when swapping a container for shape, list what the old one gave for free (selection
> model, keyboard nav, drag-and-drop, focus handling) and check each still has an equivalent before calling
> the swap done — or say up front that it doesn't, rather than letting the regression surface later.

`CFlowLayout` (`qtutils`) is now used **only** by `FrameViewerWindow`, which needs a plain non-selectable
flow grid.

**Why multi-select works despite embedded card widgets**: `ThumbnailWidget`'s mouse handlers fall through
to `QWidget::` base (leaving the event *ignored*) whenever it has nothing to do with them; Qt then propagates
the ignored event up to the view, which does click-to-select/rubber-band — or, once the pointer passes the
drag threshold, starts the card's file-export drag itself (see **Dragging cards out**, below).

**Keeping a multi-selection through a press**: a plain (no-modifier) press on an already-multi-selected item
would otherwise collapse the selection to that one item before a drag could start (so a group could never be
dragged). `setDragEnabled(true)` on the grid is what prevents it: with a drag possible, `QAbstractItemView`
**defers** the "collapse to the clicked item" to *release* rather than doing it on press, so the
multi-selection survives the press+move and the whole group drags together. Deliberately not implemented:
Explorer's release-without-drag collapse-to-single nuance.

**Dragging cards out (file export)**: the grid is a **`MediaGrid`** (`src/UiComponents/MediaGrid.h/.cpp`), a
`QListWidget` subclass whose `startDrag` exports the current selection's **source files** as `file://` URLs
(`CopyAction` — never a move/delete of the original) to Explorer or another app. It maps items → URLs through
a provider `MainWindow` sets (`dragUrlsForItems`, which skips missing files), so catalog access stays out of
the view — the same "MainWindow computes, card draws" split the cards use. The drag image is the grabbed card
(a count badge when >1 file goes along). The view has already resolved *which* cards via the normal selection
(press-on-unselected selects just it; press-within-a-group keeps the group), so `startDrag` just reads
`selectedItems()`. Since those URLs are "supported files", `MainWindow::dragEnterEvent` guards against the
export dropping back onto the window's own import handler (`event->source() == m_mediaGrid` → ignored) — the
import drop zone is for files from outside the app, not a card re-import of an already-tracked item.

**Empty state**: `MediaGrid` paints a caller-set message (`setEmptyMessage`, `InstructionText`-colored,
centered over the viewport) whenever **no item is visible** — whether none were built or the name filter hid
them all; the paint checks live visibility, so it needs no hooks in `applyNameFilter`. The wording comes from
`refreshMediaGrid` following the same computes/draws split: `Catalog::mediaItemCount() == 0` means the
library itself is empty (message points at drag-drop and the Import dialog), anything else means the filters
matched nothing.

**Selection highlight**: the `QListWidget::item:selected` background rule (`Theme::ListSelectedBg`) shows
through because `MediaItemWidget` now paints a **transparent** normal background (it owns only a rounded
border + hover fill). The opaque matte thumbnail covers its own area, so a selected card shows the highlight
around the thumbnail (footer + margins) rather than behind it — accepted for now; a fully-tinted selected
card would need the card to draw the selection itself (a `setSelected` state) instead of relying on the
list's background.

---

## Renaming a media item

The interactive rename flows live in their own **`MediaRename`** module (`src/Windows/MediaRename.h/.cpp`) —
free functions with a single public entry, `MediaRename::renameItemInteractive(id, dialogParent)`, which
dispatches by media type and returns a `MediaRename::Result { renamed, oldFolderPath, newFolderPath }`.
`MainWindow::renameItemInteractive` is a thin wrapper over it: on success it does the host-side UI fixups the
module deliberately leaves out — `refreshLibraryView()` and, when the frame viewer was showing the renamed
video's `oldFolderPath`, repointing it to `newFolderPath`. Both the Edit-menu **Rename** action and the card
context menu go through that wrapper.

Internally, `renameVideo(oldId, newFolderPath)` is the one place a video's frame folder moves on disk outside the
label-mutation paths in `Catalog` (which handle their own folder moves internally — see
`renameLabel`/`relocateFolderOffLabel` in [catalog-and-labels.md](catalog-and-labels.md)):

1. Renames the source file (if one is tracked and present) via `QFile::rename`.
2. Renames the frame folder via `QFile::rename(old, new)` with absolute paths (works across parent dirs,
   unlike `QDir(parent).rename`).
3. `Catalog::applyRename(oldId, newId, newSourcePath, newFolderAbs)` updates the model and the
   persisted record to match (see [catalog-and-labels.md](catalog-and-labels.md)). `MediaId` is filename +
   size (see [data-model.md](data-model.md)), so renaming the source file changes it — `applyRename` re-keys
   the whole `MetadataStore` record (loop intervals, labels incl. Best) from `oldId` to the new id; without
   it, that metadata would be silently orphaned under the old `MediaId`. If the source file kept its name
   (only the frame folder moved by itself), `newId == oldId` and this is just a folder/path update.
   `applyRename` refuses (returns false) when the new id collides with another tracked item — the
   interactive pre-checks only catch a file/folder collision in the *same* directory, not a name+size match
   elsewhere in the library — and `renameVideo` then renames the file and folder back, so the refusal leaves
   disk and catalog exactly as they were.
4. Returns success with the old/new folder paths in its `Result`; the `refreshLibraryView()` and
   `FrameViewerWindow` repoint happen in `MainWindow`'s wrapper (above), not in the module.

`renameVideoInteractive(id)` is the video UI-prompt wrapper (validate chars, check collisions, confirm) →
`renameVideo(...)`; an assert enforces it is only reached for videos. The module's `renameItemInteractive`
dispatcher sends videos here and photos to `renamePhotoInteractive`.

Photos rename differently because a photo *is* its file (no frame folder): `renamePhotoInteractive` →
`renamePhoto` rename only the file's base name in place, keeping its directory (an owned photo's shared
`Photos/<label>` dir, or a referenced photo's external folder — the referenced file is renamed in place) and
extension, then re-key identity through the same `Catalog::applyRename`. A missing source file is refused (a
photo with no file has nothing to rename). The context-menu label is "Rename photo" for photos, "Rename media
file" for videos.
