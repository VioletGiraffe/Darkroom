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

namespace {

constexpr const char* VIDEO_FILE_FILTER = "Video files (*.mp4 *.mov *.avi *.mkv *.flv);;All files (*)";

// Opens a file picker for the user to manually point at a source video, starting near hint (a path that
// doesn't have to exist - only its directory is used) if given, else a sensible catalog-wide default.
QString browseForSourceVideo(QWidget* parent, const QString& hint)
{
	const QString startDir = hint.isEmpty() ? Catalog::instance().anySourceVideoDir() : QFileInfo(hint).absolutePath();
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
	const auto wireAction = [this](QPushButton* button, QLabel* statusLabel, const QList<QPushButton*>& rowButtons,
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
	const auto wireSkip = [](QPushButton* skipButton, QLabel* statusLabel, const QList<QPushButton*>& rowButtons) {
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

	if (!report.relinkable.isEmpty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Relink</b> - the source video reappeared"), content));
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

			const VideoId placeholderId = candidate.placeholderId;
			const QString recordedPath = candidate.recordedSourcePath;
			const QList<QPushButton*> rowButtons{ relinkButton, browseButton, skipButton };

			wireAction(relinkButton, statusLabel, rowButtons, tr("Relinked."),
				tr("Could not relink - that source video is already tracked under a different folder."),
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

	if (!report.untracked.isEmpty())
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
					: tr("files differ - a name+size collision, not the same video");
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
			QList<QPushButton*> rowButtons{ browseButton, skipButton };
			if (registerButton)
				rowButtons.prepend(registerButton);

			if (registerButton)
			{
				wireAction(registerButton, statusLabel, rowButtons, tr("Registered."),
					tr("Could not register - that source video is already tracked under a different folder."),
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

	if (!report.ghosts.isEmpty())
	{
		contentLayout->addWidget(new QLabel(tr("<b>Ghosts</b> - catalog entry, frame folder is gone"), content));
		for (const Catalog::GhostEntry& ghost : report.ghosts)
		{
			QString detail;
			if (ghost.sourcePresent)
				detail = tr("Source still present at %1").arg(ghost.sourceVideoPath);
			else if (ghost.sourceVideoPath.isEmpty())
				detail = tr("Source is also missing");
			else
				detail = tr("Source is also missing (%1)").arg(ghost.sourceVideoPath);
			const QString status = QFileInfo(ghost.folder).fileName() + "<br>" + detail;

			const auto [row, statusLabel] = addRow(status);
			QHBoxLayout* rowLayout = static_cast<QHBoxLayout*>(row->layout());

			QPushButton* reimportButton = nullptr;
			if (ghost.sourcePresent)
			{
				reimportButton = new QPushButton(tr("Re-import"), row);
				rowLayout->addWidget(reimportButton);
			}
			QPushButton* removeButton = new QPushButton(tr("Remove"), row);
			QPushButton* skipButton   = new QPushButton(tr("Skip"), row);
			rowLayout->addWidget(removeButton);
			rowLayout->addWidget(skipButton);

			const VideoId ghostId = ghost.id;
			QList<QPushButton*> rowButtons{ removeButton, skipButton };
			if (reimportButton)
				rowButtons.prepend(reimportButton);

			if (reimportButton)
			{
				wireAction(reimportButton, statusLabel, rowButtons, tr("Re-imported."),
					tr("Re-import failed - see the error dialog for details."),
					[this, ghostId] { return m_callbacks.reimportRequested(ghostId); });
			}

			wireAction(removeButton, statusLabel, rowButtons, tr("Removed from catalog."), tr("Could not remove."),
				[this, ghostId] { return m_callbacks.removeGhostRequested(ghostId); });

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
