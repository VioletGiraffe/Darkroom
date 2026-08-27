#include "Windows/SourceRelocation.h"
#include "Core/Library.h"
#include "Core/MediaId.h"
#include "Utils.h"
#include "Windows/VideoPlayerWindow.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

namespace {

using SourceRelocation::Mode;

[[nodiscard]] QString copyOrMove(QWidget* dialogParent, const QString& srcPath, const QString& destPath, bool isMove)
{
	const bool ok = isMove ? QFile{ srcPath }.rename(destPath) : QFile::copy(srcPath, destPath);
	if (!ok)
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to %1:\n%2\nto:\n%3")
			.arg(isMove ? QObject::tr("move") : QObject::tr("copy"), srcPath, destPath));
		return srcPath;
	}
	return destPath;
}

class FileCollisionDialog final : public QDialog
{
public:
	enum class Result { Overwrite, Skip, SkipAndDelete, Cancel };

	FileCollisionDialog(Library& library, const QString& stagedPath, const QString& destPath, bool isDuplicate, QWidget* parent)
		: QDialog(parent)
	{
		setWindowTitle(isDuplicate ? tr("Duplicate File Found") : tr("File Already Exists"));

		// Keep preview player windows interactive while this decision is open.
		setWindowModality(Qt::WindowModal);

		QVBoxLayout* layout = new QVBoxLayout(this);

		QLabel* message = new QLabel(isDuplicate
			? tr("An identical file is already at the destination:\n\n%1\n\nIt won't be imported again. You can optionally delete the redundant staged copy:\n\n%2").arg(destPath, stagedPath)
			: tr("A different file with the same name already exists at the destination:\n\n%1\n\nOverwrite it with the staged file, skip importing this one, or cancel to leave it staged and decide later:\n\n%2").arg(destPath, stagedPath),
			this);
		message->setWordWrap(true);
		layout->addWidget(message);

		QHBoxLayout* buttonRow = new QHBoxLayout;

		if (!isDuplicate)
		{
			// Preview windows must outlive this short-lived collision dialog.
			QWidget* previewParent = parent;
			Library* const playerLibrary = &library;

			QPushButton* playStaged = new QPushButton(tr("Play Staged File"), this);
			connect(playStaged, &QPushButton::clicked, this, [playerLibrary, previewParent, stagedPath] {
				VideoPlayerWindow::createPlayerWindow(*playerLibrary, stagedPath, previewParent);
			});
			buttonRow->addWidget(playStaged);

			QPushButton* playExisting = new QPushButton(tr("Play Existing File"), this);
			connect(playExisting, &QPushButton::clicked, this, [playerLibrary, previewParent, destPath] {
				VideoPlayerWindow::createPlayerWindow(*playerLibrary, destPath, previewParent);
			});
			buttonRow->addWidget(playExisting);
		}

		buttonRow->addStretch(1);

		if (isDuplicate)
		{
			QPushButton* skip = new QPushButton(tr("Skip"), this);
			connect(skip, &QPushButton::clicked, this, [this] { _result = Result::Skip; accept(); });
			buttonRow->addWidget(skip);

			QPushButton* skipDelete = new QPushButton(tr("Skip and Delete Duplicate"), this);
			connect(skipDelete, &QPushButton::clicked, this, [this] { _result = Result::SkipAndDelete; accept(); });
			buttonRow->addWidget(skipDelete);
		}
		else
		{
			QPushButton* overwrite = new QPushButton(tr("Overwrite"), this);
			connect(overwrite, &QPushButton::clicked, this, [this] { _result = Result::Overwrite; accept(); });
			buttonRow->addWidget(overwrite);

			QPushButton* skip = new QPushButton(tr("Skip"), this);
			connect(skip, &QPushButton::clicked, this, [this] { _result = Result::Skip; accept(); });
			buttonRow->addWidget(skip);

			QPushButton* cancel = new QPushButton(tr("Cancel"), this);
			connect(cancel, &QPushButton::clicked, this, [this] { _result = Result::Cancel; accept(); });
			buttonRow->addWidget(cancel);
		}

		layout->addLayout(buttonRow);
	}

	[[nodiscard]] Result result() const { return _result; }

private:
	Result _result = Result::Cancel;
};

struct RelocationOutcome
{
	QString importPath;
	bool keepStaged = false;
};

// I/O failures fall back to the original path so relocation cannot silently drop an import.
[[nodiscard]] RelocationOutcome performRelocation(Library& library, QWidget* dialogParent, const QString& path, Mode mode,
	const QString& destFolder)
{
	const QString destPath = destFolder + "/" + QFileInfo(path).fileName();
	const bool isMove = (mode == Mode::Move);

	// A retry may already point at the destination; do not compare the file with itself.
	if (QFileInfo(path) == QFileInfo(destPath))
		return { path, false };

	if (!QFile::exists(destPath))
		return { copyOrMove(dialogParent, path, destPath, isMove), false };

	// MediaId is only the cheap gate; confirm duplicates byte-for-byte.
	const bool isDuplicate = (MediaId::fromFile(path) == MediaId::fromFile(destPath)) && filesAreIdentical(path, destPath);
	FileCollisionDialog collisionDialog(library, path, destPath, isDuplicate, dialogParent);
	collisionDialog.exec();

	switch (collisionDialog.result())
	{
	case FileCollisionDialog::Result::Overwrite:
		if (!QFile::remove(destPath))
		{
			QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to overwrite existing file:\n%1").arg(destPath));
			return { path, false };
		}
		return { copyOrMove(dialogParent, path, destPath, isMove), false };

	case FileCollisionDialog::Result::SkipAndDelete:
		if (!QFile::remove(path))
			QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to delete duplicate file:\n%1").arg(path));
		return { {}, false };

	case FileCollisionDialog::Result::Skip:
		return { {}, false };

	case FileCollisionDialog::Result::Cancel:
		break;
	}

	return { {}, true };
}

} // namespace

SourceRelocation::BatchResult SourceRelocation::relocateIfNeeded(Library& library, QWidget* dialogParent, const QStringList& paths,
	Mode mode, const QString& destFolder)
{
	if (mode == Mode::LeaveInPlace)
		return { paths, {} };

	if (destFolder.isEmpty())
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("No destination folder is set for relocating source files - they will be left in their original location."));
		return { paths, {} };
	}
	if (!QDir{}.mkpath(destFolder))
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Could not create or access destination folder:\n%1").arg(destFolder));
		return { paths, {} };
	}

	BatchResult result;
	result.toImport.reserve(paths.size());
	for (const QString& path : paths)
	{
		const RelocationOutcome outcome = performRelocation(library, dialogParent, path, mode, destFolder);
		if (!outcome.importPath.isEmpty())
		{
			result.toImport.append(outcome.importPath);
			if (outcome.importPath != path)
				result.relocatedTo.insert(path, outcome.importPath);
		}
		else if (outcome.keepStaged)
			result.keepStaged.append(path);
		else
			result.skipped.append(path);
	}
	return result;
}
