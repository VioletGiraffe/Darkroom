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

} // namespace

void IntegrityCheckDialog::scanAndShowUi(Callbacks callbacks, QWidget* parent)
{
	const Catalog::IntegrityReport report = Catalog::instance().scanIntegrity();
	if (report.isEmpty())
	{
		QMessageBox::information(parent, tr("Catalog integrity check"), tr("No issues found - the catalog matches what's on disk."));
		return;
	}

	IntegrityCheckDialog dialog(report, std::move(callbacks), parent);
	dialog.exec();
}

IntegrityCheckDialog::IntegrityCheckDialog(const Catalog::IntegrityReport& report, Callbacks callbacks, QWidget* parent)
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

	const auto addRow = [&](const QString& statusText) -> std::pair<QFrame*, QLabel*> {
		QFrame* row = new QFrame(content);
		row->setFrameShape(QFrame::StyledPanel);
		QHBoxLayout* rowLayout = new QHBoxLayout(row);
		QLabel* statusLabel = new QLabel(statusText, row);
		statusLabel->setWordWrap(true);
		rowLayout->addWidget(statusLabel, 1);
		contentLayout->addWidget(row);
		return { row, statusLabel };
	};

	if (!report.relinkable.empty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Relink</b> - the source file reappeared"), content));
		for (const Catalog::RelinkCandidate& candidate : report.relinkable)
		{
			const auto [row, statusLabel] = addRow(
				tr("%1<br>Recorded source: %2").arg(QFileInfo(candidate.folder).fileName(), candidate.recordedSourcePath));
			QHBoxLayout* rowLayout = static_cast<QHBoxLayout*>(row->layout());

			QPushButton* relinkButton = new QPushButton(tr("Relink"), row);
			QPushButton* browseButton = new QPushButton(tr("Browse..."), row);
			QPushButton* skipButton   = new QPushButton(tr("Skip"), row);
			rowLayout->addWidget(relinkButton);
			rowLayout->addWidget(browseButton);
			rowLayout->addWidget(skipButton);

			const MediaId placeholderId = candidate.placeholderId;
			const QString recordedPath = candidate.recordedSourcePath;
			const std::vector<QPushButton*> rowButtons{ relinkButton, browseButton, skipButton };

			wireAction(relinkButton, statusLabel, rowButtons, tr("Relinked."),
				tr("Could not relink - that source file is already tracked under a different folder."),
				[this, placeholderId, recordedPath] { return m_callbacks.relinkRequested(placeholderId, recordedPath); });

			connect(browseButton, &QPushButton::clicked, this, [this, placeholderId, recordedPath, relinkButton, browseButton, skipButton, statusLabel] {
				const QString picked = browseForSourceVideo(this, recordedPath);
				if (picked.isEmpty())
					return;
				if (m_callbacks.relinkRequested(placeholderId, picked))
				{
					statusLabel->setText(tr("Relinked to %1").arg(picked));
					relinkButton->setEnabled(false);
					browseButton->setEnabled(false);
					skipButton->setEnabled(false);
				}
				else
				{
					QMessageBox::warning(this, tr("Catalog integrity check"),
						tr("Could not relink - that file's identity is already tracked under a different folder."));
				}
			});

			wireSkip(skipButton, statusLabel, rowButtons);
		}
	}

	if (!report.untracked.empty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Untracked folders</b> - on disk, not in the catalog"), content));
		for (const Catalog::UntrackedFolder& u : report.untracked)
		{
			QString status = QFileInfo(u.folderPath).fileName();
			const bool hasClash = u.clashId.isValid();
			if (u.candidateSourcePath.isEmpty())
				status += tr("<br>No source recorded for this folder.");
			else if (hasClash)
			{
				const QString clashDetail = u.filesIdentical
					? tr("files are identical - likely already imported")
					: tr("files differ - a name+size collision, not the same file");
				status += tr("<br>%1 is already tracked elsewhere (%2).").arg(u.candidateSourcePath, clashDetail);
			}
			else
				status += tr("<br>Source found: %1").arg(u.candidateSourcePath);

			const auto [row, statusLabel] = addRow(status);
			QHBoxLayout* rowLayout = static_cast<QHBoxLayout*>(row->layout());

			QPushButton* registerButton = nullptr;
			if (!u.candidateSourcePath.isEmpty() && !hasClash)
			{
				registerButton = new QPushButton(tr("Register"), row);
				rowLayout->addWidget(registerButton);
			}
			QPushButton* browseButton = new QPushButton(tr("Browse..."), row);
			QPushButton* skipButton   = new QPushButton(tr("Skip"), row);
			rowLayout->addWidget(browseButton);
			rowLayout->addWidget(skipButton);

			const QString folderPath = u.folderPath;
			const QString candidatePath = u.candidateSourcePath;
			std::vector<QPushButton*> rowButtons{ browseButton, skipButton };
			if (registerButton)
				rowButtons.insert(rowButtons.begin(), registerButton);

			if (registerButton)
			{
				wireAction(registerButton, statusLabel, rowButtons, tr("Registered."),
					tr("Could not register - that source file is already tracked under a different folder."),
					[this, folderPath, candidatePath] { return m_callbacks.registerRequested(folderPath, candidatePath); });
			}

			connect(browseButton, &QPushButton::clicked, this, [this, folderPath, candidatePath, rowButtons, statusLabel] {
				const QString picked = browseForSourceVideo(this, candidatePath);
				if (picked.isEmpty())
					return;
				if (m_callbacks.registerRequested(folderPath, picked))
				{
					statusLabel->setText(tr("Registered with %1").arg(picked));
					for (QPushButton* b : rowButtons)
						b->setEnabled(false);
				}
				else
				{
					QMessageBox::warning(this, tr("Catalog integrity check"),
						tr("Could not register - that file's identity is already tracked under a different folder."));
				}
			});

			wireSkip(skipButton, statusLabel, rowButtons);
		}
	}

	if (!report.issues.empty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Problems</b> - tracked videos whose files don't match the catalog"), content));
		for (const Catalog::MediaIssue& issue : report.issues)
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
