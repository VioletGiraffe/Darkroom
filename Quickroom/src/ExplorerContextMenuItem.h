#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#ifdef Q_OS_WIN
class QMenu;
class QWidget;

// Adds an item that opens Explorer's context menu for paths (all in one folder) next to the item, like a submenu.
// owner is the window the Explorer menu and its verbs belong to. A picked verb runs after menu.exec() returns.
void addExplorerContextMenuItem(QMenu& menu, const QStringList& paths, QWidget& owner);
#endif
