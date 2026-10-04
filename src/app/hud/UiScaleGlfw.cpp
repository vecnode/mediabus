#include "app/hud/UiScaleGlfw.h"

#include "core/UiScale.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>

namespace media::ui {

float rawContentScale() {
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor == nullptr) {
		return 0.0f;   // "GLFW had nothing to say"; fromContentScale handles it
	}
	float scaleX = 0.0f;
	float scaleY = 0.0f;
	glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
	return scaleX > scaleY ? scaleX : scaleY;
}

float scaleForWindow(const config::Config& config) {
	const float content = rawContentScale();
	float scale = fromContentScale(content, content);
	if (config.uiScale > 0.0f) {
		// An explicit setting wins outright, in both directions: it can shrink
		// text back down on a display whose DPI is reported wrongly.
		scale = sanitizeScale(config.uiScale);
	}
	return scale;
}

namespace {

std::string describeDecisionImpl(float uiScale, float contentScale) {
	char buffer[96];
	std::snprintf(buffer, sizeof(buffer), "%s (monitor %.2f)",
		describe(uiScale).c_str(), contentScale);
	return buffer;
}

} // namespace

std::string describeDecision(float uiScale) {
	return describeDecisionImpl(uiScale, rawContentScale());
}

std::string describeDecision(float uiScale, float contentScale) {
	return describeDecisionImpl(uiScale, contentScale);
}

} // namespace media::ui
