#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDialog>
RESTORE_COMPILER_WARNINGS

class QPlainTextEdit;

// Read-only viewer for the in-memory Qt message log (loggerInstance<CLoggerInMemory>()). Refresh re-reads the
// sink, so entries logged by background threads while the dialog is open appear on demand.
class LogViewerDialog : public QDialog
{
public:
	explicit LogViewerDialog(QWidget* parent = nullptr);

private:
	void refresh();

	QPlainTextEdit* _view = nullptr;
};
