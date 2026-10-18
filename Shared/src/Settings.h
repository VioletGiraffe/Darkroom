#pragma once

namespace Settings {
	constexpr const char* FfmpegPath    = "settings/ffmpegPath";
	constexpr const char* UseTiff       = "settings/useTiff";
	constexpr const char* JpegQuality   = "settings/jpegQuality";
}

namespace Defaults {
	constexpr bool        UseTiff      = false;
	constexpr int         JpegQuality  = 1;
}
