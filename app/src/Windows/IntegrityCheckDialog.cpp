#include "Windows/IntegrityCheckDialog.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <utility>
#include <vector>

namespace {

constexpr const char* VIDEO_FILE_FILTER = "Video files (*.mp4 *.mov *.avi *.mkv *.flv);;All files (*)";

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
		tr("Differences between the catalog and what's actually on disk. Resolve each item individually, or "
		   "leave it for next time - nothing here is applied automatically."), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().InstructionText));
	outer->addWidget(instructions);

	QScrollArea* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	// The dialog-body scroll region gets the central sheet's QListWidget vocabulary (hairline frame, no fill
	// of its own) in place of the native sunken panel; QScrollArea isn't styled centrally because its other
	// use (FrameViewerWindow) wants no frame at all.
	scroll->setStyleSheet(QStringLiteral("QScrollArea { border: 1px solid %1; border-radius: %2px; background: transparent; }")
		.arg(Theme::current().BorderControl).arg(Theme::ControlRadius));
	QWidget* content = new QWidget(scroll);
	QVBoxLayout* contentLayout = new QVBoxLayout(content);

	// Wires a row's button: on click, runs action; on success, shows successText and disables every button
	// in the row (the row stays visible as a record of what happened); on failure, warns and leaves the row
	// active so the user can retry (e.g. Browse a different file).
	const auto wireAction = [this](QPushButton* button, QLabel* statusLabel, const std::vector<QPushButton*>& rowButtons,
	                                const QString& successText, const QString& failureText, std::function<bool()> action) {
		connect(button, &QPushButton::clicked, this, [=] {
			if (action())
			{
				statusLabel->setText(successText);
				for (QPushButton* b : rowButtons)
					b->setEnabled(false);
			}
			else
			{
				QMessageBox::warning(this, tr("Catalog integrity check"), failureText);
			}
		});
	};

	// A "Skip" button just acknowledges the row without acting - disables the row's other buttons so it
	// reads as dealt-with for this session; it resurfaces on the next scan since nothing was changed.
	const auto wireSkip = [](QPushButton* skipButton, QLabel* statusLabel, const std::vector<QPushButton*>& rowButtons) {
		connect(skipButton, &QPushButton::clicked, skipButton, [=] {
			statusLabel->setText(tr("%1  (skipped)").arg(statusLabel->text()));
			for (QPushButton* b : rowButtons)
				b->setEnabled(false);
		});
	};

	// Wires a "browse then act" button (the untracked Register and the photo Locate share this shape): opens a
	// file picker; if the user picked something, runs action(picked) and, on success, sets the row status from
	// successFmt (a "...%1..." string filled with the picked path) and disables the row; on failure, warns.
	const auto wireBrowse = [this](QPushButton* button, QLabel* statusLabel, const std::vector<QPushButton*>& rowButtons,
	                               std::function<QString()> browse, std::function<bool(const QString&)> action,
	                               const QString& successFmt, const QString& failureText) {
		connect(button, &QPushButton::clicked, this, [=] {
			const QString picked = browse();
			if (picked.isEmpty())
				return;
			if (action(picked))
			{
				statusLabel->setText(successFmt.arg(picked));
				for (QPushButton* b : rowButtons)
					b->setEnabled(false);
			}
			else
				QMessageBox::warning(this, tr("Catalog integrity check"), failureText);
		});
	};

	// One issue per row, drawn as a hairline card (BorderSubtle - a passive separator, not an interactive
	// control) rather than the native StyledPanel bevel. One sheet string shared by every row.
	const QString rowStyle = QStringLiteral("QFrame#integrityRow { border: 1px solid %1; border-radius: %2px; }")
		.arg(Theme::current().BorderSubtle).arg(Theme::ControlRadius);
	const auto addRow = [&](const QString& statusText) -> std::pair<QFrame*, QLabel*> {
		QFrame* row = new QFrame(content);
		row->setObjectName("integrityRow");
		row->setStyleSheet(rowStyle);
		QHBoxLayout* rowLayout = new QHBoxLayout(row);
		QLabel* statusLabel = new QLabel(statusText, row);
		statusLabel->setWordWrap(true);
		rowLayout->addWidget(statusLabel, 1);
		contentLayout->addWidget(row);
		return { row, statusLabel };
	};

	if (!report.untracked.empty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Untracked folders</b> - on disk, not in the catalog"), content));
		for (const CatalogIntegrity::UntrackedFolder& u : report.untracked)
		{
			const auto [row, statusLabel] = addRow(QFileInfo(u.folderPath).fileName());
			QHBoxLayout* rowLayout = static_cast<QHBoxLayout*>(row->layout());

			QPushButton* browseButton = new QPushButton(tr("Browse..."), row);
			QPushButton* skipButton   = new QPushButton(tr("Skip"), row);
			rowLayout->addWidget(browseButton);
			rowLayout->addWidget(skipButton);

			const QString folderPath = u.folderPath;
			const std::vector<QPushButton*> rowButtons{ browseButton, skipButton };

			// The only resolution is to point at a source video, which registers the folder against it.
			wireBrowse(browseButton, statusLabel, rowButtons,
				[this, folderPath] { return browseForSourceVideo(this, folderPath); },
				[this, folderPath](const QString& picked) { return m_callbacks.registerRequested(folderPath, picked); },
				tr("Registered with %1"),
				tr("Could not register - that file's identity is already tracked under a different folder."));

			wireSkip(skipButton, statusLabel, rowButtons);
		}
	}

	if (!report.issues.empty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Problems</b> - tracked videos whose files don't match the catalog"), content));
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

			const auto [row, statusLabel] = addRow(status);
			QHBoxLayout* rowLayout = static_cast<QHBoxLayout*>(row->layout());

			const MediaId id = issue.id;

			// Which fixes are offered follows the grid's recovery notes: Re-import re-extracts a gone deliverable
			// (needs the source); Regenerate rebuilds a missing preview from real frames or the source; Mark-split
			// fixes only a stale flag. Remove/Skip are always available.
			const bool canReimport   = issue.isGhost() && issue.sourcePresent;
			const bool canRegenerate = issue.isInvisible() && !issue.isGhost() && (issue.realFramesPresent || issue.sourcePresent);
			const bool canMarkSplit  = issue.isStale() && !issue.isInvisible();

			QPushButton* reimportButton   = canReimport   ? new QPushButton(tr("Re-import"), row) : nullptr;
			QPushButton* regenerateButton = canRegenerate ? new QPushButton(tr("Regenerate preview"), row) : nullptr;
			QPushButton* markSplitButton  = canMarkSplit  ? new QPushButton(tr("Mark as fully split"), row) : nullptr;
			QPushButton* removeButton      = new QPushButton(tr("Remove"), row);
			QPushButton* skipButton        = new QPushButton(tr("Skip"), row);

			std::vector<QPushButton*> rowButtons;
			for (QPushButton* b : { reimportButton, regenerateButton, markSplitButton, removeButton, skipButton })
				if (b)
				{
					rowLayout->addWidget(b);
					rowButtons.push_back(b);
				}

			if (reimportButton)
				wireAction(reimportButton, statusLabel, rowButtons, tr("Re-imported."),
					tr("Re-import failed - see the error dialog for details."),
					[this, id] { return m_callbacks.reimportRequested(id); });

			if (regenerateButton)
				wireAction(regenerateButton, statusLabel, rowButtons, tr("Preview regenerated."),
					tr("Could not regenerate the preview - the source may be unavailable."),
					[this, id] { return m_callbacks.regeneratePreviewRequested(id); });

			if (markSplitButton)
				wireAction(markSplitButton, statusLabel, rowButtons, tr("Marked as fully split."), tr("Could not update the entry."),
					[this, id] { return m_callbacks.markSplitRequested(id); });

			wireAction(removeButton, statusLabel, rowButtons, tr("Removed from catalog."), tr("Could not remove."),
				[this, id] { return m_callbacks.removeEntryRequested(id); });

			wireSkip(skipButton, statusLabel, rowButtons);
		}
	}

	if (!report.photoIssues.empty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Photos</b> - tracked photos whose source file is missing"), content));
		for (const CatalogIntegrity::PhotoIssue& photo : report.photoIssues)
		{
			const QString name   = photo.sourcePath.isEmpty() ? tr("(no source recorded)") : QFileInfo(photo.sourcePath).fileName();
			const QString what   = photo.referenced ? tr("referenced file moved or unmounted") : tr("the library's own file is gone");
			const QString detail = photo.sourcePath.isEmpty() ? QString{} : "<br>" + photo.sourcePath;
			const auto [row, statusLabel] = addRow(name + "<br>" + what + detail);
			QHBoxLayout* rowLayout = static_cast<QHBoxLayout*>(row->layout());

			const MediaId id = photo.id;
			const QString recordedPath = photo.sourcePath;

			// Locate is offered only for a referenced photo - its file may have just moved, so pointing at it
			// repoints the entry. An owned photo lives in the library tree, so a missing owned file is Remove/Skip.
			QPushButton* locateButton = photo.referenced ? new QPushButton(tr("Locate..."), row) : nullptr;
			QPushButton* removeButton = new QPushButton(tr("Remove"), row);
			QPushButton* skipButton   = new QPushButton(tr("Skip"), row);

			std::vector<QPushButton*> rowButtons;
			for (QPushButton* b : { locateButton, removeButton, skipButton })
				if (b)
				{
					rowLayout->addWidget(b);
					rowButtons.push_back(b);
				}

			if (locateButton)
				wireBrowse(locateButton, statusLabel, rowButtons,
					[this, recordedPath] { return browseForSourcePhoto(this, recordedPath); },
					[this, id](const QString& picked) { return m_callbacks.locatePhotoRequested(id, picked); },
					tr("Relocated to %1"),
					tr("Could not relocate - that file's identity is already tracked as a different item."));

			wireAction(removeButton, statusLabel, rowButtons, tr("Removed from catalog."), tr("Could not remove."),
				[this, id] { return m_callbacks.removeEntryRequested(id); });

			wireSkip(skipButton, statusLabel, rowButtons);
		}
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
