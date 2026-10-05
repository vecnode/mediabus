/*
 * vn-mediabus-controller - control panel for vn-mediabus-player
 *
 * The Controller is a *client*: it links no libmpv and owns no decoder. It
 * polls the Player's localhost HTTP API for status and posts commands to it,
 * and it can be scripted with an embedded Lua state so a sequence of clips can
 * be driven without touching either application's internals.
 *
 * Its interface is Dear ImGui on the OpenGL context this file creates. Drawing
 * goes through view/ControllerPanel, which never sends a request; this file
 * turns what the operator touched into a TransportAction and hands it to
 * control/TransportExecutor, which is the code the tests exercise. That split is
 * what lets the behaviour stay testable with no window and no ImGui.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "control/ControllerHttpServer.h"
#include "control/ControllerModel.h"
#include "control/LuaControllerScript.h"
#include "control/PlayerClient.h"
#include "control/TransportAction.h"
#include "core/AppConfig.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/UiScale.h"
#include "gfx/GlLoader.h"
#include "gfx/UiScaleGlfw.h"
#include "ui/UiLayer.h"
#include "view/ControllerView.h"
#include "win32/FolderPicker.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// glfwGetWin32Window lives in the native header, which must come after glfw3.h.
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
	int width = 0;    ///< 0 asks for the panel's own default
	int height = 0;
	std::string playerHost = "127.0.0.1";
	int playerPort = media::ControllerHttpServer::kPlayerPort;
	int apiPort = media::ControllerHttpServer::kDefaultPort;
	bool startOffline = false;
	std::string script;
};

/// The panel's window size, in pixels.
///
/// These are the real pixel dimensions of the framebuffer, NOT a size that is
/// then multiplied by the DPI factor. The DPI factor belongs to the FONT and is
/// applied once, when the font is rasterised (see UiLayer::loadFonts); the text
/// is already bigger on a dense display, so scaling the window as well grew it
/// twice and left the whole interface looking zoomed.
///
/// The numbers come from what is actually in the panel, measured rather than
/// guessed: four transport buttons at 84pt each, then the HUD, Fullscreen and
/// Subtitles checkboxes, the two sliders, the seek bar with its readouts, the
/// folder field with its Change button, and the script strip. At 1100 the
/// Fullscreen checkbox was cut off at the right edge.
constexpr int kDefaultWidth = 1240;
constexpr int kDefaultHeight = 560;

/// Below this the transport row or the folder field would be clipped.
constexpr int kMinimumWidth = 900;
constexpr int kMinimumHeight = 320;

void printUsage() {
	std::printf(
		"vn-mediabus-controller - control panel for vn-mediabus-player\n"
		"\n"
		"Usage: vn-mediabus-controller.exe [options]\n"
		"\n"
		"  --width N            panel width  (default %d pixels)\n"
		"  --height N           panel height (default %d pixels)\n"
		"  --player-host HOST   where the Player API is (default 127.0.0.1)\n"
		"  --player-port N      Player API port (default %d)\n"
		"  --api-port N         this Controller's own API port (default %d)\n"
		"  --script FILE        run a Lua script from <data>/controller-scripts\n"
		"  --list-scripts       print discovered scripts and exit\n"
		"  --help, -h           show this text\n"
		"\n"
		"Keys:  Space play/pause   R reload script   Esc asks to quit\n"
		"       (the arrow keys and the mouse are the interface's own)\n"
		"API:   http://127.0.0.1:%d  (localhost only)\n",
		kDefaultWidth, kDefaultHeight, media::ControllerHttpServer::kPlayerPort,
		media::ControllerHttpServer::kDefaultPort,
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
	if (out.width <= 0) {
		out.width = kDefaultWidth;
	}
	if (out.height <= 0) {
		out.height = kDefaultHeight;
	}
	out.width = std::max(out.width, kMinimumWidth);
	out.height = std::max(out.height, kMinimumHeight);
	return true;
}

/// Tell Windows this process understands DPI, before any window exists.
///
/// Without it the process is "DPI unaware", Windows lies about the panel size
/// (a 4K monitor reports 1920x1080), glfwGetMonitorContentScale returns 1.0,
/// and the text is scaled up by the compositor into a blurry mess. With it,
/// GLFW reports the real content scale and ImGui is told the same number, so
/// the widgets and their glyphs scale together.
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

/// Ask for a window whose FRAMEBUFFER is `wantW` x `wantH` physical pixels.
///
/// Everything that draws works in framebuffer pixels: ImGui's DisplaySize, the
/// GL viewport, and the layout itself. But glfwCreateWindow takes SCREEN
/// COORDINATES, so on a display at 150% scaling a request for 1240 produces a
/// 1860-pixel framebuffer. Asking for the layout size directly therefore made the
/// interface one content-scale larger than intended - the zoom this exists to
/// correct.
///
/// This verifies the result instead of assuming it. GLFW does not promise that a
/// requested window size and the framebuffer it produces agree on every platform
/// and DPI setting, and a mismatch here is otherwise invisible from inside the
/// application.
bool syncWindowToFramebuffer(GLFWwindow* window, int wantW, int wantH) {
	int fbW = 0;
	int fbH = 0;
	glfwGetFramebufferSize(window, &fbW, &fbH);
	if (fbW == wantW && fbH == wantH) {
		return true;
	}
	int winW = 0;
	int winH = 0;
	glfwGetWindowSize(window, &winW, &winH);
	if (fbW <= 0 || fbH <= 0 || winW <= 0 || winH <= 0) {
		LOG_WARN("Controller") << "no usable window or framebuffer size; "
			"laying out for " << fbW << "x" << fbH;
		return false;
	}

	// The relationship between the two is linear, so one correction lands on the
	// target whatever is doing the scaling.
	const int targetW = std::max(1, static_cast<int>(
		static_cast<float>(winW) * (static_cast<float>(wantW) / static_cast<float>(fbW)) + 0.5f));
	const int targetH = std::max(1, static_cast<int>(
		static_cast<float>(winH) * (static_cast<float>(wantH) / static_cast<float>(fbH)) + 0.5f));
	glfwSetWindowSize(window, targetW, targetH);
	glfwGetFramebufferSize(window, &fbW, &fbH);
	if (fbW == wantW && fbH == wantH) {
		LOG_NOTICE("Controller") << "window resized to " << targetW << "x" << targetH
			<< " to get a " << fbW << "x" << fbH << " framebuffer";
		return true;
	}
	// Not fatal - the layout adapts to whatever it is given - but it means the
	// display is doing something this code did not anticipate, so say so.
	LOG_WARN("Controller") << "framebuffer is " << fbW << "x" << fbH
		<< ", not the requested " << wantW << "x" << wantH
		<< "; the panel will be laid out for the size it really has";
	return false;
}

/// Physical size of the window on screen, in real device pixels.
///
/// This is the number that decides whether text is readable, and it is not the
/// framebuffer size on Windows: the framebuffer is the resolution the GL driver
/// renders at, while the window is placed and scaled by the compositor. Logging
/// both is how a DPI problem is told apart from a layout one.
void logPhysicalWindow(GLFWwindow* window) {
#if defined(_WIN32)
	HWND handle = glfwGetWin32Window(window);
	if (handle == nullptr) {
		return;
	}
	RECT rect{};
	if (GetWindowRect(handle, &rect)) {
		LOG_NOTICE("Controller") << "  window on screen: " << (rect.right - rect.left)
			<< "x" << (rect.bottom - rect.top) << " physical pixels";
	}
#else
	(void)window;
#endif
}

/// The operating system's folder picker, handed to the executor as a callback.
///
/// The executor lives in libs/control, which the headless tests link, so it
/// cannot know about the shell or about windows.h. This is where that knowledge
/// stays, and it runs on this thread, inside the frame loop: the picker is a
/// modal dialog with its own message loop and must never move to a worker.
media::TransportExecutor::FolderChoice chooseFolderDialog(
	const std::string& initialDirectory) {
	media::TransportExecutor::FolderChoice choice;
	bool cancelled = false;
	choice.path = media::ui::pickFolder("Select the media corpus folder",
		initialDirectory, &cancelled);
	choice.cancelled = cancelled;
	if (!cancelled && choice.path.empty() && !media::ui::folderPickerAvailable()) {
		LOG_WARN("Controller") << "no folder picker in this build";
	}
	return choice;
}

} // namespace

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	media::log::setThresholdFromEnv();
	// Windows-subsystem binary: with no console to write to, the log goes to
	// bin/mediabus-controller.log. See core/Log.cpp.
	media::log::useFileSink("controller");

	Options options;
	if (!parseOptions(argc, argv, options)) {
		return 0;
	}

	enableDpiAwareness();

	glfwSetErrorCallback(onGlfwError);
	if (glfwInit() != GLFW_TRUE) {
		LOG_ERROR("Controller") << "glfwInit failed";
		return 1;
	}

	// How big the widgets are drawn: the same DPI rule as the other two windows
	// (core/UiScale.h), handed to UiLayer, which scales ImGui's spacing and its
	// font size by the one number.
	media::config::Config config;
	media::config::load(config);
	const float contentScale = media::ui::rawContentScale();
	const float uiScale = media::ui::scaleForWindow(config);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	// A normal decorated window. The old bar was undecorated and moved itself by
	// handling WM_NCLBUTTONDOWN by hand, because it was a title-bar-shaped strip;
	// now that it is a control panel, the platform's own frame is better in every
	// way - it drags, resizes, minimizes and snaps as the operator expects.
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_SAMPLES, 0);

	// The window is asked for in SCREEN COORDINATES, and the framebuffer it
	// produces is that size times the monitor's content scale - so on a 150%
	// display, asking for 1240 gives a 1860-pixel framebuffer. The layout and the
	// fonts are both measured in FRAMEBUFFER pixels, so asking for the layout size
	// directly made everything that much larger again: that is the zoom the
	// division below removes.
	//
	// The result is verified rather than assumed (see syncWindowToFramebuffer):
	// GLFW does not promise the two sizes agree on every platform and DPI setting,
	// and a mismatch is otherwise invisible from inside the application.
	int wantW = std::max(1, static_cast<int>(
		static_cast<float>(options.width) / uiScale + 0.5f));
	int wantH = std::max(1, static_cast<int>(
		static_cast<float>(options.height) / uiScale + 0.5f));

	gWindow = glfwCreateWindow(wantW, wantH, "vn-mediabus controller", nullptr, nullptr);
	if (gWindow == nullptr) {
		LOG_ERROR("Controller") << "glfwCreateWindow failed";
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(gWindow);
	glfwSwapInterval(1);

	if (!media::gfx::initializeGlLoader(gWindow)) {
		LOG_ERROR("Controller") << "glewInit failed";
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	// The interface. ImGui owns the frame from here on: the RenderDevice is not
	// used by this application at all any more, because every pixel it used to
	// draw - the cards, the buttons, the 5x7 text - is now a widget.
	std::unique_ptr<media::ui::UiLayer> ui = media::ui::UiLayer::create(gWindow);
	if (!ui || !ui->ready()) {
		LOG_ERROR("Controller") << "ImGui failed to start; the Controller cannot draw";
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}
	// Correct the framebuffer to the size the layout was written for, now that
	// there is a window to measure. The layout and the font sizes are both in
	// framebuffer pixels, so this is what decides how large the panel really is.
	syncWindowToFramebuffer(gWindow, options.width, options.height);
	ui->setUiScale(uiScale);
	ui->setIniPath(media::platform::executableDirectory() + "mediabus-ui.ini");
	ui->installCallbacks();

	// --- the two halves of the app ----------------------------------------
	media::ControllerModel model;
	media::ControllerPanel panel;
	media::PlayerClient player(options.playerHost, options.playerPort);

	media::LuaControllerScript scripts;
	if (!scripts.initialize()) {
		LOG_WARN("Controller") << "Lua unavailable; the panel runs unscripted";
	}
	scripts.setCommands(&player);

	media::TransportExecutor executor(player, model, scripts.ready() ? &scripts : nullptr);
	executor.setFolderPicker(chooseFolderDialog);
	panel.setPlayerEndpoint(options.playerHost + ":" + std::to_string(options.playerPort));

	media::ControllerHost host;
	host.player = &player;
	host.model = &model;
	host.scripts = scripts.ready() ? &scripts : nullptr;

	media::ControllerHttpServer api(host);
	if (!api.start(options.apiPort)) {
		LOG_WARN("Controller") << "Controller API unavailable; the panel still controls the Player";
	}

	// Poll once before the first frame so the panel does not open showing OFFLINE
	// for a Player that is right there.
	if (!options.startOffline) {
		player.pollOnce();
	}
	model.applyState(player.state());
	player.start();

	LOG_NOTICE("Controller") << "vn-mediabus-controller";
	LOG_NOTICE("Controller") << "  interface      : " << ui->describe();
	LOG_NOTICE("Controller") << "  fonts          : " << ui->fonts().uiSource
		<< " / " << ui->fonts().monoSource;
	LOG_NOTICE("Controller") << "  text scale     : "
		<< media::ui::describeDecision(uiScale, contentScale);
	LOG_NOTICE("Controller") << "  window         : " << wantW << "x" << wantH
		<< " requested (layout size " << options.width << "x" << options.height
		<< " at " << media::ui::describe(uiScale) << ")";
	logPhysicalWindow(gWindow);
	LOG_NOTICE("Controller") << "  config file    : " << media::config::configPath();
	LOG_NOTICE("Controller") << "  folder picker  : "
		<< (media::ui::folderPickerAvailable() ? "available" : "NOT AVAILABLE in this build");
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
	std::chrono::steady_clock::time_point lastScriptCheck = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();

	int fbW = 0;
	int fbH = 0;

	// Previous frame's state of the two hotkeys, so each fires on the press edge
	// rather than on every frame the key is held. See the frame loop.
	bool spaceWasDown = false;
	bool reloadWasDown = false;

	while (glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();

		// Queued API requests run here, on the main thread that owns the client
		// and the Lua state.
		api.poll();

		// Adopt whatever the polling thread last saw.
		model.applyState(player.state());

		// --- script ---------------------------------------------------------
		// A script edited on disk is picked up without a restart. Unlike the
		// Player's mpv scripts - which mpv will not detach once started - this
		// host owns its own Lua state, so a reload really is a reload. The check
		// is a stat() call, so it runs a few times a second rather than every
		// frame, which is what makes live editing work without making the frame
		// loop do filesystem work 60 times a second.
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
			// Minimized: no frame to draw. Waiting rather than spinning keeps a
			// minimized Controller at zero CPU.
			glfwWaitEventsTimeout(0.1);
			continue;
		}

		const auto now = std::chrono::steady_clock::now();
		const double dt = std::chrono::duration<double>(now - lastFrame).count();
		lastFrame = now;

		panel.setScriptLog(scripts.lastLog());
		panel.setScriptBudget(scripts.tickBudgetFraction());

		// ImGui's frame is measured in framebuffer pixels, the same units the
		// GL viewport uses. See UiLayer.h.
		ui->beginFrame({fbW, fbH, uiScale, dt});

		// The keyboard shortcuts stay here rather than inside the panel, and are
		// suppressed while a widget has the keyboard: pressing Space in a text
		// field must type a space, not pause the video.
		//
		// Each is edge-triggered from the previous frame's state. Polling
		// glfwGetKey directly would fire the action on every frame the key is
		// held, which for "reload the script" means reloading it sixty times a
		// second.
		media::TransportAction hotkey;
		const bool spaceDown = glfwGetKey(gWindow, GLFW_KEY_SPACE) == GLFW_PRESS;
		const bool reloadDown = glfwGetKey(gWindow, GLFW_KEY_R) == GLFW_PRESS;
		if (!ui->wantsKeyboard()) {
			if (spaceDown && !spaceWasDown) {
				hotkey.valid = true;
				hotkey.command = media::ControlCommand::PlayPause;
			} else if (reloadDown && !reloadWasDown) {
				hotkey.valid = true;
				hotkey.reloadScript = true;
			}
		}
		spaceWasDown = spaceDown;
		reloadWasDown = reloadDown;

		media::ControllerPanel::Frame frame = panel.draw(*ui, model,
			scripts.currentScript(), scripts.lastError(), scripts.scriptRunning());

		if (frame.requestQuit) {
			glfwSetWindowShouldClose(gWindow, GLFW_TRUE);
		}
		if (frame.action.valid) {
			const std::string message = executor.perform(frame.action);
			if (!message.empty()) {
				model.setMessage(message);
			}
		}
		if (hotkey.valid) {
			const std::string message = executor.perform(hotkey);
			if (!message.empty()) {
				model.setMessage(message);
			}
		}

		ui->endFrame();
		glfwSwapBuffers(gWindow);
	}

	player.stop();
	api.stop();
	// ImGui first: its GL objects must be released while the context is still
	// current, which means before the window is destroyed.
	ui->shutdown();
	ui.reset();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
