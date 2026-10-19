#pragma once

#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <vector>

// Open players and image viewers close their file when it is at or under one of paths: Windows refuses to delete or move an open file.
// The windows stay open without the file; followMovedMediaFiles reopens it after a move, and a window whose own removal action fails reloads it.
void releaseMediaFilesHeldOpen(const QStringList& paths);

// Concludes a move of paths released by releaseMediaFilesHeldOpen. Players and viewers rebase the paths they list by completedMoves,
// and reopen a released file at its new location, or where it was when its move is not among completedMoves.
void followMovedMediaFiles(const QStringList& releasedPaths, const std::vector<PathMove>& completedMoves);

// QFile::rename of a file or folder between releaseMediaFilesHeldOpen and followMovedMediaFiles.
[[nodiscard]] bool renameReleasingOpenFiles(const QString& from, const QString& to);
