#include "gfx/BitmapFont.h"

#include <algorithm>

namespace media {
namespace hud {
namespace {

constexpr char kFirstGlyph = 32;   // ' '
constexpr char kLastGlyph = 95;    // '_'

#include "gfx/BitmapFontData.inc"

const std::uint8_t* glyphForIndex(int index) {
	if (index < 0 || index >= static_cast<int>(sizeof(kGlyphs) / sizeof(kGlyphs[0]))) {
		return nullptr;
	}
	return kGlyphs[index];
}

} // namespace

const std::uint8_t* glyphFor(char c) {
	// Fold to uppercase: the atlas is uppercase-only by design.
	if (c >= 'a' && c <= 'z') {
		c = static_cast<char>(c - 'a' + 'A');
	}
	if (c < kFirstGlyph || c > kLastGlyph) {
		return nullptr;
	}
	return glyphForIndex(c - kFirstGlyph);
}

bool hasGlyph(char c) {
	if (c == '\n') {
		return true;
	}
	return glyphFor(c) != nullptr;
}

float textWidth(const std::string& text, float scale) {
	float maxWidth = 0.0f;
	float lineWidth = 0.0f;
	const float advance = (kGlyphWidth + 1) * scale;
	for (char c : text) {
		if (c == '\n') {
			maxWidth = std::max(maxWidth, lineWidth);
			lineWidth = 0.0f;
			continue;
		}
		lineWidth += advance;
	}
	maxWidth = std::max(maxWidth, lineWidth);
	// The trailing inter-glyph gap is not part of the drawn width.
	return maxWidth > 0.0f ? maxWidth - scale : 0.0f;
}

} // namespace hud
} // namespace media
