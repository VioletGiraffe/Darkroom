#pragma once

// Central settings keys and compile-time defaults for QSettings.

namespace Settings {
	constexpr const char* RootFolder    = "settings/rootFolder";
	constexpr const char* RecentLibraries = "settings/recentLibraries";  // QStringList of library roots, newest first
	constexpr const char* FfmpegPath    = "settings/ffmpegPath";
	constexpr const char* UseTiff       = "settings/useTiff";
	constexpr const char* JpegQuality   = "settings/jpegQuality";
	constexpr const char* FrameStep     = "settings/frameStep";
	// Label receiving frames extracted to the library from the player. No settings UI - editable in the settings file.
	constexpr const char* ExtractedLabelName        = "settings/extractedLabelName";
	constexpr const char* ColorScheme  = "settings/colorScheme";
	// Shared (not just MainWindow's) since ImportDialog's staged cards mirror it too.
	constexpr const char* PreviewFrameCount = "mainWindow/previewFrameCount";
}

namespace Defaults {
	constexpr bool        UseTiff      = false;
	constexpr int         JpegQuality  = 1; // 1 = best quality, 31 = worst
	constexpr int         FrameStep    = 3; // 1 = every frame, 3 = every 3rd
	constexpr int         ColorScheme  = 0; // Qt::ColorScheme::Unknown = follow system
	constexpr const char* ExtractedLabelName = "Extracted";
	constexpr int         PreviewFrameCount = 4;
}
