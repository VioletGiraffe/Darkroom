#include "Windows/FindUntrackedFilesDialog.h"
#include "Core/Catalog.h"
#include "Theme/Theme.h"
#include "Utils.h"
#include "Windows/VideoPlayerWindow.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QVBoxLayout>

namespace {

// Last folder picked for scanning; local to this tool, not hoisted into the shared Settings.h.
constexpr const char* LAST_FOLDER_KEY = "findUntrackedFilesDialog/lastFolder";

// Normalized key for case/separator-insensitive path matching (Windows). Uses the
// canonical form for files that exist, falling back to a cleaned path otherwise.
QString normalizePath(const QString& path)
{
	const QString canonical = QFileInfo(path).canonicalFilePath();
	return (canonical.isEmpty() ? QDir::cleanPath(path) : canonical).toLower();
}

} // namespace

QStringList FindUntrackedFilesDialog::scanAndShowUi(const QString& rootFolder, QWidget* parent)
{
	// A video is "tracked" iff the catalog records it as some video's source path. Also remember one tracked
	// video's path - its folder is a more useful default starting point for the picker below than rootFolder
	// itself, which holds the organized collections rather than wherever the raw source footage actually lives.
	QSet<QString> tracked;
	Catalog& catalog = Catalog::instance();
	for (const MediaId& id : catalog.allMediaItems())
	{
		const QString sourcePath = catalog.sourcePathForMediaItem(id);
		if (!sourcePath.isEmpty())
			tracked.insert(normalizePath(sourcePath));
	}

	QSettings settings;
	const QString defaultStartDir = tracked.empty() ? rootFolder : QFileInfo(*tracked.begin()).absolutePath();
	const QString startDir = settings.value(LAST_FOLDER_KEY, defaultStartDir).toString();
	const QString dir = QFileDialog::getExistingDirectory(parent, tr("Scan folder for untracked videos"), startDir);
	if (dir.isEmpty())
		return {};

	settings.setValue(LAST_FOLDER_KEY, dir);

	QStringList untracked;
	size_t trackedCount = 0;
	QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
	{
		const QString path = it.next();
		if (!isSupportedVideoFile(path))
			continue;

		if (!tracked.contains(normalizePath(path)))
			untracked.push_back(QDir::toNativeSeparators(path));
		else // This file is already tracked
			++trackedCount;
	}

	if (untracked.isEmpty())
	{
		QMessageBox::information(parent, tr("Scan complete"), tr("No untracked video files were found under:\n%1").arg(QDir::toNativeSeparators(dir)));
		return {};
	}

	untracked.sort(Qt::CaseInsensitive);

	FindUntrackedFilesDialog dialog(untracked, trackedCount, parent);
	return dialog.exec() == QDialog::Accepted ? dialog.selectedForStaging() : QStringList{};
}

FindUntrackedFilesDialog::FindUntrackedFilesDialog(const QStringList& untrackedFiles, size_t trackedCount, QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Untracked Files"));

	QVBoxLayout* layout = new QVBoxLayout(this);

	QLabel* instructions = new QLabel(
		tr("Found %1 untracked video file(s) - not part of any collection yet. Double-click to "
		   "preview; select files and send them to Quick Import staging. Tracked files found: %2").arg(untrackedFiles.size()).arg(trackedCount), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().InstructionText));
	layout->addWidget(instructions);

	m_list = new QListWidget(this);
	m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);

	// Selected rows get the accent tokens (same as SegmentedToggle's selected segment: AccentBg fill +
	// AccentText) - the "this one's chosen" treatment. Hover-only rows get a neutral translucent overlay
	// of the palette text color (SegmentedToggle's hover trick) - a transient lift, not a state change.
	// outline:none drops Qt's native dotted current-item rect, which sat inset from these rounded rows.
	{
		const Theme::ThemeColors& t = Theme::current();
		const QColor text = m_list->palette().color(QPalette::Text);
		const QColor hoverFill(text.red(), text.green(), text.blue(), 14);
		m_list->setStyleSheet(QStringLiteral(R"(
			QListWidget { outline: none; }
			QListWidget::item { border-radius: %4px; padding: 4px 8px; }
			QListWidget::item:hover { background-color: %1; }
			QListWidget::item:selected { background-color: %2; color: %3; }
			QListWidget::item:selected:hover { background-color: %2; }
		)").arg(hoverFill.name(QColor::HexArgb), t.AccentBg, t.AccentText).arg(Theme::ControlRadius));
	}

	for (const QString& path : untrackedFiles)
	{
		QListWidgetItem* item = new QListWidgetItem(path, m_list);
		item->setData(Qt::UserRole, path);
		item->setToolTip(path);
	}
	layout->addWidget(m_list, 1);

	connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
		const QString path = item->data(Qt::UserRole).toString();
		if (!QFile::exists(path))
		{
			QMessageBox::warning(this, tr("Error"), tr("Video file not found at:\n%1").arg(path));
			return;
		}

		// Parented to the dialog so this modal window doesn't block it; closing
		// the dialog therefore also closes any previews opened from it.
		auto* player = new VideoPlayerWindow(path, MediaId::fromFile(path), this);
		player->show();
	});

	QHBoxLayout* buttons = new QHBoxLayout();
	buttons->addStretch(1);

	m_sendButton = new QPushButton(tr("Send selected to staging"), this);
	m_sendButton->setEnabled(false); // nothing selected initially
	buttons->addWidget(m_sendButton);

	QPushButton* closeButton = new QPushButton(tr("Close"), this);
	buttons->addWidget(closeButton);

	layout->addLayout(buttons);

	connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
		m_sendButton->setEnabled(!m_list->selectedItems().isEmpty());
	});

	connect(m_sendButton, &QPushButton::clicked, this, [this] {
		for (const QListWidgetItem* item : m_list->selectedItems())
			m_selectedForStaging << item->data(Qt::UserRole).toString();
		accept();
	});

	connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

	if (!restoreWindowGeometry(this, "findUntrackedFilesDialog"))
		resize(700, 500);
}

FindUntrackedFilesDialog::~FindUntrackedFilesDialog()
{
	saveWindowGeometry(this, "findUntrackedFilesDialog");
}
