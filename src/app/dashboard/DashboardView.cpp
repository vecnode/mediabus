#include "app/dashboard/DashboardView.h"

#include "app/hud/BitmapFont.h"
#include "app/render/RenderDevice.h"

#include <algorithm>
#include <string>

namespace media {
namespace {

constexpr std::uint8_t kPanelR = 0x1D, kPanelG = 0x22, kPanelB = 0x2B;
constexpr std::uint8_t kCardR = 0x18, kCardG = 0x1C, kCardB = 0x24;
constexpr std::uint8_t kEdgeR = 0x2C, kEdgeG = 0x35, kEdgeB = 0x42;
constexpr std::uint8_t kTextR = 0xE6, kTextG = 0xEA, kTextB = 0xF2;
constexpr std::uint8_t kDimR = 0x86, kDimG = 0x96, kDimB = 0xA8;
constexpr std::uint8_t kAccentR = 0x2E, kAccentG = 0x9E, kAccentB = 0xFF;
constexpr std::uint8_t kOkR = 0x3D, kOkG = 0xC8, kOkB = 0x7A;
constexpr std::uint8_t kWarnR = 0xE0, kWarnG = 0xB0, kWarnB = 0x54;
constexpr std::uint8_t kBadR = 0xE0, kBadG = 0x5A, kBadB = 0x54;

constexpr float kTitleScale = 2.0f;
constexpr float kBodyScale = 1.0f;

void fill(RenderDevice& device, const Rect& r,
	std::uint8_t x, std::uint8_t y, std::uint8_t z, std::uint8_t a) {
	if (!r.empty()) {
		device.drawSolid(r, x, y, z, a);
	}
}

/// Clip a single line to `width` pixels, with an ellipsis when cut.
std::string clip(const std::string& text, float width, float scale) {
	const std::size_t perChar = static_cast<std::size_t>(
		std::max(1.0f, (hud::kGlyphWidth + 1.0f) * scale));
	const std::size_t maxChars = static_cast<std::size_t>(
		std::max(0.0f, width)) / perChar;
	if (maxChars == 0 || text.size() <= maxChars) {
		return text;
	}
	if (maxChars <= 3) {
		return text.substr(0, maxChars);
	}
	return text.substr(0, maxChars - 3) + "...";
}

} // namespace

void DashboardView::drawButton(RenderDevice& device, const Rect& rect,
	const std::string& label, bool primary, bool enabled) const {
	const std::uint8_t alpha = enabled ? 0xFF : 0x60;
	fill(device, rect,
		primary && enabled ? kAccentR : kPanelR,
		primary && enabled ? kAccentG : kPanelG,
		primary && enabled ? kAccentB : kPanelB, alpha);
	device.drawOutline(rect, 1.0f, kEdgeR, kEdgeG, kEdgeB, alpha);

	const float width = hud::textWidth(label, kBodyScale);
	const float x = rect.x + std::max(0.0f, (rect.w - width) * 0.5f);
	const float y = rect.y + std::max(0.0f, (rect.h - hud::kGlyphHeight * kBodyScale) * 0.5f);
	const std::uint8_t textR = enabled ? kTextR : kDimR;
	const std::uint8_t textG = enabled ? kTextG : kDimG;
	const std::uint8_t textB = enabled ? kTextB : kDimB;
	device.drawText(label, x, y, kBodyScale, textR, textG, textB);
}

void DashboardView::drawRow(RenderDevice& device, const DashboardRow& row) const {
	fill(device, row.card, kCardR, kCardG, kCardB, 0xFF);
	device.drawOutline(row.card, 1.0f, kEdgeR, kEdgeG, kEdgeB, 0xFF);

	// Status dot: the quickest read of "is it up?" without parsing text.
	const float dotSize = 10.0f;
	const Rect dot{row.card.x + 12.0f, row.card.centreY() - dotSize * 0.5f,
		dotSize, dotSize};
	if (!row.available) {
		fill(device, dot, kWarnR, kWarnG, kWarnB, 0xFF);
	} else if (row.running) {
		fill(device, dot, kOkR, kOkG, kOkB, 0xFF);
	} else {
		fill(device, dot, kEdgeR, kEdgeG, kEdgeB, 0xFF);
	}

	const float textX = dot.x + dotSize + 12.0f;
	// Leave the buttons their space: the text column ends where stop begins.
	const float textWidth = std::max(0.0f, row.stopButton.x - textX - 10.0f);

	device.drawText(clip(row.title, textWidth, kBodyScale),
		textX, row.card.y + 14.0f, kBodyScale, kTextR, kTextG, kTextB);
	device.drawText(clip(row.subtitle, textWidth, kBodyScale),
		textX, row.card.y + 32.0f, kBodyScale,
		row.running ? kOkR : kDimR, row.running ? kOkG : kDimG, row.running ? kOkB : kDimB);
	device.drawText(clip(row.path, textWidth, kBodyScale),
		textX, row.card.y + 50.0f, kBodyScale, kDimR, kDimG, kDimB);

	// Actions. Launch is primary and inert while the app is up; stop is only
	// offered for a child this Dashboard started, so it is dimmed otherwise.
	drawButton(device, row.launchButton, row.running ? "RUNNING" : "LAUNCH",
		!row.running, row.available && !row.running);
	drawButton(device, row.stopButton, "STOP", false, row.running && row.managed);
}

void DashboardView::draw(RenderDevice& device, const DashboardModel& model) const {
	device.drawText("MEDIA PLAYER - APP LAUNCHER", model.titleArea().x,
		model.titleArea().y, kTitleScale, kTextR, kTextG, kTextB);

	for (const DashboardRow& row : model.rows()) {
		drawRow(device, row);
	}

	// Feedback line: the last launch/stop outcome, or what is missing.
	const std::string& message = model.message();
	if (!message.empty()) {
		const bool bad = message.find("not found") != std::string::npos
			|| message.find("failed") != std::string::npos;
		device.drawText(clip(message, model.messageArea().w, kBodyScale),
			model.messageArea().x, model.messageArea().y, kBodyScale,
			bad ? kBadR : kDimR, bad ? kBadG : kDimG, bad ? kBadB : kDimB);
	} else if (model.availableCount() == 0) {
		device.drawText("no applications found beside this executable",
			model.messageArea().x, model.messageArea().y, kBodyScale,
			kBadR, kBadG, kBadB);
	}
}

} // namespace media
