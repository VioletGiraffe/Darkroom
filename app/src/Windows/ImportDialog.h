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

// Stages dropped videos/photos as media cards, assigns real or session-provisional labels, then imports every
// labeled item in one pass. Provisional labels reach Catalog only when used by Import. Failed, cancelled, and
// unlabeled items remain staged; successful and explicitly skipped collisions leave staging.

class ImportDialog final : public QDialog
{
public:
	struct LabelOption
	{
		QString id;
		QString displayName;
		QString color;
		bool provisional = false;
	};

	// Host-owned operations; the dialog accesses Catalog directly for everything else.
	struct Callbacks
	{
		// Staged preview directories contain frames directly; missing entries fall back to extraction.
		std::function<void(const QString& labelId, const QStringList& videoPaths,
			const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations)> addMediaItemsRequested;
		// Returns results in path order; registeredId must key post-import Best/label updates.
		std::function<std::vector<Import::PhotoResult>(const QString& labelId, const QStringList& photoPaths,
			Import::PhotoImportMode mode)> importPhotosRequested;
		// Materializes a provisional label; empty means refusal and drops affected picks.
		std::function<QString(const QString& name, const QString& color)> createLabelRequested;
		// Called after Best/extra-label flushing when at least one item imported.
		std::function<void()> viewChanged;
	};

	ImportDialog(Library& library, Callbacks callbacks, const QString& suggestedRelocateFolder, QWidget* parent = nullptr);
	~ImportDialog() override;

	// Defers staging to the event loop; folders expand recursively.
	void addToStaging(const QStringList& paths);

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	void refreshLabelList();
	void stageMediaItems(const QStringList& paths);
	[[nodiscard]] MediaItemWidget* buildStagedCard(const MediaId& id, const QString& path, const QString& tempPreviewDir, qint64 durationMs);
	// Removes the card and its temporary preview directory.
	void unstage(const MediaId& id);
	void updateCardLabelDots(const MediaId& id);
	// Whole selection when id is selected; otherwise id alone.
	[[nodiscard]] std::vector<MediaId> effectiveStagedSelection(const MediaId& id) const;
	void showStagedCardContextMenu(const MediaId& id, const QPoint& globalPos);
	void previewStagedItem(const MediaId& id);
	void locateStagedSourceFile(const MediaId& id);
	void copyStagedSourcePath(const MediaId& id);
	void compareStagedPhotos(const std::vector<MediaId>& photoIds);
	void setBestForStagedSelection(const std::vector<MediaId>& ids, bool inBest);
	void removeStagedItems(const std::vector<MediaId>& ids);
	void deleteStagedSourceFiles(const std::vector<MediaId>& ids);
	[[nodiscard]] std::vector<MediaId> selectedStagedIds() const;
	// Renames and re-keys in place, preserving "staged key == current file id"; extension remains fixed.
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
		qint64 durationMs = -1;
		bool pendingBest = false;
		QStringList pendingLabelIds;
		QListWidgetItem* item = nullptr;
	};

	Library& _library;
	Callbacks _callbacks;
	std::vector<LabelOption> _labelOptions;
	std::vector<LabelOption> _provisionalLabels;
	int _provisionalSeq = 0;
	QHash<MediaId, StagedEntry> _staged;

	QListWidget* _labelList  = nullptr;
	QListWidget* _stagedGrid = nullptr;

	QComboBox* _relocateModeCombo = nullptr;
	QLineEdit* _relocateFolderEdit = nullptr;
	QSplitter* _splitter = nullptr;
};
