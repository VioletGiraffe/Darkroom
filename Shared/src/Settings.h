#pragma once

namespace Settings {
	// Shared: readSharedSetting / writeSharedSetting.
	constexpr const char* FfmpegPath    = "settings/ffmpegPath";
	// Each app's own: readSettingOrDarkroomDefault.
	constexpr const char* UseTiff       = "settings/useTiff";
	constexpr const char* JpegQuality   = "settings/jpegQuality";
}

namespace Defaults {
	constexpr bool        UseTiff      = false;
	constexpr int         JpegQuality  = 1;
}
