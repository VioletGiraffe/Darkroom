#pragma once

// Routes Qt messages, AdvancedAssert and Profiler output to loggerInstance<CLoggerInMemory>(); Qt messages also reach the previous handler.
// Must be called before constructing QApplication: plugin and platform diagnostics are emitted during its construction.
void installMemoryLog();
