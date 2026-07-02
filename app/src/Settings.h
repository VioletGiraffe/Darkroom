#pragma once

// Central settings keys and compile-time defaults for QSettings.
// Use QSettings{}.value(Settings::Foo, Defaults::Foo) everywhere.

namespace Settings {
	constexpr const char* RootFolder    = "settings/rootFolder";
	constexpr const char* FfmpegPath    = "settings/ffmpegPath";
	constexpr const char* UseTiff       = "settings/useTiff";
	constexpr const char* JpegQuality   = "settings/jpegQuality";
	constexpr const char* FrameStep     = "settings/frameStep";
	constexpr const char* PlaybackSpeed = "VideoPlayer/PlaybackSpeed";
	constexpr const char* PauseOnSeek  = "VideoPlayer/PauseOnSeek";
	constexpr const char* ColorScheme  = "settings/colorScheme";
	// Shared (not just MainWindow's) since QuickImportDialog's staged cards mirror it too.
	constexpr const char* PreviewFrameCount = "mainWindow/previewFrameCount";
}

namespace Defaults {
	constexpr const char* RootFolder   = "H:/VideoFrames";
	constexpr bool        UseTiff      = false;
	constexpr int         JpegQuality  = 1; // 1 = best quality, 31 = worst
	constexpr int         FrameStep    = 3; // 1 = every frame, 3 = every 3rd
	constexpr bool        PauseOnSeek  = true;
	constexpr int         ColorScheme  = 0; // Qt::ColorScheme::Unknown = follow system
	constexpr int         PreviewFrameCount = 4;
}
