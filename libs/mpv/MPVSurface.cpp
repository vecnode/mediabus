/*
 * MPVSurface — libmpv playback behind the app's RenderDevice seam.
 *
 * This is a backend, so it is allowed to know about GL: it owns the FBO that
 * mpv renders into and hands the result to the compositor as a TextureId that
 * RenderDevice can draw. Everything ABOVE this file stays API-agnostic.
 *
 * Verified against libmpv 0.41.0 / client API 2.5 on this machine; see
 * BUILDING.md and p0/ for the preflight proof.
 *
 * Copyright (c) vecnode 2026 — GPL-2.0-or-later (see LICENSE)
 */

#include "mpv/MPVSurface.h"

#include "gfx/RenderDevice.h"
#include "core/Log.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace media {
namespace {

/// mpv resolves GL entry points through this. GLFW's loader is the one that
/// knows about the context we are actually using.
void* glProcAddress(void*, const char* name) {
	return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

/// Options that configure libmpv for embedding AND close the documented
/// security holes. Each one is load-bearing:
///
///   ytdl                 default is YES, which spawns an external
///                        youtube-dl/yt-dlp process for URLs mpv cannot open
///                        directly. client.h confirms mpv may start
///                        sub-processes; this is the most important setting.
///   load-scripts         default is YES and auto-loads Lua/JS from the user's
///                        ~/.config/mpv/scripts. We load only explicit paths.
///   config / input-conf  never read the user's mpv configuration.
///   access-references    stops a file pulling in network URLs, chapter
///                        references or archives.
///   autoload-files       stops sidecar imports.
///   load-unsafe-playlists leaves the default "no": mpv's own manual warns
///                        unsafe playlists "may trigger harmful actions" and
///                        that avdevice:// is inherently unsafe.
///
/// `vo=libmpv` is what makes the render API valid; without it the render
/// context creation fails.
struct Option {
	const char* key;
	const char* value;
};

const Option kOptions[] = {
	{"config", "no"},
	{"load-scripts", "no"},
	{"ytdl", "no"},
	{"load-unsafe-playlists", "no"},
	{"access-references", "no"},
	{"autoload-files", "no"},
	{"terminal", "no"},
	{"input-default-bindings", "no"},
	{"input-conf", ""},
	{"vo", "libmpv"},
	{"hwdec", "auto-safe"},
	{"keep-open", "yes"},
	{"idle", "yes"},
	// Audio: mpv owns device selection and A/V sync, which is the whole reason
	// it is the media engine rather than a hand-rolled decode loop.
	{"audio-client-name", "media-player-cpp"},
	// Subtitle rendering goes through mpv's OSD/libass into the same FBO.
	{"sub-visibility", "yes"},
	{"osd-level", "0"},
	// Stills: display forever, so an image clip is a held frame rather than a
	// very short video that ends. mpv decodes images through its image2
	// demuxer, so a PNG takes the same FBO path as a video frame and the app
	// needs no separate image decoder.
	{"image-display-duration", "inf"},
	// Do not let mpv block inside render() waiting for a presentation time;
	// the app's own loop paces presentation.
	{"video-timing-offset", "0"},
};

} // namespace

MPVSurface::MPVSurface() = default;

MPVSurface::~MPVSurface() {
	// Cannot touch GL here (the context may already be gone), so only the mpv
	// side is torn down. Call shutdown() first for a clean release.
	if (mpv_ != nullptr) {
		LOG_WARN("MPVSurface") << "destroyed without shutdown(); releasing mpv only";
		destroyRenderContext();
		mpv_terminate_destroy(mpv_);
		mpv_ = nullptr;
	}
}

unsigned long MPVSurface::clientApiVersion() {
	return mpv_client_api_version();
}

bool MPVSurface::applyOptions() {
	bool ok = true;
	for (const Option& option : kOptions) {
		const int rc = mpv_set_option_string(mpv_, option.key, option.value);
		if (rc < 0) {
			LOG_WARN("MPVSurface") << "option rejected: " << option.key << " = "
				<< option.value << " (" << mpv_error_string(rc) << ")";
			// video-timing-offset is cosmetic rather than structural; do not
			// fail the whole surface over it.
			if (std::strcmp(option.key, "vo") == 0
				|| std::strcmp(option.key, "ytdl") == 0
				|| std::strcmp(option.key, "load-scripts") == 0) {
				ok = false;
			}
		}
	}
	return ok;
}

bool MPVSurface::initialize(RenderDevice& device) {
	if (initialized_) {
		return true;
	}

	mpv_ = mpv_create();
	if (mpv_ == nullptr) {
		// The documented cause is LC_NUMERIC not being "C" (mpv_create returns
		// NULL in that case), which main() sets.
		LOG_ERROR("MPVSurface") << "mpv_create failed — is LC_NUMERIC set to \"C\"?";
		return false;
	}

	applyOptions();

	// Scripts must be attached before mpv_initialize(): mpv only reads the
	// script options at initialization time.
	//
	// The option is `scripts` (plural), a PATH LIST -- not `script`, which the
	// mpv CLI accepts but libmpv's option table does not. Verified with
	// p0/option_probe.cpp, which reports:
	//     script                     -> rejected (option not found)
	//     scripts                    -> ACCEPTED (ok)
	// Entries are joined with the platform path-list separator (';' on
	// Windows, ':' elsewhere). Only paths discovered under <data>/scripts are
	// ever passed here, and `load-scripts` stays off, so nothing outside that
	// directory can execute.
	if (!requestedScripts_.empty()) {
#ifdef _WIN32
		constexpr char kListSeparator = ';';
#else
		constexpr char kListSeparator = ':';
#endif
		std::string list;
		for (const scripts::ScriptFile& script : requestedScripts_) {
			if (!list.empty()) {
				list.push_back(kListSeparator);
			}
			list += script.absolutePath;
		}

		const int rc = mpv_set_option_string(mpv_, "scripts", list.c_str());
		if (rc < 0) {
			LOG_WARN("MPVSurface") << "script list rejected: "
				<< mpv_error_string(rc) << " (" << requestedScripts_.size()
				<< " script(s) not loaded)";
		} else {
			for (const scripts::ScriptFile& script : requestedScripts_) {
				loadedScripts_.push_back(script.name);
			}
			LOG_NOTICE("MPVSurface") << "queued " << requestedScripts_.size()
				<< " script(s): " << list;
		}
	}

	const int irc = mpv_initialize(mpv_);
	if (irc < 0) {
		LOG_ERROR("MPVSurface") << "mpv_initialize failed: " << mpv_error_string(irc);
		mpv_terminate_destroy(mpv_);
		mpv_ = nullptr;
		return false;
	}

	// Log at info and above. This is what makes a script's mp.msg.info() output
	// visible at all: at "warn" a well-behaved script looks like it never ran.
	// Script-authored messages are routed through the app's own logging.
	mpv_request_log_messages(mpv_, "info");

	if (!createRenderContext(device)) {
		mpv_terminate_destroy(mpv_);
		mpv_ = nullptr;
		return false;
	}

	// Observe the properties the HUD and HTTP status need, so pumpEvents()
	// reads cached values instead of querying mpv every frame.
	mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv_, 0, "speed", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv_, 0, "volume", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv_, 0, "width", MPV_FORMAT_INT64);
	mpv_observe_property(mpv_, 0, "height", MPV_FORMAT_INT64);

	initialized_ = true;
	LOG_NOTICE("MPVSurface") << "libmpv ready (client API "
		<< (clientApiVersion() >> 16) << "." << (clientApiVersion() & 0xFFFF)
		<< ", vo=libmpv, hwdec=auto-safe)";
	return true;
}

bool MPVSurface::createRenderContext(RenderDevice& device) {
	// mpv's render API for OpenGL requires the GL context to be current in the
	// calling thread and to stay the same context for the render context's
	// lifetime. We render on the main thread for exactly that reason.
	if (device.nativeContextHandle() == nullptr) {
		LOG_ERROR("MPVSurface") << "no current GL context; cannot create render context";
		return false;
	}

	mpv_opengl_init_params glInit{};
	glInit.get_proc_address = glProcAddress;
	glInit.get_proc_address_ctx = nullptr;

	int advancedControl = 0;   // deliberately off; see the class comment

	// mpv_render_param.data is void*, so the API-type string needs a mutable
	// pointer. A function-local static avoids both the cast and any lifetime
	// question.
	static char apiTypeOpenGl[] = "opengl";
	static_assert(sizeof(MPV_RENDER_API_TYPE_OPENGL) - 1 == sizeof(apiTypeOpenGl) - 1,
		"mpv's OpenGL API-type string changed");

	mpv_render_param params[] = {
		{MPV_RENDER_PARAM_API_TYPE, apiTypeOpenGl},
		{MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
		{MPV_RENDER_PARAM_ADVANCED_CONTROL, &advancedControl},
		{MPV_RENDER_PARAM_INVALID, nullptr},
	};

	const int rc = mpv_render_context_create(&renderContext_, mpv_, params);
	if (rc < 0) {
		LOG_ERROR("MPVSurface") << "mpv_render_context_create failed: "
			<< mpv_error_string(rc);
		renderContext_ = nullptr;
		return false;
	}

	mpv_render_context_set_update_callback(renderContext_, &MPVSurface::onRenderUpdate, this);

	// The frame target: an RGBA8 texture owned by the device (so the compositor
	// can address it) plus an FBO of our own that mpv draws into.
	const int w = 1280;
	const int h = 720;
	frameTexture_ = device.createEmpty(kInvalidTexture, w, h);
	if (frameTexture_ == kInvalidTexture) {
		LOG_ERROR("MPVSurface") << "could not allocate the frame texture";
		return false;
	}
	const GLuint frameTextureName =
		static_cast<GLuint>(device.nativeTextureHandle(frameTexture_));
	if (frameTextureName == 0) {
		LOG_ERROR("MPVSurface") << "device returned no native handle for the frame texture";
		return false;
	}

	glGenFramebuffers(1, &frameFbo_);
	glBindFramebuffer(GL_FRAMEBUFFER, frameFbo_);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		frameTextureName, 0);
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		LOG_ERROR("MPVSurface") << "frame FBO incomplete (0x" << std::hex << status << ")";
		return false;
	}
	frameWidth_ = w;
	frameHeight_ = h;
	frameTextureName_ = frameTextureName;
	LOG_NOTICE("MPVSurface") << "frame target " << w << "x" << h
		<< " (texture " << frameTextureName_ << ", fbo " << frameFbo_ << ")";
	return true;
}

void MPVSurface::onRenderUpdate(void* context) {
	auto* self = static_cast<MPVSurface*>(context);
	// Called from an arbitrary mpv thread: set a flag and return immediately.
	// Touching GL or any other mpv function here is forbidden.
	if (self != nullptr) {
		self->framePending_.store(true, std::memory_order_release);
	}
}

void MPVSurface::destroyRenderContext() {
	if (renderContext_ != nullptr) {
		mpv_render_context_free(renderContext_);
		renderContext_ = nullptr;
	}
}

void MPVSurface::destroyTextures(RenderDevice& device) {
	if (frameTexture_ != kInvalidTexture) {
		device.destroyTexture(frameTexture_);
		frameTexture_ = kInvalidTexture;
	}
	if (frameFbo_ != 0) {
		const GLuint fbo = frameFbo_;
		glDeleteFramebuffers(1, &fbo);
		frameFbo_ = 0;
	}
}

void MPVSurface::shutdown() {
	if (mpv_ == nullptr && renderContext_ == nullptr) {
		return;
	}
	// mpv_render_context_free must run before the core is destroyed, and both
	// before the GL context goes away.
	destroyRenderContext();
	if (mpv_ != nullptr) {
		mpv_terminate_destroy(mpv_);
		mpv_ = nullptr;
	}
	initialized_ = false;
}

bool MPVSurface::queryNumber(const char* property, double& out) const {
	if (mpv_ == nullptr) {
		return false;
	}
	double value = 0.0;
	if (mpv_get_property(mpv_, property, MPV_FORMAT_DOUBLE, &value) < 0) {
		return false;
	}
	out = value;
	return true;
}

std::string MPVSurface::queryString(const char* property) const {
	if (mpv_ == nullptr) {
		return {};
	}
	char* raw = mpv_get_property_string(mpv_, property);
	if (raw == nullptr) {
		return {};
	}
	std::string value(raw);
	mpv_free(raw);
	return value;
}

void MPVSurface::pumpEvents() {
	if (mpv_ == nullptr) {
		return;
	}

	while (true) {
		mpv_event* event = mpv_wait_event(mpv_, 0);
		if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
			break;
		}

		switch (event->event_id) {
			case MPV_EVENT_FILE_LOADED: {
				double d = 0.0;
				if (queryNumber("width", d)) {
					videoWidth_ = static_cast<int>(d);
				}
				if (queryNumber("height", d)) {
					videoHeight_ = static_cast<int>(d);
				}
				decoderName_ = queryString("hwdec-current");
				if (decoderName_.empty() || decoderName_ == "no") {
					decoderName_ = queryString("video-codec");
				}
				LOG_NOTICE("MPVSurface") << "loaded " << videoWidth_ << "x" << videoHeight_
					<< " codec=\"" << queryString("video-codec") << "\""
					<< " decoder=\"" << decoderName_ << "\"";
				framePending_.store(true, std::memory_order_release);
				break;
			}
			case MPV_EVENT_END_FILE: {
				auto* data = static_cast<mpv_event_end_file*>(event->data);
				if (data != nullptr && data->reason == MPV_END_FILE_REASON_ERROR) {
					LOG_WARN("MPVSurface") << "playback error: "
						<< mpv_error_string(data->error) << " (" << currentPath_ << ")";
				} else {
					LOG_NOTICE("MPVSurface") << "end of file (" << currentPath_ << ")";
				}
				break;
			}
			case MPV_EVENT_PROPERTY_CHANGE: {
				auto* property = static_cast<mpv_event_property*>(event->data);
				if (property == nullptr || property->name == nullptr) {
					break;
				}
				const std::string name = property->name;
				if (name == "pause" && property->format == MPV_FORMAT_FLAG) {
					paused_ = *static_cast<int*>(property->data) != 0;
				} else if (name == "speed" && property->format == MPV_FORMAT_DOUBLE) {
					// consumed by state()
				} else if (name == "width" && property->format == MPV_FORMAT_INT64) {
					videoWidth_ = static_cast<int>(*static_cast<int64_t*>(property->data));
				} else if (name == "height" && property->format == MPV_FORMAT_INT64) {
					videoHeight_ = static_cast<int>(*static_cast<int64_t*>(property->data));
				}
				break;
			}
			case MPV_EVENT_LOG_MESSAGE: {
				auto* message = static_cast<mpv_event_log_message*>(event->data);
				if (message != nullptr) {
					LOG_WARN("mpv") << message->prefix << ": " << message->text;
				}
				break;
			}
			case MPV_EVENT_SHUTDOWN:
				LOG_NOTICE("MPVSurface") << "mpv core shut down";
				break;
			default:
				break;
		}
	}
}

void MPVSurface::draw(RenderDevice& device, const Rect& dest) {
	if (renderContext_ == nullptr || mpv_ == nullptr) {
		return;
	}
	if (!device.hasTexture(frameTexture_)) {
		return;
	}

	// Ask mpv for a new frame only when it said one is ready, then draw the
	// last frame into the FBO. mpv reconfigures itself when the target changes.
	if (framePending_.exchange(false, std::memory_order_acq_rel)
		|| mpv_render_context_update(renderContext_) != 0) {
		mpv_opengl_fbo target{};
		target.fbo = static_cast<int>(frameFbo_);
		target.w = frameWidth_;
		target.h = frameHeight_;
		target.internal_format = 0;

		int flipY = 1;   // we sample the texture top-down when compositing

		mpv_render_param params[] = {
			{MPV_RENDER_PARAM_OPENGL_FBO, &target},
			{MPV_RENDER_PARAM_FLIP_Y, &flipY},
			{MPV_RENDER_PARAM_INVALID, nullptr},
		};
		const int rc = mpv_render_context_render(renderContext_, params);
		if (rc < 0) {
			LOG_WARN("MPVSurface") << "render failed: " << mpv_error_string(rc);
		}
	}

	device.drawQuad(frameTexture_, dest);
}

bool MPVSurface::open(const MediaClip& clip, bool autoplay) {
	if (mpv_ == nullptr) {
		return false;
	}
	if (clip.absolutePath.empty()) {
		return false;
	}
	// Images and video both go through mpv: stills are decoded by its image2
	// demuxer and held because image-display-duration=inf. The distinction that
	// matters to callers is reported by state().isImage, not by this path.

	currentPath_ = clip.absolutePath;
	currentIsImage_ = clip.mediaType == ClipMediaType::Image;
	videoWidth_ = 0;
	videoHeight_ = 0;
	decoderName_.clear();
	paused_ = true;

	// `replace` stops the current file instead of queueing onto the playlist.
	const char* command[] = {"loadfile", clip.absolutePath.c_str(), "replace", nullptr};
	const int rc = mpv_command(mpv_, command);
	if (rc < 0) {
		LOG_WARN("MPVSurface") << "loadfile failed for " << clip.displayName
			<< ": " << mpv_error_string(rc);
		return false;
	}

	// Prime a preview frame while paused, so a clip that is opened but not started
	// is a still picture rather than a black rectangle.
	mpv_set_property_string(mpv_, "pause", "yes");

	if (autoplay && !currentIsImage_) {
		// Undo the priming pause. This is the step that was missing: mpv accepts
		// `pause no` before the file has finished loading and honours it when it
		// does, so the clip starts on its own.
		//
		// A still is left paused on purpose. It has no timeline, image-display-
		// duration=inf holds it, and "playing" is a state it cannot be in - the
		// status reports it as an image instead.
		mpv_set_property_string(mpv_, "pause", "no");
		paused_ = false;
	}
	return true;
}

void MPVSurface::close() {
	if (mpv_ == nullptr) {
		return;
	}
	const char* command[] = {"stop", nullptr};
	mpv_command(mpv_, command);
	paused_ = true;
	videoWidth_ = 0;
	videoHeight_ = 0;
	currentPath_.clear();
}

void MPVSurface::play() {
	if (mpv_ == nullptr) {
		return;
	}
	// A still is already fully displayed; "play" is a no-op rather than an
	// error, so a host can issue play unconditionally.
	if (currentIsImage_) {
		return;
	}
	mpv_set_property_string(mpv_, "pause", "no");
	paused_ = false;
}

void MPVSurface::pause() {
	if (mpv_ == nullptr) {
		return;
	}
	mpv_set_property_string(mpv_, "pause", "yes");
	paused_ = true;
}

void MPVSurface::stopToPreview() {
	// Pause in place rather than stop()+seek(0): seeking to zero before the
	// first frame is decoded is slow and can fail on some backends.
	pause();
}

TransportState MPVSurface::state() const {
	TransportState out;
	out.loaded = mpv_ != nullptr && videoWidth_ > 0;
	out.isImage = currentIsImage_;
	// A held still has no timeline: never "playing", never seekable, no clock.
	//
	// For anything else, `paused` is the single source of truth and `playing` is
	// its inverse. mpv's `pause` property is the only thing that actually decides
	// this, and the observer on it keeps `paused_` current - whereas a separate
	// `playing_` flag was a second opinion that could disagree with it, and did:
	// opening a clip came back with playing=false and paused=false at the same
	// time, which is a state no transport can be in.
	out.paused = currentIsImage_ ? true : paused_;
	out.playing = currentIsImage_ ? false : (!paused_ && out.loaded);
	out.subtitlesEnabled = subtitlesEnabled_;
	out.decoder = decoderName_;

	double value = 0.0;
	if (currentIsImage_) {
		out.seekable = false;
		out.position = 0.0;
		out.duration = 0.0;
		if (queryNumber("volume", value)) {
			out.volume = value;
		}
		return out;
	}

	if (queryNumber("time-pos", value)) {
		out.position = value;
	}
	if (queryNumber("duration", value)) {
		out.duration = value;
	}
	if (queryNumber("speed", value)) {
		out.speed = value;
	}
	if (queryNumber("volume", value)) {
		out.volume = value;
	}
	// seekable is false until a file is demuxed far enough to know.
	int seekable = 0;
	if (mpv_ != nullptr && mpv_get_property(mpv_, "seekable", MPV_FORMAT_FLAG, &seekable) >= 0) {
		out.seekable = seekable != 0;
	}
	return out;
}

void MPVSurface::seekAbsolute(double seconds) {
	if (mpv_ == nullptr) {
		return;
	}
	// "absolute+exact" gives a precise seek; mpv decodes from the previous
	// keyframe when it has to.
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
	const char* command[] = {"seek", buffer, "absolute+exact", nullptr};
	if (mpv_command(mpv_, command) < 0) {
		const char* fallback[] = {"seek", buffer, "absolute", nullptr};
		mpv_command(mpv_, fallback);
	}
	framePending_.store(true, std::memory_order_release);
}

void MPVSurface::seekRelative(double seconds) {
	if (mpv_ == nullptr) {
		return;
	}
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
	const char* command[] = {"seek", buffer, "relative", nullptr};
	mpv_command(mpv_, command);
	framePending_.store(true, std::memory_order_release);
}

void MPVSurface::seekPercent(double percent) {
	double duration = 0.0;
	if (!queryNumber("duration", duration) || duration <= 0.0) {
		LOG_WARN("MPVSurface") << "percent seek ignored: duration unknown";
		return;
	}
	seekAbsolute(duration * percent / 100.0);
}

void MPVSurface::setSpeed(double factor) {
	if (mpv_ == nullptr) {
		return;
	}
	// mpv inserts scaletempo2 automatically above 1x, so pitch is preserved.
	mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &factor);
}

void MPVSurface::setVolume(double percent) {
	if (mpv_ == nullptr) {
		return;
	}
	// mpv_set_property takes a non-const void*, so the value cannot be const.
	double clamped = percent < 0.0 ? 0.0 : (percent > 100.0 ? 100.0 : percent);
	mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &clamped);
}

void MPVSurface::setScripts(const std::vector<scripts::ScriptFile>& scripts) {
	if (mpv_ != nullptr) {
		// Re-sending the list that was queued before mpv_initialize() is not a
		// late change — those scripts are already running. Returning quietly is
		// what keeps a normal start from warning about scripts that did load,
		// which made a working script path look broken in the log.
		bool alreadyApplied = requestedScripts_.size() == scripts.size();
		for (std::size_t i = 0; alreadyApplied && i < scripts.size(); ++i) {
			alreadyApplied = requestedScripts_[i].absolutePath == scripts[i].absolutePath;
		}
		if (alreadyApplied) {
			return;
		}
		// A genuinely different list: mpv has initialized and will not accept
		// new scripts. Say so plainly rather than silently doing nothing.
		LOG_WARN("MPVSurface") << "setScripts called after initialization; "
			<< scripts.size() << " script(s) ignored until restart";
		return;
	}
	requestedScripts_ = scripts;
}

std::vector<std::string> MPVSurface::loadedScripts() const {
	return loadedScripts_;
}

void MPVSurface::setSubtitlesEnabled(bool enabled) {
	subtitlesEnabled_ = enabled;
	if (mpv_ == nullptr) {
		return;
	}
	mpv_set_property_string(mpv_, "sub-visibility", enabled ? "yes" : "no");
}

} // namespace media
