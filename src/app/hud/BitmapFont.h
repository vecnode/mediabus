#pragma once

#include "app/render/RenderDevice.h"

#include <cstdint>
#include <string>

namespace media {

/// A compact 5x7 bitmap font used by the on-screen HUD.
///
/// Deliberately not a font engine: the project has no widget toolkit and no
/// text-shaping requirement for on-screen diagnostics, so a small uppercase
/// ASCII atlas drawn as quads through RenderDevice is enough. Subtitles are a
/// separate path (mpv/libass renders them into the video frame), so nothing
/// here has to handle CJK, shaping or hinting.
///
/// Glyph coverage: space, A-Z, 0-9 and common punctuation. Lowercase input is
/// folded to uppercase.
namespace hud {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;

/// One column per byte, bit `y` is row `y` (LSB = top row).
/// Returns nullptr for characters outside the atlas.
const std::uint8_t* glyphFor(char c);

/// Whether `c` can be rendered.
bool hasGlyph(char c);

/// Width in pixels of `text` rendered at `scale` (monospace, 1px inter-glyph
/// gap).
float textWidth(const std::string& text, float scale);

} // namespace hud
} // namespace media
