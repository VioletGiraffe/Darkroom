# Main window

[← Back to architecture index](../../ARCHITECTURE.md)

`MainWindow.h/.cpp`: a left **`LabelSidebar`** (label filter) and a toolbar of view controls above a
`QListWidget` grid of `MediaItemWidget` cards, in a horizontal `QSplitter`. The card set is the sidebar's
label filter ∩ the name-filter box.

The window owns the sidebar, the toolbar controls (name filter, frames-per-card density combo, sort control),
the card grid, and a persistent `FrameViewerWindow` (reused, not recreated).

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
  the card set from `Catalog::allMediaItems()` filtered by the sidebar's **label** filter
  (`Catalog::mediaItemsForLabel`), then ends with `applyNameFilter()` so the active name filter is honoured on the
  freshly built cards. Deliberately does **not** call `Catalog::rebuildIndex()` — see
  [catalog-and-labels.md](catalog-and-labels.md#in-memory-model) for why.
- `applyNameFilter()` — hides cards not matching the name box (`setHidden`, deselecting any it hides) and
  renumbers the visible ones; the cheap per-keystroke path. `renumberGridCaptions` numbers only visible
  cards.
- `showMediaItemContextMenu()` — multi-select-aware right-click menu. Includes a **Labels** submenu: a checklist
  of every ordinary label (check state from the right-clicked card via `Catalog::mediaItemHasLabel`); toggling
  one drives the whole effective selection to that state via `Catalog::addLabel`/`removeLabel`, then
  `refreshLibraryView()`.
- `effectiveSelection(id)` — if `id` (a `MediaId`) is in the current multi-selection, returns every selected
  card's id, else just `{id}`. **Shared** by the context menu (Inspect vs "Compare selected") and the
  label-drop handler (drop on a card in the selection assigns the whole selection).
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
  viewport is event-filtered to start a `DragGestureHelper` drag for ordinary label rows only (not "All" or
  the virtual `Best`), carrying the label id as `LabelMimeType` (`src/UiComponents/LabelMimeType.h`); the card is the
  drop target (see [media-widgets.md](media-widgets.md)). `addCard` wires each card's `setOnLabelDropped`
  to add the label across the effective selection, **deferred via a queued `invokeMethod`** so the grid
  rebuild doesn't delete the card mid-drop (the rebuild happens inside the handler, which runs from the
  card's own `dropEvent`).

## LabelSidebar structure (`src/UiComponents/LabelSidebar.h/.cpp`)

A flat **`QListWidget`** (single column) whose rows are painted by a custom **`LabelRowDelegate`** — the
non-stock look from the design mockup: a leading color dot, the name, a right-aligned count, and per-row
state drawn by the delegate (active rows get a soft `Theme`-text-overlay pill **plus a left accent bar tinted
to that label's own colour**; hovered rows a dashed outline). That per-row accent colour is exactly what
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
silently via `setCurrentIndex`. **"+ Add label"** is a `QPushButton` with object name `addLabelButton`,
styled dashed/transparent by the central app sheet.

## Sidebar label management

Right-clicking an ordinary label row (not "All" / `Best`) opens a menu — **Rename / Set color / Delete** —
and `LabelSidebar` emits `renameLabelRequested` / `setLabelColorRequested` / `deleteLabelRequested` carrying
the label id. `MainWindow` handles each (mirroring `addLabelRequested → createLabelInteractive`):
`renameLabelInteractive` (`QInputDialog` → `Catalog::renameLabel`), `setLabelColorInteractive`
(`QColorDialog` → `setColor`), and `deleteLabelInteractive` (reads `deleteLabelImpact` to refuse orphaning
up front and to show relocate/untag counts in a confirm dialog, then `deleteLabel`). All finish with
`refreshLibraryView()`. See [catalog-and-labels.md](catalog-and-labels.md) for the underlying `Catalog` API.

## Card interactions

Middle-click → `FrameViewerWindow::showForFolder()`; double-click → play video; right-click → context menu;
Ctrl/Shift-click and rubber-band → multi-select. (Single+double click on the same button proved unreliable;
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
to `QWidget::` base (leaving the event *ignored*) whenever no drag starts; Qt then propagates the ignored
event up to the view, which does click-to-select/rubber-band. Drag code only consumes the event once
`tryStartDrag` actually starts a `QDrag`.

**Press-collapses-selection fix**: a plain (no-modifier) press on an already-multi-selected item used to
collapse the selection to that one item before a drag could start (so a group could never be dragged). The
"don't change selection until release" nuance lives in `QAbstractItemView::mousePressEvent`, which only
runs for presses landing on the viewport — ours land on the embedded widget first. Fix:
`ThumbnailWidget::setSelectionGuard` — on a plain left press, if the guard returns true the press is swallowed
(never propagates), preserving the selection. `MediaItemWidget` forwards it; `MainWindow` wires it per-card to
"is this card part of a multi-selection". Deliberately not implemented: Explorer's release-without-drag
collapse-to-single nuance.

**Selection highlight**: the `QListWidget::item:selected` background rule (`Theme::ListSelectedBg`) shows
through because `MediaItemWidget` now paints a **transparent** normal background (it owns only a rounded
border + hover fill). The opaque matte thumbnail covers its own area, so a selected card shows the highlight
around the thumbnail (footer + margins) rather than behind it — accepted for now; a fully-tinted selected
card would need the card to draw the selection itself (a `setSelected` state) instead of relying on the
list's background.

---

## Renaming a media item

`renameMediaItem(oldId, newFolderPath)` is the one place a video's frame folder moves on disk outside the
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
   elsewhere in the library — and `renameMediaItem` then renames the file and folder back, so the refusal leaves
   disk and catalog exactly as they were.
4. `refreshLibraryView()` + updates `FrameViewerWindow` if it's showing the renamed folder.

`renameMediaItemInteractive(id)` is the UI-prompt wrapper (validate chars, check collisions, confirm) →
`renameMediaItem(...)`, reached from the card's context menu ("Rename media file").
