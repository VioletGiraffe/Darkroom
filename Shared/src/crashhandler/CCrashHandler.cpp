#include "CCrashHandler.h"

#ifdef _WIN32
#include "CCrashHandler_win_handlers.h"

#include <assert.h>
#include <sstream>
#include <string>
#include <wchar.h>

#undef Q_ASSERT // Q_ASSERT doesn't play nicely with crash handling

CCrashHandler::CCrashHandler(std::function<void(const wchar_t*)> loggingFunction)
{
	::g_logMessageFunction = std::move(loggingFunction);
	assert(::g_logMessageFunction);
	assert(hDbgHelpDll == nullptr);

	WCHAR dbgHelpDllPath[32768] = { 0 };
	auto systemDirectoryPathLength = ::GetSystemDirectoryW(dbgHelpDllPath, std::size(dbgHelpDllPath));
	if (systemDirectoryPathLength <= 0)
	{
		const auto lastError = GetLastError();
		std::wostringstream ss;
		ss << __FUNCTION__ " GetSystemDirectoryW failed. Last error: 0x";
		ss << std::hex << lastError << "\nCCrashHandler not initialized!";
		assert(systemDirectoryPathLength > 0);
		const auto errorMessage = ss.str();
		::g_logMessageFunction(errorMessage.c_str());
		return;
	}
	else if (systemDirectoryPathLength >= std::size(dbgHelpDllPath))
	{
		assert(systemDirectoryPathLength < std::size(dbgHelpDllPath));
		::g_logMessageFunction(__FUNCTIONW__ L" GetSystemDirectoryW returned path longer than MAX_PATH! CCrashHandler not initialized");
		return;
	}

	wcscpy_s(dbgHelpDllPath + systemDirectoryPathLength, std::size(dbgHelpDllPath) - systemDirectoryPathLength, L"\\dbghelp.dll");

	hDbgHelpDll = LoadLibraryW(dbgHelpDllPath);
	if (!hDbgHelpDll)
	{
		assert(hDbgHelpDll);
		::g_logMessageFunction((__FUNCTIONW__ " Failed to load dbghelp.dll at " + std::wstring{ dbgHelpDllPath }).c_str());
		return;
	}

	hMiniDumpWriteDumpFunction = (decltype(&MiniDumpWriteDump))GetProcAddress(hDbgHelpDll, "MiniDumpWriteDump");
	if (!hMiniDumpWriteDumpFunction)
	{
		::g_logMessageFunction(__FUNCTIONW__ L" Failed to load MiniDumpWriteDump function from dbghelp.dll");
		assert(hMiniDumpWriteDumpFunction);
		return;
	}

	WCHAR appName[32768] = {0};
	GetModuleFileNameW(nullptr, appName, std::size(appName));
	g_applicationName = appName;
	if (const auto lastSlash = g_applicationName.rfind(L'\\'); lastSlash != std::string::npos)
	{
		g_applicationName.erase(g_applicationName.begin(), g_applicationName.begin() + lastSlash + 1);
	}

	assert(!g_applicationName.empty());

	SetUnhandledExceptionFilter(&seh_handler);
	[[maybe_unused]] const auto previousSignalHandler = signal(SIGABRT, &signalHandler);
	assert(previousSignalHandler == nullptr);
	AddVectoredExceptionHandler(0 /* last */, &vehHandler);

	::g_logMessageFunction((__FUNCTIONW__ L": applicationName set to " + g_applicationName).c_str());
}

CCrashHandler::~CCrashHandler()
{
	RemoveVectoredExceptionHandler(&vehHandler);
	if (hDbgHelpDll)
		FreeLibrary(hDbgHelpDll);

	hDbgHelpDll = nullptr;

	g_pendingExceptions.deinit();
	::g_logMessageFunction = [](const wchar_t*) {};
}

void CCrashHandler::setMinidumpsStorageFolderPath(std::filesystem::path folder) {
	folder.make_preferred();

	g_minidumpStoragePath = folder.native();
	if (!g_minidumpStoragePath.ends_with(L'\\'))
		g_minidumpStoragePath += L'\\';

	::g_logMessageFunction((std::wstring{L"Minidump storage folder: "} + g_minidumpStoragePath).c_str());

	g_pendingExceptions.init();
}

void CCrashHandler::setCommitId(std::string_view commit_id)
{
	::g_commitIdString = std::wstring{ commit_id.begin(), commit_id.end() };
}

#else

CCrashHandler::CCrashHandler(std::function<void(const wchar_t*)>)
{
}

CCrashHandler::~CCrashHandler()
{
}

void CCrashHandler::setMessageboxOnCrashDisabled()
{
}

void CCrashHandler::setWriteFullMemoryDump(bool /*writeFullDump*/)
{
}

void CCrashHandler::setMinidumpsStorageFolderPath(std::filesystem::path /*folder*/)
{
}

void CCrashHandler::setCommitId(std::string_view /*commit_id*/)
{
}

#endif // _WIN32
