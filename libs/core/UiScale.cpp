#include "core/UiScale.h"

#include "gfx/BitmapFont.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace media::ui {

// The layout maths below assumes this equals hud::kGlyphHeight. If the font is
// ever resized, this fires instead of every window silently mis-measuring.
static_assert(kGlyphHeight == static_cast<float>(hud::kGlyphHeight),
	"UiScale::kGlyphHeight must mirror hud::kGlyphHeight");

namespace {

constexpr float kMinScale = 1.0f;
constexpr float kMaxScale = 4.0f;

} // namespace

float sanitizeScale(float scale) {
	if (!(scale > 0.0f) || !std::isfinite(scale)) {
		return defaultScale();
	}
	return std::clamp(scale, kMinScale, kMaxScale);
}

float defaultScale() {
	return textScale(1.0f, kBodyPixels);
}

float fromContentScale(float scaleX, float scaleY) {
	const float largest = std::max(scaleX, scaleY);
	if (!(largest > 0.0f) || !std::isfinite(largest)) {
		return defaultScale();
	}
	return sanitizeScale(largest);
}

float textScale(float uiScale, float minPixels) {
	const float scale = sanitizeScale(uiScale);
	const float floorScale = minPixels / kGlyphHeight;
	return std::max(scale, sanitizeScale(floorScale));
}

float bodyScale(float uiScale) { return textScale(uiScale, kBodyPixels); }
float buttonScale(float uiScale) { return textScale(uiScale, kButtonPixels); }
float titleScale(float uiScale) { return textScale(uiScale, kTitlePixels); }
float smallScale(float uiScale) { return textScale(uiScale, kSmallPixels); }

std::string describe(float uiScale) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.2fx", sanitizeScale(uiScale));
	return buffer;
}

} // namespace media::ui
