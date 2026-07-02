#pragma once

#include "UiComponents/DragGestureHelper.h"
#include "Core/MediaId.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QStringList>

#include <functional>

class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QWidget;

// ============================================================================
// QuickImportDialog - stage new video files, label them, then import everything labeled in one "Import" step.
//
// Mirrors the main window's own label model: a small label-list panel (like LabelSidebar, minus
// filtering) sits next to a big grid of staged item cards (MediaItemWidget, same as the main grid).
// Dragging a label from the list onto a staged card tags it, exactly like dragging a label onto an item
// card in the main window; the star button toggles Best the same way too. Nothing is written to the
// Catalog until "Import" runs: every staged item that's been labeled gets imported and removed from
// staging on success, or stays staged (with its labels intact) on failure or a deferred (Cancel) relocation
// collision; a collision resolved as Skip / "Skip and Delete" also clears the entry - the destination copy
// stands in for it. Unlabeled items are left alone either way. Files can also be dropped straight onto
// the dialog to stage them.
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
	};

	// One staged item's extra-label picks, resolved against the collection it landed in (needed to derive
	// its frame folder, the same way markBestRequested's per-collection grouping does for Best). Identified by
	// MediaId, not path: relocation (Move) deletes the source from its staged path before this is applied, so
	// the path can no longer be resolved to the file - the stable id is what addLabel needs.
	struct ExtraLabelAssignment
	{
		QString collectionName;
		MediaId mediaId;
		QStringList labelIds;
	};

	struct Callbacks
	{
		// Every ordinary label, for the label-list panel. A staged item's first-dropped label also
		// decides which one of these folders it's imported into - see runImport() in the .cpp.
		std::function<QList<LabelOption>()> getLabelOptions;
		// Adds the given video files to the named collection. stagedPreviewDirs maps each staged video's MediaId
		// to the temp dir whose preview/ holds the frames already extracted for its staging card, so import
		// can reuse them by copy instead of re-running ffmpeg (see MainWindow::processVideoFile); a video absent
		// from the map, or whose staged frames are gone, is extracted fresh.
		std::function<void(const QString& collectionName, const QStringList& videoPaths,
			const QHash<MediaId, QString>& stagedPreviewDirs)> addMediaItemsRequested;
		// Creates a new collection with the given name; returns true on success
		std::function<bool(const QString& name)> createCollectionRequested;
		// True iff the item is now tracked by the Catalog at the frame folder this import derives for it
		// (<collection>/<source base name>) - checked right after addMediaItemsRequested for each item in its
		// batch, purely so runImport() knows which staged entries to clear (a successfully-added one) versus
		// leave staged (declined overwrite, a collision, etc.). Deliberately not "tracked under *some* folder":
		// on a name+size collision the id is already tracked elsewhere and import refused the staged copy -
		// a mere "is tracked" check would misreport that as a success, unstaging the entry and silently
		// dropping its pending labels. By MediaId, not path: Move relocation has already deleted the source
		// from its staged path by now.
		std::function<bool(const MediaId&, const QString& collectionName)> isMediaItemTrackedInCollection;
		// Called once per successful Import, with the items flagged "Best" keyed by the collection they were
		// added to (may be empty). The flagged items have already been added - and are therefore tracked -
		// via addMediaItemsRequested. By MediaId for the same reason as ExtraLabelAssignment above.
		std::function<void(const QHash<QString, QList<MediaId>>& bestMediaItemsByCollection)> markBestRequested;
		// Called once per successful Import, with every imported item's extra-label picks (may be empty).
		// Mirrors markBestRequested above: the items have already been added via addMediaItemsRequested by then.
		std::function<void(const QList<ExtraLabelAssignment>& assignments)> assignExtraLabelsRequested;
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

	// Pre-populates the staging area with the given video files (used when
	// handing files over from the "Scan for unknown files" tool). Deferred until
	// the dialog is shown so it paints before the blocking thumbnail extraction.
	void addToStaging(const QStringList& paths);

protected:
	// Accepts video files dropped anywhere on the dialog (mirrors MainWindow's own top-level drop handling),
	// staging them directly - same entry point as addToStaging.
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	// Drives the label list's drag-out gesture (mirrors LabelSidebar::eventFilter), installed on its viewport.
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
	void refreshLabelList();
	// Extracts temp preview frames for each path and adds it to the staged grid.
	void stageMediaItems(const QStringList& paths);
	// Deletes a staged entry's temp preview dir and removes its card; used by "Remove from staging" and
	// once an entry has been successfully imported by runImport().
	void unstage(const MediaId& id);
	// Re-derives a staged card's label dots from its pendingLabelIds.
	void updateCardLabelDots(const MediaId& id);
	// The staged items a card-targeted action applies to: the whole staged selection when `id` is part of a
	// multi-selection, otherwise just `id`. Mirrors MainWindow::effectiveSelection for the staged grid.
	[[nodiscard]] QList<MediaId> stagedSelection(const MediaId& id) const;
	void showStagedCardContextMenu(const MediaId& id, const QPoint& globalPos);
	// Every staged entry whose pendingLabelIds isn't empty: grouped by its first label and imported via
	// addMediaItemsRequested, then markBestRequested/assignExtraLabelsRequested for whatever landed.
	void runImport();
	[[nodiscard]] const LabelOption* findLabelOption(const QString& id) const;

private:
	// Per-staged-item state, accumulated by dragging labels from m_labelList onto the card in m_stagedGrid
	// and toggling its Best star; applied (and removed from here) only once "Import" runs successfully. The
	// first id in pendingLabelIds resolves the item's destination folder at Import time (see runImport) -
	// that's the only place this ordering matters; there's no other state or UI for it.
	struct StagedEntry
	{
		QString path;
		QString tempPreviewDir;      // this video's temp N-frame preview; deleted on unstage/dialog close
		bool pendingBest = false;
		QStringList pendingLabelIds;
		QListWidgetItem* item = nullptr;  // the grid item carrying this entry's MediaItemWidget card
	};

	Callbacks m_callbacks;
	QList<LabelOption> m_labelOptions;  // cached result of the last getLabelOptions() call
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
