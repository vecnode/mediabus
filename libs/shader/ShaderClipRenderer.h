#pragma once

#include <string>

namespace media {

/// Draws a fragment-shader clip: a full-frame image generated on the GPU rather
/// than decoded from a file.
///
/// THE SECOND SANCTIONED GL EXCEPTION, for the same reason as MPVSurface. This
/// must own GL objects that RenderDevice has no vocabulary for. RenderDevice is a
/// 2D compositor - textured quads, rectangles, a bitmap font - and a fragment
/// shader is not a quad and has no texture. Growing the compositor to compile
/// GLSL would put shader compilation on the one interface whose entire purpose is
/// to keep the graphics API behind a seam.
///
/// The fence still holds: THIS header names no graphics API and includes no GL
/// header, so apps/player/main.cpp can drive it without naming a single GL
/// symbol. Everything else lives in the .cpp, exactly as it does in
/// GlRenderDevice.cpp.
///
/// One instance per GL context, used only on the thread that owns that context.
class ShaderClipRenderer {
public:
	ShaderClipRenderer();
	~ShaderClipRenderer();

	ShaderClipRenderer(const ShaderClipRenderer&) = delete;
	ShaderClipRenderer& operator=(const ShaderClipRenderer&) = delete;

	/// Create the full-screen geometry. The GL context must be current. Returns
	/// false and logs why on failure; the Player then shows the clip's name with
	/// nothing drawn rather than refusing to start.
	bool initialize();

	/// Release every GL object. Safe to call more than once, and must run while
	/// the GL context is still current.
	void shutdown();

	bool ready() const { return ready_; }

	/// Compile `path` and make it the current program. True when it is now the
	/// program that draws.
	///
	/// Calling this with the path already loaded is a no-op, so a frame loop can
	/// call it every frame and the compile happens once. A shader that does not
	/// compile leaves the previous program in place and reports the compiler's own
	/// message through `error`, so a broken file does not blank the screen - and
	/// the same broken path is NOT retried every frame, which would recompile
	/// sixty times a second and bury the log. Asking for a different path clears
	/// that memory, so fixing a shader and reopening the clip works.
	bool load(const std::string& path, std::string& error);

	/// Draw the current program to fill the whole framebuffer. Main thread, and
	/// only between RenderDevice::beginFrame() and endFrame().
	void draw(int width, int height, double seconds);

	/// Absolute path of the program currently loaded, empty when none.
	const std::string& loadedPath() const { return loadedPath_; }

	/// The compiler's message from the last failed load, empty when the last load
	/// succeeded.
	const std::string& lastError() const { return lastError_; }

private:
	/// True when `path` ends in something this renderer can compile.
	static bool looksLikeShaderSource(const std::string& path);

	unsigned program_ = 0;
	unsigned vertexArray_ = 0;

	int timeUniform_ = -1;
	int resolutionUniform_ = -1;

	std::string loadedPath_;
	/// A path that failed to compile. Remembered so the failure is reported once
	/// rather than once per frame.
	std::string failedPath_;
	std::string lastError_;
	bool ready_ = false;
};

} // namespace media
