#include "Windows/LibraryPlayer.h"
#include "Windows/VideoPlayerWindow.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MediaId.h"
#include "DarkroomSettings.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QToolTip>
RESTORE_COMPILER_WARNINGS

namespace {

[[nodiscard]] QString extractedLabelName()
{
	return QSettings{}.value(Settings::ExtractedLabelName, Defaults::ExtractedLabelName).toString();
}

// Creates the label when missing. Returns its photo folder, empty after reporting why there is none.
[[nodiscard]] QString prepareExtractedLabelFolder(Catalog& catalog, QWidget* dialogParent)
{
	const QString labelName = extractedLabelName();
	QString error;
	const LabelId labelId = catalog.createLabel(labelName, {}, &error);
	if (labelId == LabelId::None)
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("Failed to create the \"%1\" label:\n%2").arg(labelName, error));
		return {};
	}

	const QString photoFolder = catalog.photoFolderForLabel(labelId);
	if (photoFolder.isEmpty())
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("The \"%1\" label has no usable photo folder.").arg(labelName));
	}
	return photoFolder;
}

// Tracks the frame where it was written, in the Extracted label's photo folder.
// The library holds no two identical files: a byte-identical tracked photo gets the label instead, and the frame is deleted.
void registerExtractedFrame(Catalog& catalog, const QString& framePath, QWidget* dialogParent)
{
	const QString labelName = extractedLabelName();
	const LabelId labelId = catalog.ordinaryLabelIdByName(labelName); // created by prepareExtractedLabelFolder
	assert_and_return_r(labelId != LabelId::None, );

	if (const QString existingPath = catalog.findPhotoBySameContent(framePath); !existingPath.isEmpty())
	{
		QFile::remove(framePath);
		catalog.addLabel(MediaId::fromFile(existingPath), labelId);
		QToolTip::showText(QCursor::pos(), QObject::tr("Already in the library as %1\nLabelled \"%2\"")
			.arg(QDir::toNativeSeparators(existingPath), labelName), dialogParent);
		return;
	}

	if (!catalog.addPhoto(MediaId::fromFile(framePath), framePath, catalog.photoFolderForLabel(labelId), /*referenced=*/false))
	{
		QFile::remove(framePath);
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("An item with the same name and file size is already tracked elsewhere:\n%1").arg(QDir::toNativeSeparators(framePath)));
		return;
	}

	QToolTip::showText(QCursor::pos(), QObject::tr("Frame imported into the library under \"%1\"").arg(labelName), dialogParent);
}

} // namespace

VideoPlayerWindow* LibraryPlayer::open(Library& library, const QString& videoPath, QWidget* parent)
{
	// Library is stable across library switches; the store it holds is not.
	VideoPlayerWindow* const player = VideoPlayerWindow::createPlayerWindow(videoPath, parent, [&library] { return &library.metadataStore(); });
	player->setFrameTarget({
		QObject::tr("Extract frame and import to library"),
		QObject::tr("Extract frame → library"),
		[&library, player] { return prepareExtractedLabelFolder(library.catalog(), player); },
		[&library, player](const QString& framePath) { registerExtractedFrame(library.catalog(), framePath, player); },
	});
	return player;
}
