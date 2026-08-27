#include "Windows/ImportExecution.h"

#include "Core/Catalog.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagebox.h"

DISABLE_COMPILER_WARNINGS
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <ranges>

namespace {

[[nodiscard]] QString videoOutputFolder(const QString& storageFolderPath, const QString& videoPath)
{
	const QFileInfo videoInfo(videoPath);
	const MediaId id = MediaId::fromFile(videoPath);
	return storageFolderPath + "/" + Catalog::frameFolderName(videoInfo.completeBaseName(), id);
}

void showImportFailures(QWidget* dialogParent, const QString& summary, const QStringList& failures)
{
	if (failures.empty())
		return;

	MessageBox::notice(dialogParent, QObject::tr("Import incomplete"), summary, failures.join("\n\n"), QMessageBox::Critical);
}

} // namespace

void ImportExecution::importVideosInteractive(Catalog& catalog, QStringList videoPaths, const QString& storageFolderPath,
	const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations, QWidget* dialogParent)
{
	if (videoPaths.empty())
		return;
	assert_and_return_r(!storageFolderPath.isEmpty(), );

	Catalog::BatchScope batch(catalog);

	QMessageBox progressBox(dialogParent);
	progressBox.setWindowTitle(QObject::tr("Processing"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	const auto partition = std::ranges::stable_partition(videoPaths, [&storageFolderPath](const QString& path) {
		return !QDir{ videoOutputFolder(storageFolderPath, path) }.exists();
	});

	QStringList failures;
	const auto processFilesRange = [&catalog, &progressBox, &storageFolderPath, &stagedPreviewDirs, &stagedDurations, dialogParent,
			&failures, totalSize = videoPaths.size()](auto begin, auto end, qsizetype firstNumber, bool overwriteExisting = false) {
		qsizetype displayNumber = firstNumber;
		for (const QString& videoPath : std::ranges::subrange(begin, end))
		{
			progressBox.setText(QObject::tr("Adding video %1/%2...").arg(displayNumber++).arg(totalSize));
			QApplication::processEvents();

			const MediaId id = MediaId::fromFile(videoPath);
			const QString stagedPreviewDir = stagedPreviewDirs.value(id);
			const qint64 stagedDurationMs = stagedDurations.value(id, -1);
			Import::Result result = Import::importVideo(
				catalog, videoPath, storageFolderPath, stagedPreviewDir, overwriteExisting, stagedDurationMs);
			if (result.status == Import::Status::FolderConflict)
			{
				const QString outputFolder = videoOutputFolder(storageFolderPath, videoPath);
				if (QMessageBox::question(dialogParent, QObject::tr("Folder Exists"),
						QObject::tr("Folder already exists:\n%1\n\nOverwrite?").arg(outputFolder),
						QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
					continue;
				result = Import::importVideo(
					catalog, videoPath, storageFolderPath, stagedPreviewDir, /*overwriteExisting=*/true, stagedDurationMs);
			}
			if (result.status == Import::Status::Error)
				failures << QObject::tr("%1:\n%2").arg(QDir::toNativeSeparators(videoPath), result.errorMessage);
		}
	};

	processFilesRange(videoPaths.begin(), partition.begin(), 1);

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
			processFilesRange(partition.begin(), partition.end(), partition.begin() - videoPaths.begin() + 1, choice == QMessageBox::YesToAll);
	}

	progressBox.hide();
	showImportFailures(dialogParent, QObject::tr("The following videos could not be imported:"), failures);
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
		results.push_back(result);
	}
	showImportFailures(dialogParent, QObject::tr("The following photos could not be imported:"), failures);
	return results;
}
