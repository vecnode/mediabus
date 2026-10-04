#include "core/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>    // _O_APPEND, _O_TEXT: the flags _open_osfhandle takes
#include <io.h>
#endif

namespace media::log {
namespace {

std::mutex gMutex;
Level gThreshold = Level::Notice;

/// A file sink for a process that has no console.
///
/// All three applications are Windows-subsystem binaries, which is what stops
/// Windows opening a console window beside them - the thing a GUI application
/// must never do. The cost is that stderr goes nowhere, so when there is no
/// console to write to the log goes to a file next to the executable instead.
///
/// This is deliberately not "always log to a file": when a person runs the app
/// from an existing console (or a script redirects its output, as
/// tools/verify-live.ps1 does), that console is what they are looking at, and
/// duplicating into a file would leave stale logs lying around for no reason.
std::FILE* gFile = nullptr;
std::string gFilePath;

/// Write one line to whichever sink is open. Caller holds the mutex.
void writeLineLocked(const char* level, const char* category,
	const std::string& text) {
	if (gFile != nullptr) {
		std::fprintf(gFile, "[%s] %s: %s\n", level, category, text.c_str());
		std::fflush(gFile);
		return;
	}
	std::fprintf(stderr, "[%s] %s: %s\n", level, category, text.c_str());
	std::fflush(stderr);
}

const char* levelName(Level level) {
	switch (level) {
		case Level::Verbose: return "verbose";
		case Level::Notice:  return "notice";
		case Level::Warning: return "warning";
		case Level::Error:   return "error";
	}
	return "?";
}

#if defined(_WIN32)

/// True when standard error already reaches something useful.
///
/// A process that has been started by a shell which redirected its output has a
/// valid stderr handle even with no console attached, and that redirection is
/// what the verification scripts rely on. Either case means "do not open a file".
bool stderrIsUseful() {
	const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	// A handle that is a pipe or a file is a redirection: write to it.
	const DWORD type = GetFileType(handle);
	if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) {
		return true;
	}
	// A character device is a console, but only if one is actually attached.
	if (type == FILE_TYPE_CHAR && GetConsoleWindow() != nullptr) {
		return true;
	}
	return false;
}

/// Try to inherit the console of the process that started us.
///
/// This is what makes `vn-mediabus-controller.exe --list-scripts` print to the
/// terminal a person launched it from, even though the binary is a subsystem
/// application. It is the standard trick for a GUI application that also has a
/// command-line mode.
void attachToParentConsole() {
	if (GetConsoleWindow() != nullptr) {
		return;   // already have one
	}
	if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
		return;   // started from Explorer or a service: there is no console
	}
	// AttachConsole gives the process a console but does NOT rebind the standard
	// handles to it. Without reopening them, every write silently goes nowhere.
	std::FILE* stream = nullptr;
	if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0) {
		(void)stream;
	}
	if (freopen_s(&stream, "CONOUT$", "w", stderr) != 0) {
		(void)stream;
	}
	// ...and unbuffer, so a short-lived command-line run prints before exiting.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::setvbuf(stderr, nullptr, _IONBF, 0);
}

#else

bool stderrIsUseful() { return true; }
void attachToParentConsole() {}

#endif

/// Open `<executable directory>/<name>.log`, or give up quietly.
///
/// Quietly is the point: a logging path that cannot be opened must not be a
/// reason for the application to refuse to start.
void openLogFile(const char* name) {
#if defined(_WIN32)
	wchar_t buffer[MAX_PATH] = {0};
	const DWORD written = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	if (written == 0 || written >= MAX_PATH) {
		return;
	}
	std::wstring path(buffer, written);
	const std::size_t slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		return;
	}
	path.erase(slash + 1);
	path += L"mediabus-";
	std::wstring wideName;
	for (const char* c = name; *c != '\0'; ++c) {
		wideName.push_back(static_cast<wchar_t>(*c));
	}
	path += wideName;
	path += L".log";

	// A handle created over a wide path sidesteps the ANSI code page entirely, so
	// a user name or install path with non-ASCII characters still works.
	const HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return;
	}
	// The CRT owns the descriptor from here and closes it at exit.
	const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
		_O_APPEND | _O_TEXT);
	if (fd == -1) {
		CloseHandle(handle);
		return;
	}
	gFile = _fdopen(fd, "a");
	if (gFile == nullptr) {
		_close(fd);
		return;
	}
	char utf8[1024] = {0};
	const int converted = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8,
		static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
	if (converted > 0) {
		gFilePath.assign(utf8, static_cast<std::size_t>(converted - 1));
	}
#else
	(void)name;
#endif
}

/// Decide where the log goes. Runs once, on the first line.
///
/// Deliberately lazy rather than a call each application has to remember: a
/// logging system that only works if the caller initialised it is a logging
/// system that silently does nothing in the one application that forgot. An
/// application that wants the file named after itself rather than after whatever
/// logged first calls useFileSink() at startup, which runs this with that name.
void ensureSink(const char* name) {
	static std::once_flag once;
	std::call_once(once, [name] {
		attachToParentConsole();
		if (!stderrIsUseful()) {
			openLogFile(name);
			if (gFile != nullptr) {
				writeLineLocked("notice", "Log",
					"no console attached; writing here instead");
			}
		}
	});
}

} // namespace

Level& threshold() {
	return gThreshold;
}

bool enabled(Level level) {
	return static_cast<int>(level) >= static_cast<int>(gThreshold);
}

void setThresholdFromEnv() {
	const char* raw = std::getenv("MEDIA_LOG_LEVEL");
	if (raw == nullptr) {
		return;
	}
	if (std::strcmp(raw, "verbose") == 0 || std::strcmp(raw, "trace") == 0) {
		gThreshold = Level::Verbose;
	} else if (std::strcmp(raw, "notice") == 0 || std::strcmp(raw, "info") == 0) {
		gThreshold = Level::Notice;
	} else if (std::strcmp(raw, "warning") == 0 || std::strcmp(raw, "warn") == 0) {
		gThreshold = Level::Warning;
	} else if (std::strcmp(raw, "error") == 0) {
		gThreshold = Level::Error;
	}
}

void useFileSink(const char* name) {
	ensureSink(name);
}

void writeLine(const char* level, const char* category, const std::string& text) {
	// One locked write so concurrent lines never interleave mid-message.
	std::lock_guard<std::mutex> lock(gMutex);
	writeLineLocked(level, category, text);
}

Line::Line(Level level, const char* category)
	: level_(level), category_(category), active_(enabled(level)) {
	if (active_) {
		// The first line the process emits decides where logs go, and the file it
		// opens is named after the application - so the three of them can run side
		// by side without fighting over one log.
		ensureSink(category_);
	}
}

Line::~Line() {
	if (!active_) {
		return;
	}
	writeLine(levelName(level_), category_, stream_.str());
}

} // namespace media::log
