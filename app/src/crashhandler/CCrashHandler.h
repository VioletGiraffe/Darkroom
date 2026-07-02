#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

class CCrashHandler
{
public:
	explicit CCrashHandler(std::function<void(const wchar_t*)> loggingFunction);
	~CCrashHandler();

	static void setMessageboxOnCrashDisabled();
	static void setWriteFullMemoryDump(bool writeFullDump);

	static void setMinidumpsStorageFolderPath(std::filesystem::path folder);
	static void setCommitId(std::string_view commit_id);
};
