#pragma once

// Central settings keys and compile-time defaults for QSettings.
// Use QSettings{}.value(Settings::Foo, Defaults::Foo) everywhere.

namespace Settings {
	constexpr const char* RootFolder    = "settings/rootFolder";
	constexpr const char* RecentLibraries = "settings/recentLibraries";  // QStringList of library roots, newest first
	constexpr const char* FfmpegPath    = "settings/ffmpegPath";
	constexpr const char* UseTiff       = "settings/useTiff";
	constexpr const char* JpegQuality   = "settings/jpegQuality";
	constexpr const char* FrameStep     = "settings/frameStep";
	constexpr const char* PlaybackSpeed = "VideoPlayer/PlaybackSpeed";
	constexpr const char* PauseOnSeek  = "VideoPlayer/PauseOnSeek";
	constexpr const char* Volume       = "VideoPlayer/Volume"; // UI slider position, 0..100 (perceptual)
	constexpr const char* Muted        = "VideoPlayer/Muted";
	constexpr const char* LastFrameExtractionMode   = "VideoPlayer/LastFrameExtractionMode";   // "library" or "folder"; unset = no extraction done yet
	constexpr const char* LastFrameExtractionFolder = "VideoPlayer/LastFrameExtractionFolder";
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
	constexpr bool        PauseOnSeek  = true;
	constexpr int         Volume       = 100; // full, on the 0..100 UI scale
	constexpr bool        Muted        = false;
	constexpr int         ColorScheme  = 0; // Qt::ColorScheme::Unknown = follow system
	constexpr const char* ExtractedLabelName = "Extracted";
	constexpr int         PreviewFrameCount = 4;
}
