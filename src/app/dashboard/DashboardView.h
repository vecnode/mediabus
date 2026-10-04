#pragma once

#include "app/dashboard/DashboardModel.h"

namespace media {

class RenderDevice;

/// Draws the Dashboard.
///
/// Everything goes through RenderDevice — solid/outline rectangles and the
/// shared 5x7 bitmap font — exactly like the Player's HUD and the Controller
/// bar. Nothing here names a graphics API.
class DashboardView {
public:
	void draw(RenderDevice& device, const DashboardModel& model) const;

private:
	void drawRow(RenderDevice& device, const DashboardRow& row) const;
	void drawButton(RenderDevice& device, const Rect& rect, const std::string& label,
		bool primary, bool enabled) const;
};

} // namespace media
