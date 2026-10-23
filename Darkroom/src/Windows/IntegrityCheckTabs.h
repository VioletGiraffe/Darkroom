#pragma once

#include "Windows/IntegrityCheckDialog.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QPersistentModelIndex>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <deque>
#include <functional>
#include <vector>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QTabWidget;
class QWidget;

// One tab per finding category, plus Resolved. Actions apply to the selected rows they fit; a fixed row moves to
// Resolved, and a video row that a repair leaves broken stays with the problems that remain.
class IntegrityCheckTabs
{
public:
	IntegrityCheckTabs(Catalog& catalog, CatalogIntegrity::IntegrityReport report, IntegrityCheckDialog::Callbacks& callbacks,
	                   QTabWidget* tabWidget, QLabel* statusLabel, QWidget* dialog);

private:
	struct Cell
	{
		QString text;
		QString toolTip;
	};

	struct Action
	{
		QPushButton* button = nullptr;
		std::function<bool(int finding)> applies;
		bool singleRowOnly = false;
		std::function<void(const std::vector<int>& findings)> run;  // receives the selected findings it applies to
	};

	// Findings are indices into the report vector of the tab's category.
	struct Tab
	{
		QString title;
		QWidget* page = nullptr;
		QHBoxLayout* actionBar = nullptr;
		QStandardItemModel* model = nullptr;
		QTableView* view = nullptr;
		std::vector<Action> actions;
		std::function<std::vector<Cell>(int finding, const QStringList& done)> cells;
		std::function<bool(int finding)> recheckLeavesProblems;  // optional; refreshes the finding after a repair
		std::vector<QPersistentModelIndex> rows;  // per finding, column 0
		std::vector<QStringList> done;            // per finding: outcomes of repairs so far
	};

	// An action with an outcome per row.
	struct EachSpec
	{
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<int>& findings)> act;  // one outcome per finding, in order
		QString outcome;     // shown in Resolved
		QString doneFmt;
		QString failedFmt;
		QString confirmFmt;  // asked before acting on several rows; empty = no confirmation
		bool quick = false;  // persists the catalog once; long (ffmpeg) actions persist per row so a crash keeps finished rows
	};

	struct LocateSpec
	{
		std::function<MediaId(int finding)> idOf;
		std::function<QString(int finding)> browseForFile;
		std::function<bool(const QString& path)> isCandidateFile;
		std::function<bool(int finding, const QString& path)> relocate;
		QString folderPrompt;
		QString doneFmt;
	};

	void buildUntrackedFolders();
	void buildUntrackedPhotos();
	void buildUnknownItems();
	void buildVideos();
	void buildMissingPhotos();
	void buildInvalidLabels();
	void buildResolvedTab();

	Tab& addTab(const QString& title, const QString& description, const QStringList& headers, size_t findingCount,
	            std::function<std::vector<Cell>(int finding, const QStringList& done)> cells);
	void addAction(Tab& tab, const QString& label, const QString& toolTip, std::function<bool(int finding)> applies,
	               bool singleRowOnly, std::function<void(const std::vector<int>& findings)> run);
	void addEachAction(Tab& tab, const QString& label, const QString& toolTip, std::function<bool(int finding)> applies, EachSpec spec);
	void addMoveToTrashAction(Tab& tab, std::function<QString(int finding)> pathOf);

	// Sorted by row.
	[[nodiscard]] std::vector<int> selectedFindings(const Tab& tab) const;
	void updateActions(Tab& tab);
	void updateTabTitles();

	void runEach(Tab& tab, const std::vector<int>& findings, const EachSpec& spec);
	// One row: picks the file. Several: matches files under a picked folder by name/size identity.
	void locate(Tab& tab, const std::vector<int>& findings, const LocateSpec& spec);
	// Records a successful repair: the row moves to Resolved unless a re-check finds problems left.
	void settle(Tab& tab, int finding, const QString& outcome);
	void updateRow(Tab& tab, int finding);

	void showStatus(const QString& text);
	void reportResult(const QString& doneText, const QString& failureText);

	[[nodiscard]] std::vector<MediaId> videoIds(const std::vector<int>& findings) const;

	[[nodiscard]] QString browseForSourceVideo(const QString& hint) const;
	[[nodiscard]] QString browseForSourcePhoto(const QString& hint) const;

	Catalog& _catalog;  // For re-checking videos and batching persistence; mutations go through _callbacks.
	CatalogIntegrity::IntegrityReport _report;  // A video's issue is replaced when it is re-checked.
	IntegrityCheckDialog::Callbacks& _callbacks;
	QTabWidget* _tabWidget;
	QLabel*     _statusLabel;
	QWidget*    _dialog;

	std::deque<Tab> _tabs;  // Handlers retain Tab references; deque preserves their addresses.
	QWidget*            _resolvedPage = nullptr;
	QStandardItemModel* _resolvedModel = nullptr;
};
