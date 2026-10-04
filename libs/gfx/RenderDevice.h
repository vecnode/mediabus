#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace media {

/// Axis-aligned rectangle in pixels, top-left origin.
struct Rect {
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;

	bool empty() const { return w <= 0.0f || h <= 0.0f; }

	/// True when (px, py) is inside the rectangle. Half-open, matching how a
	/// hit test wants to treat shared edges.
	bool hit(float px, float py) const {
		return px >= x && px < x + w && py >= y && py < y + h;
	}

	float centreX() const { return x + w * 0.5f; }
	float centreY() const { return y + h * 0.5f; }
};

/// A GPU texture the scene can draw.
using TextureId = std::uint32_t;
inline constexpr TextureId kInvalidTexture = 0;

/// Size of a texture in pixels.
struct TextureSize {
	int w = 0;
	int h = 0;
};

/// The only graphics abstraction in the project.
///
/// HARD RULE: nothing above src/app/render/ may name a graphics API. Scene code
/// draws through this interface, so replacing OpenGL with another backend is a
/// new implementation of this header rather than a rewrite of the app.
///
/// The interface is deliberately small: this is a 2D compositor that draws
/// textured quads (video frames, images, subtitle-free overlays) and outlines,
/// plus scissor clipping for layout.
class RenderDevice {
public:
	virtual ~RenderDevice() = default;

	/// Called once after the GL context exists and the loader is initialised.
	virtual bool initialize() = 0;

	/// Upload RGBA8 pixels, replacing any previous contents of `id`.
	/// `id` may be kInvalidTexture to allocate a new texture.
	virtual TextureId uploadRgba(TextureId id, int width, int height,
		const std::uint8_t* rgbaPixels) = 0;

	/// Allocate (or resize) an empty texture, e.g. a target for video frames.
	virtual TextureId createEmpty(TextureId id, int width, int height) = 0;

	virtual void destroyTexture(TextureId id) = 0;
	virtual TextureSize textureSize(TextureId id) const = 0;

	/// True when `id` refers to a texture this device knows about.
	virtual bool hasTexture(TextureId id) const = 0;

	/// Underlying API handle for a texture this device owns (a GLuint name in
	/// the OpenGL implementation), for a backend that must attach it to a
	/// framebuffer of its own. Returns 0 when unknown. The texture keeps
	/// belonging to the device and must be freed through destroyTexture().
	virtual std::uintptr_t nativeTextureHandle(TextureId id) const = 0;

	/// Draw `tex` filling `dest`, sampling the normalised source region
	/// `uvX, uvY, uvW, uvH` (0..1).
	virtual void drawQuad(TextureId tex, const Rect& dest,
		float uvX = 0.0f, float uvY = 0.0f,
		float uvW = 1.0f, float uvH = 1.0f) = 0;

	/// Draw a solid rectangle in the current colour.
	virtual void drawSolid(const Rect& dest, std::uint8_t r, std::uint8_t g,
		std::uint8_t b, std::uint8_t a) = 0;

	/// Draw an unfilled rectangle border of `thickness` pixels.
	virtual void drawOutline(const Rect& dest, float thickness,
		std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) = 0;

	/// Draw a single line of HUD text with the built-in 5x7 bitmap font,
	/// folding to uppercase. Returns the width drawn, so callers can measure.
	/// A dim shadow is drawn behind the glyphs so they stay legible over video.
	virtual float drawText(const std::string& text, float x, float y, float scale,
		std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;

	/// Clear the whole framebuffer.
	virtual void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;

	/// Clip subsequent draws to `scissor`. Nested calls intersect.
	virtual void pushScissor(const Rect& scissor) = 0;
	virtual void popScissor() = 0;

	/// Set the drawable size (window resize).
	virtual void setViewport(int width, int height) = 0;

	/// Bind the final framebuffer and reset state for a new frame.
	virtual void beginFrame() = 0;

	/// Flush and present.
	virtual void endFrame() = 0;

	/// One line of backend identity for /api/status diagnostics.
	virtual const char* backendName() const = 0;

	/// Opaque native GL context, for a backend that must create GL objects
	/// against the same context (libmpv's render API needs it). Returns
	/// whatever the platform context handle is — a GLFWwindow* in the OpenGL
	/// implementation — or nullptr. Callers must not interpret the value, only
	/// hand it to a library that understands it.
	virtual void* nativeContextHandle() const = 0;
};

/// Create the OpenGL 3.3 core implementation.
///
/// Returns nullptr if the context is not current or required GL features are
/// missing. The GL source lives only in GlRenderDevice.cpp.
std::unique_ptr<RenderDevice> createGlRenderDevice();

} // namespace media
