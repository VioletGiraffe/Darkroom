#pragma once

// Keyboard shortcuts shared across more than one window, gathered here so the sites can't silently drift.
// Single-site shortcuts stay inline at their point of use (the codebase's usual convention) - only genuinely
// cross-window ones belong here. Stored as string literals (QKeySequence's portable text form) and turned into
// a QKeySequence at the point of use, so nothing constructs a QKeySequence during static initialization.
struct Shortcuts
{
	static constexpr const char* CreateLabel    = "Ctrl+L";        // LabelSidebar + QuickImportDialog "Create label"
	static constexpr const char* RemoveFromList = "Delete";        // MainWindow "Remove from library" / QuickImport "Remove from staging"
	static constexpr const char* DeleteFile     = "Shift+Delete";  // MainWindow "Delete" / QuickImport "Delete source file(s)"
};
