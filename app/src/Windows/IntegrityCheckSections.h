#pragma once

#include "Windows/IntegrityCheckDialog.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

struct ResolvableRow
{
	QLabel*                   statusLabel = nullptr;
	std::vector<QPushButton*> buttons;
	bool                      closed = false;
	std::function<void()>     onClosed;
	std::function<void()>     reorderSection;

	void close(const QString& status, bool resolved = true);
	bool tryResolve(const QString& successText, const std::function<bool()>& action);
};

class IntegrityCheckSections
{
public:
	IntegrityCheckSections(const Catalog& catalog, const CatalogIntegrity::IntegrityReport& report, IntegrityCheckDialog::Callbacks& callbacks,
	                       QWidget* content, QVBoxLayout* contentLayout, QWidget* dialog);

private:
	struct AdoptRow        { ResolvableRow* row; QString filePath; };
	struct VideoRow        { ResolvableRow* row; MediaId id; bool canReimport; bool canRegenerate; bool sourceMissing; };
	struct MissingPhotoRow { ResolvableRow* row; MediaId id; bool referenced; };
	struct DanglingLabelRow { ResolvableRow* row; MediaId id; };

	void buildUntrackedFolders(const CatalogIntegrity::IntegrityReport& report);
	void buildUntrackedPhotos(const CatalogIntegrity::IntegrityReport& report);
	void buildVideoIssues(const CatalogIntegrity::IntegrityReport& report);
	void buildMissingPhotos(const CatalogIntegrity::IntegrityReport& report);
	void buildDanglingLabelIssues(const CatalogIntegrity::IntegrityReport& report);

	std::pair<QHBoxLayout*, ResolvableRow*> addRow(const QString& statusText);
	static QPushButton* addRowButton(QHBoxLayout* rowLayout, ResolvableRow* row, const QString& text);
	QHBoxLayout* addSectionHeader(const QString& html);

	void wireAction(QPushButton* button, ResolvableRow* row, const QString& successText, const QString& failureText,
	                std::function<bool()> action);
	void wireBrowse(QPushButton* button, ResolvableRow* row, std::function<QString()> browse,
	                std::function<bool(const QString&)> action, const QString& successFmt, const QString& failureText);
	static void wireSkip(QPushButton* skipButton, ResolvableRow* row);

	QString browseForSourceVideo(const QString& hint) const;
	QString browseForSourcePhoto(const QString& hint) const;

	template <class Row, class Applies, class Act>
	static std::pair<int, int> runBlanket(std::vector<Row>& rows, Applies applies, Act act, const QString& successText);

	void showBlanketTally(const QString& title, const QString& doneFmt, int done, const QString& failedFmt, int failed) const;

	struct LocateTally { int relocated = 0; int unmatched = 0; int failed = 0; };
	// A moved file can still be matched exactly by its name/size identity.
	template <class Row, class Applies, class Relocate>
	std::optional<LocateTally> locateAllByIdentity(std::vector<Row>& rows, Applies applies,
	                                               const std::function<bool(const QString&)>& filePredicate, Relocate relocate,
	                                               const QString& folderPrompt);

	template <class Row>
	void confirmAndRemoveAll(std::vector<Row>& rows, const QString& title, const QString& questionFmt, const QString& doneFmt);

	const Catalog& _catalog;
	IntegrityCheckDialog::Callbacks& _callbacks;
	QWidget*     _dialog;
	QWidget*     _content;
	QVBoxLayout* _layout;
	QVBoxLayout* _currentRowsLayout = nullptr;
	const QString _rowStyle;

	std::deque<ResolvableRow>     _rows;  // Handlers retain row pointers; deque preserves their addresses.
	std::vector<AdoptRow>         _untrackedPhotoRows;
	std::vector<VideoRow>         _videoRows;
	std::vector<MissingPhotoRow>  _missingPhotoRows;
	std::vector<DanglingLabelRow> _danglingLabelRows;
};

inline void ResolvableRow::close(const QString& status, bool resolved)
{
	statusLabel->setText(resolved ? QStringLiteral("✓ ") + status : status);
	statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().palette.textDim.name()));
	for (QPushButton* b : buttons)
		b->setEnabled(false);
	closed = true;
	if (reorderSection)
		reorderSection();
	if (onClosed)
		onClosed();
}

inline bool ResolvableRow::tryResolve(const QString& successText, const std::function<bool()>& action)
{
	if (!action())
		return false;
	close(successText);
	return true;
}

inline IntegrityCheckSections::IntegrityCheckSections(const Catalog& catalog, const CatalogIntegrity::IntegrityReport& report,
                                                      IntegrityCheckDialog::Callbacks& callbacks,
                                                      QWidget* content, QVBoxLayout* contentLayout, QWidget* dialog)
	: _catalog(catalog), _callbacks(callbacks), _dialog(dialog), _content(content), _layout(contentLayout),
	  _rowStyle(QStringLiteral("QFrame#integrityRow { border: 1px solid %1; border-radius: %2px; }")
		.arg(Theme::current().palette.borderSubtle.name()).arg(Theme::current().metrics.controlRadius))
{
	buildUntrackedFolders(report);
	buildUntrackedPhotos(report);
	buildVideoIssues(report);
	buildMissingPhotos(report);
	buildDanglingLabelIssues(report);
}

inline void IntegrityCheckSections::buildUntrackedFolders(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.untracked.empty())
		return;

	// Name alone cannot safely identify the source video for a blanket registration.
	addSectionHeader(QObject::tr("<b>Untracked folders</b> - on disk, not in the catalog"));
	for (const CatalogIntegrity::UntrackedFolder& u : report.untracked)
	{
		const auto [rowLayout, row] = addRow(QFileInfo(u.folderPath).fileName());

		QPushButton* browseButton = addRowButton(rowLayout, row, QObject::tr("Browse..."));
		QPushButton* skipButton   = addRowButton(rowLayout, row, QObject::tr("Skip"));

		const QString folderPath = u.folderPath;

		wireBrowse(browseButton, row,
			[this, folderPath] { return browseForSourceVideo(folderPath); },
			[this, folderPath](const QString& picked) { return _callbacks.registerRequested(folderPath, picked); },
			QObject::tr("Registered with %1"),
			QObject::tr("Could not register - that file's identity is already tracked under a different folder."));

		wireSkip(skipButton, row);
	}
}

inline void IntegrityCheckSections::buildUntrackedPhotos(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.untrackedPhotos.empty())
		return;

	QHBoxLayout* header = addSectionHeader(QObject::tr("<b>Untracked photos</b> - on disk under Photos, not in the catalog"));
	QPushButton* addAllButton = new QPushButton(QObject::tr("Add all"), _content);
	header->addWidget(addAllButton);

	const auto refreshBlanket = [this, addAllButton] {
		addAllButton->setEnabled(std::any_of(_untrackedPhotoRows.cbegin(), _untrackedPhotoRows.cend(),
			[](const AdoptRow& r) { return !r.row->closed; }));
	};

	for (const CatalogIntegrity::UntrackedPhoto& p : report.untrackedPhotos)
	{
		const auto [rowLayout, row] = addRow(QFileInfo(p.filePath).fileName() + "<br>" + QObject::tr("label: %1").arg(p.labelName));

		QPushButton* addButton  = addRowButton(rowLayout, row, QObject::tr("Add to catalog"));
		QPushButton* skipButton = addRowButton(rowLayout, row, QObject::tr("Skip"));
		row->onClosed = refreshBlanket;

		const QString filePath = p.filePath;

		wireAction(addButton, row, QObject::tr("Added to catalog."),
			QObject::tr("Could not add - a file with the same name and size is already tracked elsewhere."),
			[this, filePath] { return _callbacks.adoptPhotoRequested(filePath); });

		wireSkip(skipButton, row);
		_untrackedPhotoRows.push_back({ row, filePath });
	}

	QObject::connect(addAllButton, &QPushButton::clicked, addAllButton, [this] {
		const auto [added, failed] = runBlanket(_untrackedPhotoRows, [](const AdoptRow&) { return true; },
			[this](AdoptRow& r) { return _callbacks.adoptPhotoRequested(r.filePath); }, QObject::tr("Added to catalog."));
		showBlanketTally(QObject::tr("Add all photos"), QObject::tr("Added %1 photo(s) to the catalog."), added,
			QObject::tr("%1 could not be added - a file with the same name and size is already tracked."), failed);
	});

	refreshBlanket();
}

inline void IntegrityCheckSections::buildVideoIssues(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.issues.empty())
		return;

	QHBoxLayout* header = addSectionHeader(QObject::tr("<b>Videos</b> - tracked, but the source, frames or preview are missing on disk"));
	QPushButton* locateAllButton     = new QPushButton(QObject::tr("Locate all..."), _content);
	QPushButton* reimportAllButton   = new QPushButton(QObject::tr("Re-import all"), _content);
	QPushButton* regenerateAllButton = new QPushButton(QObject::tr("Regenerate all previews"), _content);
	QPushButton* removeAllButton     = new QPushButton(QObject::tr("Remove all"), _content);
	header->addWidget(locateAllButton);
	header->addWidget(reimportAllButton);
	header->addWidget(regenerateAllButton);
	header->addWidget(removeAllButton);

	const auto refreshBlanket = [this, locateAllButton, reimportAllButton, regenerateAllButton, removeAllButton] {
		bool anyOpen = false, anyLocate = false, anyReimport = false, anyRegenerate = false;
		for (const VideoRow& v : _videoRows)
		{
			if (v.row->closed)
				continue;
			anyOpen = true;
			anyLocate     = anyLocate     || v.sourceMissing;
			anyReimport   = anyReimport   || v.canReimport;
			anyRegenerate = anyRegenerate || v.canRegenerate;
		}
		locateAllButton->setEnabled(anyLocate);
		reimportAllButton->setEnabled(anyReimport);
		regenerateAllButton->setEnabled(anyRegenerate);
		removeAllButton->setEnabled(anyOpen);
	};

	for (const CatalogIntegrity::MediaIssue& issue : report.issues)
	{
		// Integrity verdicts are orthogonal, so report every applicable problem.
		QStringList problems;
		if (issue.extractedFramesMissing())
			problems << QObject::tr("extracted frames are gone");
		if (issue.previewMissing())
			problems << (issue.realFramesPresent ? QObject::tr("no preview - the card renders the full-size frames instead")
			                                     : QObject::tr("no preview - the card has no image to show"));
		if (issue.splitFlagStale())
			problems << QObject::tr("marked not-yet-split, but frames exist");
		if (issue.sourceMissing())
			problems << (issue.sourcePath.isEmpty() ? QObject::tr("no source recorded")
			                                        : QObject::tr("source missing: %1").arg(issue.sourcePath));
		const QString status = QFileInfo(issue.folder).fileName() + "<br>" + problems.join(QStringLiteral("; "));

		const auto [rowLayout, row] = addRow(status);

		const MediaId id = issue.id;
		const QString recordedSource = issue.sourcePath;

		const bool canReimport   = issue.extractedFramesMissing() && issue.sourcePresent;
		const bool canRegenerate = issue.previewMissing() && !issue.extractedFramesMissing() && (issue.realFramesPresent || issue.sourcePresent);
		const bool canMarkSplit  = issue.splitFlagStale() && !issue.previewMissing();
		const bool canLocate     = issue.sourceMissing();

		QPushButton* locateButton     = canLocate     ? addRowButton(rowLayout, row, QObject::tr("Locate source...")) : nullptr;
		QPushButton* reimportButton   = canReimport   ? addRowButton(rowLayout, row, QObject::tr("Re-import")) : nullptr;
		QPushButton* regenerateButton = canRegenerate ? addRowButton(rowLayout, row, QObject::tr("Regenerate preview")) : nullptr;
		QPushButton* markSplitButton  = canMarkSplit  ? addRowButton(rowLayout, row, QObject::tr("Mark as fully split")) : nullptr;
		QPushButton* removeButton     = addRowButton(rowLayout, row, QObject::tr("Remove"));
		QPushButton* skipButton       = addRowButton(rowLayout, row, QObject::tr("Skip"));
		row->onClosed = refreshBlanket;

		if (locateButton)
			wireBrowse(locateButton, row,
				[this, recordedSource] { return browseForSourceVideo(recordedSource); },
				[this, id](const QString& picked) { return _callbacks.locateSourceRequested(id, picked); },
				QObject::tr("Relocated to %1"),
				QObject::tr("Could not relocate - that file's identity is already tracked as a different item."));

		if (reimportButton)
			wireAction(reimportButton, row, QObject::tr("Re-imported."),
				QObject::tr("Re-import failed - see the error dialog for details."),
				[this, id] { return _callbacks.reimportRequested(id); });

		if (regenerateButton)
			wireAction(regenerateButton, row, QObject::tr("Preview regenerated."),
				QObject::tr("Could not regenerate the preview - the source may be unavailable."),
				[this, id] { return _callbacks.regeneratePreviewRequested(id); });

		if (markSplitButton)
			wireAction(markSplitButton, row, QObject::tr("Marked as fully split."), QObject::tr("Could not update the entry."),
				[this, id] { return _callbacks.markSplitRequested(id); });

		wireAction(removeButton, row, QObject::tr("Removed from catalog."), QObject::tr("Could not remove."),
			[this, id] { return _callbacks.removeEntryRequested(id); });

		wireSkip(skipButton, row);
		_videoRows.push_back({ row, id, canReimport, canRegenerate, canLocate });
	}

	QObject::connect(locateAllButton, &QPushButton::clicked, locateAllButton, [this, refreshBlanket] {
		const auto tally = locateAllByIdentity(_videoRows, [](const VideoRow& v) { return v.sourceMissing; }, isSupportedVideoFile,
			[this](const MediaId& id, const QString& picked) { return _callbacks.locateSourceRequested(id, picked); },
			QObject::tr("Select folder to search for moved source videos"));
		if (!tally)
			return;
		refreshBlanket();

		QStringList msg{ QObject::tr("Relocated %1 video(s).").arg(tally->relocated) };
		if (tally->unmatched)
			msg << QObject::tr("%1 had no matching file in that folder.").arg(tally->unmatched);
		if (tally->failed)
			msg << QObject::tr("%1 could not be relocated - the identity is already tracked as a different item.").arg(tally->failed);
		QMessageBox::information(_dialog, QObject::tr("Locate all videos"), msg.join(QStringLiteral("\n")));
	});

	QObject::connect(reimportAllButton, &QPushButton::clicked, reimportAllButton, [this] {
		if (QMessageBox::question(_dialog, QObject::tr("Re-import all"),
		        QObject::tr("Re-extract frames for every re-importable video? This re-runs ffmpeg and can take a while."))
		    != QMessageBox::Yes)
			return;
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const auto [done, failed] = runBlanket(_videoRows, [](const VideoRow& v) { return v.canReimport; },
			[this](VideoRow& v) { return _callbacks.reimportRequested(v.id); }, QObject::tr("Re-imported."));
		QApplication::restoreOverrideCursor();
		showBlanketTally(QObject::tr("Re-import all"), QObject::tr("Re-imported %1 video(s)."), done, QObject::tr("%1 could not be re-imported."), failed);
	});

	QObject::connect(regenerateAllButton, &QPushButton::clicked, regenerateAllButton, [this] {
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const auto [done, failed] = runBlanket(_videoRows, [](const VideoRow& v) { return v.canRegenerate; },
			[this](VideoRow& v) { return _callbacks.regeneratePreviewRequested(v.id); }, QObject::tr("Preview regenerated."));
		QApplication::restoreOverrideCursor();
		showBlanketTally(QObject::tr("Regenerate all previews"), QObject::tr("Regenerated %1 preview(s)."), done,
			QObject::tr("%1 could not be regenerated."), failed);
	});

	QObject::connect(removeAllButton, &QPushButton::clicked, removeAllButton, [this] {
		confirmAndRemoveAll(_videoRows, QObject::tr("Remove all"),
			QObject::tr("Remove all %1 broken video entries from the catalog? Frame folders and source files on disk are not touched."),
			QObject::tr("Removed %1 broken video(s) from the catalog."));
	});

	refreshBlanket();
}

inline void IntegrityCheckSections::buildMissingPhotos(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.photoIssues.empty())
		return;

	QHBoxLayout* header = addSectionHeader(QObject::tr("<b>Photos</b> - tracked photos whose source file is missing"));
	QPushButton* locateAllButton = new QPushButton(QObject::tr("Locate all..."), _content);
	QPushButton* removeAllButton = new QPushButton(QObject::tr("Remove all"), _content);
	header->addWidget(locateAllButton);
	header->addWidget(removeAllButton);

	const auto refreshBlanket = [this, locateAllButton, removeAllButton] {
		bool anyOpen = false, anyReferenced = false;
		for (const MissingPhotoRow& m : _missingPhotoRows)
		{
			if (m.row->closed)
				continue;
			anyOpen = true;
			anyReferenced = anyReferenced || m.referenced;
		}
		locateAllButton->setEnabled(anyReferenced);
		removeAllButton->setEnabled(anyOpen);
	};

	for (const CatalogIntegrity::PhotoIssue& photo : report.photoIssues)
	{
		const QString name   = photo.sourcePath.isEmpty() ? QObject::tr("(no source recorded)") : QFileInfo(photo.sourcePath).fileName();
		const QString what   = photo.referenced ? QObject::tr("referenced file moved or unmounted") : QObject::tr("the library's own file is gone");
		const QString detail = photo.sourcePath.isEmpty() ? QString{} : "<br>" + photo.sourcePath;
		const auto [rowLayout, row] = addRow(name + "<br>" + what + detail);

		const MediaId id = photo.id;
		const QString recordedPath = photo.sourcePath;

		QPushButton* locateButton = photo.referenced ? addRowButton(rowLayout, row, QObject::tr("Locate...")) : nullptr;
		QPushButton* removeButton = addRowButton(rowLayout, row, QObject::tr("Remove"));
		QPushButton* skipButton   = addRowButton(rowLayout, row, QObject::tr("Skip"));
		row->onClosed = refreshBlanket;

		if (locateButton)
			wireBrowse(locateButton, row,
				[this, recordedPath] { return browseForSourcePhoto(recordedPath); },
				[this, id](const QString& picked) { return _callbacks.locatePhotoRequested(id, picked); },
				QObject::tr("Relocated to %1"),
				QObject::tr("Could not relocate - that file's identity is already tracked as a different item."));

		wireAction(removeButton, row, QObject::tr("Removed from catalog."), QObject::tr("Could not remove."),
			[this, id] { return _callbacks.removeEntryRequested(id); });

		wireSkip(skipButton, row);
		_missingPhotoRows.push_back({ row, id, photo.referenced });
	}

	QObject::connect(locateAllButton, &QPushButton::clicked, locateAllButton, [this, refreshBlanket] {
		const auto tally = locateAllByIdentity(_missingPhotoRows, [](const MissingPhotoRow& m) { return m.referenced; }, isSupportedImageFile,
			[this](const MediaId& id, const QString& picked) { return _callbacks.locatePhotoRequested(id, picked); },
			QObject::tr("Select folder to search for missing photos"));
		if (!tally)
			return;
		refreshBlanket();

		QStringList msg{ QObject::tr("Relocated %1 photo(s).").arg(tally->relocated) };
		if (tally->unmatched)
			msg << QObject::tr("%1 had no matching file in that folder.").arg(tally->unmatched);
		if (tally->failed)
			msg << QObject::tr("%1 could not be relocated - the identity is already tracked as a different item.").arg(tally->failed);
		QMessageBox::information(_dialog, QObject::tr("Locate all photos"), msg.join(QStringLiteral("\n")));
	});

	QObject::connect(removeAllButton, &QPushButton::clicked, removeAllButton, [this] {
		confirmAndRemoveAll(_missingPhotoRows, QObject::tr("Remove all"),
			QObject::tr("Remove all %1 missing-photo entries from the catalog? Any files still on disk are not touched."),
			QObject::tr("Removed %1 photo(s) from the catalog."));
	});

	refreshBlanket();
}

inline void IntegrityCheckSections::buildDanglingLabelIssues(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.danglingLabelIssues.empty())
		return;

	QHBoxLayout* header = addSectionHeader(QObject::tr("<b>Invalid label references</b> - items refer to labels no longer in the registry"));
	QPushButton* dropAllButton = new QPushButton(QObject::tr("Drop all invalid references"), _content);
	header->addWidget(dropAllButton);

	const auto refreshBlanket = [this, dropAllButton] {
		dropAllButton->setEnabled(std::any_of(_danglingLabelRows.cbegin(), _danglingLabelRows.cend(),
			[](const DanglingLabelRow& r) { return !r.row->closed; }));
	};

	for (const CatalogIntegrity::DanglingLabelIssue& issue : report.danglingLabelIssues)
	{
		QStringList missingIds;
		for (const LabelId labelId : issue.missingLabelIds)
			missingIds << QString::number(toUInt64(labelId));

		const auto [rowLayout, row] = addRow(issue.id.name() + "<br>" + QObject::tr("missing label ID(s): %1").arg(missingIds.join(", ")));
		QPushButton* dropButton = addRowButton(rowLayout, row, QObject::tr("Drop invalid references"));
		QPushButton* skipButton = addRowButton(rowLayout, row, QObject::tr("Skip"));
		row->onClosed = refreshBlanket;

		const MediaId id = issue.id;
		wireAction(dropButton, row, QObject::tr("Invalid label references dropped."), QObject::tr("Could not update the entry."),
			[this, id] { return _callbacks.removeInvalidLabelReferencesRequested(id); });
		wireSkip(skipButton, row);
		_danglingLabelRows.push_back({ row, id });
	}

	QObject::connect(dropAllButton, &QPushButton::clicked, dropAllButton, [this] {
		const auto [done, failed] = runBlanket(_danglingLabelRows, [](const DanglingLabelRow&) { return true; },
			[this](DanglingLabelRow& r) { return _callbacks.removeInvalidLabelReferencesRequested(r.id); },
			QObject::tr("Invalid label references dropped."));
		showBlanketTally(QObject::tr("Drop invalid label references"), QObject::tr("Cleaned %1 item(s)."), done,
			QObject::tr("%1 item(s) could not be updated."), failed);
	});

	refreshBlanket();
}

inline std::pair<QHBoxLayout*, ResolvableRow*> IntegrityCheckSections::addRow(const QString& statusText)
{
	QVBoxLayout* rowsLayout = _currentRowsLayout;
	QFrame* frame = new QFrame(rowsLayout->parentWidget());
	frame->setObjectName("integrityRow");
	frame->setStyleSheet(_rowStyle);
	QHBoxLayout* rowLayout = new QHBoxLayout(frame);
	ResolvableRow& row = _rows.emplace_back();
	row.statusLabel = new QLabel(statusText, frame);
	row.statusLabel->setWordWrap(true);
	rowLayout->addWidget(row.statusLabel, 1);
	rowsLayout->addWidget(frame);
	row.reorderSection = [rowsLayout, frame] { rowsLayout->removeWidget(frame); rowsLayout->addWidget(frame); };
	return { rowLayout, &row };
}

inline QPushButton* IntegrityCheckSections::addRowButton(QHBoxLayout* rowLayout, ResolvableRow* row, const QString& text)
{
	QPushButton* button = new QPushButton(text, rowLayout->parentWidget());
	rowLayout->addWidget(button);
	row->buttons.push_back(button);
	return button;
}

inline QHBoxLayout* IntegrityCheckSections::addSectionHeader(const QString& html)
{
	QHBoxLayout* headerRow = new QHBoxLayout();
	headerRow->addWidget(new QLabel(html, _content));
	headerRow->addStretch(1);
	_layout->addLayout(headerRow);

	// Per-section containers let settled rows sink without crossing section boundaries.
	QWidget* rowsContainer = new QWidget(_content);
	_currentRowsLayout = new QVBoxLayout(rowsContainer);
	_currentRowsLayout->setContentsMargins(0, 0, 0, 0);
	_layout->addWidget(rowsContainer);

	return headerRow;
}

inline void IntegrityCheckSections::wireAction(QPushButton* button, ResolvableRow* row, const QString& successText,
                                               const QString& failureText, std::function<bool()> action)
{
	QObject::connect(button, &QPushButton::clicked, button, [this, row, successText, failureText, action = std::move(action)] {
		if (!row->tryResolve(successText, action))
			QMessageBox::warning(_dialog, QObject::tr("Catalog integrity check"), failureText);
	});
}

inline void IntegrityCheckSections::wireBrowse(QPushButton* button, ResolvableRow* row, std::function<QString()> browse,
                                               std::function<bool(const QString&)> action, const QString& successFmt,
                                               const QString& failureText)
{
	QObject::connect(button, &QPushButton::clicked, button,
		[this, row, browse = std::move(browse), action = std::move(action), successFmt, failureText] {
			const QString picked = browse();
			if (picked.isEmpty())
				return;
			if (!row->tryResolve(successFmt.arg(picked), [&] { return action(picked); }))
				QMessageBox::warning(_dialog, QObject::tr("Catalog integrity check"), failureText);
		});
}

inline void IntegrityCheckSections::wireSkip(QPushButton* skipButton, ResolvableRow* row)
{
	QObject::connect(skipButton, &QPushButton::clicked, skipButton, [row] {
		row->close(QObject::tr("%1  (skipped)").arg(row->statusLabel->text()), /*resolved=*/false);
	});
}

inline QString IntegrityCheckSections::browseForSourceVideo(const QString& hint) const
{
	const QString startDir = hint.isEmpty() ? _catalog.anySourceDir() : QFileInfo(hint).absolutePath();
	return QFileDialog::getOpenFileName(_dialog, QObject::tr("Select source video"), startDir,
		QObject::tr("Video files (*.mp4 *.mov *.avi *.mkv *.flv);;All files (*)"));
}

inline QString IntegrityCheckSections::browseForSourcePhoto(const QString& hint) const
{
	const QString startDir = hint.isEmpty() ? QString{} : QFileInfo(hint).absolutePath();
	const QString filter = QObject::tr("Image files (%1);;All files (*)").arg(IMAGE_FILE_FILTERS.join(QStringLiteral(" ")));
	return QFileDialog::getOpenFileName(_dialog, QObject::tr("Locate photo"), startDir, filter);
}

template <class Row, class Applies, class Act>
std::pair<int, int> IntegrityCheckSections::runBlanket(std::vector<Row>& rows, Applies applies, Act act, const QString& successText)
{
	int done = 0, failed = 0;
	for (Row& r : rows)
	{
		if (r.row->closed || !applies(r))
			continue;
		if (r.row->tryResolve(successText, [&] { return act(r); })) ++done;
		else ++failed;
	}
	return { done, failed };
}

inline void IntegrityCheckSections::showBlanketTally(const QString& title, const QString& doneFmt, int done,
                                                     const QString& failedFmt, int failed) const
{
	QString msg = doneFmt.arg(done);
	if (failed)
		msg += "\n" + failedFmt.arg(failed);
	QMessageBox::information(_dialog, title, msg);
}

template <class Row, class Applies, class Relocate>
std::optional<IntegrityCheckSections::LocateTally> IntegrityCheckSections::locateAllByIdentity(
	std::vector<Row>& rows, Applies applies, const std::function<bool(const QString&)>& filePredicate, Relocate relocate,
	const QString& folderPrompt)
{
	const QString dir = QFileDialog::getExistingDirectory(_dialog, folderPrompt);
	if (dir.isEmpty())
		return std::nullopt;

	QHash<QString, QString> byIdentity;   // MediaId::key() -> first file found under dir carrying that identity
	for (const QString& path : collectFilesInDirectory(dir, /*recursive=*/true, filePredicate))
	{
		const QString identity = MediaId::fromFile(path).key();
		if (!byIdentity.contains(identity))
			byIdentity.insert(identity, QDir::toNativeSeparators(path));
	}

	LocateTally tally;
	for (Row& r : rows)
	{
		if (r.row->closed || !applies(r))
			continue;
		const auto found = byIdentity.constFind(r.id.key());
		if (found == byIdentity.constEnd()) { ++tally.unmatched; continue; }
		if (relocate(r.id, found.value())) { r.row->close(QObject::tr("Relocated to %1").arg(found.value())); ++tally.relocated; }
		else ++tally.failed;
	}
	return tally;
}

template <class Row>
void IntegrityCheckSections::confirmAndRemoveAll(std::vector<Row>& rows, const QString& title, const QString& questionFmt,
                                                 const QString& doneFmt)
{
	const int open = static_cast<int>(std::count_if(rows.cbegin(), rows.cend(), [](const Row& r) { return !r.row->closed; }));
	if (open == 0)
		return;
	if (QMessageBox::question(_dialog, title, questionFmt.arg(open)) != QMessageBox::Yes)
		return;
	runBlanket(rows, [](const Row&) { return true; },
		[this](Row& r) { _callbacks.removeEntryRequested(r.id); return true; }, QObject::tr("Removed from catalog."));
	QMessageBox::information(_dialog, title, doneFmt.arg(open));
}
