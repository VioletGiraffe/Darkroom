#include "LogViewerDialog.h"

#include "logger/cloggerinmemory.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

LogViewerDialog::LogViewerDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Log"));
	resize(900, 500);

	m_view = new QPlainTextEdit(this);
	m_view->setReadOnly(true);
	m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));  // monospaced keeps the file:line columns readable

	auto* buttons = new QDialogButtonBox(this);
	QPushButton* refreshButton = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
	QPushButton* copyButton = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
	buttons->addButton(QDialogButtonBox::Close);

	connect(refreshButton, &QPushButton::clicked, this, &LogViewerDialog::refresh);
	connect(copyButton, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(m_view->toPlainText()); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(m_view);
	layout->addWidget(buttons);

	refresh();
}

void LogViewerDialog::refresh()
{
	m_view->setPlainText(loggerInstance<CLoggerInMemory>().contents().join('\n'));
	m_view->moveCursor(QTextCursor::End);  // entries are appended in order, so the newest sit at the bottom
}
