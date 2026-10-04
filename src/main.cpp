/*
 * media-player-cpp - application shell
 *
 * Owns the window, the GL context, the frame loop and the control plane. All
 * drawing goes through media::RenderDevice; this file must never name an OpenGL
 * symbol, and no widget toolkit is involved: the HUD is drawn on the canvas.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "app/HttpControlServer.h"
#include "app/hud/BitmapFont.h"
#include "app/render/RenderDevice.h"
#include "backends/mpv/MPVSurface.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "media/MediaClipLibrary.h"
#include "media/MediaPlayerController.h"

#include <mpv/client.h>

#define GLFW_INCLUDE_NONE
#include <GL/glew.h>       // loader must precede GLFW so GLFW does not pull in GL
#include <GLFW/glfw3.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;

GLFWwindow* gWindow = nullptr;
bool gFullscreen = false;

/// Window presentation the HTTP API is allowed to toggle. Owned here because
/// this file owns the render loop; the API reaches it only through the
/// PresentationHooks closures below, never by naming a graphics API.
struct PresentationState {
	bool hudVisible = true;
	int windowedWidth = kDefaultWidth;
	int windowedHeight = kDefaultHeight;
};

/// Command line, kept deliberately small: the Controller drives everything
/// else over HTTP, so these are only the things that must be decided before
/// the window exists.
struct Options {
	int width = kDefaultWidth;
	int height = kDefaultHeight;
	bool fullscreen = false;
	bool hudVisible = true;
	int port = media::HttpControlServer::kDefaultPort;
};

void printUsage() {
	std::printf(
		"media-player-cpp - scriptable video player with a localhost control API\n"
		"\n"
		"Usage: media-player-cpp.exe [options]\n"
		"\n"
		"  --width N          window width  (default %d)\n"
		"  --height N         window height (default %d)\n"
		"  --fullscreen       start fullscreen\n"
		"  --no-hud           start with the status HUD hidden\n"
		"  --port N           control API port (default %d)\n"
		"  --help, -h         show this text\n"
		"\n"
		"Keys:  H toggle HUD   F11 toggle fullscreen   Esc quit\n"
		"API:   http://127.0.0.1:%d  (localhost only)\n",
		kDefaultWidth, kDefaultHeight, media::HttpControlServer::kDefaultPort,
		media::HttpControlServer::kDefaultPort);
}

/// Parse argv. Unknown arguments are reported and ignored rather than fatal, so
/// a launcher passing a flag this build does not know still starts the player.
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
		} else if (arg == "--port") {
			nextInt(out.port);
		} else if (arg == "--fullscreen") {
			out.fullscreen = true;
		} else if (arg == "--no-hud" || arg == "--hide-hud") {
			out.hudVisible = false;
		} else {
			LOG_WARN("App") << "ignoring unknown argument: " << arg;
		}
	}
	if (out.width < 320) out.width = 320;
	if (out.height < 240) out.height = 240;
	if (out.port <= 0 || out.port > 65535) {
		out.port = media::HttpControlServer::kDefaultPort;
	}
	return true;
}

/// Move the window between the monitor and its windowed size. Shared by the
/// F11 handler and the /api/fullscreen route so both agree.
void applyFullscreen(GLFWwindow* window, PresentationState& state, bool enabled,
	int windowedWidth, int windowedHeight) {
	(void)state;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	if (mode == nullptr) {
		LOG_WARN("App") << "no video mode reported; fullscreen change ignored";
		return;
	}
	gFullscreen = enabled;
	glfwSetWindowMonitor(window, enabled ? monitor : nullptr, 0, 0,
		enabled ? mode->width : windowedWidth,
		enabled ? mode->height : windowedHeight,
		mode->refreshRate);
}

void onGlfwError(int code, const char* description) {
	LOG_ERROR("GLFW") << code << ": " << description;
}

void onKey(GLFWwindow* window, int key, int, int action, int) {
	if (action != GLFW_PRESS && action != GLFW_REPEAT) {
		return;
	}
	auto* state = static_cast<PresentationState*>(glfwGetWindowUserPointer(window));
	switch (key) {
		case GLFW_KEY_ESCAPE:
			glfwSetWindowShouldClose(window, GLFW_TRUE);
			break;
		case GLFW_KEY_H:
			if (state != nullptr) {
				state->hudVisible = !state->hudVisible;
			}
			break;
		case GLFW_KEY_F11:
			if (state != nullptr) {
				applyFullscreen(window, *state, !gFullscreen,
					state->windowedWidth, state->windowedHeight);
			}
			break;
		default:
			break;
	}
}

/// Width-fit, vertically centred: the rule the video path has always used.
/// Content taller than the frame fills the height and crops the sides.
media::Rect widthFitRect(float mediaW, float mediaH, float viewW, float viewH) {
	if (mediaW <= 0.0f || mediaH <= 0.0f || viewW <= 0.0f || viewH <= 0.0f) {
		return {0.0f, 0.0f, viewW, viewH};
	}
	const float scale = viewW / mediaW;
	const float fullH = mediaH * scale;
	if (fullH <= viewH) {
		return {0.0f, (viewH - fullH) * 0.5f, viewW, fullH};
	}
	const float fullW = viewH / mediaH * mediaW;
	return {(viewW - fullW) * 0.5f, 0.0f, fullW, viewH};
}

void logStartup(const media::RenderDevice& device, const media::HttpControlServer& server) {
	const unsigned long api = mpv_client_api_version();
	LOG_NOTICE("App") << "media-player-cpp";
	LOG_NOTICE("App") << "  render backend : " << device.backendName();
	LOG_NOTICE("App") << "  GL version     : " << (const char*)glGetString(GL_VERSION);
	LOG_NOTICE("App") << "  renderer       : " << (const char*)glGetString(GL_RENDERER);
	LOG_NOTICE("App") << "  libmpv client  : " << (api >> 16) << "." << (api & 0xFFFF);
	LOG_NOTICE("App") << "  data root      : " << media::platform::dataDirectory();
	LOG_NOTICE("App") << "  control API    : "
		<< (server.isRunning() ? "listening on 127.0.0.1:" + std::to_string(server.port())
			: std::string("DISABLED"));
}

} // namespace

int main(int argc, char** argv) {
	// Startup diagnostics must survive an abrupt exit (a killed GUI process
	// never flushes a redirected stdout).
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	media::log::setThresholdFromEnv();

	Options options;
	if (!parseOptions(argc, argv, options)) {
		return 0;   // --help, already printed
	}

	// libmpv requires LC_NUMERIC == "C"; mpv_create() returns NULL otherwise.
	// On a locale using ',' as the decimal separator this is a silent failure.
	std::setlocale(LC_NUMERIC, "C");

	glfwSetErrorCallback(onGlfwError);
	if (glfwInit() != GLFW_TRUE) {
		LOG_ERROR("App") << "glfwInit failed";
		return 1;
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_SAMPLES, 0);

	gWindow = glfwCreateWindow(options.width, options.height, "media-player-cpp",
		nullptr, nullptr);
	if (gWindow == nullptr) {
		LOG_ERROR("App") << "glfwCreateWindow failed";
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(gWindow);
	glfwSwapInterval(1);

	// Presentation the API may toggle. The window pointer is the user-data slot
	// so the key handler can reach it without a second global.
	PresentationState presentation;
	presentation.hudVisible = options.hudVisible;
	presentation.windowedWidth = options.width;
	presentation.windowedHeight = options.height;
	glfwSetWindowUserPointer(gWindow, &presentation);
	glfwSetKeyCallback(gWindow, onKey);

	const GLenum glewStatus = glewInit();
	if (glewStatus != GLEW_OK) {
		LOG_ERROR("App") << "glewInit failed: " << glewGetErrorString(glewStatus);
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	std::unique_ptr<media::RenderDevice> device = media::createGlRenderDevice();
	if (!device || !device->initialize()) {
		LOG_ERROR("App") << "RenderDevice::initialize failed";
		glfwDestroyWindow(gWindow);
		glfwTerminate();
		return 1;
	}

	// --- media engine ------------------------------------------------------
	media::MPVSurface surface;

	// Scripts must be attached BEFORE the surface initializes: mpv only reads
	// the `scripts` option during mpv_initialize(), so handing them over
	// afterwards silently does nothing.
	//
	// The controller is the single source of truth for the script list: main
	// sets it here and setup() forwards it to the backend. Setting it on the
	// surface directly would leave the controller unable to report what is on
	// disk.
	media::MediaClipLibrary library;
	library.scan();

	media::MediaPlayerController controller(library, &surface);
	controller.setScripts(media::scripts::discover());

	const bool videoReady = surface.initialize(*device);
	if (!videoReady) {
		LOG_WARN("App") << "video unavailable - the control API still runs";
	}

	if (!controller.setup()) {
		LOG_WARN("App") << "no clips loaded; put media in "
			<< media::platform::dataDirectory();
	}

	// The API may toggle the HUD and fullscreen, but it must never touch the
	// render loop or name a graphics API. These closures are the only path:
	// every call runs on the main thread, inside dispatch/poll().
	media::PresentationHooks hooks;
	hooks.getHud = [&presentation] { return presentation.hudVisible; };
	hooks.setHud = [&presentation](bool visible) {
		presentation.hudVisible = visible;
		return true;
	};
	hooks.getFullscreen = [] { return gFullscreen; };
	hooks.setFullscreen = [&presentation](bool enabled) {
		applyFullscreen(gWindow, presentation, enabled,
			presentation.windowedWidth, presentation.windowedHeight);
		return true;
	};

	media::HttpControlServer server(controller, hooks);
	if (!server.start(options.port)) {
		LOG_WARN("App") << "control API unavailable; the window still runs";
	}

	int fbW = 0, fbH = 0;
	glfwGetFramebufferSize(gWindow, &fbW, &fbH);
	device->setViewport(fbW, fbH);
	if (options.fullscreen) {
		applyFullscreen(gWindow, presentation, true,
			presentation.windowedWidth, presentation.windowedHeight);
	}
	logStartup(*device, server);

	while (glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();

		// Execute queued API commands on this thread, then let the HTTP workers
		// answer. Everything that touches the decoder happens in here.
		server.poll();
		surface.pumpEvents();

		glfwGetFramebufferSize(gWindow, &fbW, &fbH);
		if (fbW <= 0 || fbH <= 0) {
			glfwWaitEventsTimeout(0.1);
			continue;
		}
		const float vw = static_cast<float>(fbW);
		const float vh = static_cast<float>(fbH);

		device->setViewport(fbW, fbH);
		device->beginFrame();
		device->clear(0x08, 0x08, 0x0A);

		const media::MediaPlayerStatus status = controller.getStatus();

		// Video and stills both arrive from mpv as a decoded frame in the same
		// FBO, so one width-fit draw covers both. A held image keeps rendering
		// because mpv is configured with image-display-duration=inf.
		if (videoReady && surface.videoWidth() > 0) {
			const media::Rect dest = widthFitRect(
				static_cast<float>(surface.videoWidth()),
				static_cast<float>(surface.videoHeight()), vw, vh);
			surface.draw(*device, dest);
		}

		// --- HUD ----------------------------------------------------------
		// Hidden means exactly that: nothing is drawn over the frame, so the
		// player is pure video until the Controller (POST /api/hud) or the H
		// key brings it back.
		if (presentation.hudVisible) {
			const float pad = 18.0f;
			const float scale = 3.0f;
			const float lineHeight = (media::hud::kGlyphHeight + 4) * scale;
			const float panelH = lineHeight * 4.0f + pad;
			device->drawSolid({0.0f, 0.0f, vw, panelH}, 0x00, 0x00, 0x00, 0x8C);

			char line[320];
			float y = pad * 0.6f;

			std::snprintf(line, sizeof(line), "CLIP %d/%d  %s",
				static_cast<int>(status.loaded ? status.clipIndex + 1 : 0),
				static_cast<int>(status.clipCount),
				status.clipName.empty() ? "(none)" : status.clipName.c_str());
			device->drawText(line, pad, y, scale, 0xFF, 0xFF, 0xFF);
			y += lineHeight;

			std::snprintf(line, sizeof(line), "STATE %s%s",
				status.isImage ? "IMAGE" : (status.playing ? "PLAYING" : "STOPPED"),
				status.paused ? " (PAUSED)" : "");
			device->drawText(line, pad, y, scale, 0xB0, 0xD8, 0xFF);
			y += lineHeight;

			if (status.duration > 0.0) {
				std::snprintf(line, sizeof(line), "TIME %.1f / %.1f  %.2fX  VOL %.0f",
					status.position, status.duration, status.speed, status.volume);
			} else {
				std::snprintf(line, sizeof(line), "TIME %.1f  %.2fX  VOL %.0f",
					status.position, status.speed, status.volume);
			}
			device->drawText(line, pad, y, scale, 0xB0, 0xD8, 0xFF);
			y += lineHeight;

			std::snprintf(line, sizeof(line), "API %d  SUB %s  DEC %s",
				server.port(),
				status.subtitlesEnabled ? "ON" : "OFF",
				status.decoder.empty() ? "-" : status.decoder.c_str());
			device->drawText(line, pad, y, scale, 0x86, 0x96, 0xA8);

			// Progress bar, driven by real position when the media is seekable.
			const float barH = 6.0f;
			float progress = 0.0f;
			if (status.duration > 0.0 && status.position >= 0.0) {
				progress = static_cast<float>(status.position / status.duration);
			} else if (status.clipCount > 0) {
				progress = static_cast<float>(status.clipIndex + 1)
					/ static_cast<float>(status.clipCount);
			}
			progress = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
			const float filled = vw * progress;
			device->drawSolid({0.0f, vh - barH, filled, barH}, 0x2E, 0x9E, 0xFF, 0xFF);
			device->drawSolid({filled, vh - barH, vw - filled, barH}, 0x20, 0x24, 0x2C, 0xFF);
		}

		device->endFrame();
		glfwSwapBuffers(gWindow);
	}

	server.stop();
	// mpv must release its render context while the GL context is still alive.
	surface.shutdown();
	device.reset();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
