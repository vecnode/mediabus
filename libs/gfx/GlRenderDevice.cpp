#include "gfx/RenderDevice.h"

#include "gfx/BitmapFont.h"

// All OpenGL knowledge in the project lives in this translation unit.
#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace media {
namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;   // 0..1 quad space
layout(location = 1) in vec2 aUv;
uniform vec4 uDest;                  // x, y, w, h in pixels
uniform vec2 uTarget;                // target size in pixels
out vec2 vUv;
void main() {
    vec2 p = uDest.xy + aPos * uDest.zw;
    vec2 ndc = vec2(p.x / uTarget.x, p.y / uTarget.y) * 2.0 - 1.0;
    // Pixels have a top-left origin; NDC has a bottom-left origin.
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUv = aUv;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform vec4 uUvRect;      // x, y, w, h normalised
uniform vec4 uColor;
uniform int uMode;         // 0 = textured, 1 = solid
out vec4 FragColor;
void main() {
    if (uMode == 1) {
        FragColor = uColor;
        return;
    }
    vec2 uv = uUvRect.xy + vUv * uUvRect.zw;
    FragColor = texture(uTex, uv) * uColor;
}
)";

GLuint compile(GLenum type, const char* src) {
	const GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[1024] = {0};
		glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
		std::fprintf(stderr, "RenderDevice: shader compile failed: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

class GlRenderDevice final : public RenderDevice {
public:
	bool initialize() override {
		const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
		const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
		if (vs == 0 || fs == 0) {
			return false;
		}

		program_ = glCreateProgram();
		glAttachShader(program_, vs);
		glAttachShader(program_, fs);
		glLinkProgram(program_);
		glDeleteShader(vs);
		glDeleteShader(fs);

		GLint linked = GL_FALSE;
		glGetProgramiv(program_, GL_LINK_STATUS, &linked);
		if (linked != GL_TRUE) {
			char log[1024] = {0};
			glGetProgramInfoLog(program_, sizeof(log) - 1, nullptr, log);
			std::fprintf(stderr, "RenderDevice: program link failed: %s\n", log);
			return false;
		}

		uDest_ = glGetUniformLocation(program_, "uDest");
		uTarget_ = glGetUniformLocation(program_, "uTarget");
		uUvRect_ = glGetUniformLocation(program_, "uUvRect");
		uColor_ = glGetUniformLocation(program_, "uColor");
		uMode_ = glGetUniformLocation(program_, "uMode");
		uTex_ = glGetUniformLocation(program_, "uTex");

		// Unit quad, two triangles, drawn as a triangle strip.
		const float verts[] = {
			// x, y, u, v
			0.0f, 0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 1.0f,
			1.0f, 1.0f, 1.0f, 1.0f,
		};
		glGenVertexArrays(1, &vao_);
		glGenBuffers(1, &vbo_);
		glBindVertexArray(vao_);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
			reinterpret_cast<void*>(2 * sizeof(float)));
		glBindVertexArray(0);

		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Nearest-neighbour keeps a 1:1 pixel draw crisp; the video path is
		// already scaled by mpv into the FBO.
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		return true;
	}

	TextureId uploadRgba(TextureId id, int width, int height,
		const std::uint8_t* rgbaPixels) override {
		if (width <= 0 || height <= 0 || rgbaPixels == nullptr) {
			return kInvalidTexture;
		}
		GLuint tex = id;
		const bool isNew = (tex == 0);
		if (isNew) {
			glGenTextures(1, &tex);
		}
		glBindTexture(GL_TEXTURE_2D, tex);
		if (isNew || textures_[tex].w != width || textures_[tex].h != height) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, rgbaPixels);
		} else {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
				GL_UNSIGNED_BYTE, rgbaPixels);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);

		textures_[tex] = {width, height};
		return tex;
	}

	TextureId createEmpty(TextureId id, int width, int height) override {
		if (width <= 0 || height <= 0) {
			return kInvalidTexture;
		}
		GLuint tex = id;
		if (tex == 0) {
			glGenTextures(1, &tex);
		}
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		textures_[tex] = {width, height};
		return tex;
	}

	void destroyTexture(TextureId id) override {
		if (id == kInvalidTexture) {
			return;
		}
		const GLuint tex = id;
		glDeleteTextures(1, &tex);
		textures_.erase(id);
	}

	TextureSize textureSize(TextureId id) const override {
		const auto it = textures_.find(id);
		return it == textures_.end() ? TextureSize{} : it->second;
	}

	bool hasTexture(TextureId id) const override {
		return id != kInvalidTexture && textures_.find(id) != textures_.end();
	}

	std::uintptr_t nativeTextureHandle(TextureId id) const override {
		return hasTexture(id) ? static_cast<std::uintptr_t>(id) : 0;
	}

	void drawQuad(TextureId tex, const Rect& dest, float uvX, float uvY,
		float uvW, float uvH) override {
		if (dest.empty() || tex == kInvalidTexture) {
			return;
		}
		glUseProgram(program_);
		glUniform2f(uTarget_, static_cast<float>(viewportW_),
			static_cast<float>(viewportH_));
		glUniform4f(uDest_, dest.x, dest.y, dest.w, dest.h);
		glUniform4f(uUvRect_, uvX, uvY, uvW, uvH);
		glUniform4f(uColor_, 1.0f, 1.0f, 1.0f, 1.0f);
		glUniform1i(uMode_, 0);
		glUniform1i(uTex_, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
	}

	void drawSolid(const Rect& dest, std::uint8_t r, std::uint8_t g,
		std::uint8_t b, std::uint8_t a) override {
		drawColored(dest, r, g, b, a, false);
	}

	void drawOutline(const Rect& dest, float thickness, std::uint8_t r,
		std::uint8_t g, std::uint8_t b, std::uint8_t a) override {
		if (dest.empty() || thickness <= 0.0f) {
			return;
		}
		const float t = std::min(thickness, std::min(dest.w, dest.h) * 0.5f);
		drawSolid({dest.x, dest.y, dest.w, t}, r, g, b, a);
		drawSolid({dest.x, dest.y + dest.h - t, dest.w, t}, r, g, b, a);
		drawSolid({dest.x, dest.y + t, t, dest.h - 2 * t}, r, g, b, a);
		drawSolid({dest.x + dest.w - t, dest.y + t, t, dest.h - 2 * t}, r, g, b, a);
	}

	void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
		glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	float drawText(const std::string& text, float x, float y, float scale,
		std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
		if (scale <= 0.0f) {
			return 0.0f;
		}
		const float advance = (hud::kGlyphWidth + 1) * scale;
		float penX = x;

		// Two passes: a dark shadow offset by one scaled pixel, then the glyphs.
		// Over video this is what keeps the HUD readable.
		for (int pass = 0; pass < 2; ++pass) {
			const bool shadow = (pass == 0);
			const float ox = shadow ? scale : 0.0f;
			const float oy = shadow ? scale : 0.0f;
			const std::uint8_t cr = shadow ? 0x00 : r;
			const std::uint8_t cg = shadow ? 0x00 : g;
			const std::uint8_t cb = shadow ? 0x00 : b;
			const std::uint8_t ca = shadow ? 0xB0 : 0xFF;

			float cx = penX;
			for (char ch : text) {
				if (ch == '\n') {
					continue;   // single-line HUD; callers split lines themselves
				}
				const std::uint8_t* columns = hud::glyphFor(ch);
				if (columns != nullptr) {
					for (int gx = 0; gx < hud::kGlyphWidth; ++gx) {
						const std::uint8_t column = columns[gx];
						if (column == 0) {
							continue;
						}
						for (int gy = 0; gy < hud::kGlyphHeight; ++gy) {
							if ((column & (1u << gy)) == 0) {
								continue;
							}
							drawColored({cx + static_cast<float>(gx) * scale + ox,
									y + static_cast<float>(gy) * scale + oy,
									scale, scale},
								cr, cg, cb, ca, false);
						}
					}
				}
				cx += advance;
			}
		}
		return hud::textWidth(text, scale);
	}

	void pushScissor(const Rect& scissor) override {
		// Intersect with the parent so nested clips compose correctly.
		Rect r = scissor;
		if (!scissorStack_.empty()) {
			const Rect& p = scissorStack_.back();
			const float x0 = std::max(p.x, r.x);
			const float y0 = std::max(p.y, r.y);
			const float x1 = std::min(p.x + p.w, r.x + r.w);
			const float y1 = std::min(p.y + p.h, r.y + r.h);
			r = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
		}
		scissorStack_.push_back(r);
		applyScissor();
	}

	void popScissor() override {
		if (!scissorStack_.empty()) {
			scissorStack_.pop_back();
		}
		applyScissor();
	}

	void setViewport(int width, int height) override {
		viewportW_ = width;
		viewportH_ = height;
		glViewport(0, 0, width, height);
	}

	void beginFrame() override {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDisable(GL_SCISSOR_TEST);
		scissorStack_.clear();
		glUseProgram(program_);
	}

	void endFrame() override {
		glUseProgram(0);
		glFlush();
	}

	const char* backendName() const override { return "OpenGL 3.3 core (GLEW)"; }

	void* nativeContextHandle() const override { return glfwGetCurrentContext(); }

private:
	void drawColored(const Rect& dest, std::uint8_t r, std::uint8_t g,
		std::uint8_t b, std::uint8_t a, bool) {
		if (dest.empty()) {
			return;
		}
		glUseProgram(program_);
		glUniform2f(uTarget_, static_cast<float>(viewportW_),
			static_cast<float>(viewportH_));
		glUniform4f(uDest_, dest.x, dest.y, dest.w, dest.h);
		glUniform4f(uUvRect_, 0.0f, 0.0f, 1.0f, 1.0f);
		glUniform4f(uColor_, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
		glUniform1i(uMode_, 1);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
	}

	void applyScissor() {
		if (scissorStack_.empty()) {
			glDisable(GL_SCISSOR_TEST);
			return;
		}
		const Rect& r = scissorStack_.back();
		glEnable(GL_SCISSOR_TEST);
		// GL scissor origin is bottom-left.
		const int y = viewportH_ - static_cast<int>(r.y + r.h);
		glScissor(static_cast<int>(r.x), y,
			static_cast<int>(r.w), static_cast<int>(r.h));
	}

	GLuint program_ = 0;
	GLuint vao_ = 0;
	GLuint vbo_ = 0;
	GLint uDest_ = -1, uTarget_ = -1, uUvRect_ = -1, uColor_ = -1, uMode_ = -1, uTex_ = -1;
	int viewportW_ = 0;
	int viewportH_ = 0;
	std::unordered_map<TextureId, TextureSize> textures_;
	std::vector<Rect> scissorStack_;
};

} // namespace

std::unique_ptr<RenderDevice> createGlRenderDevice() {
	return std::make_unique<GlRenderDevice>();
}

} // namespace media
