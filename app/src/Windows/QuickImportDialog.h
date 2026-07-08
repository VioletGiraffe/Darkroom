#pragma once

#include "UiComponents/DragGestureHelper.h"
#include "Core/MediaId.h"
#include "Import.h"  // Import::PhotoImportMode / PhotoResult, used in the importPhotosRequested callback

#include <QDialog>
#include <QHash>
#include <QStringList>

#include <functional>
#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QWidget;
class MediaItemWidget;

// ============================================================================
// QuickImportDialog - stage new video and photo files, label them, then import everything labeled in one
// "Import" step.
//
// Mirrors the main window's own label model: a small label-list panel (like LabelSidebar, minus
// filtering) sits next to a big grid of staged item cards (MediaItemWidget, same as the main grid).
// Dragging a label from the list onto a staged card tags it, exactly like dragging a label onto an item
// card in the main window; the star button toggles Best the same way too.
//
// The label list holds the Catalog's real labels plus this session's *provisional* labels: dropping a folder
// auto-creates one per folder (named for the folder's relative path, e.g. "Root-cars-2026") and pre-assigns
// its files, and "Create label" makes one too. Provisional labels live only in this dialog - right-click to
// rename/recolor/delete them - and are created in the Catalog for real only when "Import" materializes the ones
// actually used (see runImport); a real label can't be edited from here. Nothing is written to the
// Catalog until "Import" runs: every staged item that's been labeled gets imported and removed from
// staging on success, or stays staged (with its labels intact) on failure or a deferred (Cancel) relocation
// collision; a collision resolved as Skip / "Skip and Delete" also clears the entry - the destination copy
// stands in for it. Unlabeled items are left alone either way. Files or folders can also be dropped straight
// onto the dialog to stage them (a folder is scanned recursively for the supported files under it).
// ============================================================================

class QuickImportDialog final : public QDialog
{
public:
	// An ordinary (non-virtual) label, for the label-list panel.
	struct LabelOption
	{
		QString id;
		QString displayName;
		QString color;  // hex string e.g. "#378ADD"; empty = unset - mirrors Catalog::Label::color
		bool provisional = false;  // true = minted in this dialog (id "new:<n>"), not yet in the Catalog; created on Import
	};

	// One staged item's extra-label picks (every pending label beyond the destination-deciding first one).
	// Identified by MediaId, not path: relocation (Move) deletes the source from its staged path before this
	// is applied, so the path can no longer be resolved to the file - the stable id is what addLabel needs.
	struct ExtraLabelAssignment
	{
		MediaId mediaId;
		QStringList labelIds;
	};

	struct Callbacks
	{
		// Every ordinary label, for the label-list panel. A staged item's first-dropped label also
		// decides which one of these folders it's imported into - see runImport() in the .cpp.
		std::function<std::vector<LabelOption>()> getLabelOptions;
		// Adds the given video files to the named collection. stagedPreviewDirs maps each staged video's MediaId
		// to the temp dir whose preview/ holds the frames already extracted for its staging card, so import
		// can reuse them by copy instead of re-running ffmpeg (see Import::importVideo); a video absent
		// from the map, or whose staged frames are gone, is extracted fresh. stagedDurations likewise carries
		// the duration already probed for each video during staging, so import records it without re-probing.
		std::function<void(const QString& collectionName, const QStringList& videoPaths,
			const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations)> addMediaItemsRequested;
		// Imports the given photos under the label (owned modes copy/move each file into the label's photo
		// dir, Reference tracks them in place - see Import::importPhoto). Returns one result per path, in
		// order; the host reports Error results itself, so the dialog only branches on the status (an
		// IdCollision in Reference mode gets the "import an owned copy instead?" escape hatch). A result's
		// registeredId is the identity actually registered - an owned-import auto-rename changes it from the
		// staged id, so all post-import bookkeeping (Best, extra labels) must use it.
		std::function<std::vector<Import::PhotoResult>(const QString& labelId, const QStringList& photoPaths,
			Import::PhotoImportMode mode)> importPhotosRequested;
		// The source path of an already-imported photo with byte-identical content (matched by size, any
		// name - catches renamed duplicates), or empty if none. Checked when staging a photo, so a duplicate
		// is flagged before it can be labeled and imported.
		std::function<QString(const QString& photoPath)> findAlreadyImportedDuplicatePhoto;
		// Materializes one provisional label at Import time (called per used provisional from runImport): ensures a
		// label with this name exists in the catalog and returns its id - which is what the dialog then rewrites
		// the staged items' pending picks to, replacing the provisional stand-in id. The color applies only when
		// the label is genuinely new; an existing same-name label keeps its own. Empty return = creation refused
		// (reserved/invalid name), and the affected picks are dropped rather than remapped.
		std::function<QString(const QString& name, const QString& color)> createCollectionRequested;
		// A fresh random label color ("#rrggbb"), matching what the Catalog assigns a new label - gives a
		// provisional (folder-derived or manually added) label a swatch before Import creates it for real.
		std::function<QString()> generateLabelColor;
		// True iff the item is now tracked by the Catalog at the frame folder this import derives for it
		// (<collection>/<source base name>) - checked right after addMediaItemsRequested for each item in its
		// batch, purely so runImport() knows which staged entries to clear (a successfully-added one) versus
		// leave staged (declined overwrite, a collision, etc.). Deliberately not "tracked under *some* folder":
		// on a name+size collision the id is already tracked elsewhere and import refused the staged copy -
		// a mere "is tracked" check would misreport that as a success, unstaging the entry and silently
		// dropping its pending labels. By MediaId, not path: Move relocation has already deleted the source
		// from its staged path by now.
		std::function<bool(const MediaId&, const QString& collectionName)> isMediaItemTrackedInCollection;
		// Called once per successful Import, with the items flagged "Best" (may be empty). The flagged items
		// have already been added - and are therefore tracked - via their type's apply path above. By MediaId
		// for the same reason as ExtraLabelAssignment above.
		std::function<void(const std::vector<MediaId>& bestItems)> markBestRequested;
		// Called once per successful Import, with every imported item's extra-label picks (may be empty).
		// Mirrors markBestRequested above: the items have already been added via their type's apply path.
		std::function<void(const std::vector<ExtraLabelAssignment>& assignments)> assignExtraLabelsRequested;
		// Called once at the end of an Import that imported at least one item, after the Best/extra-label flush
		// above. addMediaItemsRequested may refresh the host view mid-Import (it imports folder-by-folder), but the
		// post-import label flush has no refresh of its own - so without this, the host shows each item's
		// folder label but not its extra labels/Best until the dialog is closed. The dialog stays open after
		// an Import, so that gap is visible. Lets the host repaint once with the fully-applied state.
		std::function<void()> viewChanged;
	};

	// suggestedRelocateFolder pre-fills the relocation destination on first use (when
	// nothing's been persisted yet); see "Source file relocation row" in the .cpp.
	QuickImportDialog(Callbacks callbacks, const QString& suggestedRelocateFolder, QWidget* parent = nullptr);
	~QuickImportDialog() override;

	// Pre-populates the staging area with the given files and/or folders (a folder is scanned recursively for
	// the supported media under it). Used when handing paths over from the main window's drop or the "Scan for
	// untracked files" tool. Deferred until the dialog is shown so it paints before the blocking thumbnail extraction.
	void addToStaging(const QStringList& paths);

protected:
	// Accepts files and folders dropped anywhere on the dialog (mirrors MainWindow's own top-level drop
	// handling), staging them directly - same entry point as addToStaging (a folder is expanded to its
	// supported files).
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	// Drives the label list's drag-out gesture (mirrors LabelSidebar::eventFilter), installed on its viewport.
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void refreshLabelList();
	// Extracts temp preview frames for each path and adds it to the staged grid.
	void stageMediaItems(const QStringList& paths);
	// Builds and wires one staged card for the given identity/file (the caller inserts it into the grid). Per-type
	// differences (canvas size, preview images) are derived from the path here; durationMs is passed in since it
	// isn't derivable from the path (it's probed during staging). Shared by stageMediaItems and renameStagedItem.
	[[nodiscard]] MediaItemWidget* buildStagedCard(const MediaId& id, const QString& path, const QString& tempPreviewDir, qint64 durationMs);
	// Deletes a staged entry's temp preview dir and removes its card; used by "Remove from staging" and
	// once an entry has been successfully imported by runImport().
	void unstage(const MediaId& id);
	// Re-derives a staged card's label dots from its pendingLabelIds.
	void updateCardLabelDots(const MediaId& id);
	// The staged items a card-targeted action applies to: the whole staged selection when `id` is part of a
	// multi-selection, otherwise just `id`. Mirrors MainWindow::effectiveSelection for the staged grid.
	[[nodiscard]] std::vector<MediaId> stagedSelection(const MediaId& id) const;
	void showStagedCardContextMenu(const MediaId& id, const QPoint& globalPos);
	// Staged-card actions, mirroring MainWindow's media-item context menu but adapted for untracked items. The
	// single-id ones act on the right-clicked card; the vector ones act on the effective staged selection.
	void previewStagedItem(const MediaId& id);                       // open a photo in the system viewer / play a video
	void locateStagedSourceFile(const MediaId& id);                  // reveal the source file in the file manager
	void copyStagedSourcePath(const MediaId& id);                    // copy the native source path to the clipboard
	void compareStagedPhotos(const std::vector<MediaId>& photoIds);  // open the staged photos in a PhotoCompareWindow
	void setBestForStagedSelection(const std::vector<MediaId>& ids, bool inBest);  // set pendingBest + sync each card's star
	void removeStagedItems(const std::vector<MediaId>& ids);         // drop from staging; no change on disk
	void deleteStagedSourceFiles(const std::vector<MediaId>& ids);   // delete the source files from disk (confirmed), then unstage
	[[nodiscard]] std::vector<MediaId> selectedStagedIds() const;    // ids under the grid's current selection (keyboard accelerators)
	// Renames the staged file on disk and re-keys the entry to the new name-derived MediaId, rebuilding the card in
	// place (same grid item) so its callbacks bind to the new id - preserving the "staged key == current file's
	// MediaId" invariant runImport relies on. The extension is kept fixed so the type stays valid.
	void renameStagedItem(const MediaId& id);
	// Every staged entry whose pendingLabelIds isn't empty: grouped by its first label and imported via
	// addMediaItemsRequested, then markBestRequested/assignExtraLabelsRequested for whatever landed.
	void runImport();
	[[nodiscard]] const LabelOption* findLabelOption(const QString& id) const;

	// --- Provisional labels (folder-derived or "Create label"); all mutate m_provisionalLabels and re-render via
	// refreshLabelList. See QuickImportDialog.cpp for the model. ---------------------------------------------------
	[[nodiscard]] static bool isProvisionalId(const QString& id);
	// The id of the label whose display name matches (case-insensitive), skipping excludeId; empty if none.
	[[nodiscard]] QString findLabelIdByName(const QString& name, const QString& excludeId) const;
	QString addProvisionalLabel(const QString& name);        // unconditionally mints one, returns its id
	QString ensureLabelForFolderName(const QString& name);   // reuse an existing same-name label, else mint provisional
	void showLabelListContextMenu(const QPoint& pos);
	void renameProvisionalLabel(const QString& provisionalId);
	void setProvisionalLabelColor(const QString& provisionalId);
	void deleteProvisionalLabel(const QString& provisionalId);
	// Reassign every staged pick from provisionalId to targetId, then drop the merged provisional (purely local).
	void mergeProvisionalInto(const QString& provisionalId, const QString& targetId);
	void updateAllCardLabelDots();  // re-derive every staged card's dots after a label name/color change
	// Import prologue: create in the Catalog each provisional label a staged item actually uses, then rewrite every
	// staged pick from its provisional stand-in to the real id, so the rest of runImport sees only real labels.
	void materializeUsedProvisionalLabels();

private:
	// Per-staged-item state, accumulated by dragging labels from m_labelList onto the card in m_stagedGrid
	// and toggling its Best star; applied (and removed from here) only once "Import" runs successfully. The
	// first id in pendingLabelIds resolves the item's destination folder at Import time (see runImport) -
	// that's the only place this ordering matters; there's no other state or UI for it.
	struct StagedEntry
	{
		QString path;
		QString tempPreviewDir;      // a video's temp N-frame preview, deleted on unstage/dialog close; empty for a photo (its card decodes the file directly)
		qint64 durationMs = -1;      // a video's source length in ms, probed during staging; -1 for a photo / an unprobed video. Carried into the Catalog at import
		bool pendingBest = false;
		QStringList pendingLabelIds;
		QListWidgetItem* item = nullptr;  // the grid item carrying this entry's MediaItemWidget card
	};

	Callbacks m_callbacks;
	std::vector<LabelOption> m_labelOptions;       // cached list: real labels (getLabelOptions()) + m_provisionalLabels
	std::vector<LabelOption> m_provisionalLabels;  // labels minted in-dialog, not in the Catalog until Import materializes them
	int m_provisionalSeq = 0;                      // mints unique provisional ids ("new:<n>")
	QHash<MediaId, StagedEntry> m_staged;

	QListWidget* m_labelList  = nullptr;
	QListWidget* m_stagedGrid = nullptr;

	// Drag-out state for m_labelList, mirroring LabelSidebar's own drag-a-row gesture.
	DragGestureHelper m_labelDragHelper;
	QListWidgetItem*  m_labelPressedItem = nullptr;

	// Source-file relocation controls - see "performRelocation" in QuickImportDialog.cpp.
	QComboBox* m_relocateModeCombo = nullptr;
	QLineEdit* m_relocateFolderEdit = nullptr;
	QSplitter* m_splitter = nullptr;
};
