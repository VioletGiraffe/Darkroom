#include "Windows/IntegrityCheckDialog.h"
#include "Windows/IntegrityCheckSections.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <memory>
#include <utility>

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

	// A member (not a local) because it owns the row state the section button handlers share.
	m_sections = std::make_unique<IntegrityCheckSections>(report, m_callbacks, content, contentLayout, this);

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
