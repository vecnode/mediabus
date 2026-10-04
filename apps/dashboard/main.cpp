/*
 * vn-mediabus-dashboard - launcher for the Player and the Controller
 *
 * This is the "mother app": the one thing an operator starts, and the one thing
 * that stays running. It lives in the notification area (the tray) with a menu
 * that starts and stops the other two applications, so the Player and the
 * Controller can be closed and reopened without losing the launcher.
 *
 * Closing its window hides it rather than exiting, and QUIT in the tray menu is
 * the only way out — unless the tray could not be created at all, in which case
 * the window goes back to being an ordinary window that closes normally. That
 * fallback matters: a launcher with no tray and no exit is a process the user
 * cannot get rid of.
 *
 * Liveness is decided by each application's own health endpoint, not by process
 * enumeration: that is portable, it needs no elevation, and it answers the
 * question that matters ("is its API answering?") rather than a proxy for it.
 *
 * Drawing goes through media::RenderDevice, as in both other apps: this file
 * must never name an OpenGL symbol. The tray is shell integration, not
 * rendering, and it is the only Win32 detail in here — and it lives in
 * TrayIcon.cpp, not in this file.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "control/AppLauncher.h"
#include "control/DashboardModel.h"
#include "view/DashboardView.h"
#include "win32/TrayIcon.h"
#include "win32/FolderPicker.h"
#include "gfx/UiScaleGlfw.h"
#include "net/HttpJsonClient.h"
#include "gfx/RenderDevice.h"
#include "core/AppConfig.h"
#include "core/Log.h"
#include "core/UiScale.h"

#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

GLFWwindow* gWindow = nullptr;

struct Options {
	int width = media::DashboardModel::kDefaultWidth;
	int height = media::DashboardModel::kDefaultHeight;
	/// Start hidden, living only in the tray. What run.bat asks for.
	bool startInTray = false;
	/// Never create the tray icon: an ordinary window that closes normally.
	/// The escape hatch for a machine where Shell_NotifyIcon does not work.
	bool noTray = false;
};

void printUsage() {
	std::printf(
		"vn-mediabus-dashboard - launcher for the Player and the Controller\n"
		"\n"
		"Usage: vn-mediabus-dashboard.exe [options]\n"
		"\n"
		"  --width N     window width  (default %d, times the monitor DPI scale)\n"
		"  --height N    window height (default %d, times the monitor DPI scale)\n"
		"  --tray        start hidden, in the notification area\n"
		"  --no-tray     never add a tray icon; the window is the whole UI and\n"
		"                closing it exits (use this if no tray icon appears)\n"
		"  --help, -h    show this text\n"
		"\n"
		"Tray:  right-click the icon for Launch Player / Launch Controller / Quit.\n"
		"       Left-click shows or hides this window.\n"
		"Window: click LAUNCH on a row. STOP only stops an app this launcher "
		"started.\n"
		"        CHANGE... picks the media corpus folder the Player reads.\n"
		"Keys:  Esc hides the window while the tray icon is there; QUIT in the "
		"tray menu exits.\n",
		media::DashboardModel::kDefaultWidth, media::DashboardModel::kDefaultHeight);
}

bool parseOptions(int argc, char** argv, Options& out) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto nextInt = [&](int& target) {
			if (i + 1 < argc) {
				target = std::atoi(argv[++i]);
			}
		};
		if (arg == "--help" || arg == "-h") {
			printUsage();
			return false;
		} else if (arg == "--width") {
			nextInt(out.width);
		} else if (arg == "--height") {
			nextInt(out.height);
		} else if (arg == "--tray" || arg == "--hidden") {
			out.startInTray = true;
		} else if (arg == "--no-tray") {
			out.noTray = true;
		} else {
			LOG_WARN("Dashboard") << "ignoring unknown argument: " << arg;
		}
	}
	// --tray asks to be invisible with only an icon to reach it; --no-tray asks
	// for exactly the opposite. Honouring both would produce a process with no
	// icon and no window, so the explicit "no tray" wins and says so.
	if (out.noTray && out.startInTray) {
		LOG_WARN("Dashboard") << "--no-tray overrides --tray: the window is shown";
		out.startInTray = false;
	}
	return true;
}

/// Tell Windows this process understands DPI, before any window exists, so
/// glfwGetMonitorContentScale reports the real scale instead of 1.0 and the
/// text is drawn at real pixels rather than scaled up by the compositor.
/// Mirrors the same call in the Controller and the Player.
void enableDpiAwareness() {
#if defined(_WIN32)
	typedef BOOL (WINAPI *SetAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
	if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
		auto setContext = reinterpret_cast<SetAwarenessContextFn>(
			reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
		if (setContext != nullptr
			&& setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
			return;
		}
	}
	SetProcessDPIAware();
#endif
}

void onGlfwError(int code, const char* description) {
	LOG_ERROR("GLFW") << code << ": " << description;
}

/// The launcher state: one probe per application plus the model the view draws.
///
/// Probing does a real HTTP request, so it runs on its own thread and publishes
/// a snapshot under a mutex — never on the frame loop, which must not wait on a
/// socket even with a short timeout.
class Dashboard {
public:
	/// What the Player says about its corpus, read fresh on every probe pass.
	///
	/// The Player is the authority here: it owns the library, so asking it is
	/// what keeps this panel from reporting a folder the decoder is not using.
	struct CorpusSnapshot {
		bool online = false;
		std::string folder;
		std::size_t clipCount = 0;
	};

	Dashboard()
		: player_(media::DashboardApp::Player),
		  controller_(media::DashboardApp::Controller) {}

	void start() {
		refresh();
		thread_ = std::thread([this] {
			while (!stopping_.load()) {
				// Cheap enough to poll often, and it is what makes a launch
				// from another shell show up here within a fraction of a second.
				for (int slept = 0; slept < 700 && !stopping_.load(); slept += 20) {
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				if (!stopping_.load()) {
					refresh();
				}
			}
		});
	}

	void stop() {
		stopping_ = true;
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	/// One synchronous pass: probe both applications, read the Player's corpus
	/// state, and publish the result.
	void refresh() {
		player_.probe();
		controller_.probe();
		CorpusSnapshot corpus = readCorpus();

		std::lock_guard<std::mutex> lock(mutex_);
		playerStatus_ = player_.status();
		controllerStatus_ = controller_.status();
		playerPath_ = player_.executablePath();
		controllerPath_ = controller_.executablePath();
		corpus_ = std::move(corpus);
	}

	/// Publish the latest snapshot into the model the view draws.
	void applyTo(media::DashboardModel& model) {
		std::lock_guard<std::mutex> lock(mutex_);
		model.setStatus(media::DashboardApp::Player, playerStatus_, playerPath_,
			media::AppProbe::kPlayerPort);
		model.setStatus(media::DashboardApp::Controller, controllerStatus_,
			controllerPath_, media::AppProbe::kControllerPort);

		// The *configured* folder is what this Dashboard is offering; the live
		// count comes from the Player. Keeping both means the panel stays
		// honest when someone edits mediabus.ini or starts the Player with a
		// different folder than the one recorded here.
		media::config::Config config;
		media::config::load(config);
		model.setCorpus(config.mediaFolder, corpus_.clipCount, corpus_.online,
			corpus_.folder);
	}

	void handle(media::DashboardAction action, media::DashboardModel& model) {
		std::string error;
		bool ok = false;
		switch (action) {
			case media::DashboardAction::LaunchPlayer:
				ok = player_.launch(error);
				break;
			case media::DashboardAction::LaunchController:
				ok = controller_.launch(error);
				break;
			case media::DashboardAction::StopPlayer:
				ok = player_.stop(error);
				break;
			case media::DashboardAction::StopController:
				ok = controller_.stop(error);
				break;
			case media::DashboardAction::ChooseMediaFolder:
				chooseMediaFolder(model);
				return;
			case media::DashboardAction::None:
				return;
		}
		model.setMessage(ok ? std::string("ok") : error);
		refresh();
	}

	/// Ask for a folder, persist it, and apply it to a running Player.
	///
	/// Runs on the main thread: the picker is a modal dialog with its own
	/// message loop, so it must never run on the probe thread or on an HTTP
	/// worker.
	void chooseMediaFolder(media::DashboardModel& model) {
		bool cancelled = false;
		media::config::Config current;
		media::config::load(current);
		const std::string chosen = media::ui::pickFolder(
			"Select the media corpus folder", current.mediaFolder, &cancelled);
		if (cancelled) {
			return;   // a cancelled dialog is not an outcome worth reporting
		}
		if (chosen.empty()) {
			model.setMessage(media::ui::folderPickerAvailable()
				? "no folder chosen" : "no folder picker in this build");
			return;
		}

		current.mediaFolder = chosen;
		const bool saved = media::config::save(current);

		// A running Player is re-pointed immediately; one that is down will
		// read the file at its next start. Both paths end in the same state.
		std::string note;
		if (player_.status().running()) {
			media::HttpJsonClient client("127.0.0.1", media::AppProbe::kPlayerPort);
			const media::HttpJsonClient::Json body{{"path", chosen}};
			media::HttpJsonClient::Json reply;
			std::string postError;
			if (client.post("/api/media-dir", body, reply, postError)) {
				note = "media folder set: " + chosen;
			} else {
				note = "saved, but the Player refused it: " + postError;
			}
		} else {
			note = "media folder saved (takes effect when the Player starts): " + chosen;
		}
		if (!saved) {
			note += "  [warning] could not write " + media::config::configPath();
		}
		model.setMessage(note);
		refresh();
	}

private:
	/// One GET /api/media-dir. Cheap (localhost, small body) and short-fused,
	/// so it is safe on the probe thread. A Player that is down just leaves the
	/// snapshot offline, which is a state the panel draws.
	CorpusSnapshot readCorpus() const {
		CorpusSnapshot snapshot;
		if (!player_.status().running()) {
			return snapshot;
		}
		media::HttpJsonClient client("127.0.0.1", media::AppProbe::kPlayerPort);
		media::HttpJsonClient::Json reply;
		std::string error;
		if (!client.get("/api/media-dir", reply, error) || !reply.is_object()) {
			return snapshot;
		}
		snapshot.online = true;
		if (reply.contains("mediaFolder") && reply["mediaFolder"].is_string()) {
			snapshot.folder = reply["mediaFolder"].get<std::string>();
		}
		if (reply.contains("clipCount") && reply["clipCount"].is_number_unsigned()) {
			snapshot.clipCount = reply["clipCount"].get<std::size_t>();
		}
		return snapshot;
	}

	media::AppProbe player_;
	media::AppProbe controller_;
	std::thread thread_;
	std::atomic<bool> stopping_{false};

	std::mutex mutex_;
	media::AppStatus playerStatus_;
	media::AppStatus controllerStatus_;
	std::string playerPath_;
	std::string controllerPath_;
	CorpusSnapshot corpus_;
};

/// Everything the window callbacks need. One instance lives for the whole run,
/// and the window's user-data slot points at it.
struct UiState {
	/// True once the tray icon exists. While it does, closing or hiding the
	/// window keeps the launcher alive; without it, the window is the only way
	/// to quit and must behave normally.
	bool trayAvailable = false;
	/// Set by QUIT in the tray menu: the only way out.
	bool quit = false;

	bool clickPending = false;
	double x = 0.0;
	double y = 0.0;
};

void setWindowVisible(GLFWwindow* window, bool visible) {
	if (visible) {
		glfwShowWindow(window);
		glfwFocusWindow(window);
	} else {
		glfwHideWindow(window);
	}
}

/// Hover text for the tray icon: the one thing visible while the window is not.
std::string trayTooltip(const media::DashboardModel& model) {
	std::string text = "vn-mediabus launcher";
	for (const media::DashboardRow& row : model.rows()) {
		text += row.app == media::DashboardApp::Player ? " | Player: " : " | Controller: ";
		text += row.running ? "up" : "down";
	}
	return text;
}

} // namespace

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	media::log::setThresholdFromEnv();

	Options options;
	if (!parseOptions(argc, argv, options)) {
		return 0;
	}

	enableDpiAwareness();

	glfwSetErrorCallback(onGlfwError);
	if (glfwInit() != GLFW_TRUE) {
		LOG_ERROR("Dashboard") << "glfwInit failed";
		return 1;
	}

	// Text size, by the same DPI rule as the other two windows (core/UiScale.h).
	// Read before the window exists so the initial layout is already right.
	media::config::Config config;
	media::config::load(config);
	const float contentScale = media::ui::rawContentScale();
	const float uiScale = media::ui::scaleForWindow(config);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_SAMPLES, 0);
	// --tray means the icon is the whole user interface until it is asked for.
	glfwWindowHint(GLFW_VISIBLE, options.startInTray ? GLFW_FALSE : GLFW_TRUE);

	// The layout is written in framebuffer pixels, so the window is asked for at
	// the layout size times the content scale. See the note on
	// syncWindowToFramebuffer in controller_main.cpp for why that ratio matters.
	const int wantW = static_cast<int>(
		static_cast<float>(options.width) * uiScale + 0.5f);
	const int wantH = static_cast<int>(
		static_cast<float>(options.height) * uiScale + 0.5f);
	gWindow = glfwCreateWindow(wantW, wantH, "vn-mediabus-dashboard", nullptr, nullptr);
	if (gWindow == nullptr) {
		LOG_ERROR("Dashboard") << "glfwCreateWindow failed";
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(gWindow);
	glfwSwapInterval(1);

	const GLenum glewStatus = glewInit();
	if (glewStatus != GLEW_OK) {
		LOG_ERROR("Dashboard") << "glewInit failed: " << glewGetErrorString(glewStatus);
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	std::unique_ptr<media::RenderDevice> device = media::createGlRenderDevice();
	if (!device || !device->initialize()) {
		LOG_ERROR("Dashboard") << "RenderDevice::initialize failed";
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	media::DashboardModel model;
	media::DashboardView view;
	Dashboard dashboard;

	dashboard.refresh();
	dashboard.applyTo(model);

	// --- input and tray ---------------------------------------------------
	UiState ui;
	glfwSetWindowUserPointer(gWindow, &ui);

	// The tray menu runs the same two things the window's buttons do, through
	// the same Dashboard object, so there is no second control path that could
	// disagree with the window about what is running.
	media::TrayIcon tray;
	const auto onTray = [&](media::TrayAction action) {
		switch (action) {
			case media::TrayAction::LaunchPlayer:
				dashboard.handle(media::DashboardAction::LaunchPlayer, model);
				break;
			case media::TrayAction::LaunchController:
				dashboard.handle(media::DashboardAction::LaunchController, model);
				break;
			case media::TrayAction::StopPlayer:
				dashboard.handle(media::DashboardAction::StopPlayer, model);
				break;
			case media::TrayAction::StopController:
				dashboard.handle(media::DashboardAction::StopController, model);
				break;
			case media::TrayAction::ToggleWindow:
				setWindowVisible(gWindow, glfwGetWindowAttrib(gWindow, GLFW_VISIBLE) == 0);
				break;
			case media::TrayAction::Quit:
				ui.quit = true;
				break;
			case media::TrayAction::None:
				break;
		}
	};

	// --no-tray: skip the icon entirely and be an ordinary window. This is the
	// escape hatch for a machine where Shell_NotifyIcon is refused, which is
	// not something this process can detect any other way than by being asked.
	if (options.noTray) {
		LOG_NOTICE("Dashboard") << "--no-tray: no icon; the window is the whole UI "
			"and closing it exits";
	} else {
		switch (tray.create(trayTooltip(model), onTray)) {
			case media::TrayResult::Created:
				ui.trayAvailable = true;
				break;

			case media::TrayResult::AlreadyRunning:
				// A launcher is already in the tray for this session. Exit
				// quietly: becoming a second window would be a worse answer
				// than doing nothing, since the one already there can do
				// everything this one could.
				LOG_NOTICE("Dashboard") << "the launcher is already running; nothing to do";
				dashboard.stop();
				device.reset();
				glfwDestroyWindow(gWindow);
				glfwTerminate();
				return 0;

			case media::TrayResult::Unavailable:
			case media::TrayResult::Unsupported:
				// Never leave an invisible process behind, and never leave one
				// whose only exit is a tray icon that does not exist. The window
				// is shown whatever was asked for, and the status line says why,
				// because "the launcher did not start" is what a hidden window
				// with no icon looks like from the outside.
				LOG_WARN("Dashboard") << "no tray icon: running as an ordinary window, "
					"where closing really exits";
				glfwShowWindow(gWindow);
				model.setMessage("no tray icon available here - this window is now "
					"the launcher; closing it exits");
				break;
		}
	}

	glfwSetMouseButtonCallback(gWindow, [](GLFWwindow* window, int button, int action, int) {
		if (button != GLFW_MOUSE_BUTTON_LEFT) {
			return;
		}
		auto* state = static_cast<UiState*>(glfwGetWindowUserPointer(window));
		if (state == nullptr) {
			return;
		}
		if (action == GLFW_PRESS) {
			double x = 0.0;
			double y = 0.0;
			glfwGetCursorPos(window, &x, &y);
			state->x = x;
			state->y = y;
			state->clickPending = true;
		}
	});

	glfwSetKeyCallback(gWindow, [](GLFWwindow* window, int key, int, int action, int) {
		if (key != GLFW_KEY_ESCAPE || (action != GLFW_PRESS && action != GLFW_REPEAT)) {
			return;
		}
		auto* state = static_cast<UiState*>(glfwGetWindowUserPointer(window));
		if (state != nullptr && state->trayAvailable) {
			// Hiding, not closing: the tray icon is how it comes back, and QUIT
			// in its menu is the only exit.
			glfwHideWindow(window);
			return;
		}
		glfwSetWindowShouldClose(window, GLFW_TRUE);
	});

	glfwSetWindowCloseCallback(gWindow, [](GLFWwindow* window) {
		auto* state = static_cast<UiState*>(glfwGetWindowUserPointer(window));
		if (state == nullptr || !state->trayAvailable) {
			return;   // no tray: closing really closes, or there is no way out
		}
		glfwSetWindowShouldClose(window, GLFW_FALSE);
		glfwHideWindow(window);
	});

	int fbW = 0;
	int fbH = 0;
	glfwGetFramebufferSize(gWindow, &fbW, &fbH);
	model.layout(static_cast<float>(fbW), static_cast<float>(fbH), uiScale);
	dashboard.start();

	LOG_NOTICE("Dashboard") << "vn-mediabus-dashboard";
	LOG_NOTICE("Dashboard") << "  render backend : " << device->backendName();
	LOG_NOTICE("Dashboard") << "  text scale     : "
		<< media::ui::describeDecision(uiScale, contentScale)
		<< " -> body " << media::ui::describe(media::ui::bodyScale(uiScale));
	LOG_NOTICE("Dashboard") << "  config file    : " << media::config::configPath();
	LOG_NOTICE("Dashboard") << "  folder picker  : "
		<< (media::ui::folderPickerAvailable() ? "available" : "NOT AVAILABLE in this build");
	LOG_NOTICE("Dashboard") << "  player API     : 127.0.0.1:"
		<< media::AppProbe::kPlayerPort;
	LOG_NOTICE("Dashboard") << "  controller API : 127.0.0.1:"
		<< media::AppProbe::kControllerPort;
	LOG_NOTICE("Dashboard") << "  exit           : QUIT in the tray menu"
		<< (ui.trayAvailable ? "" : " (no tray; close the window instead)");

	while (!ui.quit && glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();
		if (ui.quit) {
			break;
		}

		dashboard.applyTo(model);

		// Keep the menu and the hover text honest about what is running, and
		// about which applications this launcher is allowed to stop.
		media::TrayState trayState;
		for (const media::DashboardRow& row : model.rows()) {
			const bool isPlayer = row.app == media::DashboardApp::Player;
			if (isPlayer) {
				trayState.playerRunning = row.running;
				// Same rule the window's STOP button uses: only a running child
				// of this process may be stopped from here.
				trayState.playerManaged = row.running && row.managed;
			} else {
				trayState.controllerRunning = row.running;
				trayState.controllerManaged = row.running && row.managed;
			}
		}
		const bool windowVisible = glfwGetWindowAttrib(gWindow, GLFW_VISIBLE) != 0;
		trayState.windowVisible = windowVisible;
		tray.setState(trayState);
		tray.setTooltip(trayTooltip(model));

		if (!windowVisible && ui.trayAvailable) {
			// Hidden in the tray. Block until something happens — a tray click
			// wakes this, because GLFW waits on every message for the thread,
			// not only on its own window's — rather than spinning a frame loop
			// nobody can see.
			//
			// Without a tray icon there is nothing to click, so this branch is
			// only taken when one exists; otherwise the loop keeps running and
			// the window can be brought back by any normal means.
			glfwWaitEventsTimeout(0.5);
			continue;
		}

		if (ui.clickPending) {
			ui.clickPending = false;
			const media::DashboardAction action = model.hitTest(
				static_cast<float>(ui.x), static_cast<float>(ui.y));
			if (action != media::DashboardAction::None) {
				dashboard.handle(action, model);
			}
		}

		glfwGetFramebufferSize(gWindow, &fbW, &fbH);
		if (fbW <= 0 || fbH <= 0) {
			glfwWaitEventsTimeout(0.05);
			continue;
		}
		model.layout(static_cast<float>(fbW), static_cast<float>(fbH), uiScale);

		device->setViewport(fbW, fbH);
		device->beginFrame();
		device->drawSolid({0.0f, 0.0f, static_cast<float>(fbW), static_cast<float>(fbH)},
			0x10, 0x13, 0x19, 0xFF);
		view.draw(*device, model, uiScale);
		device->endFrame();
		glfwSwapBuffers(gWindow);
	}

	// Quit is an explicit "I am done", so it takes the applications this
	// launcher started with it. One that someone else started is left alone:
	// AppProbe::stop refuses anything that is not this process's child.
	LOG_NOTICE("Dashboard") << "quitting; stopping the applications this launcher started";
	dashboard.handle(media::DashboardAction::StopPlayer, model);
	dashboard.handle(media::DashboardAction::StopController, model);

	dashboard.stop();
	tray.destroy();
	device.reset();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
