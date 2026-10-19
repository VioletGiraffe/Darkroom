#include "SharedSettings.h"

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QCoreApplication>
#include <QSettings>
RESTORE_COMPILER_WARNINGS

QVariant readSharedSetting(const char* key, const QVariant& defaultValue)
{
	if (const QSettings own; own.contains(key))
		return own.value(key);

	return QSettings{ QCoreApplication::organizationName(), DarkroomApplicationName }.value(key, defaultValue);
}
