#pragma once

#include "control/ControllerModel.h"

#include <string>

namespace media {

class RenderDevice;

/// Draws the Controller bar.
///
/// Everything goes through RenderDevice: solid and outline rectangles plus the
/// shared 5x7 bitmap font. This file must never name OpenGL, exactly like the
/// Player's HUD — which is why the bar renders identically on any backend the
/// RenderDevice rule eventually grows.
///
/// There is no widget toolkit here: the buttons are rectangles hit-tested by
/// ControllerModel, drawn by this class.
class ControllerView {
public:
	/// The whole bar: background, status chip, title, media corpus field,
	/// transport row, seek bar, readouts and the message strip. The scene
	/// clears the window first, so this draws only the bar's own furniture.
	void draw(RenderDevice& device, const ControllerModel& model) const;

private:
	/// Label drawn inside a button, centred by the font metric. The caller
	/// resolves any state-dependent label (play vs pause) before calling here,
	/// and passes the text scale it resolved from the model's DPI scale.
	void drawButton(RenderDevice& device, const ControlButton& button,
		float textScale) const;
};

} // namespace media
