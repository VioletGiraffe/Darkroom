#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QVariant>
RESTORE_COMPILER_WARNINGS

// Darkroom's QSettings application name: the fallback store of readSharedSetting.
constexpr const char* DarkroomApplicationName = "Darkroom";

// Reads one of Settings.h's keys from this app's own store, else from Darkroom's, else returns defaultValue.
// Writes go to the app's own store, which then overrides Darkroom's value for this app.
[[nodiscard]] QVariant readSharedSetting(const char* key, const QVariant& defaultValue = {});
