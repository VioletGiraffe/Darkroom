#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
RESTORE_COMPILER_WARNINGS

// Open players and image viewers close their file when it is at or under one of paths: Windows refuses to delete or move an open file.
// The windows stay open without the file; only a window whose own removal action then fails reloads it.
void releaseMediaFilesHeldOpen(const QStringList& paths);
