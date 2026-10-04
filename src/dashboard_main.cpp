/*
 * media-dashboard-cpp - launcher for the Player and the Controller
 *
 * A third, optional window whose only job is to show whether the Player and the
 * Controller are running and to start or stop them. It exists so an operator
 * does not have to know which of two executables to double-click in what order,
 * which is the one piece of friction the two-app layout would otherwise have.
 *
 * Liveness is decided by each application's own health endpoint, not by process
 * enumeration: that is portable, it needs no elevation, and it answers the
 * question that matters ("is its API answering?") rather than a proxy for it.
 *
 * Closing the Dashboard closes the children it started: the process handles are
 * released and the documented Windows behaviour for a job-less process handle
 * does the rest. STOP only ever targets a child this process created, so a
 * Player launched from Explorer is never killed from here.
 *
 * Drawing goes through media::RenderDevice, as in both other apps: this file
 * must never name an OpenGL symbol.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "app/dashboard/AppLauncher.h"
#include "app/dashboard/DashboardModel.h"
#include "app/dashboard/DashboardView.h"
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
};

void printUsage() {
	std::printf(
		"media-dashboard-cpp - launcher for the Player and the Controller\n"
		"\n"
		"Usage: media-dashboard-cpp.exe [options]\n"
		"\n"
		"  --width N     window width  (default %d)\n"
		"  --height N    window height (default %d)\n"
		"  --help, -h    show this text\n"
		"\n"
		"Launch: click LAUNCH on a row. STOP only stops an app this Dashboard "
		"started.\n"
		"Keys:  Esc quit\n",
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

	struct InputState {
		bool clickPending = false;
		double x = 0.0;
		double y = 0.0;
	};
	InputState input;
	glfwSetWindowUserPointer(gWindow, &input);

	glfwSetMouseButtonCallback(gWindow, [](GLFWwindow* window, int button, int action, int) {
		if (button != GLFW_MOUSE_BUTTON_LEFT) {
			return;
		}
		auto* state = static_cast<InputState*>(glfwGetWindowUserPointer(window));
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
		if (key == GLFW_KEY_ESCAPE && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
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

	while (glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();

		if (input.clickPending) {
			input.clickPending = false;
			const media::DashboardAction action = model.hitTest(
				static_cast<float>(input.x), static_cast<float>(input.y));
			if (action != media::DashboardAction::None) {
				dashboard.handle(action, model);
			}
		}

		dashboard.applyTo(model);

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

	dashboard.stop();
	device.reset();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
