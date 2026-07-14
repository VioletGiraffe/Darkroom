#pragma once

#include "Core/MediaId.h"
#include "Import.h"  // Import::PhotoImportMode / PhotoResult, used in the importPhotosRequested callback
#include "Windows/SourceRelocation.h"  // SourceRelocation::Mode, importVideoGroup's parameter

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
class Library;

// ============================================================================
// ImportDialog - stage new video and photo files, label them, then import everything labeled in one
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

class ImportDialog final : public QDialog
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

	// Host (MainWindow) actions the dialog can't do itself: each either drives host-owned state (the busy
	// guard, progress UI, view refresh) or is shared with other host flows (label creation). Plain
	// Catalog reads and writes are done directly - the dialog makes no attempt to be Catalog-agnostic.
	struct Callbacks
	{
		// Adds the given video files to the named label. stagedPreviewDirs maps each staged video's MediaId
		// to the temp dir whose preview/ holds the frames already extracted for its staging card, so import
		// can reuse them by copy instead of re-running ffmpeg (see Import::importVideo); a video absent
		// from the map, or whose staged frames are gone, is extracted fresh. stagedDurations likewise carries
		// the duration already probed for each video during staging, so import records it without re-probing.
		std::function<void(const QString& labelName, const QStringList& videoPaths,
			const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations)> addMediaItemsRequested;
		// Imports the given photos under the label (owned modes copy/move each file into the label's photo
		// dir, Reference tracks them in place - see Import::importPhoto). Returns one result per path, in
		// order; the host reports Error results itself, so the dialog only branches on the status (an
		// IdCollision in Reference mode gets the "import an owned copy instead?" escape hatch). A result's
		// registeredId is the identity actually registered - an owned-import auto-rename changes it from the
		// staged id, so all post-import bookkeeping (Best, extra labels) must use it.
		std::function<std::vector<Import::PhotoResult>(const QString& labelId, const QStringList& photoPaths,
			Import::PhotoImportMode mode)> importPhotosRequested;
		// Materializes one provisional label at Import time (called per used provisional from runImport): ensures a
		// label with this name exists in the catalog and returns its id - which is what the dialog then rewrites
		// the staged items' pending picks to, replacing the provisional stand-in id. The color applies only when
		// the label is genuinely new; an existing same-name label keeps its own. Empty return = creation refused
		// (reserved/invalid name), and the affected picks are dropped rather than remapped.
		std::function<QString(const QString& name, const QString& color)> createLabelRequested;
		// Called once at the end of an Import that imported at least one item, after the dialog's own
		// Best/extra-label flush. addMediaItemsRequested may refresh the host view mid-Import (it imports
		// folder-by-folder), but that refresh predates the flush - so without this, the host shows each item's
		// folder label but not its extra labels/Best until the dialog is closed. The dialog stays open after
		// an Import, so that gap is visible. Lets the host repaint once with the fully-applied state.
		std::function<void()> viewChanged;
	};

	// suggestedRelocateFolder pre-fills the relocation destination on first use (when
	// nothing's been persisted yet); see "Source file relocation row" in the .cpp.
	ImportDialog(Library& library, Callbacks callbacks, const QString& suggestedRelocateFolder, QWidget* parent = nullptr);
	~ImportDialog() override;

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
	[[nodiscard]] std::vector<MediaId> effectiveStagedSelection(const MediaId& id) const;
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
	// --- Import (the "Import" button); see runImport for the flow ------------------------------------------
	// One imported item's extra-label picks (every pending label beyond the destination-deciding first one).
	// Keyed by the *registered* id - an owned-photo auto-rename changes the identity from the staged one.
	struct ExtraLabelAssignment
	{
		MediaId mediaId;
		QStringList labelIds;
	};
	// Everything one Import run accomplished, accumulated across the per-type group importers below and
	// applied by runImport's epilogue: succeeded and skipped entries leave staging; the Best flags and
	// extra-label picks of what landed are flushed to the Catalog.
	struct ImportOutcome
	{
		std::vector<MediaId> succeededIds;
		std::vector<MediaId> skippedIds;  // relocation collision resolved as "don't import" - cleared from staging like a success, minus the label flush
		std::vector<MediaId> bestItems;
		std::vector<ExtraLabelAssignment> extraLabelAssignments;
	};
	// Imports one first-label group's photos under the label via the host, with the "import an owned copy
	// instead?" escape hatch for a Reference-mode id collision.
	void importPhotoGroup(const QString& labelId, const std::vector<MediaId>& photoIds, Import::PhotoImportMode mode, ImportOutcome& outcome);
	// Imports one first-label group's videos: optionally relocates each source file (SourceRelocation), hands
	// the batch to the host, then classifies each entry into the outcome; a relocated-but-not-imported entry's
	// staged path follows the file, so a later retry starts from where the file really is.
	void importVideoGroup(const QString& labelName, const std::vector<MediaId>& videoIds, SourceRelocation::Mode relocateMode, ImportOutcome& outcome);
	// Every staged entry whose pendingLabelIds isn't empty: grouped by the first label dropped on it (which
	// decides the import destination) and handed to the per-type group importers above, then the accumulated
	// outcome is applied - the Catalog label flush, unstaging, one host repaint.
	void runImport();
	[[nodiscard]] const LabelOption* findLabelOption(const QString& id) const;

	// --- Provisional labels (folder-derived or "Create label"); all mutate m_provisionalLabels and re-render via
	// refreshLabelList. See ImportDialog.cpp for the model. ---------------------------------------------------
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
	// The shared mechanics of the label ops above and of materialization below: rewrites every staged entry's
	// pendingLabelIds through the mapping - an unmapped id passes through, an id mapped to empty is dropped,
	// and a duplicate the rewrite produces is collapsed (order kept, so the destination-deciding first pick
	// stays first). Re-derives the dots of every changed card. Returns whether any pick was dropped.
	bool remapStagedLabelIds(const QHash<QString, QString>& mapping);
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

	Library& m_library;
	Callbacks m_callbacks;
	std::vector<LabelOption> m_labelOptions;       // cached list: m_provisionalLabels + the Catalog's real labels
	std::vector<LabelOption> m_provisionalLabels;  // labels minted in-dialog, not in the Catalog until Import materializes them
	int m_provisionalSeq = 0;                      // mints unique provisional ids ("new:<n>")
	QHash<MediaId, StagedEntry> m_staged;

	QListWidget* m_labelList  = nullptr;
	QListWidget* m_stagedGrid = nullptr;

	// Source-file relocation controls - the machinery lives in SourceRelocation.h (see runImport for the use).
	QComboBox* m_relocateModeCombo = nullptr;
	QLineEdit* m_relocateFolderEdit = nullptr;
	QSplitter* m_splitter = nullptr;
};
