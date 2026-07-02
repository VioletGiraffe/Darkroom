#include "ccrashhandler.h"
#include "NtErrorCodesEnum.h"
#include "NtErrorsReflection.hpp"

#include <assert.h>
#include <atomic>
#include <charconv>
#include <mutex>
#include <string>
#include <string.h>
#include <unordered_map>

#include <Windows.h>
#include <DbgHelp.h>
#include <intrin.h> // rtsc()
#include <signal.h>

static LONG WINAPI seh_handler(EXCEPTION_POINTERS* e);
static void WINAPI on_critical_error(EXCEPTION_POINTERS* e);

static bool g_displayMessageboxOnCrash = true;
static bool g_writeFullMemoryDump = false;
static std::function g_logMessageFunction = [](const wchar_t*) {};

static HMODULE hDbgHelpDll = nullptr;
static decltype(&MiniDumpWriteDump) hMiniDumpWriteDumpFunction = nullptr;

static std::wstring g_minidumpStoragePath;
static std::wstring g_applicationName;
static std::wstring g_commitIdString;

static void logMessage(const wchar_t* message) noexcept
{
	::g_logMessageFunction(message);
}

inline void logMessage(const std::wstring& message) noexcept
{
	logMessage(message.c_str());
}

inline void logMessage(const std::string& message) noexcept
{
	logMessage(std::wstring(message.begin(), message.end()));
}

template <typename T>
inline std::wstring toHexString(T value)
{
	char str[20] = { 0 };

	static constexpr auto prefixLength = 2;
	::memcpy(str, "0x", prefixLength);

	std::to_chars(std::begin(str) + prefixLength, std::end(str), value, 16);

	const auto len = ::strlen(str);
	std::wstring wstr;
	wstr.resize(len);
	for (size_t i = 0; i < len; ++i)
		wstr[i] = str[i];

	return wstr;
}

void CCrashHandler::setMessageboxOnCrashDisabled()
{
	::g_displayMessageboxOnCrash = false;
}

void CCrashHandler::setWriteFullMemoryDump(bool writeFullDump)
{
	::g_writeFullMemoryDump = writeFullDump;
}

namespace {
	struct PendingExceptions {
		struct Guard {
			explicit Guard(PendingExceptions& e) noexcept : _e{ e } {
				if (_e._lock)
					_e._lock.load()->lock();
				else
					assert(_e._lock);
			}

			~Guard() noexcept {
				if (_e._lock)
					_e._lock.load()->unlock();
				else
					assert(_e._lock);
			}

			PendingExceptions* operator->() noexcept {
				return std::addressof(_e);
			}

		private:
			PendingExceptions& _e;
		};

		struct ExceptionInfo {
			explicit ExceptionInfo(const EXCEPTION_POINTERS* excInfo) noexcept {
				// free() in the destructor?
				_e.ContextRecord = (PCONTEXT)malloc(sizeof(*_e.ContextRecord));
				_e.ExceptionRecord = (PEXCEPTION_RECORD)malloc(sizeof(*_e.ExceptionRecord));

				fillFrom(excInfo);
			}

			void fillFrom(const EXCEPTION_POINTERS* excInfo) noexcept {

				// This allocates dynamic memory, but should be okay inside VEH handler? No critical error has yet occurred as this is a first-chance handler.
				memcpy(_e.ContextRecord, excInfo->ContextRecord, sizeof(*excInfo->ContextRecord));
				memcpy(_e.ExceptionRecord, excInfo->ExceptionRecord, sizeof(*excInfo->ExceptionRecord));

				assert(excInfo->ExceptionRecord->ExceptionRecord == nullptr);
				_e.ExceptionRecord->ExceptionRecord = nullptr; // Making sure there are no chained references that may become dangling
			}

			EXCEPTION_POINTERS _e;
		};

		Guard accessor() noexcept {
			return Guard{ *this };
		}

		void registerException(const EXCEPTION_POINTERS* excInfo) {
			const auto tid = GetCurrentThreadId();
			auto existingRecord = _exceptions.find(tid);
			if (existingRecord != _exceptions.end()) {
				existingRecord->second.fillFrom(excInfo);
			}
			else {
				_exceptions.emplace(tid, excInfo);
			}
		}

		EXCEPTION_POINTERS* exceptionForCurrentThread() noexcept {
			auto it = _exceptions.find(GetCurrentThreadId());
			return it != _exceptions.end() ? std::addressof(it->second._e) : nullptr;
		}

		void init() noexcept {
			assert(!_lock);
			_lock = new(std::nothrow) std::mutex;
		}

		void deinit() noexcept {
			auto* lock = _lock.load();
			assert(lock);
			{
				std::lock_guard locker{ *lock };
				_lock = nullptr;
			}

			delete lock;
			_exceptions.clear();
		}

	private:
		std::unordered_map<DWORD /* tid */, ExceptionInfo> _exceptions;
		std::atomic<std::mutex*> _lock{ nullptr };
	};
} // namespace

static PendingExceptions g_pendingExceptions;

static void logException(DWORD excCode)
{
	char message[64] = { 0 };
	const auto exceptionName = EnumReflection::enum_name((NtStatus)excCode);
	//const auto exceptionName = magic_enum::enum_name((Win32ErrorCode)excCode);
	if (!exceptionName.empty())
	{
		static constexpr const char messageStart[] = "Non-C++ VEH exception: ";
		memcpy(message, messageStart, std::size(messageStart) - 1);
		memcpy(message + std::size(messageStart) - 1, exceptionName.data(), exceptionName.size());
	}
	else
	{
		static constexpr const char messageStart[] = "Non-C++ VEH exception: 0x";
		memcpy(message, messageStart, std::size(messageStart) - 1);
		std::to_chars(std::begin(message) + std::size(messageStart) - 1, std::end(message), excCode, 16);
	}

	wchar_t wMessage[sizeof(message)];
	std::copy(std::begin(message), std::end(message), std::begin(wMessage));
	logMessage(wMessage);
}

static LONG WINAPI vehHandler(EXCEPTION_POINTERS* e)
{
	if (!e)
		return EXCEPTION_CONTINUE_SEARCH;

	static constexpr auto isInRange = [](auto low, auto v, auto high) -> bool {
		return v >= low && v <= high;
	};

	static constexpr DWORD CppExceptionCode = 0xe06d7363;

	const DWORD excCode = e->ExceptionRecord->ExceptionCode;

	if (excCode == CppExceptionCode)
	{
		auto exceptions = g_pendingExceptions.accessor();
		// This allocates dynamic memory, but should be okay inside VEH handler? No critical error has yet occurred as this is a first-chance handler.
		exceptions->registerException(e);
		return EXCEPTION_CONTINUE_SEARCH;
	}
	else if (excCode == NtStatus::AccessViolation || excCode == NtStatus::IntegerDivideByZero)
	{
		// Fall through to the fatal error handler
	}
	else if (excCode == STATUS_STACK_OVERFLOW)
	{
		logMessage(L"Stack overflow detected!");
		DebugBreak(); // Stack overflow!

		// Fall through to the fatal error handler
	}
	else if (excCode == STATUS_HEAP_CORRUPTION)
	{
		logMessage(L"Heap corruption detected! Ignoring it for now.");
		return EXCEPTION_CONTINUE_SEARCH;
	}
	else if (
		   isInRange(0x40'00'00'00u, excCode, 0x4F'FF'FF'FFu) // Debug exceptions
		|| isInRange(0x04'00'00'00u, excCode, 0x04'FF'FF'FFu) // Some CLR debug exception 0x04'24'24'20
		|| isInRange(0x80'00'00'00u, excCode, 0x8F'FF'FF'FFu) // WinAPI / COM codes
		|| isInRange(0x6A4u, excCode, 0xF6Eu) // RPC (COM?) codes when not biased to 0x80000000 base
		|| isInRange(0xE0'00'00'00u, excCode, 0xE0'FF'FF'FFu) // Unknown software exception 0xe0434352
		|| isInRange(0x01'00'00'00u, excCode, 0x0F'FF'FF'FFu)
		|| isInRange(0x01'00'00'00u, excCode, 0x0F'FF'FF'FFu)
		|| isInRange(0xC0'00'00'00, excCode, 0xC0'FF'FF'FF)
		|| excCode == NtStatus::IllegalInstruction /* This exception may be used to test whether CPU supports an instruction,
											in which case it's intended to be caught by a SEH handler (executed after VEH).
											Known to occur in dbgeng.dll. */
		|| excCode == NtStatus::PrivilegedInstruction
		|| excCode == 0x0EEDFADE /* Delphi exception code */
		)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}
	else // Unexpected/unknown exception code, log it just in case
	{
		logException(excCode);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Only save a dump and terminate on access violation. Other unexpected errors will be logged above, but are not considered fatal.
	// Thet may be handled by a DLL that caused them with its own SEH handler.
	logException(excCode);
	on_critical_error(e); // noreturn
	return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CALLBACK MyMiniDumpCallback(PVOID /*pParam*/, const PMINIDUMP_CALLBACK_INPUT pInput, PMINIDUMP_CALLBACK_OUTPUT pOutput)
{
	if (!pInput || !pOutput)
		return FALSE;

	BOOL bRet = FALSE;
	switch (pInput->CallbackType)
	{
	case IncludeModuleCallback:
		// Include the module into the dump
		bRet = TRUE;
		break;

	case IncludeThreadCallback:
		// Include the thread into the dump
		bRet = TRUE;
		break;

	case ModuleCallback:
		// Are data sections available for this module ?
		if (pOutput->ModuleWriteFlags & ModuleWriteDataSeg)
		{
			// Yes, but do we really need them?
			if ((pOutput->ModuleWriteFlags & ModuleReferencedByMemory) == 0)
				pOutput->ModuleWriteFlags &= (~ModuleWriteModule); // Do not save info about modules whose memory is not referenced by any call stack
		}

		bRet = TRUE;
		break;

	case ThreadCallback:
		// Include all thread information into the minidump
		bRet = TRUE;
		break;

	case ThreadExCallback:
		// Include this information
		bRet = TRUE;
		break;
	case CancelCallback:
		// No cancel callbacks anymore
		pOutput->Cancel = FALSE;
		pOutput->CheckCancel = FALSE;
		bRet = TRUE;
		break;

	default:
		// Do not return any information for unrecognized
		// callback types.
		bRet = FALSE;
		break;
	}

	return bRet;
}

static void make_minidump(EXCEPTION_POINTERS* e, bool fullDump = false)
{
	assert(hMiniDumpWriteDumpFunction);

	constexpr size_t fileNameBufferSize = 6000;
	WCHAR fileName[fileNameBufferSize];

	{
		WCHAR* name = fileName;
		if (wcscpy_s(name, fileNameBufferSize - 1, g_minidumpStoragePath.data()) != 0)
		{
			logMessage(L"Error in make_minidump::wcscpy_s !!!");
			return;
		}

		auto len = wcslen(fileName);
		name = fileName + len;
		if (wcscpy_s(name, fileNameBufferSize - 1 - len, g_applicationName.c_str()) != 0)
		{
			logMessage(L"Error in make_minidump::wcscpy_s !!!");
			return;
		}

		len = wcslen(fileName);
		name = fileName + len;

		SYSTEMTIME t;
		GetSystemTime(&t);

		// Disambiguation in case multiple threads crash at the same time, just in case
		const auto random = __rdtsc();

		if (swprintf_s(name, fileNameBufferSize - 1 - len, L"_%s_%4d-%02d-%02d_%02d-%02d-%02d-%llu.dmp", ::g_commitIdString.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, random) <= 0)
		{
			logMessage(L"Error in make_minidump::swprintf_s !!!");
			return;
		}
	}

	auto hFile = CreateFileW(fileName, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		assert(hFile != INVALID_HANDLE_VALUE);
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
	exceptionInfo.ThreadId = GetCurrentThreadId();
	exceptionInfo.ExceptionPointers = e;
	exceptionInfo.ClientPointers = FALSE;

	MINIDUMP_CALLBACK_INFORMATION mci;
	mci.CallbackRoutine = (MINIDUMP_CALLBACK_ROUTINE)MyMiniDumpCallback;
	mci.CallbackParam = nullptr; // No context required

	auto minidumpFlags =
		MiniDumpWithIndirectlyReferencedMemory
		| MiniDumpIgnoreInaccessibleMemory
		| MiniDumpScanMemory // This results in MyMiniDumpCallback being called which can decide what info to write and what to omit
		| MiniDumpWithFullAuxiliaryState
		| MiniDumpWithProcessThreadData
		| MiniDumpWithThreadInfo
		;

	if (fullDump)
		minidumpFlags |= (MiniDumpWithFullMemory | MiniDumpWithDataSegs);

	const auto result = hMiniDumpWriteDumpFunction(
		GetCurrentProcess(),
		GetCurrentProcessId(),
		hFile,
		static_cast<MINIDUMP_TYPE>(minidumpFlags),
		e ? &exceptionInfo : nullptr,
		nullptr,
		nullptr);

	const auto errorCode = ::GetLastError();

	CloseHandle(hFile);

	if (result != TRUE)
	{
		static constexpr wchar_t msgPrefix[] = L"hMiniDumpWriteDump returned an error: 0x";

		char errorCodeText[30] = { 0 };
		std::to_chars(std::begin(errorCodeText), std::end(errorCodeText), errorCode, 16);

		wchar_t wMessage[std::size(msgPrefix) + std::size(errorCodeText)];
		memcpy(wMessage, msgPrefix, sizeof(msgPrefix));

		std::copy(std::begin(errorCodeText), std::end(errorCodeText), std::begin(wMessage) + std::size(msgPrefix) - 1);
		logMessage(wMessage);
	}
}

static LONG WINAPI seh_handler(EXCEPTION_POINTERS* e)
{
	const auto excCode = e ? e->ExceptionRecord->ExceptionCode : 0u;
#ifdef _DEBUG
	if (excCode == 0x80000003U) // Breakpoint
		return EXCEPTION_CONTINUE_EXECUTION;
#endif

	if (   excCode == NtStatus::IllegalInstruction
		|| excCode == NtStatus::PrivilegedInstruction
		)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	logMessage(L"SEH exception, continuing search! " + toHexString(excCode));
	//on_critical_error(e); // noreturn
	return EXCEPTION_CONTINUE_SEARCH;
}

static void signalHandler(int signal)
{
	if (signal == SIGABRT)
	{
		auto exceptions = g_pendingExceptions.accessor();
		auto* currentException = exceptions->exceptionForCurrentThread();
		// null if no pending exception in this thread. This shouldn't happen: why SIGABRT if there is no uncaught exception? Other kinds of faults should be handled by SEH / VEH
		if (currentException)
			logMessage(L"SIGABRT with a pending exception.");
		else
			logMessage(L"SIGABRT received, but no pending exception! Creating minidump with EXCEPTION_POINTERS == null.");

		on_critical_error(currentException);
	}
}

static void WINAPI on_critical_error(EXCEPTION_POINTERS* e)
{
	static bool crashAlreadyRecorded = false;
	static std::mutex mtx;

	std::unique_lock lock{ mtx };
	if (crashAlreadyRecorded)
		return;

	crashAlreadyRecorded = true;

	if (e)
		logMessage(L"Critical error: " + toHexString(e->ExceptionRecord->ExceptionCode));
	else
		logMessage(L"Critical error, EXCEPTION_POINTERS is NULL!!!");

	make_minidump(e, g_writeFullMemoryDump);

	if (g_displayMessageboxOnCrash)
	{
		::MessageBoxA(
			nullptr,
			"A critical error occurred, the application will be closed.",
			"Critical error",
			MB_SYSTEMMODAL | MB_ICONERROR | MB_OK
		);
	}

	// To prevent falling into the catch(...) block or encountering secondary crashes in global object destructors
	lock.unlock();
	ExitProcess(0xDEAD);
}
