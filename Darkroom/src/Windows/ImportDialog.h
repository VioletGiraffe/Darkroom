#pragma once

#include "Core/MediaItemId.h"
#include "Import.h"  // Import::PhotoImportMode, used by importPhotoGroup
#include "UiComponents/ContentWidthListWidget.h"
#include "Windows/SourceRelocation.h"  // SourceRelocation::Mode, importVideoGroup's parameter
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDialog>
#include <QHash>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <cstdint>
#include <functional>
#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QSplitter;
class QTabWidget;
class QTimer;
class QWidget;
class MediaItemWidget;
class Library;
class PreviewFrameCountCombo;
class SegmentedToggle;

// Stages dropped videos and photos as media cards on separate tabs, assigns real or session-provisional labels, then
// imports every labeled item of a tab in one pass. Provisional labels reach Catalog only when used by an import. Failed,
// cancelled, and unlabeled items remain staged; successful and explicitly skipped collisions leave staging.

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
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	// Dialog-local handle of a staged file; a rename keeps it.
	enum class StagedId : uint32_t { None = 0 };
	class StagedGridItem;

	void refreshLabelList();
	void stageMediaItems(const QStringList& paths);
	void updateStagedTabTitles();
	void resortStagedItems();
	void suggestLabels();
	[[nodiscard]] MediaItemWidget* buildStagedCard(StagedGridItem* item, const QString& path, const QString& tempPreviewDir, qint64 durationMs);
	// Removes the card and its temporary preview directory.
	void unstage(StagedId id);
	void updateCardLabelDots(StagedId id);
	[[nodiscard]] int stagedPreviewFrameCount() const;
	void stagedPreviewFrameCountChanged();
	void regenerateInsufficientStagedVideoPreviews(int frameCount);
	void zoomStagedCards(int steps);
	void rebuildAllStagedCards();
	// By pathComparisonKey; None when no staged file is at path.
	[[nodiscard]] StagedId stagedIdAtPath(const QString& path) const;
	// Whole selection when item is selected; otherwise that item alone.
	[[nodiscard]] std::vector<StagedId> effectiveStagedSelection(const StagedGridItem* item) const;
	void showStagedCardContextMenu(StagedGridItem* item, const QPoint& globalPos);
	void previewStagedItem(StagedId id);
	// Does nothing when the item is no longer staged.
	void selectAndScrollToStagedItem(StagedId id);
	void locateStagedSourceFile(StagedId id);
	void copyStagedSourcePath(StagedId id);
	void compareStagedPhotos(const std::vector<StagedId>& photoIds);
	void setBestForStagedSelection(const std::vector<StagedId>& ids, bool inBest);
	void removeStagedItems(const std::vector<StagedId>& ids);
	void deleteStagedSourceFiles(const std::vector<StagedId>& ids);
	[[nodiscard]] static std::vector<StagedId> selectedStagedIds(const QListWidget* grid);
	// Renames the file in place; the extension remains fixed.
	void renameStagedItem(StagedId id);

	struct ExtraLabelAssignment
	{
		MediaItemId mediaItemId = MediaItemId::None;
		QStringList labelIds;
	};
	struct ImportOutcome
	{
		std::vector<StagedId> succeededIds;
		std::vector<StagedId> skippedIds;
		std::vector<MediaItemId> bestItems;
		std::vector<ExtraLabelAssignment> extraLabelAssignments;
	};
	void importPhotoGroup(const QString& labelId, const std::vector<StagedId>& photoIds, Import::PhotoImportMode mode, ImportOutcome& outcome);
	// A relocated but unimported entry follows its file so retry starts from the actual path.
	void importVideoGroup(const QString& labelId, const std::vector<StagedId>& videoIds, SourceRelocation::Mode relocateMode, ImportOutcome& outcome);
	// Empty, after prompting for what is missing, when grid has no labeled card or modeCombo has no choice.
	[[nodiscard]] std::vector<StagedId> labeledIdsReadyForImport(const QListWidget* grid, QComboBox* modeCombo);
	using GroupImporter = std::function<void(const QString& labelId, const std::vector<StagedId>& ids, ImportOutcome& outcome)>;
	// Groups ids by their first (destination) label, imports, flushes labels, and unstages outcomes.
	void importByDestinationLabel(const std::vector<StagedId>& ids, const GroupImporter& importGroup);
	void runPhotoImport();
	void runVideoImport();
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
	// Materializes the provisional labels ids use and rewrites staged picks to real ids.
	void materializeUsedProvisionalLabels(const std::vector<StagedId>& ids);

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
	QHash<StagedId, StagedEntry> _staged;
	uint32_t _lastStagedId = 0;

	ContentWidthListWidget* _labelList = nullptr;
	QTabWidget* _stagedTabs = nullptr;
	QListWidget* _videoGrid = nullptr;
	QListWidget* _photoGrid = nullptr;

	QComboBox* _photoModeCombo = nullptr;
	QComboBox* _videoRelocateModeCombo = nullptr;
	PreviewFrameCountCombo* _previewFrameCountCombo = nullptr;
	SegmentedToggle* _sortDirectionToggle = nullptr;
	QLineEdit* _relocateFolderEdit = nullptr;
	QSplitter* _splitter = nullptr;
	QTimer* _stagedCardZoomDebounce = nullptr;
};
