#pragma once

#include "Windows/IntegrityCheckDialog.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QApplication>
#include <QCoreApplication>
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

#include <algorithm>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// ============================================================================
// Private implementation detail of IntegrityCheckDialog - included only by its .cpp. Header-only by design:
// declarations first, out-of-line definitions below in the same file.
// ============================================================================

// Shared state for one issue row: its status label, its buttons, and whether it's been dealt with. Both the row's
// own buttons and its section's blanket action resolve through close(), so a row settled either way updates once and
// is skipped by the other. onClosed lets the section re-evaluate which blanket buttons still apply; reorderSection
// sinks the settled row below its section's still-open ones. Rows are owned by IntegrityCheckSections::m_rows, which
// the dialog keeps alive as long as itself - so every handler capturing a ResolvableRow* dies no later than the row.
struct ResolvableRow
{
	QLabel*                   statusLabel = nullptr;
	std::vector<QPushButton*> buttons;
	bool                      closed = false;
	std::function<void()>     onClosed;
	std::function<void()>     reorderSection;

	// Settles the row: rewrites its status (a resolved row is prefixed with a check; either way the text dims),
	// disables its buttons, sinks it below the section's still-open rows, and lets the section re-evaluate its
	// blanket buttons. resolved=false is the Skip path - dimmed and sunk like the rest, but without the check.
	void close(const QString& status, bool resolved = true);
	// Runs one resolution and, on success, closes the row (records successText, disables the row's buttons).
	// Returns success so the caller - a single-row button, or a blanket driver - can react (warn, or tally).
	bool tryResolve(const QString& successText, const std::function<bool()>& action);
};

// Builds the dialog's issue sections into the scroll-area content widget and services their button handlers:
// one hairline-card row per finding with its own resolution buttons, plus per-section blanket actions that run
// the same per-row callbacks in a loop. Owns the row state those handlers share, so it must outlive the buttons -
// the dialog holds it as a member.
class IntegrityCheckSections
{
public:
	IntegrityCheckSections(const Catalog& catalog, const CatalogIntegrity::IntegrityReport& report, IntegrityCheckDialog::Callbacks& callbacks,
	                       QWidget* content, QVBoxLayout* contentLayout, QWidget* dialog);

private:
	// Per-section row state: the shared ResolvableRow plus what the section's blanket actions need per item.
	struct AdoptRow        { ResolvableRow* row; QString filePath; };
	struct VideoRow        { ResolvableRow* row; MediaId id; bool canReimport; bool canRegenerate; bool sourceMissing; };
	struct MissingPhotoRow { ResolvableRow* row; MediaId id; bool referenced; };

	void buildUntrackedFolders(const CatalogIntegrity::IntegrityReport& report);
	void buildUntrackedPhotos(const CatalogIntegrity::IntegrityReport& report);
	void buildVideoIssues(const CatalogIntegrity::IntegrityReport& report);
	void buildMissingPhotos(const CatalogIntegrity::IntegrityReport& report);

	// Adds one issue row (a hairline card holding the status label) and returns its layout for buttons plus the row state.
	std::pair<QHBoxLayout*, ResolvableRow*> addRow(const QString& statusText);
	// Creates one row button, adds it to the row's layout, and registers it so the row's close() disables it later.
	static QPushButton* addRowButton(QHBoxLayout* rowLayout, ResolvableRow* row, const QString& text);
	// A section header: the bold class caption on the left, its blanket-action buttons (appended by the caller)
	// pushed to the right. Returns the header layout so the caller can add those buttons after the stretch.
	QHBoxLayout* addSectionHeader(const QString& html);

	// Wires a single-row action button: on click run the resolution; on success the row records it and disables
	// (stays visible as a record of what happened); on failure warn and leave the row active so the user can retry.
	// The blanket drivers reuse the very same callbacks but aggregate into one summary instead of warning per item.
	void wireAction(QPushButton* button, ResolvableRow* row, const QString& successText, const QString& failureText,
	                std::function<bool()> action);
	// Wires a "browse then act" button (the untracked Register and the photo Locate share this shape): opens a file
	// picker; if the user picked something, runs action(picked) and, on success, sets the row status from successFmt
	// (a "...%1..." string filled with the picked path); on failure warns.
	void wireBrowse(QPushButton* button, ResolvableRow* row, std::function<QString()> browse,
	                std::function<bool(const QString&)> action, const QString& successFmt, const QString& failureText);
	// A "Skip" button just acknowledges the row without acting - closes it so it reads as dealt-with for this session;
	// it resurfaces on the next scan since nothing was changed.
	static void wireSkip(QPushButton* skipButton, ResolvableRow* row);

	// Opens a file picker for the user to manually point at a source video, starting near hint (a path that
	// doesn't have to exist - only its directory is used) if given, else a sensible catalog-wide default.
	QString browseForSourceVideo(const QString& hint) const;
	// The photo counterpart (the referenced-photo Locate resolution): point at an image file, starting in hint's
	// directory if given. The filter is built from the app-wide image extensions (IMAGE_FILE_FILTERS).
	QString browseForSourcePhoto(const QString& hint) const;

	// The batch analog of tryResolve: run a blanket action over rows. For each still-open row that `applies`, run the
	// resolution; a success closes the row and counts toward done, a failure counts toward failed. The {done, failed}
	// tally lets the driver compose its summary; rows re-enable their blanket buttons via onClosed.
	template <class Row, class Applies, class Act>
	static std::pair<int, int> runBlanket(std::vector<Row>& rows, Applies applies, Act act, const QString& successText);

	// The blanket drivers' summary box: doneFmt.arg(done), plus a failedFmt.arg(failed) line when anything failed.
	void showBlanketTally(const QString& title, const QString& doneFmt, int done, const QString& failedFmt, int failed) const;

	// What locateAllByIdentity reports back: how the chosen folder's contents matched the still-open rows.
	struct LocateTally { int relocated = 0; int unmatched = 0; int failed = 0; };
	// The shared "Locate all" driver behind both the missing-photo and moved-source-video sections: ask for a folder,
	// index every file under it that `filePredicate` accepts by its MediaId identity (name + size), then relink each
	// still-open row that `applies` to the file carrying its identity. The moved file keeps that identity, so the
	// match is exact - and it's the only signal left, since the original is gone (no content to compare against).
	// Closes each relinked row; returns the tally, or nullopt if the user cancelled the folder pick. Refresh and
	// summary wording stay at the call site (the two sections count photos vs videos and re-enable different buttons).
	template <class Row, class Applies, class Relocate>
	std::optional<LocateTally> locateAllByIdentity(std::vector<Row>& rows, Applies applies,
	                                               const std::function<bool(const QString&)>& filePredicate, Relocate relocate,
	                                               const QString& folderPrompt);

	// The shared "Remove all" driver: destructive, so confirm with the count of still-open rows first (questionFmt and
	// doneFmt are "...%1..." strings filled with it). Removal always succeeds - it only forgets the catalog record.
	template <class Row>
	void confirmAndRemoveAll(std::vector<Row>& rows, const QString& title, const QString& questionFmt, const QString& doneFmt);

	const Catalog& m_catalog;
	IntegrityCheckDialog::Callbacks& m_callbacks;
	QWidget*     m_dialog;   // parent for message boxes and file pickers
	QWidget*     m_content;  // the scroll-area content widget rows and headers are parented to
	QVBoxLayout* m_layout;   // its layout
	QVBoxLayout* m_currentRowsLayout = nullptr;  // the section being built: its own rows container, where addRow appends
	const QString m_rowStyle;

	std::deque<ResolvableRow>    m_rows;  // deque: handlers hold ResolvableRow*, so addresses must be stable
	std::vector<AdoptRow>        m_untrackedPhotoRows;
	std::vector<VideoRow>        m_videoRows;
	std::vector<MissingPhotoRow> m_missingPhotoRows;
};

// ============================================================================
// Definitions
// ============================================================================

inline void ResolvableRow::close(const QString& status, bool resolved)
{
	statusLabel->setText(resolved ? QStringLiteral("✓ ") + status : status);
	statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().MutedText));
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
	: m_catalog(catalog), m_callbacks(callbacks), m_dialog(dialog), m_content(content), m_layout(contentLayout),
	  // One issue per row, drawn as a hairline card (BorderSubtle - a passive separator, not an interactive
	  // control) rather than the native StyledPanel bevel. One sheet string shared by every row.
	  m_rowStyle(QStringLiteral("QFrame#integrityRow { border: 1px solid %1; border-radius: %2px; }")
		.arg(Theme::current().BorderSubtle).arg(Theme::ControlRadius))
{
	buildUntrackedFolders(report);
	buildUntrackedPhotos(report);
	buildVideoIssues(report);
	buildMissingPhotos(report);
}

inline void IntegrityCheckSections::buildUntrackedFolders(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.untracked.empty())
		return;

	// Registering an untracked folder needs a per-folder source pick (name alone can't safely identify the
	// source video), so this class has no blanket action - only per-row Browse.
	addSectionHeader(QObject::tr("<b>Untracked folders</b> - on disk, not in the catalog"));
	for (const CatalogIntegrity::UntrackedFolder& u : report.untracked)
	{
		const auto [rowLayout, row] = addRow(QFileInfo(u.folderPath).fileName());

		QPushButton* browseButton = addRowButton(rowLayout, row, QObject::tr("Browse..."));
		QPushButton* skipButton   = addRowButton(rowLayout, row, QObject::tr("Skip"));

		const QString folderPath = u.folderPath;

		// The only resolution is to point at a source video, which registers the folder against it.
		wireBrowse(browseButton, row,
			[this, folderPath] { return browseForSourceVideo(folderPath); },
			[this, folderPath](const QString& picked) { return m_callbacks.registerRequested(folderPath, picked); },
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
	QPushButton* addAllButton = new QPushButton(QObject::tr("Add all"), m_content);
	header->addWidget(addAllButton);

	// Add all applies to any still-open row, so it stays enabled while one exists.
	const auto refreshBlanket = [this, addAllButton] {
		addAllButton->setEnabled(std::any_of(m_untrackedPhotoRows.cbegin(), m_untrackedPhotoRows.cend(),
			[](const AdoptRow& r) { return !r.row->closed; }));
	};

	for (const CatalogIntegrity::UntrackedPhoto& p : report.untrackedPhotos)
	{
		const auto [rowLayout, row] = addRow(QFileInfo(p.filePath).fileName() + "<br>" + QObject::tr("label: %1").arg(p.labelName));

		QPushButton* addButton  = addRowButton(rowLayout, row, QObject::tr("Add to catalog"));
		QPushButton* skipButton = addRowButton(rowLayout, row, QObject::tr("Skip"));
		row->onClosed = refreshBlanket;

		const QString filePath = p.filePath;

		// The file already lives in its label's Photos dir, so adopting it is a one-click register as an owned photo.
		wireAction(addButton, row, QObject::tr("Added to catalog."),
			QObject::tr("Could not add - a file with the same name and size is already tracked elsewhere."),
			[this, filePath] { return m_callbacks.adoptPhotoRequested(filePath); });

		wireSkip(skipButton, row);
		m_untrackedPhotoRows.push_back({ row, filePath });
	}

	// Add all: adopt every still-open untracked photo. Each already sits in its label's Photos dir, so this is
	// just the row action run in a loop; a name+size clash counts as a failure and leaves that row open.
	QObject::connect(addAllButton, &QPushButton::clicked, addAllButton, [this] {
		const auto [added, failed] = runBlanket(m_untrackedPhotoRows, [](const AdoptRow&) { return true; },
			[this](AdoptRow& r) { return m_callbacks.adoptPhotoRequested(r.filePath); }, QObject::tr("Added to catalog."));
		showBlanketTally(QObject::tr("Add all photos"), QObject::tr("Added %1 photo(s) to the catalog."), added,
			QObject::tr("%1 could not be added - a file with the same name and size is already tracked."), failed);
	});

	refreshBlanket();
}

inline void IntegrityCheckSections::buildVideoIssues(const CatalogIntegrity::IntegrityReport& report)
{
	if (report.issues.empty())
		return;

	QHBoxLayout* header = addSectionHeader(QObject::tr("<b>Problems</b> - tracked videos whose files don't match the catalog"));
	QPushButton* locateAllButton     = new QPushButton(QObject::tr("Locate all..."), m_content);
	QPushButton* reimportAllButton   = new QPushButton(QObject::tr("Re-import all"), m_content);
	QPushButton* regenerateAllButton = new QPushButton(QObject::tr("Regenerate all previews"), m_content);
	QPushButton* removeAllButton     = new QPushButton(QObject::tr("Remove all"), m_content);
	header->addWidget(locateAllButton);
	header->addWidget(reimportAllButton);
	header->addWidget(regenerateAllButton);
	header->addWidget(removeAllButton);

	// Each blanket action stays enabled only while some open row still admits it: Remove applies to any open row;
	// Locate to a row whose source is missing; Re-import to a ghost whose source is present; Regenerate to a
	// rebuildable-but-invisible entry.
	const auto refreshBlanket = [this, locateAllButton, reimportAllButton, regenerateAllButton, removeAllButton] {
		bool anyOpen = false, anyLocate = false, anyReimport = false, anyRegenerate = false;
		for (const VideoRow& v : m_videoRows)
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
		// One line per broken video listing everything wrong with it - the grid's verdicts are orthogonal,
		// so several can apply at once (e.g. frames gone and source also missing).
		QStringList problems;
		if (issue.isGhost())
			problems << QObject::tr("frames are gone");
		if (issue.isInvisible())
			problems << QObject::tr("no preview - the card can't be shown");
		if (issue.isStale())
			problems << QObject::tr("marked not-yet-split, but frames exist");
		if (issue.sourceMissing())
			problems << (issue.sourcePath.isEmpty() ? QObject::tr("no source recorded")
			                                        : QObject::tr("source missing: %1").arg(issue.sourcePath));
		const QString status = QFileInfo(issue.folder).fileName() + "<br>" + problems.join(QStringLiteral("; "));

		const auto [rowLayout, row] = addRow(status);

		const MediaId id = issue.id;
		const QString recordedSource = issue.sourcePath;

		// Which fixes are offered follows the grid's recovery notes: Locate repoints a moved/unmounted source (and is
		// the precondition for Re-import when the source is gone); Re-import re-extracts a gone deliverable (needs the
		// source present); Regenerate rebuilds a missing preview from real frames or the source; Mark-split fixes only
		// a stale flag. Remove/Skip are always available.
		const bool canReimport   = issue.isGhost() && issue.sourcePresent;
		const bool canRegenerate = issue.isInvisible() && !issue.isGhost() && (issue.realFramesPresent || issue.sourcePresent);
		const bool canMarkSplit  = issue.isStale() && !issue.isInvisible();
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
				[this, id](const QString& picked) { return m_callbacks.locateSourceRequested(id, picked); },
				QObject::tr("Relocated to %1"),
				QObject::tr("Could not relocate - that file's identity is already tracked as a different item."));

		if (reimportButton)
			wireAction(reimportButton, row, QObject::tr("Re-imported."),
				QObject::tr("Re-import failed - see the error dialog for details."),
				[this, id] { return m_callbacks.reimportRequested(id); });

		if (regenerateButton)
			wireAction(regenerateButton, row, QObject::tr("Preview regenerated."),
				QObject::tr("Could not regenerate the preview - the source may be unavailable."),
				[this, id] { return m_callbacks.regeneratePreviewRequested(id); });

		if (markSplitButton)
			wireAction(markSplitButton, row, QObject::tr("Marked as fully split."), QObject::tr("Could not update the entry."),
				[this, id] { return m_callbacks.markSplitRequested(id); });

		wireAction(removeButton, row, QObject::tr("Removed from catalog."), QObject::tr("Could not remove."),
			[this, id] { return m_callbacks.removeEntryRequested(id); });

		wireSkip(skipButton, row);
		m_videoRows.push_back({ row, id, canReimport, canRegenerate, canLocate });
	}

	// Locate all: relink every open source-missing video to its identity-match (name + size) under a chosen folder -
	// the batch form of the per-row Locate, for when a whole tree of sources moved at once. A relinked ghost becomes
	// re-importable, but stays a ghost until the next scan surfaces it with Re-import enabled.
	QObject::connect(locateAllButton, &QPushButton::clicked, locateAllButton, [this, refreshBlanket] {
		const auto tally = locateAllByIdentity(m_videoRows, [](const VideoRow& v) { return v.sourceMissing; }, isSupportedVideoFile,
			[this](const MediaId& id, const QString& picked) { return m_callbacks.locateSourceRequested(id, picked); },
			QObject::tr("Select folder to search for moved source videos"));
		if (!tally)
			return;
		refreshBlanket();

		QStringList msg{ QObject::tr("Relocated %1 video(s).").arg(tally->relocated) };
		if (tally->unmatched)
			msg << QObject::tr("%1 had no matching file in that folder.").arg(tally->unmatched);
		if (tally->failed)
			msg << QObject::tr("%1 could not be relocated - the identity is already tracked as a different item.").arg(tally->failed);
		QMessageBox::information(m_dialog, QObject::tr("Locate all videos"), msg.join(QStringLiteral("\n")));
	});

	// Re-import all: re-extract every open ghost whose source is present. Heavy (an ffmpeg run per video), so
	// confirm first and show a wait cursor for the batch.
	QObject::connect(reimportAllButton, &QPushButton::clicked, reimportAllButton, [this] {
		if (QMessageBox::question(m_dialog, QObject::tr("Re-import all"),
		        QObject::tr("Re-extract frames for every re-importable video? This re-runs ffmpeg and can take a while."))
		    != QMessageBox::Yes)
			return;
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const auto [done, failed] = runBlanket(m_videoRows, [](const VideoRow& v) { return v.canReimport; },
			[this](VideoRow& v) { return m_callbacks.reimportRequested(v.id); }, QObject::tr("Re-imported."));
		QApplication::restoreOverrideCursor();
		showBlanketTally(QObject::tr("Re-import all"), QObject::tr("Re-imported %1 video(s)."), done, QObject::tr("%1 could not be re-imported."), failed);
	});

	// Regenerate all previews: rebuild the card render for every open invisible entry that can be rebuilt (from
	// real frames where present, else the source). Lighter than re-import but still worth a wait cursor.
	QObject::connect(regenerateAllButton, &QPushButton::clicked, regenerateAllButton, [this] {
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const auto [done, failed] = runBlanket(m_videoRows, [](const VideoRow& v) { return v.canRegenerate; },
			[this](VideoRow& v) { return m_callbacks.regeneratePreviewRequested(v.id); }, QObject::tr("Preview regenerated."));
		QApplication::restoreOverrideCursor();
		showBlanketTally(QObject::tr("Regenerate all previews"), QObject::tr("Regenerated %1 preview(s)."), done,
			QObject::tr("%1 could not be regenerated."), failed);
	});

	QObject::connect(removeAllButton, &QPushButton::clicked, removeAllButton, [this] {
		confirmAndRemoveAll(m_videoRows, QObject::tr("Remove all"),
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
	QPushButton* locateAllButton = new QPushButton(QObject::tr("Locate all..."), m_content);
	QPushButton* removeAllButton = new QPushButton(QObject::tr("Remove all"), m_content);
	header->addWidget(locateAllButton);
	header->addWidget(removeAllButton);

	// Locate applies only to a referenced photo (an owned photo's file belongs in the library tree, so a missing
	// one is Remove-only); Remove applies to any open row.
	const auto refreshBlanket = [this, locateAllButton, removeAllButton] {
		bool anyOpen = false, anyReferenced = false;
		for (const MissingPhotoRow& m : m_missingPhotoRows)
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

		// Locate is offered only for a referenced photo - its file may have just moved, so pointing at it
		// repoints the entry. An owned photo lives in the library tree, so a missing owned file is Remove/Skip.
		QPushButton* locateButton = photo.referenced ? addRowButton(rowLayout, row, QObject::tr("Locate...")) : nullptr;
		QPushButton* removeButton = addRowButton(rowLayout, row, QObject::tr("Remove"));
		QPushButton* skipButton   = addRowButton(rowLayout, row, QObject::tr("Skip"));
		row->onClosed = refreshBlanket;

		if (locateButton)
			wireBrowse(locateButton, row,
				[this, recordedPath] { return browseForSourcePhoto(recordedPath); },
				[this, id](const QString& picked) { return m_callbacks.locatePhotoRequested(id, picked); },
				QObject::tr("Relocated to %1"),
				QObject::tr("Could not relocate - that file's identity is already tracked as a different item."));

		wireAction(removeButton, row, QObject::tr("Removed from catalog."), QObject::tr("Could not remove."),
			[this, id] { return m_callbacks.removeEntryRequested(id); });

		wireSkip(skipButton, row);
		m_missingPhotoRows.push_back({ row, id, photo.referenced });
	}

	// Locate all: relink every open referenced photo to its identity-match under a chosen folder (see locateAllByIdentity).
	QObject::connect(locateAllButton, &QPushButton::clicked, locateAllButton, [this, refreshBlanket] {
		const auto tally = locateAllByIdentity(m_missingPhotoRows, [](const MissingPhotoRow& m) { return m.referenced; }, isSupportedImageFile,
			[this](const MediaId& id, const QString& picked) { return m_callbacks.locatePhotoRequested(id, picked); },
			QObject::tr("Select folder to search for missing photos"));
		if (!tally)
			return;
		refreshBlanket();

		QStringList msg{ QObject::tr("Relocated %1 photo(s).").arg(tally->relocated) };
		if (tally->unmatched)
			msg << QObject::tr("%1 had no matching file in that folder.").arg(tally->unmatched);
		if (tally->failed)
			msg << QObject::tr("%1 could not be relocated - the identity is already tracked as a different item.").arg(tally->failed);
		QMessageBox::information(m_dialog, QObject::tr("Locate all photos"), msg.join(QStringLiteral("\n")));
	});

	QObject::connect(removeAllButton, &QPushButton::clicked, removeAllButton, [this] {
		confirmAndRemoveAll(m_missingPhotoRows, QObject::tr("Remove all"),
			QObject::tr("Remove all %1 missing-photo entries from the catalog? Any files still on disk are not touched."),
			QObject::tr("Removed %1 photo(s) from the catalog."));
	});

	refreshBlanket();
}

inline std::pair<QHBoxLayout*, ResolvableRow*> IntegrityCheckSections::addRow(const QString& statusText)
{
	QVBoxLayout* rowsLayout = m_currentRowsLayout;   // this section's container; captured per-row for the sink below
	QFrame* frame = new QFrame(rowsLayout->parentWidget());
	frame->setObjectName("integrityRow");
	frame->setStyleSheet(m_rowStyle);
	QHBoxLayout* rowLayout = new QHBoxLayout(frame);
	ResolvableRow& row = m_rows.emplace_back();
	row.statusLabel = new QLabel(statusText, frame);
	row.statusLabel->setWordWrap(true);
	rowLayout->addWidget(row.statusLabel, 1);
	rowsLayout->addWidget(frame);
	// Settling the row re-appends it, dropping it below the section's still-open rows (its layout holds only them).
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
	headerRow->addWidget(new QLabel(html, m_content));
	headerRow->addStretch(1);
	m_layout->addLayout(headerRow);

	// Each section's rows go in their own container so a settled row can sink to the section's bottom by simple
	// re-append, with no index bookkeeping and without disturbing other sections. Zero margins so rows stay flush
	// with the header; spacing is left at the layout default, matching m_layout.
	QWidget* rowsContainer = new QWidget(m_content);
	m_currentRowsLayout = new QVBoxLayout(rowsContainer);
	m_currentRowsLayout->setContentsMargins(0, 0, 0, 0);
	m_layout->addWidget(rowsContainer);

	return headerRow;
}

inline void IntegrityCheckSections::wireAction(QPushButton* button, ResolvableRow* row, const QString& successText,
                                               const QString& failureText, std::function<bool()> action)
{
	QObject::connect(button, &QPushButton::clicked, button, [this, row, successText, failureText, action = std::move(action)] {
		if (!row->tryResolve(successText, action))
			QMessageBox::warning(m_dialog, QObject::tr("Catalog integrity check"), failureText);
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
				QMessageBox::warning(m_dialog, QObject::tr("Catalog integrity check"), failureText);
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
	const QString startDir = hint.isEmpty() ? m_catalog.anySourceDir() : QFileInfo(hint).absolutePath();
	return QFileDialog::getOpenFileName(m_dialog, QObject::tr("Select source video"), startDir,
		QObject::tr("Video files (*.mp4 *.mov *.avi *.mkv *.flv);;All files (*)"));
}

inline QString IntegrityCheckSections::browseForSourcePhoto(const QString& hint) const
{
	const QString startDir = hint.isEmpty() ? QString{} : QFileInfo(hint).absolutePath();
	const QString filter = QObject::tr("Image files (%1);;All files (*)").arg(IMAGE_FILE_FILTERS.join(QStringLiteral(" ")));
	return QFileDialog::getOpenFileName(m_dialog, QObject::tr("Locate photo"), startDir, filter);
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
	QMessageBox::information(m_dialog, title, msg);
}

template <class Row, class Applies, class Relocate>
std::optional<IntegrityCheckSections::LocateTally> IntegrityCheckSections::locateAllByIdentity(
	std::vector<Row>& rows, Applies applies, const std::function<bool(const QString&)>& filePredicate, Relocate relocate,
	const QString& folderPrompt)
{
	const QString dir = QFileDialog::getExistingDirectory(m_dialog, folderPrompt);
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
	if (QMessageBox::question(m_dialog, title, questionFmt.arg(open)) != QMessageBox::Yes)
		return;
	runBlanket(rows, [](const Row&) { return true; },
		[this](Row& r) { m_callbacks.removeEntryRequested(r.id); return true; }, QObject::tr("Removed from catalog."));
	QMessageBox::information(m_dialog, title, doneFmt.arg(open));
}
