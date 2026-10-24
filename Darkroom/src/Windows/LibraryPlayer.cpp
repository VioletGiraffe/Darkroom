#include "Windows/LibraryPlayer.h"
#include "Windows/VideoPlayerWindow.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MediaItemId.h"
#include "Core/MetadataStore.h"
#include "DarkroomSettings.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QJsonValue>
#include <QMessageBox>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringView>
#include <QToolTip>
RESTORE_COMPILER_WARNINGS

#include <utility>

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

	if (const MediaItemId existing = catalog.findMediaItemWithSameContent(framePath); existing != MediaItemId::None)
	{
		QFile::remove(framePath);
		catalog.addLabel(existing, labelId);
		QToolTip::showText(QCursor::pos(), QObject::tr("Already in the library as %1\nLabelled \"%2\"")
			.arg(QDir::toNativeSeparators(catalog.sourcePathForMediaItem(existing)), labelName), dialogParent);
		return;
	}

	if (catalog.addPhoto(framePath, catalog.photoFolderForLabel(labelId), /*referenced=*/false) == MediaItemId::None)
	{
		QFile::remove(framePath);
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("The extracted frame could not be registered in the library:\n%1").arg(QDir::toNativeSeparators(framePath)));
		return;
	}

	QToolTip::showText(QCursor::pos(), QObject::tr("Frame imported into the library under \"%1\"").arg(labelName), dialogParent);
}

} // namespace

VideoPlayerWindow* LibraryPlayer::open(Library& library, const QString& videoPath, QWidget* parent)
{
	// Library is stable across library switches; its catalog and store are not.
	VideoPlayerWindow::PlaybackSettingsStore settingsStore{
		.load = [&library](const QString& path, QStringView field) {
			const MediaItemId id = library.catalog().mediaItemAtSourcePath(path);
			return id == MediaItemId::None ? QJsonValue(QJsonValue::Undefined) : library.metadataStore().get(id, field);
		},
		.save = [&library](const QString& path, QStringView field, const QJsonValue& value) {
			const MediaItemId id = library.catalog().mediaItemAtSourcePath(path);
			if (id == MediaItemId::None)
				return;
			MetadataStore::Writer writer = library.metadataStore().beginBatch();
			if (value.isUndefined())
				writer.removeField(id, field);
			else
				writer.set(id, field, value);
		},
	};
	VideoPlayerWindow* const player = VideoPlayerWindow::createPlayerWindow(videoPath, parent, std::move(settingsStore));
	player->setFrameTarget({
		QObject::tr("Extract frame and import to library"),
		QObject::tr("Extract frame → library"),
		[&library, player] { return prepareExtractedLabelFolder(library.catalog(), player); },
		[&library, player](const QString& framePath) { registerExtractedFrame(library.catalog(), framePath, player); },
	});
	return player;
}
