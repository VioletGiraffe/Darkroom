# Main window

[← Back to architecture index](../../ARCHITECTURE.md)

`MainWindow` owns the stable `Library`, the `MediaBrowserWidget`, and a persistent `FrameViewerWindow`.
`MediaBrowserWidget` owns the browser toolbar, label sidebar, media grid, view settings, filtering, sorting, card
construction, selection, and item workflows. It borrows `Library&` for life.

Signals between them cross genuine ownership boundaries: MainWindow maintains shell action state, satisfies
on-demand frame extraction before presenting its viewer, and keeps that persistent viewer coherent when the browser
renames or removes its current item.

## Library lifetime and switching

Startup loads a library before constructing the browser because the browser and sidebar retain `Library&`. First-run
selection, normal startup, and later switching all converge on `Library::setRoot`; there is no separate startup
loader. Cancelling the first-run choice leaves the window unbuilt.

`setRoot` fully loads a candidate before publishing it and first flushes the current library, so save failure blocks
replacement. After a successful switch MainWindow closes player windows, clears the persistent frame viewer, and asks
the browser to discard library-specific rows and filters. The `Library` object itself remains stable, so borrowers
never rebind.

Library switching and settings changes are refused while a shell-owned long-running workflow pumps events. The
application-modal Import dialog provides the equivalent exclusion for import-owned interactive work.

Create and Open use the same switch transaction. Creation differs only by refusing a folder that already holds a
library; `Library::setRoot` itself accepts an absent root and absent persistence files as a new library.

### Recent libraries

Recent entries are normalized root strings and deliberately perform no filesystem queries while the menu opens. A
library may live on a disconnected drive or network share, where probing existence can block the UI; stale entries
therefore remain visible until the normal switch attempt reports failure. Reconnecting the storage makes the same
entry usable again.

The menu rebuilds from settings when shown. `recordCurrentLibrary` is the single writer of both the current-root and
recent-root settings, keeping startup and later switches consistent.

Library save failures are reported through MainWindow. Dirty state remains in memory for retry, and closing with
unflushed changes requires an explicit discard decision.

## Browser update layers

The browser uses the narrowest update that represents a change:

1. Library replacement, label filtering, and media-type filtering replace the structural row set.
2. Card geometry or preview-count changes preserve rows but recreate attached cards.
3. Sorting reorders existing rows together with their attached cards.
4. Name filtering only hides or reveals rows.

The All/Videos/Photos filter is combined with label and name filtering. Photo and video activation differs, but every
catalog item admitted by the structural filters receives a row even when its source or preview is unavailable. This
keeps selection-based recovery and management actions reachable.

Catalog mutations and library replacement arrive through `Library::catalogChanged`. The browser queues and
coalesces this signal into one structural refresh, avoiding synchronous destruction of a card inside the mutation
stack. Direct refreshes remain only for disk changes that may occur without a catalog mutation, such as incomplete
physical deletion or preview regeneration.

## Lazy grid population

The grid separates lightweight rows from rich `MediaItemWidget` cards. All structurally admitted items receive rows
immediately, preserving layout, selection, keyboard navigation, sort order, and scroll geometry. Cards are
materialized only near the viewport and retained once built; revisiting an area avoids thumbnail reloads, while a
session that traverses the entire library may eventually materialize every card.

The split depends on three invariants:

1. Each row has its media-type-specific size before a card exists.
2. Everything a later-built card displays is derivable from current Catalog and row state.
3. View restoration uses `MediaId`, not row index or raw scroll offset, because filtering and sorting change both.

Qt defers icon-view layout, so startup restores filters first and constructs rows after the post-show geometry turn;
only then can it restore the persisted anchor and materialize initial cards. In-session structural rebuilds preserve
the current identity-based view state around row replacement.

Grid population queries the live Catalog model and never reloads persistence or scans disk.

## Media grid & multi-select

`MediaGrid` is a `QListWidget` in IconMode with native extended selection. This combination provides the required
wrapping layout, selection model, keyboard navigation, drag-and-drop, and focus behavior. Replacing it with a plain
flow layout would require explicit equivalents for all of those capabilities.

Enabling drag on the item view preserves a multi-selection through press-and-move, allowing the whole selection to
export together. `MediaGrid::startDrag` exports source-file URLs; Catalog knowledge remains in
`MediaBrowserWidget`, which supplies the URL provider. MainWindow rejects drags originating from this grid in its
import drop handler, preventing an export from feeding back into ImportDialog.

## Item and label workflows

Card context actions and label drops use the same multi-selection-aware target calculation. Label assignment mutates
Catalog by stable label id. A drop is deferred until the event unwinds because the resulting structural refresh may
delete the receiving card.

`LabelSidebar` presents the virtual filters and ordinary labels; `LabelManagement` owns create, rename, color, and
delete interaction. Catalog owns validation, backing paths, relocation, persistence, and change publication. The
Import dialog reuses the same label-creation workflow when materializing provisionals.

`MediaItemManagement` owns confirmation and execution for physical Delete and catalog-only Remove. User-requested
physical deletion moves files and frame folders to the platform Trash first. A Trash failure reports Qt's available
system diagnostic and offers an explicit, default-cancelled permanent-deletion fallback for that path.
For videos, the source is attempted only after the frame folder is gone. Catalog records are removed only after all
of an item's required paths are gone. Catalog removal refreshes the browser through `catalogChanged`; workflow
results carry only effects that blanket invalidation cannot express, notably frame-folder changes needed to
synchronize MainWindow's persistent viewer. Remove from library leaves disk content untracked by design.

## Renaming media

`MediaRename` owns interactive video and photo rename transactions. The caller addresses the item by `MediaId`;
successful Catalog publication refreshes the browser, while the result carries old/new frame-folder information for
the persistent frame viewer.

A video rename changes the source file, then its frame folder, then re-keys the metadata through
`Catalog::applyRename`. If the new identity collides or Catalog publication fails, both filesystem changes are
rolled back. This is the only video frame-folder move outside Catalog's label-reconciliation paths.

A photo rename changes only the file's base name in place, preserving directory and extension, then re-keys through
the same Catalog operation. Missing sources are refused.

## Logging and diagnostics

`main()` installs the Qt message handler before constructing `QApplication`, capturing early platform and plugin
diagnostics while chaining to the previous handler so normal stderr output remains. The shared
`CLoggerInMemory` sink is thread-safe and surfaced by MainWindow's log viewer. It is memory-only and currently
unbounded.
