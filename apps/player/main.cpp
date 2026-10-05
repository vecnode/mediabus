/*
 * vn-mediabus-player - application shell
 *
 * Owns the window, the GL context, the frame loop and the control plane. All
 * drawing goes through media::RenderDevice; this file must never name an OpenGL
 * symbol, and no widget toolkit is involved: the HUD is drawn on the canvas.
 *
 * Copyright (c) vecnode 2026 - GPL-2.0-or-later (see LICENSE)
 */

#include "net/HttpControlServer.h"
#include "gfx/BitmapFont.h"
#include "gfx/UiScaleGlfw.h"
#include "gfx/GlLoader.h"
#include "gfx/RenderDevice.h"
#include "mpv/MPVSurface.h"
#include "core/AppConfig.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/UiScale.h"
#include "media/MediaClipLibrary.h"
#include "media/MediaPlayerController.h"
#include "shader/ShaderClipRenderer.h"

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
		"vn-mediabus-player - scriptable video player with a localhost control API\n"
		"\n"
		"Usage: vn-mediabus-player.exe [options]\n"
		"\n"
		"  --width N          window width  (default %d)\n"
		"  --height N         window height (default %d)\n"
		"  --fullscreen       start fullscreen\n"
		"  --no-hud           start with the status HUD hidden\n"
		"  --port N           control API port (default %d)\n"
		"  --help, -h         show this text\n"
		"\n"
		"Keys:  H toggle HUD   F11 toggle fullscreen   Esc leave fullscreen\n"
		"       (Esc does not quit: close the window for that)\n"
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
			// Esc LEAVES FULLSCREEN. It deliberately does not quit.
			//
			// Quitting on Esc cost the whole session: the decoder, the control API
			// on :8080 and whatever the Controller was doing all went with the
			// keypress, and Esc is the reflex for "get me out of here" while
			// watching something that filled the screen. The window's own close
			// button is the way out of the application.
			if (gFullscreen && state != nullptr) {
				applyFullscreen(window, *state, false,
					state->windowedWidth, state->windowedHeight);
				LOG_NOTICE("App") << "Esc: left fullscreen";
			}
			// Windowed already: nothing to leave, so nothing happens. Doing
			// nothing is the point - an inert Esc cannot end the session.
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

void logStartup(const media::RenderDevice& device, const media::HttpControlServer& server,
	const media::MediaClipLibrary& library, float uiScale, float hudScale,
	float contentScale) {
	const unsigned long api = mpv_client_api_version();
	LOG_NOTICE("App") << "vn-mediabus-player";
	LOG_NOTICE("App") << "  render backend : " << device.backendName();
	LOG_NOTICE("App") << "  GL version     : " << (const char*)glGetString(GL_VERSION);
	LOG_NOTICE("App") << "  renderer       : " << (const char*)glGetString(GL_RENDERER);
	LOG_NOTICE("App") << "  libmpv client  : " << (api >> 16) << "." << (api & 0xFFFF);
	LOG_NOTICE("App") << "  shader library : "
		<< (library.hasBuiltinRoot() ? library.builtinRoot()
			: std::string("(none found - no shader clips)"));
	if (library.roots().empty()) {
		LOG_NOTICE("App") << "  media folder   : (none chosen)";
	} else {
		// One line per folder: with several merged, a single line naming only the
		// first is exactly the kind of half-truth that makes a missing clip hard
		// to explain.
		for (const std::string& folder : library.roots()) {
			LOG_NOTICE("App") << "  media folder   : " << folder;
		}
	}
	// Three states to describe, not two: nothing to walk, still walking, done.
	if (library.scanning()) {
		LOG_NOTICE("App") << "  media found    : scanning - "
			<< library.scanEntries() << " entries so far, " << library.size()
			<< " media file(s); the window is live and will load when it finishes";
	} else {
		LOG_NOTICE("App") << "  media found    : " << library.size() << " clip(s)";
	}
	LOG_NOTICE("App") << "  config file    : " << media::config::configPath();
	LOG_NOTICE("App") << "  text scale     : "
		<< media::ui::describeDecision(uiScale, contentScale)
		<< " -> HUD " << media::ui::describe(hudScale);
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
	// This binary is a Windows-subsystem application, so when it is started from
	// Explorer - or by the launcher - there is no console and stderr goes
	// nowhere. Deciding the sink now names the log file after the application
	// rather than after whichever line happened to be written first.
	media::log::useFileSink("player");

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

	// How big text is drawn. The bitmap font is a fixed 5x7 atlas, so "font
	// size" here is a multiplier on that cell. It comes from the monitor's
	// content scale (what the Windows DPI setting reports) through the shared
	// helper, and is floored by ui::*Pixels so a 4K display at 100% scaling is
	// still readable - plain DPI multiplication would leave that case exactly
	// as unreadable as it is today.
	media::config::Config config;
	media::config::load(config);

	// mediabus.ini also carries the folder chosen in the Dashboard or the
	// Controller, read further down before the first scan.
	const float contentScale = media::ui::rawContentScale();
	const float uiScale = media::ui::scaleForWindow(config);
	const float hudTextScale = media::ui::textScale(uiScale, 22.0f);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	glfwWindowHint(GLFW_SAMPLES, 0);

	gWindow = glfwCreateWindow(options.width, options.height, "vn-mediabus-player",
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

	// GLEW through the shared helper: correct ordering, and the drain that
	// clears the spurious core-profile GL_INVALID_ENUM glewInit leaves in the
	// error queue. See libs/gfx/GlLoader.h.
	if (!media::gfx::initializeGlLoader(gWindow)) {
		LOG_ERROR("App") << "glewInit failed";
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

	// The shader renderer needs the GL context, which is current by now. A failure
	// here is not fatal: the Player still plays everything else, and reports what
	// happened when a shader clip comes up.
	media::ShaderClipRenderer shaders;
	if (!shaders.initialize()) {
		LOG_WARN("App") << "shader clips are unavailable in this session";
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

	// The shader library is the ONE folder the operator never chose, and it is
	// walked unconditionally, so there is always something to play even before a
	// corpus has been picked. It is not one of the roots: setBuiltinRoot() is
	// deliberately separate so hasRoot() stays false and nothing claims an
	// operator chose a folder they did not.
	library.setBuiltinRoot(media::platform::shaderDirectory());
	if (!library.hasBuiltinRoot()) {
		LOG_WARN("App") << "no shader library found next to the executable; "
			<< "shader clips will not be in the playlist";
	}

	media::MediaPlayerController controller(library, &surface);
	controller.setScripts(media::scripts::discover());

	// NOTHING OF THE OPERATOR'S IS WALKED UNLESS A FOLDER WAS CHOSEN, and even
	// then the walk is only STARTED here - pollScan() in the frame loop below
	// carries it forward a slice at a time. Both halves matter:
	//
	//   - a fresh install opens instantly with only the shader library, instead of
	//     silently playing whatever happened to ship in bin/data, so "no clips"
	//     stops being indistinguishable from "the folder moved";
	//   - pointing the Player at a folder far larger than a corpus - a Desktop,
	//     a whole drive - cannot stop the window painting. That was the bug: the
	//     walk ran to completion before the first frame, so a 536,394-file folder
	//     produced a window that never drew, which is indistinguishable from a
	//     hang and is exactly how "the Player will not open from the Dashboard"
	//     presented.
	if (config.mediaFolders.empty()) {
		LOG_NOTICE("App") << "no media folder chosen: the playlist is the built-in "
			<< "shader library. Add one with CHANGE... in the launcher, or POST "
			<< "/api/media-dir";
	}
	// Every chosen folder, merged into one playlist by the library.
	controller.beginStartupScan(config.mediaFolders);

	const bool videoReady = surface.initialize(*device);
	if (!videoReady) {
		LOG_WARN("App") << "video unavailable - the control API still runs";
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
	// Read by /api/status as "mediaFolders" (and "mediaFolder", the first one).
	// Reading through the controller rather than a cached copy means the folders
	// the Controller displays are always the folders the decoder is pointed at.
	hooks.getMediaFolders = [&controller] { return controller.mediaFolders(); };

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
	logStartup(*device, server, library, uiScale, hudTextScale, contentScale);

	/// u_time origin for the shader clip on screen, and which clip that is. Reset
	/// whenever the clip changes, so every shader starts at its own zero.
	std::string shaderClipPath;
	double shaderStartedAt = 0.0;

	while (glfwWindowShouldClose(gWindow) == GLFW_FALSE) {
		glfwPollEvents();

		// Execute queued API commands on this thread, then let the HTTP workers
		// answer. Everything that touches the decoder happens in here.
		server.poll();
		surface.pumpEvents();

		// Carry the media walk forward, and open the first clip the moment it
		// ends. A slice per frame is what keeps this loop free to draw, so the
		// window stays live however large the chosen folder turns out to be.
		controller.pollScan();

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

		// A shader clip is GENERATED, not decoded: there is no mpv frame to
		// composite, so the renderer compiles the clip's source (once, on the frame
		// the clip changes) and fills the frame with it. Drawn first, before
		// RenderDevice has batched anything, so the HUD lands on top.
		if (status.isShader && status.loaded && !status.clipPath.empty()) {
			if (status.clipPath != shaderClipPath) {
				// Each shader starts its own clock at its own zero: u_time is
				// "seconds since this clip was opened", not since the process
				// started, which is what makes a clip look the same however long
				// the Player has been up.
				shaderClipPath = status.clipPath;
				shaderStartedAt = glfwGetTime();
				LOG_NOTICE("App") << "shader clip: " << status.clipPath;
			}
			// Called every frame by design. It compiles once per clip, and a
			// failure is reported once rather than once per frame; the HUD says
			// what went wrong while the previous shader stays on screen.
			std::string shaderError;
			shaders.load(status.clipPath, shaderError);
			shaders.draw(fbW, fbH, glfwGetTime() - shaderStartedAt);
		} else {
			shaderClipPath.clear();
		}

		// Video and stills both arrive from mpv as a decoded frame in the same
		// FBO, so one width-fit draw covers both. A held image keeps rendering
		// because mpv is configured with image-display-duration=inf.
		if (!status.isShader && videoReady && surface.videoWidth() > 0) {
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
			// DPI-derived with a floor, so the overlay is readable on a 4K
			// panel instead of a 7-pixel capital.
			const float scale = hudTextScale;
			const float lineHeight = (media::hud::kGlyphHeight + 4) * scale;
			const float panelH = lineHeight * 5.0f + pad;
			device->drawSolid({0.0f, 0.0f, vw, panelH}, 0x00, 0x00, 0x00, 0x8C);

			char line[320];
			float y = pad * 0.6f;

			// A running scan is REPORTED, never hidden. While the library is
			// still being walked the count is a running total rather than an
			// answer, and saying so is the whole difference between "working" and
			// "hung" - the state this build used to leave the operator in.
			if (status.scanning) {
				std::snprintf(line, sizeof(line), "SCANNING  %d found so far",
					static_cast<int>(status.clipCount));
			} else if (status.scanTruncated) {
				std::snprintf(line, sizeof(line),
					"CLIP 0/0  (folder too large - scan refused, see the log)");
			} else {
				std::snprintf(line, sizeof(line), "CLIP %d/%d  %s",
					static_cast<int>(status.loaded ? status.clipIndex + 1 : 0),
					static_cast<int>(status.clipCount),
					status.clipName.empty()
						? (status.clipCount == 0 ? "(no clips)" : "(none)")
						: status.clipName.c_str());
			}
			device->drawText(line, pad, y, scale, 0xFF, 0xFF, 0xFF);
			y += lineHeight;

			// Which folders the playlist came from, and - when there are none -
			// what to do about it. "0 clips" alone is ambiguous between four
			// problems with four different fixes: nothing chosen yet, a folder that
			// moved, a folder that holds no media, and a set of folders too large to
			// walk. Naming the situation is cheap; guessing at it is not.
			const std::vector<std::string> folders = controller.mediaFolders();
			if (folders.empty()) {
				std::snprintf(line, sizeof(line),
					"DIR (no folder chosen)  -  change it in the launcher's Applications tab");
			} else if (folders.size() == 1) {
				std::snprintf(line, sizeof(line), "DIR %s", folders.front().c_str());
			} else {
				std::snprintf(line, sizeof(line), "DIR %s  (+%d more, merged)",
					folders.front().c_str(), static_cast<int>(folders.size() - 1));
			}
			device->drawText(line, pad, y, scale, 0x86, 0x96, 0xA8);
			y += lineHeight;

			std::snprintf(line, sizeof(line), "STATE %s%s",
				status.isShader ? "SHADER"
					: (status.isImage ? "IMAGE"
						: (status.playing ? "PLAYING" : "STOPPED")),
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

			if (status.isShader && !shaders.lastError().empty()) {
				// The compiler's own words. This is the only line that makes a
				// shader that will not compile fixable at all, so it wins the slot
				// over the decoder readout - which says nothing about a shader.
				std::snprintf(line, sizeof(line), "SHADER ERROR %s",
					shaders.lastError().c_str());
			} else if (status.isShader) {
				std::snprintf(line, sizeof(line), "API %d  SHADER generated clip",
					server.port());
			} else {
				std::snprintf(line, sizeof(line), "API %d  SUB %s  DEC %s",
					server.port(),
					status.subtitlesEnabled ? "ON" : "OFF",
					status.decoder.empty() ? "-" : status.decoder.c_str());
			}
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
	// Both GL owners release while the context is still alive, and both before
	// the device: mpv's render context and the shader programs are GL objects.
	surface.shutdown();
	shaders.shutdown();
	device.reset();
	glfwDestroyWindow(gWindow);
	glfwTerminate();
	return 0;
}
