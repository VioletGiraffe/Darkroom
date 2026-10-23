#include "Windows/IntegrityCheckDialog.h"
#include "Windows/IntegrityCheckTabs.h"
#include "Theme/Theme.h"
#include "compiler/compiler_warnings_control.h"
#include "widgets/cpersistenceenabler.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

#include <memory>
#include <utility>

bool IntegrityCheckDialog::scanAndShowUi(Catalog& catalog, const QString& rootFolder, Callbacks callbacks, QWidget* parent)
{
	// Modal: the scan pumps events while it borrows the catalog.
	QMessageBox progressBox(parent);
	progressBox.setWindowTitle(tr("Catalog integrity check"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	const auto showProgress = [&progressBox](const QString& text) {
		progressBox.setText(text);
		QApplication::processEvents();
	};
	showProgress(tr("Checking catalog entries..."));

	// Throttled: each text update relayouts the box.
	QElapsedTimer sinceProgressShown;
	sinceProgressShown.start();
	CatalogIntegrity::IntegrityReport report = CatalogIntegrity::scan(catalog, rootFolder,
		[&](CatalogIntegrity::ScanPhase phase, int done, int total) {
			if (sinceProgressShown.elapsed() < 50)
				return;
			sinceProgressShown.restart();
			showProgress(phase == CatalogIntegrity::ScanPhase::Items ? tr("Checking catalog entry %1/%2...").arg(done + 1).arg(total)
			                                                         : tr("Scanning library folders (%1)...").arg(done + 1));
		});

	if (report.isEmpty())
	{
		progressBox.close();
		QMessageBox::information(parent, tr("Catalog integrity check"),
			tr("No issues found - catalog entries, label references, and on-disk content are consistent."));
		return false;
	}

	showProgress(tr("Preparing the results..."));
	IntegrityCheckDialog dialog(catalog, std::move(report), std::move(callbacks), parent);
	progressBox.close();
	dialog.exec();
	return true;
}

IntegrityCheckDialog::IntegrityCheckDialog(Catalog& catalog, CatalogIntegrity::IntegrityReport report, Callbacks callbacks, QWidget* parent)
	: QDialog(parent), _callbacks(std::move(callbacks))
{
	setWindowTitle(tr("Catalog Integrity Check"));

	QVBoxLayout* outer = new QVBoxLayout(this);

	QLabel* instructions = new QLabel(
		tr("Catalog inconsistencies and differences from what's actually on disk. Select rows (Ctrl+A selects all) and "
		   "apply an action - nothing here is applied automatically. Fixed rows move to the Resolved tab."), this);
	instructions->setWordWrap(true);
	instructions->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().instructionText.name()));
	outer->addWidget(instructions);

	QTabWidget* tabWidget = new QTabWidget(this);
	outer->addWidget(tabWidget, 1);

	QHBoxLayout* bottomRow = new QHBoxLayout();
	QLabel* statusLabel = new QLabel(this);
	statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::current().instructionText.name()));
	bottomRow->addWidget(statusLabel, 1);
	QPushButton* closeButton = new QPushButton(tr("Close"), this);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
	bottomRow->addWidget(closeButton);
	outer->addLayout(bottomRow);

	_tabs = std::make_unique<IntegrityCheckTabs>(catalog, std::move(report), _callbacks, tabWidget, statusLabel, this);

	resize(900, 600);
	enablePersistence(this, "integrityCheckDialog/window", CPersistenceEnabler::Delayed{ true }, CPersistenceEnabler::SetDefaultSize{ false });
}

IntegrityCheckDialog::~IntegrityCheckDialog() = default;
