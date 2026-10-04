#pragma once

/// The GLEW initialisation sequence, in the one order that works.
///
/// This header includes GL because it has to call into it; it lives in libs/gfx
/// beside the only other GL-aware translation units, and nothing above it uses
/// it. Every application calls glewInit() through here so the drain below cannot
/// be forgotten in one of them.
///
/// Two facts are worth recording, because both cost real time when they bite:
///
///   1. glewInit() must run AFTER glfwMakeContextCurrent and AFTER
///      glfwSwapInterval, or it resolves nothing and every call is a null
///      pointer.
///
///   2. On a CORE profile context GLEW 2.x reports a spurious GL_INVALID_ENUM.
///      It sets glewExperimental internally on the core-profile path, the
///      driver rejects one of the enum queries it then makes, and the error is
///      left sitting in the queue. Nothing reads it today, so it is invisible -
///      until something does. Dear ImGui's own backends, and any future
///      glGetError() check in this project, would see a failure that happened
///      before a single one of our calls. Draining it here makes the error
///      queue start empty for the whole application.

#define GLFW_INCLUDE_NONE
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <cstdio>

namespace media::gfx {

/// Call after glfwMakeContextCurrent + glfwSwapInterval, before creating any GL
/// object. Returns true when GLEW initialised.
inline bool initializeGlLoader(GLFWwindow* window) {
	(void)window;
	const GLenum status = glewInit();
	if (status != GLEW_OK) {
		std::fprintf(stderr, "glewInit failed: %s\n", glewGetErrorString(status));
		return false;
	}

	// Drain whatever the initialisation itself left behind, and say so when it
	// is more than the one documented core-profile enum error: a different
	// error here means the driver disagrees with GLEW about something real.
	int drained = 0;
	GLenum error = GL_NO_ERROR;
	while ((error = glGetError()) != GL_NO_ERROR) {
		++drained;
		if (drained == 1 && error != GL_INVALID_ENUM) {
			std::fprintf(stderr,
				"GL error 0x%04X raised during glewInit; continuing\n",
				static_cast<unsigned>(error));
		}
	}
	return true;
}

} // namespace media::gfx
