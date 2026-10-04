/*
 * media-controller-cpp - control bar for media-player-cpp
 *
 * The Controller is a *client*: it links no libmpv and owns no decoder. It
 * polls the Player's localhost HTTP API for status and posts commands to it,
 * and it can be scripted with an embedded Lua state so a sequence of clips can
 * be driven without touching either application's internals.
 *
 * This file owns the window, the GL context, the frame loop and the input. All
 * drawing goes through media::RenderDevice, exactly like the Player: this file
 * must never name an OpenGL symbol.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "app/control/ControllerHttpServer.h"
#include "app/control/ControllerModel.h"
#include "app/control/ControllerView.h"
#include "app/control/LuaControllerScript.h"
#include "app/control/PlayerClient.h"
#include "app/hud/BitmapFont.h"
#include "app/render/RenderDevice.h"
#include "core/Log.h"
#include "core/Platform.h"

#define GLFW_INCLUDE_NONE
#include <GL/glew.h>       // loader must precede GLFW so GLFW does not pull in GL
#include <GLFW/glfw3.h>
#if defined(_WIN32)
// glfwGetWin32Window lives in the native header, which must come after glfw3.h.
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
	int width = media::ControllerModel::kDefaultWidth;
	int height = media::ControllerModel::kDefaultHeight;
	std::string playerHost = "127.0.0.1";
	int playerPort = media::ControllerHttpServer::kPlayerPort;
	int apiPort = media::ControllerHttpServer::kDefaultPort;
	bool startOffline = false;
	std::string script;
};

void printUsage() {
	std::printf(
		"media-controller-cpp - control bar for media-player-cpp\n"
		"\n"
		"Usage: media-controller-cpp.exe [options]\n"
		"\n"
		"  --width N            bar width  (default %d)\n"
		"  --height N           bar height (default %d)\n"
		"  --player-host HOST   where the Player API is (default 127.0.0.1)\n"
		"  --player-port N      Player API port (default %d)\n"
		"  --api-port N         this Controller's own API port (default %d)\n"
		"  --script FILE        run a Lua script from <data>/controller-scripts\n"
		"  --list-scripts       print discovered scripts and exit\n"
		"  --help, -h           show this text\n"
		"\n"
		"Keys:  H hide/show the Player HUD   F toggle Player fullscreen\n"
		"       S subtitles   Space play/pause   R reload script   Esc quit\n"
		"API:   http://127.0.0.1:%d  (localhost only)\n",
		media::ControllerModel::kDefaultWidth, media::ControllerModel::kDefaultHeight,
		media::ControllerHttpServer::kPlayerPort, media::ControllerHttpServer::kDefaultPort,
		media::ControllerHttpServer::kDefaultPort);
}

bool parseOptions(int argc, char** argv, Options& out) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto nextString = [&](std::string& target) {
			if (i + 1 < argc) {
				target = argv[++i];
			}
		};
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
		} else if (arg == "--player-host") {
			nextString(out.playerHost);
		} else if (arg == "--player-port") {
			nextInt(out.playerPort);
		} else if (arg == "--api-port") {
			nextInt(out.apiPort);
		} else if (arg == "--script") {
			nextString(out.script);
		} else if (arg == "--list-scripts") {
			for (const media::ControllerScriptFile& script
				: media::discoverControllerScripts()) {
				std::printf("%s\n", script.name.c_str());
			}
			std::exit(0);
		} else if (arg == "--start-offline") {
			// Diagnostics only: show the offline state without a Player.
			out.startOffline = true;
		} else {
			LOG_WARN("Controller") << "ignoring unknown argument: " << arg;
		}
	}
	if (out.width < 480) out.width = 480;
	if (out.height < 72) out.height = 72;
	return true;
}

void onGlfwError(int code, const char* description) {
	LOG_ERROR("GLFW") << code << ": " << description;
}

/// Drag an undecorated window by its body.
///
/// GLFW has no "move window by mouse" API, so this is the one place that talks
/// to the operating system. The window is undecorated (it must be: it is a
/// title-bar-shaped control strip), and without this it could not be moved at
/// all. Buttons are hit-tested first by the caller, so dragging never eats a
/// click.
void beginWindowDrag(GLFWwindow* window) {
#if defined(_WIN32)
	HWND handle = glfwGetWin32Window(window);
	if (handle == nullptr) {
		return;
	}
	// Release the capture GLFW may hold, or the move loop fights it.
	ReleaseCapture();
	SendMessageW(handle, WM_NCLBUTTONDOWN, HTCAPTION, 0);
#else
	(void)window;
#endif
}

/// Width-fit rect helper for a full-bleed fill.
media::Rect wholeWindow(float width, float height) {
	return {0.0f, 0.0f, width, height};
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
		LOG_ERROR("Controller") << "glfwInit failed";
		return 1;
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	// Undecorated and floating: a bar small enough to sit beside the Player,
	// always visible, and still a normal window with a taskbar button. No
	// WS_EX_TOOLWINDOW trickery, so it stays portable and double-clickable.
	glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
	glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_SAMPLES, 0);

	gWindow = glfwCreateWindow(options.width, options.height,
		"media-controller-cpp", nullptr, nullptr);
	if (gWindow == nullptr) {
		LOG_ERROR("Controller") << "glfwCreateWindow failed";
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(gWindow);
	glfwSwapInterval(1);

	const GLenum glewStatus = glewInit();
	if (glewStatus != GLEW_OK) {
		LOG_ERROR("Controller") << "glewInit failed: " << glewGetErrorString(glewStatus);
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	std::unique_ptr<media::RenderDevice> device = media::createGlRenderDevice();
	if (!device || !device->initialize()) {
		LOG_ERROR("Controller") << "RenderDevice::initialize failed";
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	// --- the two halves of the app ----------------------------------------
	media::ControllerModel model;
	media::ControllerView view;
	media::PlayerClient player(options.playerHost, options.playerPort);

	media::LuaControllerScript scripts;
	if (!scripts.initialize()) {
		LOG_WARN("Controller") << "Lua unavailable; the bar runs unscripted";
	}
	scripts.setCommands(&player);

	media::ControllerHost host;
	host.player = &player;
	host.model = &model;
	host.scripts = scripts.ready() ? &scripts : nullptr;

	media::ControllerHttpServer api(host);
	if (!api.start(options.apiPort)) {
		LOG_WARN("Controller") << "Controller API unavailable; the bar still controls the Player";
	}

	// Poll once before the first frame so the bar does not open showing OFFLINE
	// for a Player that is right there.
	if (!options.startOffline) {
		player.pollOnce();
	}
	model.applyState(player.state());
	player.start();

	// The commands a *script* issues go through the same client the bar uses,
	// so scripting cannot become a second, divergent control path.

	// --- input ------------------------------------------------------------
	/// What a click or key press resolves to. Kept as a small enum rather than
	/// overloading an int, so "no action" cannot be confused with a seek of 0%.
	enum class Pending {
		None,
		PlayPause,
		ToggleHud,
		ToggleFullscreen,
		ToggleSubtitles,
		ReloadScript,
		Seek,
	};

	struct InputState {
		/// Set on press, consumed on release. A press is only an action if the
		/// pointer stayed put; otherwise it was a drag of the bar.
		bool pressed = false;
		double pressX = 0.0;
		double pressY = 0.0;
		double releaseX = 0.0;
		double releaseY = 0.0;
		bool clickPending = false;
		Pending pending = Pending::None;
		double pendingValue = 0.0;
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
		double x = 0.0;
		double y = 0.0;
		glfwGetCursorPos(window, &x, &y);
		if (action == GLFW_PRESS) {
			state->pressed = true;
			state->clickPending = true;
			state->pressX = x;
			state->pressY = y;
		} else if (action == GLFW_RELEASE) {
			state->pressed = false;
			// The release position decides hit tests, so a button that moved
			// under the pointer cannot fire by accident.
			state->releaseX = x;
			state->releaseY = y;
		}
	});

	glfwSetKeyCallback(gWindow, [](GLFWwindow* window, int key, int, int action, int) {
		if (action != GLFW_PRESS && action != GLFW_REPEAT) {
			return;
		}
		auto* state = static_cast<InputState*>(glfwGetWindowUserPointer(window));
		if (state == nullptr) {
			return;
		}
		switch (key) {
			case GLFW_KEY_ESCAPE:
				glfwSetWindowShouldClose(window, GLFW_TRUE);
				break;
			case GLFW_KEY_SPACE:
				state->pending = Pending::PlayPause;
				break;
			case GLFW_KEY_H:
				state->pending = Pending::ToggleHud;
				break;
			case GLFW_KEY_F:
				state->pending = Pending::ToggleFullscreen;
				break;
			case GLFW_KEY_S:
				state->pending = Pending::ToggleSubtitles;
				break;
			case GLFW_KEY_R:
				// Advertised in the usage text; reloading is the one thing a
				// scripted bar needs that the Player's mpv scripts cannot do.
				state->pending = Pending::ReloadScript;
				break;
			default:
				break;
		}
	});

	int fbW = 0;
	int fbH = 0;
	glfwGetFramebufferSize(gWindow, &fbW, &fbH);
	model.layout(static_cast<float>(fbW), static_cast<float>(fbH));

	LOG_NOTICE("Controller") << "media-controller-cpp";
	LOG_NOTICE("Controller") << "  render backend : " << device->backendName();
	LOG_NOTICE("Controller") << "  GL version     : " << (const char*)glGetString(GL_VERSION);
	LOG_NOTICE("Controller") << "  player         : http://" << options.playerHost
		<< ":" << options.playerPort;
	LOG_NOTICE("Controller") << "  controller API : "
		<< (api.isRunning() ? "http://127.0.0.1:" + std::to_string(api.port())
			: std::string("DISABLED"));

	if (!options.script.empty()) {
		// Resolved the same way the API resolves {"path": ...}: a name inside
		// <data>/controller-scripts, never an arbitrary path.
		const std::string path = media::findControllerScript(options.script);
		std::string error;
		if (path.empty()) {
			LOG_WARN("Controller") << "no script named '" << options.script
				<< "' in " << media::platform::dataDirectory() << "controller-scripts";
		} else if (!scripts.runFile(path, error)) {
			LOG_WARN("Controller") << "startup script failed: " << error;
		}
	}

	// When the running script's file was last checked for changes on disk.
	std::chrono::steady_clock::time_point lastScriptCheck =
		std::chrono::steady_clock::now();

	while (glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();

		// Queued API requests run here, on the main thread that owns the client.
		api.poll();

		// Adopt whatever the polling thread last saw. applyState is what tells
		// the bar whether anything changed.
		model.applyState(player.state());

		// --- input resolution ---------------------------------------------
		// A click resolves on release, not on press, so a drag can be told apart
		// from a click without firing a command the operator did not mean.
		if (input.clickPending && !input.pressed) {
			input.clickPending = false;
			const float fx = static_cast<float>(input.releaseX);
			const float fy = static_cast<float>(input.releaseY);
			const bool moved = std::abs(input.releaseX - input.pressX)
				+ std::abs(input.releaseY - input.pressY) > 5.0;

			// Buttons are hit-tested first: a click on one is never swallowed by
			// the window-move path. The seek bar is second. Anything else that
			// was dragged moves the window.
			const media::ControlCommand command = model.hitTest(fx, fy);
			double percent = 0.0;
			const bool onSeekBar = model.seekPercentAt(fx, fy, percent);

			if (command != media::ControlCommand::None) {
				input.pending = Pending::None;
				std::string error;
				if (!player.send(command, 0.0, error)) {
					model.setMessage("player: " + error);
				}
			} else if (onSeekBar) {
				input.pending = Pending::Seek;
				input.pendingValue = percent;
			} else if (moved) {
				beginWindowDrag(gWindow);
			}
		}

		if (input.pending != Pending::None) {
			const Pending pending = input.pending;
			const double value = input.pendingValue;
			input.pending = Pending::None;
			std::string error;
			media::ControlCommand command = media::ControlCommand::None;
			switch (pending) {
				case Pending::PlayPause: command = media::ControlCommand::PlayPause; break;
				case Pending::ToggleHud: command = media::ControlCommand::ToggleHud; break;
				case Pending::ToggleFullscreen:
					command = media::ControlCommand::ToggleFullscreen; break;
				case Pending::ToggleSubtitles:
					command = media::ControlCommand::ToggleSubtitles; break;
				case Pending::ReloadScript:
					// Reloading is a script-host action, not a Player command.
					if (scripts.ready()) {
						std::string reloadError;
						if (!scripts.forceReload(reloadError)) {
							model.setMessage("script: " + reloadError);
						}
					}
					break;
				case Pending::Seek:
					if (!player.seekPercent(value, error)) {
						model.setMessage("player: " + error);
					}
					break;
				case Pending::None:
					break;
			}
			if (command != media::ControlCommand::None
				&& !player.send(command, value, error)) {
				model.setMessage("player: " + error);
			}
		}

		// --- script ---------------------------------------------------------
		// A script edited on disk is picked up without a restart. Unlike the
		// Player's mpv scripts — which mpv will not detach once started — this
		// host owns its own Lua state, so a reload really is a reload. The
		// check is a stat() call, so it runs a few times a second rather than
		// every frame, which is what makes live editing work without making
		// the frame loop do filesystem work 60 times a second.
		if (scripts.ready() && scripts.scriptRunning()) {
			const auto now = std::chrono::steady_clock::now();
			if (now - lastScriptCheck > std::chrono::milliseconds(500)) {
				lastScriptCheck = now;
				scripts.reloadIfChanged();
			}
			scripts.tick();
		}

		glfwGetFramebufferSize(gWindow, &fbW, &fbH);
		if (fbW <= 0 || fbH <= 0) {
			glfwWaitEventsTimeout(0.05);
			continue;
		}
		model.layout(static_cast<float>(fbW), static_cast<float>(fbH));

		device->setViewport(fbW, fbH);
		device->beginFrame();
		const media::Rect full = wholeWindow(static_cast<float>(fbW),
			static_cast<float>(fbH));
		device->drawSolid(full, 0x14, 0x17, 0x1D, 0xFF);
		view.draw(*device, model);
		device->endFrame();
		glfwSwapBuffers(gWindow);
	}

	player.stop();
	api.stop();
	device.reset();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
