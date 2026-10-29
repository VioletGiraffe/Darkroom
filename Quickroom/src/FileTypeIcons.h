#pragma once

#include "compiler/compiler_warnings_control.h"
#include "container/flat_map.hpp"

DISABLE_COMPILER_WARNINGS
#include <QByteArray>
#include <QIcon>
#include <QString>
RESTORE_COMPILER_WARNINGS

// Quickroom's per-format icons from res/filetypes, the ones the installer associates with each type.
namespace FileTypeIcons
{
	// Keys: image formats as QImageReader::format names them, and lowercase camera raw and video suffixes. Values: icon file stems.
	inline const flat_map<QByteArray, QString> ICON_STEMS{
		{ "jpeg", "jpg" }, { "png", "png" }, { "webp", "webp" }, { "tiff", "tiff" }, { "gif", "gif" }, { "bmp", "bmp" },
		{ "svg", "svg" }, { "svgz", "svg" },
		{ "3fr", "3fr" }, { "ari", "ari" }, { "arw", "arw" }, { "bay", "bay" }, { "cr2", "cr2" }, { "cr3", "cr3" },
		{ "crw", "crw" }, { "dcr", "dcr" }, { "dng", "dng" }, { "erf", "erf" }, { "fff", "fff" }, { "iiq", "iiq" },
		{ "k25", "k25" }, { "kdc", "kdc" }, { "mdc", "mdc" }, { "mef", "mef" }, { "mos", "mos" }, { "mrw", "mrw" },
		{ "nef", "nef" }, { "nefx", "nefx" }, { "nrw", "nrw" }, { "orf", "orf" }, { "pef", "pef" }, { "raf", "raf" },
		{ "raw", "raw" }, { "rw2", "rw2" }, { "rwl", "rwl" }, { "sr2", "sr2" }, { "srf", "srf" }, { "srw", "srw" },
		{ "mp4", "mp4" }, { "m4v", "mp4" }, { "mov", "mov" }, { "avi", "avi" }, { "mkv", "mkv" }, { "flv", "flv" },
		{ "webm", "webm" }, { "wmv", "wmv" }, { "mpg", "mpg" }, { "mpeg", "mpg" }, { "m2ts", "mts" }, { "mts", "mts" },
		{ "ts", "mts" },
	};

	// Null for a type without an icon.
	[[nodiscard]] inline QIcon forType(const QByteArray& type)
	{
		const auto stem = ICON_STEMS.find(type);
		if (stem == ICON_STEMS.end())
			return {};

		// Loaded once per icon, on first use: QIcon needs the application object and reads every size of the file.
		static flat_map<QString, QIcon> loaded;
		auto icon = loaded.find(stem.value());
		if (icon == loaded.end())
			icon = loaded.try_emplace(stem.value(), QStringLiteral(":/filetypes/%1.ico").arg(stem.value())).first;
		return icon.value();
	}
}
