#pragma once

#include "gfx/RenderDevice.h"        // Rect, TextureId, RenderDevice
#include "media/MediaPlayerController.h"   // IPlaybackBackend
#include "media/ScriptHost.h"              // scripts::ScriptFile

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_render_context;

namespace media {

class RenderDevice;

/// libmpv-backed playback surface.
///
/// Design notes (all four points were verified by the P0 probe before this
/// class existed):
///
///  * `vo=libmpv` plus MPV_RENDER_API_TYPE_OPENGL means mpv renders into an FBO
///    this class owns, so the frame can be composited anywhere in the scene
///    rather than only ever filling the window.
///
///  * Decode is hardware-accelerated (`hwdec=auto-safe`); the probe reported
///    `d3d11va-copy` on this machine. Software decode remains the automatic
///    fallback, so a machine without a usable decoder still plays.
///
///  * Rendering happens on the MAIN thread, inside RenderDevice::beginFrame() /
///    endFrame(). Per mpv's render.h the OpenGL context must be current in the
///    calling thread and be the same context the render context was created
///    with; single-threaded rendering satisfies that by construction.
///    `video-timing-offset=0` plus MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME=0
///    stop mpv sleeping inside the render call, so it never stalls the loop.
///    Moving this to a shared-context render thread is the documented future
///    optimisation (see BUILDING.md), not a correctness requirement.
///
///  * `mpv_create()` requires LC_NUMERIC == "C"; that is set in main() before
///    anything here runs.
class MPVSurface final : public IPlaybackBackend {
public:
	MPVSurface();
	~MPVSurface() override;

	MPVSurface(const MPVSurface&) = delete;
	MPVSurface& operator=(const MPVSurface&) = delete;

	/// Create the mpv instance and its render context. `device` supplies the
	/// live GL context and the GL entry points mpv needs. Returns false and
	/// logs why on failure; the app keeps running without video if so.
	///
	/// Any scripts set with setScripts() are applied here, before
	/// mpv_initialize(), which is the only time mpv accepts them.
	bool initialize(RenderDevice& device);

	/// Release mpv. Safe to call more than once. Must run before the GL context
	/// is destroyed.
	void shutdown();

	bool isInitialized() const { return initialized_; }

	/// Pump mpv events. Main thread, once per frame, before draw().
	void pumpEvents();

	/// Draw the current video frame into the scene at `dest`. Main thread.
	void draw(RenderDevice& device, const Rect& dest);

	/// Size of the video, in pixels (0x0 when nothing is loaded).
	int videoWidth() const { return videoWidth_; }
	int videoHeight() const { return videoHeight_; }

	/// Raw mpv handle, for scripting and property access. Never null once
	/// initialized.
	mpv_handle* handle() const { return mpv_; }

	/// mpv client API version this build targets.
	static unsigned long clientApiVersion();

	// --- IPlaybackBackend -------------------------------------------------
	bool open(const MediaClip& clip, bool autoplay) override;
	void close() override;
	void play() override;
	void pause() override;
	void stopToPreview() override;
	TransportState state() const override;
	void seekAbsolute(double seconds) override;
	void seekRelative(double seconds) override;
	void seekPercent(double percent) override;
	void setSpeed(double factor) override;
	void setVolume(double percent) override;
	void setSubtitlesEnabled(bool enabled) override;
	void setScripts(const std::vector<scripts::ScriptFile>& scripts) override;
	std::vector<std::string> loadedScripts() const override;

private:
	bool createRenderContext(RenderDevice& device);
	void destroyRenderContext();
	void destroyTextures(RenderDevice& device);

	/// Read a double-valued mpv property; returns false when unavailable.
	bool queryNumber(const char* property, double& out) const;
	/// Read a string-valued mpv property.
	std::string queryString(const char* property) const;
	/// Apply the option set that both configures libmpv for embedding and
	/// closes the documented security holes. Returns false if any is rejected.
	bool applyOptions();

	mpv_handle* mpv_ = nullptr;
	mpv_render_context* renderContext_ = nullptr;
	bool initialized_ = false;

	// The frame mpv draws into, exposed as a texture for the compositor.
	TextureId frameTexture_ = kInvalidTexture;
	std::uintptr_t frameTextureName_ = 0;
	unsigned frameFbo_ = 0;
	int frameWidth_ = 0;
	int frameHeight_ = 0;

	int videoWidth_ = 0;
	int videoHeight_ = 0;

	bool paused_ = true;
	bool subtitlesEnabled_ = true;
	/// Whether the current clip is a still. A held image is not seekable and is
	/// never "playing", whatever mpv's transport says.
	bool currentIsImage_ = false;
	std::string decoderName_;
	std::string currentPath_;

	/// Scripts requested before initialize(). Indices whose `script` option mpv
	/// rejected are not added to loadedScripts_.
	std::vector<scripts::ScriptFile> requestedScripts_;
	std::vector<std::string> loadedScripts_;

	/// Set from mpv's update callback, which runs on an arbitrary thread.
	std::atomic<bool> framePending_{false};

	static void onRenderUpdate(void* context);
};

} // namespace media
