#pragma once

#include "Core/MediaId.h"
#include "Import.h"  // Import::PhotoImportMode, used by importPhotoGroup
#include "Windows/SourceRelocation.h"  // SourceRelocation::Mode, importVideoGroup's parameter

#include <QDialog>
#include <QHash>
#include <QStringList>

#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QSplitter;
class QTimer;
class QWidget;
class MediaItemWidget;
class Library;
class PreviewFrameCountCombo;
class SegmentedToggle;

// Stages dropped videos/photos as media cards, assigns real or session-provisional labels, then imports every
// labeled item in one pass. Provisional labels reach Catalog only when used by Import. Failed, cancelled, and
// unlabeled items remain staged; successful and explicitly skipped collisions leave staging.

class ImportDialog final : public QDialog
{
	Q_OBJECT
public:
	struct LabelOption
	{
		QString id;
		QString displayName;
		QString color;
		bool provisional = false;
	};

	ImportDialog(Library& library, const QString& suggestedRelocateFolder, QWidget* parent = nullptr);
	~ImportDialog() override;

	// Defers staging to the event loop; folders expand recursively.
	void addToStaging(const QStringList& paths);

protected:
	void reject() override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	class StagedGridItem;

	void refreshLabelList();
	void stageMediaItems(const QStringList& paths);
	[[nodiscard]] bool stagedItemMatchesMediaTypeFilter(const QString& path) const;
	void applyStagedMediaTypeFilter();
	void resortStagedItems();
	void suggestLabels();
	[[nodiscard]] MediaItemWidget* buildStagedCard(StagedGridItem* item, const QString& path, const QString& tempPreviewDir, qint64 durationMs);
	// Removes the card and its temporary preview directory.
	void unstage(const MediaId& id);
	void updateCardLabelDots(const MediaId& id);
	[[nodiscard]] int stagedPreviewFrameCount() const;
	void stagedPreviewFrameCountChanged();
	void regenerateInsufficientStagedVideoPreviews(int frameCount);
	void zoomStagedCards(int steps);
	void rebuildAllStagedCards();
	// Whole selection when item is selected; otherwise that item alone.
	[[nodiscard]] std::vector<MediaId> effectiveStagedSelection(const StagedGridItem* item) const;
	void showStagedCardContextMenu(StagedGridItem* item, const QPoint& globalPos);
	void previewStagedItem(const MediaId& id);
	void locateStagedSourceFile(const MediaId& id);
	void copyStagedSourcePath(const MediaId& id);
	void compareStagedPhotos(const std::vector<MediaId>& photoIds);
	void setBestForStagedSelection(const std::vector<MediaId>& ids, bool inBest);
	void removeStagedItems(const std::vector<MediaId>& ids);
	void deleteStagedSourceFiles(const std::vector<MediaId>& ids);
	[[nodiscard]] std::vector<MediaId> selectedStagedIds() const;
	// Renames and re-keys in place, preserving "staged key == row id == current file id"; extension remains fixed.
	void renameStagedItem(const MediaId& id);

	// Keyed by registered id because owned-photo import may auto-rename.
	struct ExtraLabelAssignment
	{
		MediaId mediaId;
		QStringList labelIds;
	};
	struct ImportOutcome
	{
		std::vector<MediaId> succeededIds;
		std::vector<MediaId> skippedIds;
		std::vector<MediaId> bestItems;
		std::vector<ExtraLabelAssignment> extraLabelAssignments;
	};
	void importPhotoGroup(const QString& labelId, const std::vector<MediaId>& photoIds, Import::PhotoImportMode mode, ImportOutcome& outcome);
	// A relocated but unimported entry follows its file so retry starts from the actual path.
	void importVideoGroup(const QString& labelId, const std::vector<MediaId>& videoIds, SourceRelocation::Mode relocateMode, ImportOutcome& outcome);
	// Groups labeled entries by their first (destination) label, imports, flushes labels, and unstages outcomes.
	void runImport();
	[[nodiscard]] const LabelOption* findLabelOption(const QString& id) const;

	[[nodiscard]] static bool isProvisionalId(const QString& id);
	[[nodiscard]] QString findLabelIdByName(const QString& name, const QString& excludeId) const;
	QString addProvisionalLabel(const QString& name);
	QString ensureLabelForFolderName(const QString& name);
	void showLabelListContextMenu(const QPoint& pos);
	void renameProvisionalLabel(const QString& provisionalId);
	void setProvisionalLabelColor(const QString& provisionalId);
	void deleteProvisionalLabel(const QString& provisionalId);
	void mergeProvisionalInto(const QString& provisionalId, const QString& targetId);
	// Rewrites staged ids, dropping empty mappings and duplicate results while preserving first-label order.
	bool remapStagedLabelIds(const QHash<QString, QString>& mapping);
	void updateAllCardLabelDots();
	// Materializes used provisional labels and rewrites staged picks to real ids.
	void materializeUsedProvisionalLabels();

private:
	// pendingLabelIds[0] is the destination label; the remaining ids are extra labels.
	struct StagedEntry
	{
		QString path;
		QString tempPreviewDir;
		QString suggestedLabelName;
		qint64 durationMs = -1;
		bool pendingBest = false;
		QStringList pendingLabelIds;
		StagedGridItem* item = nullptr;
	};

	Library& _library;
	std::vector<LabelOption> _labelOptions;
	std::vector<LabelOption> _provisionalLabels;
	int _provisionalSeq = 0;
	QHash<MediaId, StagedEntry> _staged;

	QListWidget* _labelList  = nullptr;
	QListWidget* _stagedGrid = nullptr;

	QComboBox* _relocateModeCombo = nullptr;
	SegmentedToggle* _mediaTypeFilter = nullptr;
	PreviewFrameCountCombo* _previewFrameCountCombo = nullptr;
	SegmentedToggle* _sortDirectionToggle = nullptr;
	QLineEdit* _relocateFolderEdit = nullptr;
	QSplitter* _splitter = nullptr;
	QTimer* _stagedCardZoomDebounce = nullptr;
};
