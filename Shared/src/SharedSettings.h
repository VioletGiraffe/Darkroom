#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QVariant>
RESTORE_COMPILER_WARNINGS

// Darkroom's QSettings application name: the store of shared settings and the defaults of Quickroom's own.
constexpr const char* DarkroomApplicationName = "Darkroom";

// One value for both apps, kept in Darkroom's store.
[[nodiscard]] QVariant readSharedSetting(const char* key, const QVariant& defaultValue = {});
void writeSharedSetting(const char* key, const QVariant& value);

// Reads this app's own store, else Darkroom's, else returns defaultValue. Writes go to the app's own store.
[[nodiscard]] QVariant readSettingOrDarkroomDefault(const char* key, const QVariant& defaultValue);
