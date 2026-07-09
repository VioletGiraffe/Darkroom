#include "Windows/IntegrityCheckDialog.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QApplication>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr const char* VIDEO_FILE_FILTER = "Video files (*.mp4 *.mov *.avi *.mkv *.flv);;All files (*)";

// Shared state for one issue row: its status label, its buttons, and whether it's been dealt with. Both the row's
// own buttons and its section's blanket action resolve through close(), so a row settled either way updates once and
// is skipped by the other. onClosed lets the section re-evaluate which blanket buttons still apply (it holds only a
// weak reference to the section's row list, so storing it here can't form an ownership cycle with that list).
struct ResolvableRow
{
	QLabel*                   statusLabel = nullptr;
	std::vector<QPushButton*> buttons;
	bool                      closed = false;
	std::function<void()>     onClosed;

	void close(const QString& status)
	{
		statusLabel->setText(status);
		for (QPushButton* b : buttons)
			b->setEnabled(false);
		closed = true;
		if (onClosed)
			onClosed();
	}
};

// Opens a file picker for the user to manually point at a source video, starting near hint (a path that
// doesn't have to exist - only its directory is used) if given, else a sensible catalog-wide default.
QString browseForSourceVideo(QWidget* parent, const QString& hint)
{
	const QString startDir = hint.isEmpty() ? Catalog::instance().anySourceDir() : QFileInfo(hint).absolutePath();
	return QFileDialog::getOpenFileName(parent, QObject::tr("Select source video"), startDir, QObject::tr(VIDEO_FILE_FILTER));
}

// The photo counterpart (the referenced-photo Locate resolution): point at an image file, starting in hint's
// directory if given. The filter is built from the app-wide image extensions (IMAGE_FILE_FILTERS).
QString browseForSourcePhoto(QWidget* parent, const QString& hint)
{
	const QString startDir = hint.isEmpty() ? QString{} : QFileInfo(hint).absolutePath();
	const QString filter = QObject::tr("Image files (%1);;All files (*)").arg(IMAGE_FILE_FILTERS.join(QStringLiteral(" ")));
	return QFileDialog::getOpenFileName(parent, QObject::tr("Locate photo"), startDir, filter);
}

} // namespace

void IntegrityCheckDialog::scanAndShowUi(Callbacks callbacks, QWidget* parent)
{
	const CatalogIntegrity::IntegrityReport report = CatalogIntegrity::scan();
	if (report.isEmpty())
	{
		QMessageBox::information(parent, tr("Catalog integrity check"), tr("No issues found - the catalog matches what's on disk."));
		return;
	}

	IntegrityCheckDialog dialog(report, std::move(callbacks), parent);
	dialog.exec();
}

IntegrityCheckDialog::IntegrityCheckDialog(const CatalogIntegrity::IntegrityReport& report, Callbacks callbacks, QWidget* parent)
	: QDialog(parent), m_callbacks(std::move(callbacks))
{
	setWindowTitle(tr("Catalog Integrity Check"));

	QVBoxLayout* outer = new QVBoxLayout(this);

	QLabel* instructions = new QLabel(
		tr("Differences between the catalog and what's actually on disk. Resolve each row on its own, or use a "
		   "section's blanket action - nothing here is applied automatically."), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().InstructionText));
	outer->addWidget(instructions);

	QScrollArea* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	// The dialog-body scroll region gets the central sheet's QListWidget vocabulary (hairline frame, no fill
	// of its own) in place of the native sunken panel; QScrollArea isn't styled centrally because its other
	// use (FrameViewerWindow) wants no frame at all.
	scroll->setStyleSheet(QStringLiteral("QScrollArea { border: 1px solid %1; border-radius: %2px; background: transparent; }")
		.arg(Theme::current().BorderMedium).arg(Theme::ControlRadius));
	QWidget* content = new QWidget(scroll);
	QVBoxLayout* contentLayout = new QVBoxLayout(content);

	// Runs one resolution on a row and, on success, closes it (records the outcome, disables the row's buttons).
	// Returns success so the caller - a single-row button, or a blanket driver - can react (warn, or tally).
	const auto resolve = [](const std::shared_ptr<ResolvableRow>& row, const QString& successText,
	                        const std::function<bool()>& action) -> bool {
		if (!action())
			return false;
		row->close(successText);
		return true;
	};

	// The batch analog of resolve: run a blanket action over rows. For each still-open row that `applies`, run the
	// resolution; a success closes the row (via resolve) and counts toward done, a failure counts toward failed. The
	// {done, failed} tally lets the driver compose its summary; rows re-enable their blanket buttons via onClosed.
	const auto runBlanket = [resolve](auto& rows, auto applies, auto act, const QString& successText) {
		int done = 0, failed = 0;
		for (auto& r : rows)
		{
			if (r.row->closed || !applies(r))
				continue;
			if (resolve(r.row, successText, [&] { return act(r); })) ++done;
			else ++failed;
		}
		return std::pair{done, failed};
	};

	// Wires a single-row action button: on click run the resolution; on success the row records it and disables
	// (stays visible as a record of what happened); on failure warn and leave the row active so the user can retry.
	// The blanket drivers below reuse the very same callbacks but aggregate into one summary instead of warning per item.
	const auto wireAction = [this, resolve](QPushButton* button, const std::shared_ptr<ResolvableRow>& row,
	                                        const QString& successText, const QString& failureText, std::function<bool()> action) {
		connect(button, &QPushButton::clicked, this, [=] {
			if (!resolve(row, successText, action))
				QMessageBox::warning(this, tr("Catalog integrity check"), failureText);
		});
	};

	// Wires a "browse then act" button (the untracked Register and the photo Locate share this shape): opens a file
	// picker; if the user picked something, runs action(picked) and, on success, sets the row status from successFmt
	// (a "...%1..." string filled with the picked path); on failure warns.
	const auto wireBrowse = [this, resolve](QPushButton* button, const std::shared_ptr<ResolvableRow>& row,
	                                        std::function<QString()> browse, std::function<bool(const QString&)> action,
	                                        const QString& successFmt, const QString& failureText) {
		connect(button, &QPushButton::clicked, this, [=] {
			const QString picked = browse();
			if (picked.isEmpty())
				return;
			if (!resolve(row, successFmt.arg(picked), [=] { return action(picked); }))
				QMessageBox::warning(this, tr("Catalog integrity check"), failureText);
		});
	};

	// A "Skip" button just acknowledges the row without acting - closes it so it reads as dealt-with for this session;
	// it resurfaces on the next scan since nothing was changed.
	const auto wireSkip = [](QPushButton* skipButton, const std::shared_ptr<ResolvableRow>& row) {
		connect(skipButton, &QPushButton::clicked, skipButton, [=] {
			row->close(tr("%1  (skipped)").arg(row->statusLabel->text()));
		});
	};

	// One issue per row, drawn as a hairline card (BorderSubtle - a passive separator, not an interactive
	// control) rather than the native StyledPanel bevel. One sheet string shared by every row.
	const QString rowStyle = QStringLiteral("QFrame#integrityRow { border: 1px solid %1; border-radius: %2px; }")
		.arg(Theme::current().BorderSubtle).arg(Theme::ControlRadius);
	const auto addRow = [&](const QString& statusText) -> std::pair<QHBoxLayout*, std::shared_ptr<ResolvableRow>> {
		QFrame* frame = new QFrame(content);
		frame->setObjectName("integrityRow");
		frame->setStyleSheet(rowStyle);
		QHBoxLayout* rowLayout = new QHBoxLayout(frame);
		auto row = std::make_shared<ResolvableRow>();
		row->statusLabel = new QLabel(statusText, frame);
		row->statusLabel->setWordWrap(true);
		rowLayout->addWidget(row->statusLabel, 1);
		contentLayout->addWidget(frame);
		return { rowLayout, row };
	};

	// Creates one row button, adds it to the row's layout, and registers it so the row's close() disables it later.
	// Returns the button so the caller can wire it (parent comes from the layout, i.e. the row frame).
	const auto addRowButton = [](QHBoxLayout* rowLayout, const std::shared_ptr<ResolvableRow>& row, const QString& text) {
		QPushButton* button = new QPushButton(text, rowLayout->parentWidget());
		rowLayout->addWidget(button);
		row->buttons.push_back(button);
		return button;
	};

	// A section header: the bold class caption on the left, its blanket-action buttons (appended by the caller)
	// pushed to the right. Returns the header layout so the caller can add those buttons after the stretch.
	const auto addSectionHeader = [&](const QString& html) -> QHBoxLayout* {
		QHBoxLayout* headerRow = new QHBoxLayout();
		headerRow->addWidget(new QLabel(html, content));
		headerRow->addStretch(1);
		contentLayout->addLayout(headerRow);
		return headerRow;
	};

	if (!report.untracked.empty())
	{
		// Registering an untracked folder needs a per-folder source pick (name alone can't safely identify the
		// source video), so this class has no blanket action - only per-row Browse.
		addSectionHeader(tr("<b>Untracked folders</b> - on disk, not in the catalog"));
		for (const CatalogIntegrity::UntrackedFolder& u : report.untracked)
		{
			const auto [rowLayout, row] = addRow(QFileInfo(u.folderPath).fileName());

			QPushButton* browseButton = addRowButton(rowLayout, row, tr("Browse..."));
			QPushButton* skipButton   = addRowButton(rowLayout, row, tr("Skip"));

			const QString folderPath = u.folderPath;

			// The only resolution is to point at a source video, which registers the folder against it.
			wireBrowse(browseButton, row,
				[this, folderPath] { return browseForSourceVideo(this, folderPath); },
				[this, folderPath](const QString& picked) { return m_callbacks.registerRequested(folderPath, picked); },
				tr("Registered with %1"),
				tr("Could not register - that file's identity is already tracked under a different folder."));

			wireSkip(skipButton, row);
		}
	}

	if (!report.untrackedPhotos.empty())
	{
		QHBoxLayout* header = addSectionHeader(tr("<b>Untracked photos</b> - on disk under Photos, not in the catalog"));
		QPushButton* addAllButton = new QPushButton(tr("Add all"), content);
		header->addWidget(addAllButton);

		struct AdoptRow { std::shared_ptr<ResolvableRow> row; QString filePath; };
		auto rows = std::make_shared<std::vector<AdoptRow>>();
		const std::weak_ptr<std::vector<AdoptRow>> weakRows = rows;
		const auto refreshBlanket = [weakRows, addAllButton] {
			const auto rows = weakRows.lock();
			if (!rows)
				return;
			addAllButton->setEnabled(std::any_of(rows->cbegin(), rows->cend(), [](const AdoptRow& r) { return !r.row->closed; }));
		};

		for (const CatalogIntegrity::UntrackedPhoto& p : report.untrackedPhotos)
		{
			const auto [rowLayout, row] = addRow(QFileInfo(p.filePath).fileName() + "<br>" + tr("label: %1").arg(p.labelName));

			QPushButton* addButton  = addRowButton(rowLayout, row, tr("Add to catalog"));
			QPushButton* skipButton = addRowButton(rowLayout, row, tr("Skip"));
			row->onClosed = refreshBlanket;

			const QString filePath = p.filePath;

			// The file already lives in its label's Photos dir, so adopting it is a one-click register as an owned photo.
			wireAction(addButton, row, tr("Added to catalog."),
				tr("Could not add - a file with the same name and size is already tracked elsewhere."),
				[this, filePath] { return m_callbacks.adoptPhotoRequested(filePath); });

			wireSkip(skipButton, row);
			rows->push_back({ row, filePath });
		}

		// Add all: adopt every still-open untracked photo. Each already sits in its label's Photos dir, so this is
		// just the row action run in a loop; a name+size clash counts as a failure and leaves that row open.
		connect(addAllButton, &QPushButton::clicked, this, [this, rows, runBlanket] {
			const auto [added, failed] = runBlanket(*rows, [](const AdoptRow&) { return true; },
				[this](AdoptRow& r) { return m_callbacks.adoptPhotoRequested(r.filePath); }, tr("Added to catalog."));
			QString msg = tr("Added %1 photo(s) to the catalog.").arg(added);
			if (failed)
				msg += "\n" + tr("%1 could not be added - a file with the same name and size is already tracked.").arg(failed);
			QMessageBox::information(this, tr("Add all photos"), msg);
		});

		refreshBlanket();
	}

	if (!report.issues.empty())
	{
		QHBoxLayout* header = addSectionHeader(tr("<b>Problems</b> - tracked videos whose files don't match the catalog"));
		QPushButton* reimportAllButton   = new QPushButton(tr("Re-import all"), content);
		QPushButton* regenerateAllButton = new QPushButton(tr("Regenerate all previews"), content);
		QPushButton* removeAllButton     = new QPushButton(tr("Remove all"), content);
		header->addWidget(reimportAllButton);
		header->addWidget(regenerateAllButton);
		header->addWidget(removeAllButton);

		struct VideoRow { std::shared_ptr<ResolvableRow> row; MediaId id; bool canReimport; bool canRegenerate; };
		auto rows = std::make_shared<std::vector<VideoRow>>();
		const std::weak_ptr<std::vector<VideoRow>> weakRows = rows;
		// Each blanket action stays enabled only while some open row still admits it: Remove applies to any open row;
		// Re-import to a ghost whose source is present; Regenerate to a rebuildable-but-invisible entry.
		const auto refreshBlanket = [weakRows, reimportAllButton, regenerateAllButton, removeAllButton] {
			const auto rows = weakRows.lock();
			if (!rows)
				return;
			bool anyOpen = false, anyReimport = false, anyRegenerate = false;
			for (const VideoRow& v : *rows)
			{
				if (v.row->closed)
					continue;
				anyOpen = true;
				anyReimport   = anyReimport   || v.canReimport;
				anyRegenerate = anyRegenerate || v.canRegenerate;
			}
			reimportAllButton->setEnabled(anyReimport);
			regenerateAllButton->setEnabled(anyRegenerate);
			removeAllButton->setEnabled(anyOpen);
		};

		for (const CatalogIntegrity::MediaIssue& issue : report.issues)
		{
			// One line per broken video listing everything wrong with it - the grid's verdicts are orthogonal,
			// so several can apply at once (e.g. frames gone and source also missing).
			QStringList problems;
			if (issue.isGhost())
				problems << tr("frames are gone");
			if (issue.isInvisible())
				problems << tr("no preview - the card can't be shown");
			if (issue.isStale())
				problems << tr("marked not-yet-split, but frames exist");
			if (issue.sourceMissing())
				problems << (issue.sourcePath.isEmpty() ? tr("no source recorded")
				                                        : tr("source missing: %1").arg(issue.sourcePath));
			const QString status = QFileInfo(issue.folder).fileName() + "<br>" + problems.join(QStringLiteral("; "));

			const auto [rowLayout, row] = addRow(status);

			const MediaId id = issue.id;

			// Which fixes are offered follows the grid's recovery notes: Re-import re-extracts a gone deliverable
			// (needs the source); Regenerate rebuilds a missing preview from real frames or the source; Mark-split
			// fixes only a stale flag. Remove/Skip are always available.
			const bool canReimport   = issue.isGhost() && issue.sourcePresent;
			const bool canRegenerate = issue.isInvisible() && !issue.isGhost() && (issue.realFramesPresent || issue.sourcePresent);
			const bool canMarkSplit  = issue.isStale() && !issue.isInvisible();

			QPushButton* reimportButton   = canReimport   ? addRowButton(rowLayout, row, tr("Re-import")) : nullptr;
			QPushButton* regenerateButton = canRegenerate ? addRowButton(rowLayout, row, tr("Regenerate preview")) : nullptr;
			QPushButton* markSplitButton  = canMarkSplit  ? addRowButton(rowLayout, row, tr("Mark as fully split")) : nullptr;
			QPushButton* removeButton     = addRowButton(rowLayout, row, tr("Remove"));
			QPushButton* skipButton       = addRowButton(rowLayout, row, tr("Skip"));
			row->onClosed = refreshBlanket;

			if (reimportButton)
				wireAction(reimportButton, row, tr("Re-imported."),
					tr("Re-import failed - see the error dialog for details."),
					[this, id] { return m_callbacks.reimportRequested(id); });

			if (regenerateButton)
				wireAction(regenerateButton, row, tr("Preview regenerated."),
					tr("Could not regenerate the preview - the source may be unavailable."),
					[this, id] { return m_callbacks.regeneratePreviewRequested(id); });

			if (markSplitButton)
				wireAction(markSplitButton, row, tr("Marked as fully split."), tr("Could not update the entry."),
					[this, id] { return m_callbacks.markSplitRequested(id); });

			wireAction(removeButton, row, tr("Removed from catalog."), tr("Could not remove."),
				[this, id] { return m_callbacks.removeEntryRequested(id); });

			wireSkip(skipButton, row);
			rows->push_back({ row, id, canReimport, canRegenerate });
		}

		// Re-import all: re-extract every open ghost whose source is present. Heavy (an ffmpeg run per video), so
		// confirm first and show a wait cursor for the batch.
		connect(reimportAllButton, &QPushButton::clicked, this, [this, rows, runBlanket] {
			if (QMessageBox::question(this, tr("Re-import all"),
			        tr("Re-extract frames for every re-importable video? This re-runs ffmpeg and can take a while."))
			    != QMessageBox::Yes)
				return;
			QApplication::setOverrideCursor(Qt::WaitCursor);
			const auto [done, failed] = runBlanket(*rows, [](const VideoRow& v) { return v.canReimport; },
				[this](VideoRow& v) { return m_callbacks.reimportRequested(v.id); }, tr("Re-imported."));
			QApplication::restoreOverrideCursor();
			QString msg = tr("Re-imported %1 video(s).").arg(done);
			if (failed)
				msg += "\n" + tr("%1 could not be re-imported.").arg(failed);
			QMessageBox::information(this, tr("Re-import all"), msg);
		});

		// Regenerate all previews: rebuild the card render for every open invisible entry that can be rebuilt (from
		// real frames where present, else the source). Lighter than re-import but still worth a wait cursor.
		connect(regenerateAllButton, &QPushButton::clicked, this, [this, rows, runBlanket] {
			QApplication::setOverrideCursor(Qt::WaitCursor);
			const auto [done, failed] = runBlanket(*rows, [](const VideoRow& v) { return v.canRegenerate; },
				[this](VideoRow& v) { return m_callbacks.regeneratePreviewRequested(v.id); }, tr("Preview regenerated."));
			QApplication::restoreOverrideCursor();
			QString msg = tr("Regenerated %1 preview(s).").arg(done);
			if (failed)
				msg += "\n" + tr("%1 could not be regenerated.").arg(failed);
			QMessageBox::information(this, tr("Regenerate all previews"), msg);
		});

		// Remove all: drop every still-open broken entry from the catalog. Destructive, so confirm with the count;
		// on-disk frame folders and source files are left untouched (removal only forgets the catalog record).
		connect(removeAllButton, &QPushButton::clicked, this, [this, rows, runBlanket] {
			const int open = static_cast<int>(std::count_if(rows->cbegin(), rows->cend(), [](const VideoRow& v) { return !v.row->closed; }));
			if (open == 0)
				return;
			if (QMessageBox::question(this, tr("Remove all"),
			        tr("Remove all %1 broken video entries from the catalog? Frame folders and source files on disk are not touched.").arg(open))
			    != QMessageBox::Yes)
				return;
			runBlanket(*rows, [](const VideoRow&) { return true; },
				[this](VideoRow& v) { m_callbacks.removeEntryRequested(v.id); return true; }, tr("Removed from catalog."));
			QMessageBox::information(this, tr("Remove all"), tr("Removed %1 broken video(s) from the catalog.").arg(open));
		});

		refreshBlanket();
	}

	if (!report.photoIssues.empty())
	{
		QHBoxLayout* header = addSectionHeader(tr("<b>Photos</b> - tracked photos whose source file is missing"));
		QPushButton* locateAllButton = new QPushButton(tr("Locate all..."), content);
		QPushButton* removeAllButton = new QPushButton(tr("Remove all"), content);
		header->addWidget(locateAllButton);
		header->addWidget(removeAllButton);

		struct MissingPhotoRow { std::shared_ptr<ResolvableRow> row; MediaId id; bool referenced; };
		auto rows = std::make_shared<std::vector<MissingPhotoRow>>();
		const std::weak_ptr<std::vector<MissingPhotoRow>> weakRows = rows;
		// Locate applies only to a referenced photo (an owned photo's file belongs in the library tree, so a missing
		// one is Remove-only); Remove applies to any open row.
		const auto refreshBlanket = [weakRows, locateAllButton, removeAllButton] {
			const auto rows = weakRows.lock();
			if (!rows)
				return;
			bool anyOpen = false, anyReferenced = false;
			for (const MissingPhotoRow& m : *rows)
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
			const QString name   = photo.sourcePath.isEmpty() ? tr("(no source recorded)") : QFileInfo(photo.sourcePath).fileName();
			const QString what   = photo.referenced ? tr("referenced file moved or unmounted") : tr("the library's own file is gone");
			const QString detail = photo.sourcePath.isEmpty() ? QString{} : "<br>" + photo.sourcePath;
			const auto [rowLayout, row] = addRow(name + "<br>" + what + detail);

			const MediaId id = photo.id;
			const QString recordedPath = photo.sourcePath;

			// Locate is offered only for a referenced photo - its file may have just moved, so pointing at it
			// repoints the entry. An owned photo lives in the library tree, so a missing owned file is Remove/Skip.
			QPushButton* locateButton = photo.referenced ? addRowButton(rowLayout, row, tr("Locate...")) : nullptr;
			QPushButton* removeButton = addRowButton(rowLayout, row, tr("Remove"));
			QPushButton* skipButton   = addRowButton(rowLayout, row, tr("Skip"));
			row->onClosed = refreshBlanket;

			if (locateButton)
				wireBrowse(locateButton, row,
					[this, recordedPath] { return browseForSourcePhoto(this, recordedPath); },
					[this, id](const QString& picked) { return m_callbacks.locatePhotoRequested(id, picked); },
					tr("Relocated to %1"),
					tr("Could not relocate - that file's identity is already tracked as a different item."));

			wireAction(removeButton, row, tr("Removed from catalog."), tr("Could not remove."),
				[this, id] { return m_callbacks.removeEntryRequested(id); });

			wireSkip(skipButton, row);
			rows->push_back({ row, id, photo.referenced });
		}

		// Locate all: search a chosen folder recursively and relink each open referenced photo to the file whose
		// identity (name + byte size) equals its own. The moved file keeps that identity, so the match is exact - and
		// it's the only signal available, since the original file is gone (there's no content left to compare against).
		connect(locateAllButton, &QPushButton::clicked, this, [this, rows, refreshBlanket] {
			const QString dir = QFileDialog::getExistingDirectory(this, tr("Select folder to search for missing photos"));
			if (dir.isEmpty())
				return;

			QHash<QString, QString> byIdentity;   // MediaId::key() -> first file found under dir carrying that identity
			QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext())
			{
				const QString path = it.next();
				if (!isSupportedImageFile(path))
					continue;
				const QString identity = MediaId::fromFile(path).key();
				if (!byIdentity.contains(identity))
					byIdentity.insert(identity, QDir::toNativeSeparators(path));
			}

			int relocated = 0, unmatched = 0, failed = 0;
			for (MissingPhotoRow& m : *rows)
			{
				if (m.row->closed || !m.referenced)
					continue;
				const auto found = byIdentity.constFind(m.id.key());
				if (found == byIdentity.constEnd()) { ++unmatched; continue; }
				if (m_callbacks.locatePhotoRequested(m.id, found.value())) { m.row->close(tr("Relocated to %1").arg(found.value())); ++relocated; }
				else ++failed;
			}
			refreshBlanket();

			QStringList msg{ tr("Relocated %1 photo(s).").arg(relocated) };
			if (unmatched)
				msg << tr("%1 had no matching file in that folder.").arg(unmatched);
			if (failed)
				msg << tr("%1 could not be relocated - the identity is already tracked as a different item.").arg(failed);
			QMessageBox::information(this, tr("Locate all photos"), msg.join(QStringLiteral("\n")));
		});

		// Remove all: drop every still-open missing-photo entry from the catalog. Destructive, so confirm with the
		// count; any file still on disk is left untouched (removal only forgets the catalog record).
		connect(removeAllButton, &QPushButton::clicked, this, [this, rows, runBlanket] {
			const int open = static_cast<int>(std::count_if(rows->cbegin(), rows->cend(), [](const MissingPhotoRow& m) { return !m.row->closed; }));
			if (open == 0)
				return;
			if (QMessageBox::question(this, tr("Remove all"),
			        tr("Remove all %1 missing-photo entries from the catalog? Any files still on disk are not touched.").arg(open))
			    != QMessageBox::Yes)
				return;
			runBlanket(*rows, [](const MissingPhotoRow&) { return true; },
				[this](MissingPhotoRow& m) { m_callbacks.removeEntryRequested(m.id); return true; }, tr("Removed from catalog."));
			QMessageBox::information(this, tr("Remove all"), tr("Removed %1 photo(s) from the catalog.").arg(open));
		});

		refreshBlanket();
	}

	contentLayout->addStretch(1);
	scroll->setWidget(content);
	outer->addWidget(scroll, 1);

	QHBoxLayout* buttons = new QHBoxLayout();
	buttons->addStretch(1);
	QPushButton* closeButton = new QPushButton(tr("Close"), this);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
	buttons->addWidget(closeButton);
	outer->addLayout(buttons);

	if (!restoreWindowGeometry(this, "integrityCheckDialog"))
		resize(700, 500);
}

IntegrityCheckDialog::~IntegrityCheckDialog()
{
	saveWindowGeometry(this, "integrityCheckDialog");
}
