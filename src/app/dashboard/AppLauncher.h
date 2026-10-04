#pragma once

#include "app/http/HttpJsonClient.h"

#include <string>

namespace media {

/// Which application a Dashboard button refers to.
enum class DashboardApp {
	Player,
	Controller,
	Dashboard,
};

const char* toString(DashboardApp app);

/// A child process this process started, or null when the PID is not ours.
///
/// On Windows the handle is closed on drop, which (per the documented
/// CreateProcess contract) is what makes the running process terminate when the
/// Dashboard itself is closed without stopping it. That is deliberate: a
/// launcher should not leave orphans.
struct ChildProcess {
	void* handle = nullptr;      ///< HANDLE on Windows, nullptr elsewhere.
	unsigned long processId = 0;

	ChildProcess() = default;
	~ChildProcess();
	ChildProcess(const ChildProcess&) = delete;
	ChildProcess& operator=(const ChildProcess&) = delete;
	ChildProcess(ChildProcess&& other) noexcept;
	ChildProcess& operator=(ChildProcess&& other) noexcept;

	bool valid() const { return handle != nullptr; }
	/// Release the handle without touching the process (detach semantics).
	void release();
};

/// Whether an application is up, and how we know.
struct AppStatus {
	/// The health endpoint answered.
	bool apiUp = false;
	/// The PID of a child this process started. 0 when we did not start it.
	unsigned long childPid = 0;
	/// Human-readable reason the probe failed; empty while apiUp.
	std::string detail;

	bool running() const { return apiUp; }
};

/// Start and probe one of this repository's applications.
///
/// Discovery of the counterpart executables is by name in the *same directory*
/// as the running executable, which is what makes a Dashboard double-click work
/// with no arguments and no registry entry. A side-by-side layout is therefore
/// required — `build.ps1` and `tools/package_release.ps1` both produce one.
class AppProbe {
public:
	/// The player's control API, and the controller's. The Controller declares
	/// the same pair on its own server type; this is the one place the numbers
	/// are written down.
	static constexpr int kPlayerPort = 8080;
	static constexpr int kControllerPort = 8081;

	explicit AppProbe(DashboardApp app);

	/// Absolute path of the executable, or empty when it was not found.
	const std::string& executablePath() const { return exePath_; }
	bool found() const { return !exePath_.empty(); }
	int port() const { return port_; }

	/// Ask the application's health endpoint whether it is up. Short-fused: a
	/// down app fails in milliseconds.
	void probe();
	const AppStatus& status() const { return status_; }

	/// Launch the executable. Returns false and fills `error` when the file is
	/// missing or the spawn failed. Before spawning, probe() is re-run, so a
	/// double-click cannot start a second copy of an app that is already up.
	bool launch(std::string& error);

	/// Stop the child this process started. Returns false (with `error`) when
	/// the process is not ours — a Player started elsewhere is not ours to kill.
	bool stop(std::string& error);

	/// Forget the child handle without stopping it.
	void detach();

	/// Extra arguments, appended to the launch line.
	void setArguments(std::string arguments) { arguments_ = std::move(arguments); }

private:
	DashboardApp app_ = DashboardApp::Player;
	std::string exePath_;
	std::string arguments_;
	int port_ = 0;
	AppStatus status_;
	ChildProcess child_;
};

/// Name of the executable for `app`, without a directory.
std::string dashboardExecutableName(DashboardApp app);

} // namespace media
