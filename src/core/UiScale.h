#pragma once

#include <string>

/// How the internal 5x7 bitmap font is sized to pixels.
///
/// The glyphs are a fixed 5x7 atlas drawn as quads, so the pixel size of a line
/// of text is glyphHeight * scale. On a 1080p display a scale of 1 (a 7-pixel
/// capital) is a legible HUD annotation; on a 4K display at 150-200% Windows
/// scaling it is unreadable.
///
/// The rule every window follows:
///
///     pixel height = glyphs * uiScale, but never below a per-role floor
///
/// The floor is the part that matters: it guarantees readable text even when a
/// monitor reports a content scale of 1.0 (a 4K display at 100% scaling, or a
/// remote desktop session), which plain DPI multiplication would not fix.
///
/// This file is arithmetic only - no GLFW, no GL - so the layout code that uses
/// it stays unit-testable without a window. Each application asks GLFW for its
/// monitor's content scale and passes the number in.
namespace media::ui {

/// Glyph cell of the bitmap font, mirrored from hud::kGlyphHeight so this
/// header does not have to reach into the render layer. A static_assert in
/// UiScale.cpp keeps the two honest.
inline constexpr float kGlyphHeight = 7.0f;

/// Smallest pixel height each role of text is allowed to shrink to. These are
/// the "I should be able to read this" numbers, chosen against a 4K panel.
inline constexpr float kBodyPixels = 15.0f;    ///< status lines, list rows
inline constexpr float kButtonPixels = 22.0f;  ///< transport button labels
inline constexpr float kTitlePixels = 17.0f;   ///< window / section headings
inline constexpr float kSmallPixels = 14.0f;   ///< chips, footnotes, clocks

/// Clamp a scale factor into something a person can see. Below 1.0 nothing is
/// drawn at all in some call sites, and an absurd value would make the text
/// unusable; 4.0 is roughly a 28-pixel capital, which is already large.
float sanitizeScale(float scale);

/// The scale to use when nothing is known and no monitor reports a DPI: the
/// floor for body text. Never below 1.0.
float defaultScale();

/// The content scale to use, taking the larger axis and clamping it. Pass the
/// values from glfwGetMonitorContentScale; 0 or a negative value means "GLFW
/// had nothing to say", which yields defaultScale().
float fromContentScale(float scaleX, float scaleY);

/// Text scale for a role, given the window's DPI scale: DPI-scaled, but never
/// below `minPixels` of drawn glyph height.
float textScale(float uiScale, float minPixels);

/// The four roles, so a call site names what it is drawing rather than a
/// number. Each returns textScale(uiScale, <its floor>).
float bodyScale(float uiScale);
float buttonScale(float uiScale);
float titleScale(float uiScale);
float smallScale(float uiScale);

/// "1.50x" - for the one log line that records what was decided.
std::string describe(float uiScale);

} // namespace media::ui
