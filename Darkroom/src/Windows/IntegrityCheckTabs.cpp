#include "Windows/IntegrityCheckTabs.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"
#include "utils/scoped_cursor.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <optional>
#include <utility>

using CatalogIntegrity::MediaIssue;
using FrameExtraction::Outcome;

namespace {

// Adapts a per-row action to the batch form EachSpec::act takes.
[[nodiscard]] std::function<std::vector<Outcome>(const std::vector<int>&)> perFinding(std::function<bool(int finding)> act)
{
	return [act = std::move(act)](const std::vector<int>& findings) {
		std::vector<Outcome> outcomes;
		outcomes.reserve(findings.size());
		for (const int f : findings)
			outcomes.push_back(act(f) ? Outcome::Done : Outcome::Failed);
		return outcomes;
	};
}

constexpr int FindingRole = Qt::UserRole;
constexpr int MaxInitialColumnWidth = 360;

class NaturalSortItem final : public QStandardItem
{
public:
	using QStandardItem::QStandardItem;

	[[nodiscard]] bool operator<(const QStandardItem& other) const override
	{
		return NaturalSort::lessCaseInsensitive(text(), other.text());
	}
};

[[nodiscard]] QList<QStandardItem*> makeItems(const QStringList& texts, const QStringList& toolTips = {})
{
	QList<QStandardItem*> items;
	for (qsizetype c = 0; c < texts.size(); ++c)
	{
		auto* item = new NaturalSortItem(texts[c]);
		item->setToolTip(toolTips.value(c));
		items.push_back(item);
	}
	return items;
}

[[nodiscard]] QTableView* createTableView(QStandardItemModel* model, QWidget* parent)
{
	QTableView* view = new QTableView(parent);
	view->setModel(model);
	view->setSelectionBehavior(QAbstractItemView::SelectRows);
	view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	view->setEditTriggers(QAbstractItemView::NoEditTriggers);
	view->setWordWrap(false);
	view->setTextElideMode(Qt::ElideMiddle);
	view->verticalHeader()->hide();
	view->horizontalHeader()->setStretchLastSection(true);
	view->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
	view->setSortingEnabled(true);

	view->resizeColumnsToContents();
	for (int c = 0; c < model->columnCount() - 1; ++c)
		view->setColumnWidth(c, std::min(view->columnWidth(c), MaxInitialColumnWidth));
	return view;
}

[[nodiscard]] bool canReextractFrames(const MediaIssue& i)   { return i.extractedFramesMissing() && i.sourcePresent; }
[[nodiscard]] bool canKeepPreviewOnly(const MediaIssue& i)   { return i.extractedFramesMissing() && (i.previewPresent || i.sourcePresent); }
[[nodiscard]] bool canRegeneratePreview(const MediaIssue& i) { return i.previewMissing() && !i.extractedFramesMissing() && (i.realFramesPresent || i.sourcePresent); }
[[nodiscard]] bool canMarkSplit(const MediaIssue& i)         { return i.splitFlagStale() && !i.previewMissing(); }

// Integrity verdicts are orthogonal, so every applicable problem is listed.
[[nodiscard]] QString videoProblems(const MediaIssue& issue)
{
	QStringList problems;
	if (issue.extractedFramesMissing())
		problems << QObject::tr("extracted frames are gone");
	if (issue.previewMissing())
		problems << (issue.realFramesPresent ? QObject::tr("no preview - the card renders the full-size frames instead")
		                                     : QObject::tr("no preview - the card has no image to show"));
	if (issue.splitFlagStale())
		problems << QObject::tr("marked not-yet-split, but frames exist");
	if (issue.sourceMissing())
		problems << (issue.sourcePath.isEmpty() ? QObject::tr("no source recorded") : QObject::tr("source missing"));
	return problems.join(QStringLiteral("; "));
}

} // namespace

IntegrityCheckTabs::IntegrityCheckTabs(Catalog& catalog, CatalogIntegrity::IntegrityReport report,
                                       IntegrityCheckDialog::Callbacks& callbacks, QTabWidget* tabWidget, QLabel* statusLabel,
                                       QWidget* dialog)
	: _catalog(catalog), _report(std::move(report)), _callbacks(callbacks), _tabWidget(tabWidget), _statusLabel(statusLabel),
	  _dialog(dialog)
{
	buildUntrackedFolders();
	buildUntrackedPhotos();
	buildVideos();
	buildMissingPhotos();
	buildInvalidLabels();
	buildResolvedTab();
	updateTabTitles();
}

void IntegrityCheckTabs::buildUntrackedFolders()
{
	if (_report.untracked.empty())
		return;

	Tab& tab = addTab(QObject::tr("Untracked folders"), QObject::tr("Frame folders on disk that no catalog entry claims."),
		{ QObject::tr("Folder"), QObject::tr("Path") }, _report.untracked.size(),
		[this](int f, const QStringList&) {
			const QString path = QDir::toNativeSeparators(_report.untracked[f].folderPath);
			return std::vector<Cell>{ { QFileInfo(path).fileName(), path }, { path, path } };
		});

	// Name alone cannot safely identify the source video, so each folder is registered with a picked file.
	addAction(tab, QObject::tr("Register..."), QObject::tr("Pick the source video these frames were extracted from"),
		[](int) { return true; }, /*singleRowOnly=*/true,
		[this, &tab](const std::vector<int>& findings) {
			const int f = findings.front();
			const QString& folderPath = _report.untracked[f].folderPath;
			const QString picked = browseForSourceVideo(folderPath);
			if (picked.isEmpty())
				return;
			if (!_callbacks.registerRequested(folderPath, picked))
			{
				reportResult({}, QObject::tr("Could not register - that file's identity is already tracked under a different folder."));
				return;
			}
			const QString outcome = QObject::tr("Registered with %1").arg(QDir::toNativeSeparators(picked));
			settle(tab, f, outcome);
			showStatus(outcome);
		});
}

void IntegrityCheckTabs::buildUntrackedPhotos()
{
	if (_report.untrackedPhotos.empty())
		return;

	Tab& tab = addTab(QObject::tr("Untracked photos"), QObject::tr("Image files under Photos that are not in the catalog."),
		{ QObject::tr("File"), QObject::tr("Label") }, _report.untrackedPhotos.size(),
		[this](int f, const QStringList&) {
			const CatalogIntegrity::UntrackedPhoto& photo = _report.untrackedPhotos[f];
			return std::vector<Cell>{ { QFileInfo(photo.filePath).fileName(), QDir::toNativeSeparators(photo.filePath) }, { photo.labelName, {} } };
		});

	addEachAction(tab, QObject::tr("Add to catalog"), {}, [](int) { return true; }, {
		.act = perFinding([this](int f) { return _callbacks.adoptPhotoRequested(_report.untrackedPhotos[f].filePath); }),
		.outcome = QObject::tr("Added to catalog"),
		.doneFmt = QObject::tr("Added %1 photo(s) to the catalog."),
		.failedFmt = QObject::tr("%1 could not be added - a file with the same name and size is already tracked elsewhere."),
		.quick = true,
	});
}

void IntegrityCheckTabs::buildVideos()
{
	if (_report.issues.empty())
		return;

	Tab& tab = addTab(QObject::tr("Videos"), QObject::tr("Tracked videos whose source, frames or preview are missing on disk."),
		{ QObject::tr("Folder"), QObject::tr("Problems"), QObject::tr("Source"), QObject::tr("Done") }, _report.issues.size(),
		[this](int f, const QStringList& done) {
			const MediaIssue& issue = _report.issues[f];
			const QString problems = videoProblems(issue);
			const QString source = QDir::toNativeSeparators(issue.sourcePath);
			const QString doneText = done.join(QStringLiteral("; "));
			return std::vector<Cell>{ { QFileInfo(issue.folder).fileName(), QDir::toNativeSeparators(issue.folder) },
			                          { problems, problems }, { source, source }, { doneText, doneText } };
		});

	tab.recheckLeavesProblems = [this](int f) {
		MediaIssue& issue = _report.issues[f];
		if (!_catalog.containsMediaItem(issue.id))
			return false;
		issue = CatalogIntegrity::inspectVideo(_catalog, issue.id);
		return !issue.healthy();
	};

	addAction(tab, QObject::tr("Locate..."),
		QObject::tr("One row: pick the source video. Several rows: search a folder for files with the same name and size."),
		[this](int f) { return _report.issues[f].sourceMissing(); }, /*singleRowOnly=*/false,
		[this, &tab](const std::vector<int>& findings) {
			locate(tab, findings, {
				.idOf = [this](int f) { return _report.issues[f].id; },
				.browseForFile = [this](int f) { return browseForSourceVideo(_report.issues[f].sourcePath); },
				.isCandidateFile = isSupportedVideoFile,
				.relocate = [this](int f, const QString& path) {
					const MediaId newId = _callbacks.locateSourceRequested(_report.issues[f].id, path);
					if (!newId.isValid())
						return false;
					_report.issues[f].id = newId;
					return true;
				},
				.folderPrompt = QObject::tr("Select folder to search for moved source videos"),
				.doneFmt = QObject::tr("Relocated %1 video(s)."),
			});
		});

	addEachAction(tab, QObject::tr("Re-extract frames"), QObject::tr("Extract every frame and the preview again from the source video"),
		[this](int f) { return canReextractFrames(_report.issues[f]); }, {
		.act = [this](const std::vector<int>& findings) { return _callbacks.reextractFramesRequested(videoIds(findings)); },
		.outcome = QObject::tr("Frames re-extracted"),
		.doneFmt = QObject::tr("Re-extracted the frames of %1 video(s)."),
		.failedFmt = QObject::tr("%1 could not be re-extracted - see the error dialogs for details."),
		.confirmFmt = QObject::tr("Re-extract every frame of %1 videos? This re-runs ffmpeg and can take a while."),
	});

	addEachAction(tab, QObject::tr("Keep preview only"),
		QObject::tr("Go back to preview-only: frames are extracted again the first time they are viewed"),
		[this](int f) { return canKeepPreviewOnly(_report.issues[f]); }, {
		.act = [this](const std::vector<int>& findings) { return _callbacks.keepPreviewOnlyRequested(videoIds(findings)); },
		.outcome = QObject::tr("Kept as preview only"),
		.doneFmt = QObject::tr("Kept %1 video(s) as preview only."),
		.failedFmt = QObject::tr("%1 could not get a preview - the source may be unavailable."),
	});

	addEachAction(tab, QObject::tr("Regenerate preview"), {}, [this](int f) { return canRegeneratePreview(_report.issues[f]); }, {
		.act = [this](const std::vector<int>& findings) { return _callbacks.regeneratePreviewRequested(videoIds(findings)); },
		.outcome = QObject::tr("Preview regenerated"),
		.doneFmt = QObject::tr("Regenerated %1 preview(s)."),
		.failedFmt = QObject::tr("%1 could not be regenerated - the source may be unavailable."),
	});

	addEachAction(tab, QObject::tr("Mark as fully split"), {}, [this](int f) { return canMarkSplit(_report.issues[f]); }, {
		.act = perFinding([this](int f) { return _callbacks.markSplitRequested(_report.issues[f].id); }),
		.outcome = QObject::tr("Marked as fully split"),
		.doneFmt = QObject::tr("Marked %1 video(s) as fully split."),
		.failedFmt = QObject::tr("%1 could not be updated."),
		.quick = true,
	});

	addEachAction(tab, QObject::tr("Remove"), QObject::tr("Forget the entry; frame folders and source files on disk are not touched"),
		[](int) { return true; }, {
		.act = perFinding([this](int f) { return _callbacks.removeEntryRequested(_report.issues[f].id); }),
		.outcome = QObject::tr("Removed from catalog"),
		.doneFmt = QObject::tr("Removed %1 video(s) from the catalog."),
		.failedFmt = QObject::tr("%1 could not be removed."),
		.confirmFmt = QObject::tr("Remove %1 video entries from the catalog? Frame folders and source files on disk are not touched."),
		.quick = true,
	});
}

void IntegrityCheckTabs::buildMissingPhotos()
{
	if (_report.photoIssues.empty())
		return;

	Tab& tab = addTab(QObject::tr("Missing photos"), QObject::tr("Tracked photos whose file is missing."),
		{ QObject::tr("File"), QObject::tr("Problem"), QObject::tr("Path") }, _report.photoIssues.size(),
		[this](int f, const QStringList&) {
			const CatalogIntegrity::PhotoIssue& photo = _report.photoIssues[f];
			const QString name = photo.sourcePath.isEmpty() ? QObject::tr("(no source recorded)") : QFileInfo(photo.sourcePath).fileName();
			const QString problem = photo.referenced ? QObject::tr("referenced file moved or unmounted") : QObject::tr("the library's own file is gone");
			const QString path = QDir::toNativeSeparators(photo.sourcePath);
			return std::vector<Cell>{ { name, path }, { problem, {} }, { path, path } };
		});

	addAction(tab, QObject::tr("Locate..."),
		QObject::tr("One row: pick the photo. Several rows: search a folder for files with the same name and size."),
		[this](int f) { return _report.photoIssues[f].referenced; }, /*singleRowOnly=*/false,
		[this, &tab](const std::vector<int>& findings) {
			locate(tab, findings, {
				.idOf = [this](int f) { return _report.photoIssues[f].id; },
				.browseForFile = [this](int f) { return browseForSourcePhoto(_report.photoIssues[f].sourcePath); },
				.isCandidateFile = isSupportedImageFile,
				.relocate = [this](int f, const QString& path) { return _callbacks.locatePhotoRequested(_report.photoIssues[f].id, path); },
				.folderPrompt = QObject::tr("Select folder to search for missing photos"),
				.doneFmt = QObject::tr("Relocated %1 photo(s)."),
			});
		});

	addEachAction(tab, QObject::tr("Remove"), QObject::tr("Forget the entry; any file still on disk is not touched"),
		[](int) { return true; }, {
		.act = perFinding([this](int f) { return _callbacks.removeEntryRequested(_report.photoIssues[f].id); }),
		.outcome = QObject::tr("Removed from catalog"),
		.doneFmt = QObject::tr("Removed %1 photo(s) from the catalog."),
		.failedFmt = QObject::tr("%1 could not be removed."),
		.confirmFmt = QObject::tr("Remove %1 missing-photo entries from the catalog? Any files still on disk are not touched."),
		.quick = true,
	});
}

void IntegrityCheckTabs::buildInvalidLabels()
{
	if (_report.danglingLabelIssues.empty())
		return;

	Tab& tab = addTab(QObject::tr("Invalid labels"), QObject::tr("Items that refer to labels no longer in the label registry."),
		{ QObject::tr("Item"), QObject::tr("Missing label IDs") }, _report.danglingLabelIssues.size(),
		[this](int f, const QStringList&) {
			const CatalogIntegrity::DanglingLabelIssue& issue = _report.danglingLabelIssues[f];
			QStringList missingIds;
			for (const LabelId labelId : issue.missingLabelIds)
				missingIds << QString::number(toUInt64(labelId));
			return std::vector<Cell>{ { issue.id.name(), {} }, { missingIds.join(QStringLiteral(", ")), {} } };
		});

	addEachAction(tab, QObject::tr("Drop invalid references"), {}, [](int) { return true; }, {
		.act = perFinding([this](int f) { return _callbacks.removeInvalidLabelReferencesRequested(_report.danglingLabelIssues[f].id); }),
		.outcome = QObject::tr("Invalid label references dropped"),
		.doneFmt = QObject::tr("Cleaned %1 item(s)."),
		.failedFmt = QObject::tr("%1 item(s) could not be updated."),
		.quick = true,
	});
}

void IntegrityCheckTabs::buildResolvedTab()
{
	_resolvedPage = new QWidget(_tabWidget);
	QVBoxLayout* layout = new QVBoxLayout(_resolvedPage);
	_resolvedModel = new QStandardItemModel(0, 3, _resolvedPage);
	_resolvedModel->setHorizontalHeaderLabels({ QObject::tr("Category"), QObject::tr("Item"), QObject::tr("Outcome") });
	layout->addWidget(createTableView(_resolvedModel, _resolvedPage));
	_tabWidget->addTab(_resolvedPage, {});
}

IntegrityCheckTabs::Tab& IntegrityCheckTabs::addTab(const QString& title, const QString& description, const QStringList& headers,
                                                     size_t findingCount, std::function<std::vector<Cell>(int, const QStringList&)> cells)
{
	Tab& tab = _tabs.emplace_back();
	tab.title = title;
	tab.cells = std::move(cells);
	tab.page = new QWidget(_tabWidget);
	tab.done.resize(findingCount);
	tab.rows.reserve(findingCount);

	QVBoxLayout* layout = new QVBoxLayout(tab.page);
	QLabel* descriptionLabel = new QLabel(description, tab.page);
	descriptionLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().instructionText.name()));
	layout->addWidget(descriptionLabel);
	tab.actionBar = new QHBoxLayout();
	tab.actionBar->addStretch(1);
	layout->addLayout(tab.actionBar);

	// Filled before the view attaches, so the rows cost no view updates.
	tab.model = new QStandardItemModel(0, static_cast<int>(headers.size()), tab.page);
	tab.model->setHorizontalHeaderLabels(headers);
	for (int f = 0; f < static_cast<int>(findingCount); ++f)
	{
		QStringList texts, toolTips;
		for (const Cell& cell : tab.cells(f, {}))
		{
			texts << cell.text;
			toolTips << cell.toolTip;
		}
		QList<QStandardItem*> items = makeItems(texts, toolTips);
		items.front()->setData(f, FindingRole);
		tab.model->appendRow(items);
		tab.rows.emplace_back(tab.model->index(tab.model->rowCount() - 1, 0));
	}

	tab.view = createTableView(tab.model, tab.page);
	layout->addWidget(tab.view, 1);
	QObject::connect(tab.view->selectionModel(), &QItemSelectionModel::selectionChanged, tab.view, [this, &tab] { updateActions(tab); });

	_tabWidget->addTab(tab.page, title);
	return tab;
}

void IntegrityCheckTabs::addAction(Tab& tab, const QString& label, const QString& toolTip, std::function<bool(int)> applies,
                                   bool singleRowOnly, std::function<void(const std::vector<int>&)> run)
{
	QPushButton* button = new QPushButton(label, tab.page);
	button->setToolTip(toolTip);
	button->setEnabled(false);
	tab.actionBar->insertWidget(tab.actionBar->count() - 1, button);  // before the trailing stretch
	tab.actions.push_back({ button, std::move(applies), singleRowOnly, std::move(run) });

	QObject::connect(button, &QPushButton::clicked, button, [this, &tab, actionIndex = tab.actions.size() - 1] {
		const Action& action = tab.actions[actionIndex];
		std::vector<int> findings = selectedFindings(tab);
		std::erase_if(findings, [&action](int f) { return !action.applies(f); });
		if (findings.empty())
			return;

		action.run(findings);
		updateTabTitles();
		updateActions(tab);
	});
}

void IntegrityCheckTabs::addEachAction(Tab& tab, const QString& label, const QString& toolTip, std::function<bool(int)> applies, EachSpec spec)
{
	addAction(tab, label, toolTip, std::move(applies), /*singleRowOnly=*/false,
		[this, &tab, spec = std::move(spec)](const std::vector<int>& findings) { runEach(tab, findings, spec); });
}

std::vector<int> IntegrityCheckTabs::selectedFindings(const Tab& tab) const
{
	QModelIndexList selectedRows = tab.view->selectionModel()->selectedRows();
	std::ranges::sort(selectedRows, {}, &QModelIndex::row);

	std::vector<int> findings;
	findings.reserve(static_cast<size_t>(selectedRows.size()));
	for (const QModelIndex& index : selectedRows)
		findings.push_back(index.data(FindingRole).toInt());
	return findings;
}

void IntegrityCheckTabs::updateActions(Tab& tab)
{
	const std::vector<int> selected = selectedFindings(tab);
	for (const Action& action : tab.actions)
	{
		const auto applicable = std::ranges::count_if(selected, action.applies);
		action.button->setEnabled(action.singleRowOnly ? applicable == 1 : applicable > 0);
	}
}

void IntegrityCheckTabs::updateTabTitles()
{
	for (const Tab& tab : _tabs)
		_tabWidget->setTabText(_tabWidget->indexOf(tab.page), QStringLiteral("%1 (%2)").arg(tab.title).arg(tab.model->rowCount()));
	_tabWidget->setTabText(_tabWidget->indexOf(_resolvedPage), QObject::tr("Resolved (%1)").arg(_resolvedModel->rowCount()));
}

void IntegrityCheckTabs::runEach(Tab& tab, const std::vector<int>& findings, const EachSpec& spec)
{
	const int count = static_cast<int>(findings.size());
	if (count > 1 && !spec.confirmFmt.isEmpty()
		&& QMessageBox::question(_dialog, _dialog->windowTitle(), spec.confirmFmt.arg(count)) != QMessageBox::Yes)
		return;

	int done = 0, failed = 0, cancelled = 0;
	{
		std::optional<ScopedWaitCursor> waitCursor;
		std::optional<Catalog::BatchScope> batch;
		if (spec.quick)
		{
			waitCursor.emplace();
			batch.emplace(_catalog);
		}

		// Long actions show their own progress dialog.
		const std::vector<Outcome> outcomes = spec.act(findings);
		assert_r(outcomes.size() == findings.size());
		for (size_t k = 0; k < std::min(outcomes.size(), findings.size()); ++k)
		{
			switch (outcomes[k])
			{
			case Outcome::Done:
				++done;
				settle(tab, findings[k], spec.outcome);
				break;
			case Outcome::Failed:
				++failed;
				break;
			case Outcome::Cancelled:
				++cancelled;
				break;
			}
		}
	}

	QString doneText = spec.doneFmt.arg(done);
	if (cancelled)
		doneText += ' ' + QObject::tr("%1 cancelled.").arg(cancelled);
	reportResult(doneText, failed ? spec.failedFmt.arg(failed) : QString{});
}

std::vector<MediaId> IntegrityCheckTabs::videoIds(const std::vector<int>& findings) const
{
	std::vector<MediaId> ids;
	ids.reserve(findings.size());
	for (const int f : findings)
		ids.push_back(_report.issues[f].id);
	return ids;
}

void IntegrityCheckTabs::locate(Tab& tab, const std::vector<int>& findings, const LocateSpec& spec)
{
	if (findings.size() == 1)
	{
		const int f = findings.front();
		const QString picked = spec.browseForFile(f);
		if (picked.isEmpty())
			return;
		if (!spec.relocate(f, picked))
		{
			reportResult({}, QObject::tr("Could not relocate - that file's identity is already tracked as a different item."));
			return;
		}
		const QString outcome = QObject::tr("Relocated to %1").arg(QDir::toNativeSeparators(picked));
		settle(tab, f, outcome);
		showStatus(outcome);
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(_dialog, spec.folderPrompt);
	if (dir.isEmpty())
		return;

	int relocated = 0, unmatched = 0, failed = 0;
	{
		const ScopedWaitCursor waitCursor;
		QHash<QString, QString> byIdentity;   // MediaId::key() -> first file found under dir carrying that identity
		for (const QString& path : collectFilesInDirectory(dir, /*recursive=*/true, spec.isCandidateFile))
		{
			const QString identity = MediaId::fromFile(path).key();
			if (!byIdentity.contains(identity))
				byIdentity.insert(identity, QDir::toNativeSeparators(path));
		}

		Catalog::BatchScope batch(_catalog);
		for (const int f : findings)
		{
			const auto found = byIdentity.constFind(spec.idOf(f).key());
			if (found == byIdentity.constEnd())
			{
				++unmatched;
				continue;
			}
			if (!spec.relocate(f, found.value()))
			{
				++failed;
				continue;
			}
			++relocated;
			settle(tab, f, QObject::tr("Relocated to %1").arg(found.value()));
		}
	}

	QString doneText = spec.doneFmt.arg(relocated);
	if (unmatched)
		doneText += ' ' + QObject::tr("%1 had no matching file in that folder.").arg(unmatched);
	reportResult(doneText, failed ? QObject::tr("%1 could not be relocated - the identity is already tracked as a different item.").arg(failed)
	                              : QString{});
}

void IntegrityCheckTabs::settle(Tab& tab, int finding, const QString& outcome)
{
	tab.done[finding] << outcome;
	if (tab.recheckLeavesProblems && tab.recheckLeavesProblems(finding))
	{
		updateRow(tab, finding);
		return;
	}

	const QPersistentModelIndex& row = tab.rows[finding];
	_resolvedModel->appendRow(makeItems({ tab.title, row.data().toString(), tab.done[finding].join(QStringLiteral("; ")) }));
	tab.model->removeRow(row.row());
}

void IntegrityCheckTabs::updateRow(Tab& tab, int finding)
{
	const int row = tab.rows[finding].row();
	const std::vector<Cell> cells = tab.cells(finding, tab.done[finding]);
	for (int c = 0; c < static_cast<int>(cells.size()); ++c)
	{
		QStandardItem* item = tab.model->item(row, c);
		item->setText(cells[c].text);
		item->setToolTip(cells[c].toolTip);
	}
}

void IntegrityCheckTabs::showStatus(const QString& text)
{
	_statusLabel->setText(text);
}

void IntegrityCheckTabs::reportResult(const QString& doneText, const QString& failureText)
{
	if (failureText.isEmpty())
	{
		showStatus(doneText);
		return;
	}
	const QString text = doneText.isEmpty() ? failureText : doneText + ' ' + failureText;
	showStatus(text);
	QMessageBox::warning(_dialog, _dialog->windowTitle(), text);
}

QString IntegrityCheckTabs::browseForSourceVideo(const QString& hint) const
{
	const QString startDir = hint.isEmpty() ? _catalog.anySourceDir() : QFileInfo(hint).absolutePath();
	const QString filter = QObject::tr("Video files (%1);;All files (*)").arg(supportedVideoFileGlobs());
	return QFileDialog::getOpenFileName(_dialog, QObject::tr("Select source video"), startDir, filter);
}

QString IntegrityCheckTabs::browseForSourcePhoto(const QString& hint) const
{
	const QString startDir = hint.isEmpty() ? QString{} : QFileInfo(hint).absolutePath();
	const QString filter = QObject::tr("Image files (%1);;All files (*)").arg(supportedImageFileGlobs());
	return QFileDialog::getOpenFileName(_dialog, QObject::tr("Locate photo"), startDir, filter);
}
