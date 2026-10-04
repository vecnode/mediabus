#include "control/AppLauncher.h"

#include "core/Log.h"
#include "core/Platform.h"

#include <filesystem>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace media {
namespace {

/// Candidate file names for an application, most specific first.
std::vector<std::string> candidateNames(DashboardApp app) {
	switch (app) {
		case DashboardApp::Player:
			return {"vn-mediabus-player.exe"};
		case DashboardApp::Controller:
			return {"vn-mediabus-controller.exe"};
		case DashboardApp::Dashboard:
			return {"vn-mediabus-dashboard.exe"};
	}
	return {};
}

const char* appLabel(DashboardApp app) {
	switch (app) {
		case DashboardApp::Player: return "Player";
		case DashboardApp::Controller: return "Controller";
		case DashboardApp::Dashboard: return "Dashboard";
	}
	return "Application";
}

} // namespace

const char* toString(DashboardApp app) {
	switch (app) {
		case DashboardApp::Player: return "player";
		case DashboardApp::Controller: return "controller";
		case DashboardApp::Dashboard: return "dashboard";
	}
	return "unknown";
}

std::string dashboardExecutableName(DashboardApp app) {
	const std::vector<std::string> names = candidateNames(app);
	return names.empty() ? std::string() : names.front();
}

ChildProcess::~ChildProcess() {
	release();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
	: handle(other.handle), processId(other.processId) {
	other.handle = nullptr;
	other.processId = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
	if (this != &other) {
		release();
		handle = other.handle;
		processId = other.processId;
		other.handle = nullptr;
		other.processId = 0;
	}
	return *this;
}

void ChildProcess::release() {
#if defined(_WIN32)
	if (handle != nullptr) {
		CloseHandle(static_cast<HANDLE>(handle));
	}
#endif
	handle = nullptr;
	processId = 0;
}

AppProbe::AppProbe(DashboardApp app) : app_(app) {
	// The counterpart apps live beside this executable, which is exactly how
	// bin/ and dist/ are laid out. Resolving through Platform keeps this
	// independent of the working directory the Dashboard was launched from.
	const std::string directory = platform::executableDirectory();
	std::error_code ec;
	for (const std::string& name : candidateNames(app_)) {
		const std::string candidate = directory + name;
		if (std::filesystem::is_regular_file(candidate, ec)) {
			exePath_ = candidate;
			break;
		}
	}
	switch (app_) {
		case DashboardApp::Player: port_ = kPlayerPort; break;
		case DashboardApp::Controller: port_ = kControllerPort; break;
		case DashboardApp::Dashboard: port_ = 0; break;
	}
	if (!found()) {
		LOG_WARN("Dashboard") << appLabel(app_) << " not found next to "
			<< directory << " (expected " << dashboardExecutableName(app_) << ")";
	}
}

void AppProbe::probe() {
	if (app_ == DashboardApp::Dashboard) {
		// The Dashboard is trivially "up": it is the thing doing the asking.
		status_.apiUp = true;
		status_.detail.clear();
		return;
	}
	HttpJsonClient client("127.0.0.1", port_);
	std::string error;
	status_.apiUp = client.ping(error);	status_.detail = status_.apiUp ? std::string() : error;

	// A child that exited on its own must stop reporting a stale PID, or the
	// Dashboard would offer to stop something that is already gone.
	if (status_.childPid != 0 && !status_.apiUp) {
#if defined(_WIN32)
		HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
			status_.childPid);
		if (handle == nullptr) {
			child_.release();
			status_.childPid = 0;
		} else {
			DWORD code = 0;
			const bool exited = GetExitCodeProcess(handle, &code) && code != STILL_ACTIVE;
			CloseHandle(handle);
			if (exited) {
				child_.release();
				status_.childPid = 0;
			}
		}
#endif
	}
}

bool AppProbe::launch(std::string& error) {
	if (!found()) {
		error = std::string("not found: ") + dashboardExecutableName(app_);
		return false;
	}
	// Re-probe first: an app that is already up must not be started twice.
	probe();
	if (status_.apiUp) {
		error = std::string(appLabel(app_)) + " is already running";
		return false;
	}

#if defined(_WIN32)
	std::string commandLine = "\"" + exePath_ + "\"";
	if (!arguments_.empty()) {
		commandLine += " " + arguments_;
	}
	// CreateProcess needs a mutable buffer, and the working directory should be
	// the app's own directory so any relative path it uses means what it means.
	std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
	mutableCommand.push_back('\0');

	std::string workingDirectory = exePath_;
	const std::size_t slash = workingDirectory.find_last_of("\\/");
	workingDirectory = (slash == std::string::npos)
		? std::string() : workingDirectory.substr(0, slash);

	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION info{};
	const BOOL spawned = CreateProcessA(
		nullptr,
		mutableCommand.data(),
		nullptr, nullptr,
		FALSE,
		0,
		nullptr,
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
		&startup, &info);
	if (!spawned) {
		error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
		return false;
	}
	child_.handle = info.hProcess;
	child_.processId = info.dwProcessId;
	// The thread handle is not needed; leaving it open would leak it.
	CloseHandle(info.hThread);

	status_.childPid = child_.processId;
	LOG_NOTICE("Dashboard") << "launched " << appLabel(app_) << " (pid "
		<< status_.childPid << ")";
	error.clear();
	return true;
#else
	error = "launching is only implemented on Windows in this build";
	return false;
#endif
}

bool AppProbe::stop(std::string& error) {
	if (child_.processId == 0) {
		error = std::string(appLabel(app_))
			+ " was not started by this Dashboard; stop it from its own window";
		return false;
	}
#if defined(_WIN32)
	HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, child_.processId);
	if (handle == nullptr) {
		error = "could not open the process";
		return false;
	}
	const BOOL killed = TerminateProcess(handle, 0);
	CloseHandle(handle);
	if (!killed) {
		error = "TerminateProcess failed";
		return false;
	}
	child_.release();
	status_.childPid = 0;
	LOG_NOTICE("Dashboard") << "stopped " << appLabel(app_);
	error.clear();
	return true;
#else
	error = "stopping is only implemented on Windows in this build";
	return false;
#endif
}

void AppProbe::detach() {
	child_.release();
	status_.childPid = 0;
}

} // namespace media
