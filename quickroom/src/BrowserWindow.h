#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QMainWindow>
RESTORE_COMPILER_WARNINGS

// Quickroom's main window: a filesystem browser showing folders and media items as a thumbnail grid.
class BrowserWindow final : public QMainWindow
{
public:
	BrowserWindow();
};
