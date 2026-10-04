#pragma once

#include "app/dashboard/DashboardModel.h"

namespace media {

class RenderDevice;

/// Draws the Dashboard.
///
/// Everything goes through RenderDevice — solid/outline rectangles and the
/// shared 5x7 bitmap font — exactly like the Player's HUD and the Controller
/// bar. Nothing here names a graphics API.
///
/// `uiScale` is passed in rather than read from the model: the scale is a
/// property of the monitor the window is on, which the frame loop owns, while
/// the model is pure layout arithmetic the tests can drive without a window.
class DashboardView {
public:
	void draw(RenderDevice& device, const DashboardModel& model, float uiScale) const;

private:
	void drawRow(RenderDevice& device, const DashboardRow& row,
		float bodyScale, float buttonScale) const;
	void drawCorpus(RenderDevice& device, const DashboardCorpus& corpus,
		float bodyScale, float buttonScale) const;
	void drawButton(RenderDevice& device, const Rect& rect, const std::string& label,
		bool primary, bool enabled, float textScale) const;
};

} // namespace media
