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
#include <memory>
#include <string>

namespace {

constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;

GLFWwindow* gWindow = nullptr;
bool gFullscreen = false;

void onGlfwError(int code, const char* description) {
	LOG_ERROR("GLFW") << code << ": " << description;
}

void onKey(GLFWwindow* window, int key, int, int action, int) {
	if (action != GLFW_PRESS && action != GLFW_REPEAT) {
		return;
	}
	switch (key) {
		case GLFW_KEY_ESCAPE:
			glfwSetWindowShouldClose(window, GLFW_TRUE);
			break;
		case GLFW_KEY_F11: {
			gFullscreen = !gFullscreen;
			GLFWmonitor* monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = glfwGetVideoMode(monitor);
			glfwSetWindowMonitor(window, gFullscreen ? monitor : nullptr, 0, 0,
				gFullscreen ? mode->width : kDefaultWidth,
				gFullscreen ? mode->height : kDefaultHeight,
				mode->refreshRate);
			break;
		}
		default:
			break;
	}
}

/// Width-fit, vertically centred: the rule the old MediaRenderer used for
/// video. Content taller than the frame fills the height and crops the sides.
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

int main() {
	// Startup diagnostics must survive an abrupt exit (a killed GUI process
	// never flushes a redirected stdout).
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	media::log::setThresholdFromEnv();

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

	gWindow = glfwCreateWindow(kDefaultWidth, kDefaultHeight, "media-player-cpp",
		nullptr, nullptr);
	if (gWindow == nullptr) {
		LOG_ERROR("App") << "glfwCreateWindow failed";
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(gWindow);
	glfwSwapInterval(1);
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

	media::HttpControlServer server(controller);
	if (!server.start(media::HttpControlServer::kDefaultPort)) {
		LOG_WARN("App") << "control API unavailable; the window still runs";
	}

	int fbW = 0, fbH = 0;
	glfwGetFramebufferSize(gWindow, &fbW, &fbH);
	device->setViewport(fbW, fbH);
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
