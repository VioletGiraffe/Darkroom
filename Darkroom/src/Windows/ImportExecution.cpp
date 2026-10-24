#include "Windows/ImportExecution.h"

#include "Core/Catalog.h"
#include "Windows/OpenFileRelease.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagedialog.h"

DISABLE_COMPILER_WARNINGS
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QObject>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <ranges>
#include <vector>

namespace {

void showImportFailures(QWidget* dialogParent, const QString& summary, const QStringList& failures)
{
	if (failures.empty())
		return;

	MessageDialog::notice(dialogParent, QObject::tr("Import incomplete"), summary, failures.join("\n\n"), QMessageBox::Critical);
}

} // namespace

std::vector<MediaItemId> ImportExecution::importVideosInteractive(Catalog& catalog, const std::vector<StagedVideo>& videos,
	const QString& storageFolderPath, QWidget* dialogParent)
{
	std::vector<MediaItemId> registeredIds(videos.size(), MediaItemId::None);
	if (videos.empty())
		return registeredIds;
	assert_and_return_r(!storageFolderPath.isEmpty(), registeredIds);

	Catalog::BatchScope batch(catalog);

	QMessageBox progressBox(dialogParent);
	progressBox.setWindowTitle(QObject::tr("Processing"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	// Videos whose output folder exists go last, behind a single overwrite choice.
	std::vector<size_t> order(videos.size());
	for (size_t i = 0; i < order.size(); ++i)
		order[i] = i;
	const auto partition = std::ranges::stable_partition(order, [&](size_t i) {
		return !QDir{ catalog.frameFolderForNewVideo(storageFolderPath, videos[i].path) }.exists();
	});

	QStringList failures;
	const auto processFilesRange = [&catalog, &videos, &registeredIds, &progressBox, &storageFolderPath, dialogParent,
			&failures](auto begin, auto end, qsizetype firstNumber, bool overwriteExisting = false) {
		qsizetype displayNumber = firstNumber;
		for (const size_t i : std::ranges::subrange(begin, end))
		{
			const StagedVideo& video = videos[i];
			progressBox.setText(QObject::tr("Adding video %1/%2...").arg(displayNumber++).arg(videos.size()));
			QApplication::processEvents();

			Import::Result result = Import::importVideo(catalog, video.path, storageFolderPath, video.previewDir, overwriteExisting, video.durationMs);
			if (result.status == Import::Status::FolderConflict)
			{
				const QString outputFolder = catalog.frameFolderForNewVideo(storageFolderPath, video.path);
				if (QMessageBox::question(dialogParent, QObject::tr("Folder Exists"),
						QObject::tr("Folder already exists:\n%1\n\nOverwrite?").arg(outputFolder),
						QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
					continue;
				result = Import::importVideo(catalog, video.path, storageFolderPath, video.previewDir, /*overwriteExisting=*/true, video.durationMs);
			}
			if (result.status == Import::Status::Error)
				failures << QObject::tr("%1:\n%2").arg(QDir::toNativeSeparators(video.path), result.errorMessage);
			registeredIds[i] = result.registeredId;
		}
	};

	processFilesRange(order.begin(), partition.begin(), 1);

	if (partition.begin() != partition.end())
	{
		QMessageBox conflictChoice(dialogParent);
		conflictChoice.setIcon(QMessageBox::Question);
		conflictChoice.setWindowTitle(QObject::tr("Folder conflict"));
		conflictChoice.setText(QObject::tr("One or more videos have existing output folders. Overwrite all, skip all, or decide one by one?"));
		conflictChoice.setStandardButtons(QMessageBox::YesToAll | QMessageBox::Yes | QMessageBox::NoToAll);
		conflictChoice.button(QMessageBox::YesToAll)->setText(QObject::tr("Overwrite all"));
		conflictChoice.button(QMessageBox::Yes)->setText(QObject::tr("Decide one by one"));
		conflictChoice.button(QMessageBox::NoToAll)->setText(QObject::tr("Skip all"));
		conflictChoice.setDefaultButton(QMessageBox::YesToAll);

		const int choice = conflictChoice.exec();
		if (choice != QMessageBox::NoToAll)
			processFilesRange(partition.begin(), partition.end(), partition.begin() - order.begin() + 1, choice == QMessageBox::YesToAll);
	}

	progressBox.hide();
	showImportFailures(dialogParent, QObject::tr("The following videos could not be imported:"), failures);
	return registeredIds;
}

std::vector<Import::PhotoResult> ImportExecution::importPhotosInteractive(
	Catalog& catalog, LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode, QWidget* dialogParent)
{
	const Catalog::Label* label = catalog.labelById(labelId);
	if (!label || label->isVirtual())
		return {};

	const QString photoFolder = catalog.photoFolderForLabel(labelId);
	if (photoFolder.isEmpty())
	{
		QMessageBox::warning(dialogParent, QObject::tr("Import"),
			QObject::tr("This label does not have a safe photo-storage path:\n%1").arg(label->displayName));
		return {};
	}

	Catalog::BatchScope batch(catalog);

	const bool movingPhotos = mode == Import::PhotoImportMode::Move;
	if (movingPhotos)
		releaseMediaFilesHeldOpen(photoPaths);
	std::vector<PathMove> completedMoves;

	std::vector<Import::PhotoResult> results;
	results.reserve(photoPaths.size());
	QStringList failures;
	for (const QString& path : photoPaths)
	{
		const Import::PhotoResult result = Import::importPhoto(catalog, photoFolder, path, mode);
		if (result.status == Import::PhotoStatus::Error)
			failures << QObject::tr("%1:\n%2").arg(QDir::toNativeSeparators(path), result.errorMessage);
		// Referenced photos have no storage folder from which to derive this label.
		if (result.status == Import::PhotoStatus::Success && mode == Import::PhotoImportMode::Reference)
			catalog.addLabel(result.registeredId, labelId);
		// An identical library file adopted in place of a move stands in for the photo left behind.
		if (result.status == Import::PhotoStatus::Success && movingPhotos)
			completedMoves.push_back({ path, catalog.sourcePathForMediaItem(result.registeredId) });
		results.push_back(result);
	}
	if (movingPhotos)
		followMovedMediaFiles(photoPaths, completedMoves);
	showImportFailures(dialogParent, QObject::tr("The following photos could not be imported:"), failures);
	return results;
}
