/*
 * media-dashboard-cpp - launcher for the Player and the Controller
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

#include "app/dashboard/AppLauncher.h"
#include "app/dashboard/DashboardModel.h"
#include "app/dashboard/DashboardView.h"
#include "app/dashboard/TrayIcon.h"
#include "app/render/RenderDevice.h"
#include "core/Log.h"

#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

GLFWwindow* gWindow = nullptr;

struct Options {
	int width = media::DashboardModel::kDefaultWidth;
	int height = media::DashboardModel::kDefaultHeight;
	/// Start hidden, living only in the tray. What run.bat asks for.
	bool startInTray = false;
};

void printUsage() {
	std::printf(
		"media-dashboard-cpp - launcher for the Player and the Controller\n"
		"\n"
		"Usage: media-dashboard-cpp.exe [options]\n"
		"\n"
		"  --width N     window width  (default %d)\n"
		"  --height N    window height (default %d)\n"
		"  --tray        start hidden, in the notification area\n"
		"  --help, -h    show this text\n"
		"\n"
		"Tray:  right-click the icon for Launch Player / Launch Controller / Quit.\n"
		"       Left-click shows or hides this window.\n"
		"Window: click LAUNCH on a row. STOP only stops an app this launcher "
		"started.\n"
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
		} else {
			LOG_WARN("Dashboard") << "ignoring unknown argument: " << arg;
		}
	}
	return true;
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

	/// One synchronous pass: probe both applications and publish the result.
	void refresh() {
		player_.probe();
		controller_.probe();
		std::lock_guard<std::mutex> lock(mutex_);
		playerStatus_ = player_.status();
		controllerStatus_ = controller_.status();
		playerPath_ = player_.executablePath();
		controllerPath_ = controller_.executablePath();
	}

	/// Publish the latest snapshot into the model the view draws.
	void applyTo(media::DashboardModel& model) {
		std::lock_guard<std::mutex> lock(mutex_);
		model.setStatus(media::DashboardApp::Player, playerStatus_, playerPath_,
			media::AppProbe::kPlayerPort);
		model.setStatus(media::DashboardApp::Controller, controllerStatus_,
			controllerPath_, media::AppProbe::kControllerPort);
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
			case media::DashboardAction::None:
				return;
		}
		model.setMessage(ok ? std::string("ok") : error);
		refresh();
	}

private:
	media::AppProbe player_;
	media::AppProbe controller_;
	std::thread thread_;
	std::atomic<bool> stopping_{false};

	std::mutex mutex_;
	media::AppStatus playerStatus_;
	media::AppStatus controllerStatus_;
	std::string playerPath_;
	std::string controllerPath_;
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
	std::string text = "mediaplayer-app launcher";
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

	glfwSetErrorCallback(onGlfwError);
	if (glfwInit() != GLFW_TRUE) {
		LOG_ERROR("Dashboard") << "glfwInit failed";
		return 1;
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_SAMPLES, 0);
	// --tray means the icon is the whole user interface until it is asked for.
	glfwWindowHint(GLFW_VISIBLE, options.startInTray ? GLFW_FALSE : GLFW_TRUE);

	gWindow = glfwCreateWindow(options.width, options.height,
		"media-dashboard-cpp", nullptr, nullptr);
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
	switch (tray.create(trayTooltip(model), onTray)) {
		case media::TrayResult::Created:
			ui.trayAvailable = true;
			break;

		case media::TrayResult::AlreadyRunning:
			// A launcher is already in the tray for this session. Exit quietly:
			// becoming a second window would be a worse answer than doing
			// nothing, since the one already there can do everything this one
			// could.
			LOG_NOTICE("Dashboard") << "the launcher is already running; nothing to do";
			dashboard.stop();
			device.reset();
			glfwDestroyWindow(gWindow);
			glfwTerminate();
			return 0;

		case media::TrayResult::Unavailable:
		case media::TrayResult::Unsupported:
			LOG_WARN("Dashboard") << "running as an ordinary window: Esc closes it";
			// Never leave an invisible process behind. --tray creates the window
			// hidden, so if the icon could not be added there would be nothing
			// on screen and nothing in the tray: a process only Task Manager
			// could reach. Showing the window is the safety net for that.
			if (options.startInTray) {
				glfwShowWindow(gWindow);
				LOG_WARN("Dashboard") << "no tray icon, so the window is shown instead";
			}
			break;
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
	model.layout(static_cast<float>(fbW), static_cast<float>(fbH));
	dashboard.start();

	LOG_NOTICE("Dashboard") << "media-dashboard-cpp";
	LOG_NOTICE("Dashboard") << "  render backend : " << device->backendName();
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

		if (!windowVisible) {
			// Hidden in the tray. Block until something happens — a tray click
			// wakes this, because GLFW waits on every message for the thread,
			// not only on its own window's — rather than spinning a frame loop
			// nobody can see.
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
		model.layout(static_cast<float>(fbW), static_cast<float>(fbH));

		device->setViewport(fbW, fbH);
		device->beginFrame();
		device->drawSolid({0.0f, 0.0f, static_cast<float>(fbW), static_cast<float>(fbH)},
			0x10, 0x13, 0x19, 0xFF);
		view.draw(*device, model);
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
