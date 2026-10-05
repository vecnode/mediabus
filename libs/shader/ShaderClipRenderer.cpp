#include "shader/ShaderClipRenderer.h"

#include "core/Log.h"
#include "core/Platform.h"

// All OpenGL knowledge for shader clips lives in this translation unit. The
// header deliberately names none of it - see the note there. GLEW is already
// initialised by the application (libs/gfx/GlLoader.h) before anything here runs.
#define GLFW_INCLUDE_NONE
#include <GL/glew.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace media {
namespace {

/// The geometry every shader clip is drawn on: one triangle that covers the
/// viewport.
///
/// A single oversized triangle rather than a quad of two triangles: it has no
/// diagonal seam for an interpolation artefact to show up along, it is one draw
/// call of three vertices, and there is no vertex buffer at all - the positions
/// come from gl_VertexID. GL 3.3 core still requires a bound vertex array object
/// even when no attribute is read, which is the only thing initialize() creates.
///
/// v_uv comes out 0..1 across the frame with (0,0) at the TOP LEFT, which is the
/// contract assets/shaders/README.md documents. NDC has its origin at the
/// bottom-left, so the y flip in gl_Position is what makes the two agree.
const char* kFullScreenVertexShader = R"(#version 330 core
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
)";

/// A media corpus can hold any .frag, and a wrong file should be refused with a
/// sentence rather than by handing 4 MB to the GLSL compiler.
constexpr std::size_t kMaxShaderBytes = 256u * 1024u;

GLuint compileStage(GLenum type, const std::string& source, std::string& error) {
	const GLuint shader = glCreateShader(type);
	const char* text = source.c_str();
	glShaderSource(shader, 1, &text, nullptr);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE) {
		char log[2048] = {0};
		glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
		error = log;
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

} // namespace

ShaderClipRenderer::ShaderClipRenderer() = default;

ShaderClipRenderer::~ShaderClipRenderer() {
	// shutdown() is the caller's job, because it must happen while the context is
	// current. Nothing here can do that safely, so the destructor only records the
	// mistake rather than calling GL with no context.
	if (ready_ || program_ != 0) {
		std::fprintf(stderr, "ShaderClipRenderer: destroyed without shutdown(); "
			"GL objects leaked\n");
	}
}

bool ShaderClipRenderer::looksLikeShaderSource(const std::string& path) {
	const std::string ext = platform::lowerExtension(path);
	return ext == ".frag" || ext == ".glsl";
}

bool ShaderClipRenderer::initialize() {
	if (ready_) {
		return true;
	}

	glGenVertexArrays(1, &vertexArray_);
	if (vertexArray_ == 0) {
		LOG_ERROR("ShaderClip") << "could not create the vertex array; shader clips "
			"will not draw";
		return false;
	}

	ready_ = true;
	LOG_NOTICE("ShaderClip") << "shader clip renderer ready (GL "
		<< (const char*)glGetString(GL_VERSION) << ")";
	return true;
}

void ShaderClipRenderer::shutdown() {
	if (program_ != 0) {
		glDeleteProgram(program_);
		program_ = 0;
	}
	if (vertexArray_ != 0) {
		glDeleteVertexArrays(1, &vertexArray_);
		vertexArray_ = 0;
	}
	timeUniform_ = -1;
	resolutionUniform_ = -1;
	loadedPath_.clear();
	failedPath_.clear();
	ready_ = false;
}

bool ShaderClipRenderer::load(const std::string& path, std::string& error) {
	error.clear();

	if (path == loadedPath_ && program_ != 0) {
		// Already current. This is the common case in a frame loop.
		return true;
	}
	if (path == failedPath_) {
		// Already reported. Saying it again sixty times a second would bury
		// everything else in the log, and the screen already shows the reason.
		error = lastError_;
		return false;
	}
	if (!ready_) {
		error = "the shader renderer is not initialized";
		return false;
	}
	if (!looksLikeShaderSource(path)) {
		error = "not a shader source file: " + path;
		failedPath_ = path;
		lastError_ = error;
		return false;
	}

	std::ifstream file(path, std::ios::binary);
	if (!file) {
		error = "cannot read " + path;
		failedPath_ = path;
		lastError_ = error;
		return false;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	const std::string source = buffer.str();

	if (source.empty()) {
		error = "shader is empty: " + path;
		failedPath_ = path;
		lastError_ = error;
		return false;
	}
	if (source.size() > kMaxShaderBytes) {
		error = "shader is implausibly large (" + std::to_string(source.size())
			+ " bytes): " + path;
		failedPath_ = path;
		lastError_ = error;
		return false;
	}

	const GLuint vs = compileStage(GL_VERTEX_SHADER, kFullScreenVertexShader, error);
	if (vs == 0) {
		// A failure in the built-in vertex shader is a bug in this file, not in
		// the operator's, so it is reported differently from a bad fragment.
		error = "internal: the full-screen vertex shader did not compile: " + error;
		failedPath_ = path;
		lastError_ = error;
		LOG_ERROR("ShaderClip") << error;
		return false;
	}

	std::string fragmentError;
	const GLuint fs = compileStage(GL_FRAGMENT_SHADER, source, fragmentError);
	if (fs == 0) {
		glDeleteShader(vs);
		error = "does not compile: " + fragmentError;
		if (source.find("#version") == std::string::npos) {
			// The overwhelmingly common cause, and one the compiler's own message
			// does not explain: without a #version line GLSL assumes 1.10, where
			// `in`/`out` and `texture()` do not exist.
			error += "  (this file has no #version line; every shader here must "
				"start with: #version 330 core)";
		}
		failedPath_ = path;
		lastError_ = error;
		LOG_ERROR("ShaderClip") << path << ": " << error;
		return false;
	}

	const GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE) {
		char log[2048] = {0};
		glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
		glDeleteProgram(program);
		error = std::string("does not link: ") + log;
		failedPath_ = path;
		lastError_ = error;
		LOG_ERROR("ShaderClip") << path << ": " << error;
		return false;
	}

	// The new program is good, so it replaces the old one only now. A failed
	// edit therefore leaves the working shader on screen instead of a black
	// frame, which is the difference between a tool and a trap.
	if (program_ != 0) {
		glDeleteProgram(program_);
	}
	program_ = program;
	timeUniform_ = glGetUniformLocation(program_, "u_time");
	resolutionUniform_ = glGetUniformLocation(program_, "u_resolution");
	loadedPath_ = path;
	failedPath_.clear();
	lastError_.clear();

	// Reported, not warned about: a shader is free to ignore u_time (a still) or
	// u_resolution (a pattern that does not care how big the window is), and the
	// GLSL compiler removes a uniform nothing reads - so a -1 here is normal
	// rather than a defect. It is worth a verbose line because "my shader reacts
	// to nothing" is otherwise a mystery.
	LOG_VERBOSE("ShaderClip") << "compiled " << path
		<< (timeUniform_ < 0 ? "  (does not use u_time: a still)" : "")
		<< (resolutionUniform_ < 0 ? "  (does not use u_resolution)" : "");
	return true;
}

void ShaderClipRenderer::draw(int width, int height, double seconds) {
	if (!ready_ || program_ == 0 || width <= 0 || height <= 0) {
		return;
	}

	// No blending, no depth test: this fills the frame rather than compositing
	// onto it. RenderDevice sets what it needs before each of its own draws, so
	// nothing here has to be restored afterwards - except the program and the
	// vertex array, which are unbound below for the same reason.
	glUseProgram(program_);
	if (timeUniform_ >= 0) {
		glUniform1f(timeUniform_, static_cast<float>(seconds));
	}
	if (resolutionUniform_ >= 0) {
		glUniform2f(resolutionUniform_, static_cast<float>(width),
			static_cast<float>(height));
	}

	glBindVertexArray(vertexArray_);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glUseProgram(0);
}

} // namespace media
