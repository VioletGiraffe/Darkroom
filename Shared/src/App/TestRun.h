#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

// `<app> --test-run`, for CI: starts the deployed app on settings in a temporary folder, then exits 0 once a window
// is up, no dialog blocks it, and the Qt plugins the apps need are deployed.
// Application runs the steps every app shares; an app adds only the fixtures its startup needs.
namespace TestRun {

// Only the first argument counts: Quickroom takes a path there otherwise.
[[nodiscard]] bool requested(int argc, char** argv);
// Must run before the first QSettings is constructed.
void isolateSettings();
// Holds the isolated settings; an app's fixtures go here too. Removed at a normal exit.
[[nodiscard]] QString temporaryFolder();
// Requires the QApplication. The verdict is due once the event loop has run for a few seconds.
void scheduleVerdict();
void reportFailure(const QString& reason);

} // namespace TestRun
