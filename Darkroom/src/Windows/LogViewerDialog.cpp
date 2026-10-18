#include "LogViewerDialog.h"

#include "compiler/compiler_warnings_control.h"
#include "logger/cloggerinmemory.h"

DISABLE_COMPILER_WARNINGS
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
RESTORE_COMPILER_WARNINGS

LogViewerDialog::LogViewerDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Log"));
	resize(900, 500);

	_view = new QPlainTextEdit(this);
	_view->setReadOnly(true);
	_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

	auto* buttons = new QDialogButtonBox(this);
	QPushButton* refreshButton = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
	QPushButton* copyButton = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
	buttons->addButton(QDialogButtonBox::Close);

	connect(refreshButton, &QPushButton::clicked, this, &LogViewerDialog::refresh);
	connect(copyButton, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(_view->toPlainText()); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(_view);
	layout->addWidget(buttons);

	refresh();
}

void LogViewerDialog::refresh()
{
	_view->setPlainText(loggerInstance<CLoggerInMemory>().contents().join('\n'));
	_view->moveCursor(QTextCursor::End);
}
