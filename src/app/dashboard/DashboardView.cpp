#include "app/dashboard/DashboardView.h"

#include "app/hud/BitmapFont.h"
#include "app/render/RenderDevice.h"
#include "core/UiScale.h"

#include <algorithm>
#include <cstdio>
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

/// Text roles, in pixels of drawn glyph height before the DPI scale is
/// applied. The floors live in core/UiScale.h so all three windows agree.
constexpr float kTitleRolePixels = 24.0f;
constexpr float kBodyRolePixels = 15.0f;
constexpr float kSmallRolePixels = 14.0f;

void fill(RenderDevice& device, const Rect& r,
	std::uint8_t x, std::uint8_t y, std::uint8_t z, std::uint8_t a) {
	if (!r.empty()) {
		device.drawSolid(r, x, y, z, a);
	}
}

/// Clip a single line to `width` pixels, with an ellipsis when cut. `scale`
/// matters: the character pitch grows with the font.
std::string clip(const std::string& text, float width, float scale) {
	const float advance = (hud::kGlyphWidth + 1.0f) * std::max(0.01f, scale);
	const std::size_t perChar = std::max<std::size_t>(
		1u, static_cast<std::size_t>(advance));
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

/// The tail of a path, for a card that is one line tall: the last component
/// plus its parent when that identifies it better than the leaf alone.
std::string shortFolder(const std::string& folder) {
	if (folder.empty()) {
		return "(the app's own data folder)";
	}
	std::string text = folder;
	while (!text.empty() && (text.back() == '\\' || text.back() == '/')) {
		text.pop_back();
	}
	const std::size_t last = text.find_last_of("\\/");
	if (last == std::string::npos) {
		return text;
	}
	const std::size_t previous = (last == 0)
		? std::string::npos : text.find_last_of("\\/", last - 1);
	if (previous == std::string::npos) {
		return text;
	}
	const std::string tail = text.substr(previous + 1);
	return tail.size() <= 2 ? text : tail;
}

} // namespace

void DashboardView::drawButton(RenderDevice& device, const Rect& rect,
	const std::string& label, bool primary, bool enabled, float textScale) const {
	const std::uint8_t alpha = enabled ? 0xFF : 0x60;
	fill(device, rect,
		primary && enabled ? kAccentR : kPanelR,
		primary && enabled ? kAccentG : kPanelG,
		primary && enabled ? kAccentB : kPanelB, alpha);
	device.drawOutline(rect, 1.0f, kEdgeR, kEdgeG, kEdgeB, alpha);

	const float width = hud::textWidth(label, textScale);
	const float x = rect.x + std::max(0.0f, (rect.w - width) * 0.5f);
	const float y = rect.y + std::max(0.0f, (rect.h - hud::kGlyphHeight * textScale) * 0.5f);
	const std::uint8_t textR = enabled ? kTextR : kDimR;
	const std::uint8_t textG = enabled ? kTextG : kDimG;
	const std::uint8_t textB = enabled ? kTextB : kDimB;
	device.drawText(label, x, y, textScale, textR, textG, textB);
}

void DashboardView::drawRow(RenderDevice& device, const DashboardRow& row,
	float bodyScale, float buttonScale) const {
	fill(device, row.card, kCardR, kCardG, kCardB, 0xFF);
	device.drawOutline(row.card, 1.0f, kEdgeR, kEdgeG, kEdgeB, 0xFF);

	// Status dot: the quickest read of "is it up?" without parsing text.
	const float dotSize = 10.0f * bodyScale;
	const float padX = 12.0f * bodyScale;
	const Rect dot{row.card.x + padX, row.card.centreY() - dotSize * 0.5f,
		dotSize, dotSize};
	if (!row.available) {
		fill(device, dot, kWarnR, kWarnG, kWarnB, 0xFF);
	} else if (row.running) {
		fill(device, dot, kOkR, kOkG, kOkB, 0xFF);
	} else {
		fill(device, dot, kEdgeR, kEdgeG, kEdgeB, 0xFF);
	}

	const float textX = dot.x + dotSize + padX;
	// Leave the buttons their space: the text column ends where stop begins.
	const float textWidth = std::max(0.0f, row.stopButton.x - textX - padX * 0.6f);
	const float lineHeight = bodyScale * 20.0f;
	float textY = row.card.y + std::max(4.0f, (row.card.h - lineHeight * 3.0f) * 0.5f);

	device.drawText(clip(row.title, textWidth, bodyScale),
		textX, textY, bodyScale, kTextR, kTextG, kTextB);
	textY += lineHeight;
	device.drawText(clip(row.subtitle, textWidth, bodyScale),
		textX, textY, bodyScale,
		row.running ? kOkR : kDimR, row.running ? kOkG : kDimG, row.running ? kOkB : kDimB);
	textY += lineHeight;
	device.drawText(clip(row.path, textWidth, bodyScale),
		textX, textY, bodyScale, kDimR, kDimG, kDimB);

	// Actions. Launch is primary and inert while the app is up; stop is only
	// offered for a child this Dashboard started, so it is dimmed otherwise.
	drawButton(device, row.launchButton, row.running ? "RUNNING" : "LAUNCH",
		!row.running, row.available && !row.running, buttonScale);
	drawButton(device, row.stopButton, "STOP", false, row.running && row.managed,
		buttonScale);
}

void DashboardView::drawCorpus(RenderDevice& device, const DashboardCorpus& corpus,
	float bodyScale, float buttonScale) const {
	fill(device, corpus.card, kPanelR, kPanelG, kPanelB, 0xFF);
	device.drawOutline(corpus.card, 1.0f,
		corpus.folder.empty() ? kAccentR : kEdgeR,
		corpus.folder.empty() ? kAccentG : kEdgeG,
		corpus.folder.empty() ? kAccentB : kEdgeB,
		corpus.folder.empty() ? 0xC0 : 0xFF);

	const float padX = 14.0f * bodyScale;
	const float textX = corpus.card.x + padX;
	const float textWidth = std::max(0.0f,
		corpus.chooseButton.x - textX - padX * 0.6f);
	const float lineHeight = bodyScale * 20.0f;
	float textY = corpus.card.y + std::max(4.0f,
		(corpus.card.h - lineHeight * 2.0f) * 0.5f);

	device.drawText(clip("MEDIA FOLDER", textWidth, bodyScale),
		textX, textY, bodyScale, kDimR, kDimG, kDimB);
	textY += lineHeight;

	// One line that says both what the setting is and whether the Player agrees
	// with it. "0 videos" is a normal reading, not an error: an unset folder
	// simply has nothing in it, and the whole point is that this must not look
	// like a failure.
	char summary[512];
	const std::string shown = shortFolder(corpus.folder);
	if (corpus.playerOnline) {
		std::snprintf(summary, sizeof(summary), "%s  -  %zu video%s",
			shown.c_str(), corpus.clipCount, corpus.clipCount == 1 ? "" : "s");
	} else {
		std::snprintf(summary, sizeof(summary), "%s  -  player not running", shown.c_str());
	}
	device.drawText(clip(summary, textWidth, bodyScale), textX, textY, bodyScale,
		corpus.folder.empty() ? kAccentR : kTextR,
		corpus.folder.empty() ? kAccentG : kTextG,
		corpus.folder.empty() ? kAccentB : kTextB);

	drawButton(device, corpus.chooseButton, "CHANGE...", true, true, buttonScale);
}

void DashboardView::draw(RenderDevice& device, const DashboardModel& model,
	float uiScale) const {
	const float titleScale = ui::textScale(uiScale, kTitleRolePixels);
	const float bodyScale = ui::textScale(uiScale, kBodyRolePixels);
	const float smallScale = ui::textScale(uiScale, kSmallRolePixels);
	const float buttonScale = ui::textScale(uiScale, kBodyRolePixels);

	device.drawText("MEDIA PLAYER - APP LAUNCHER", model.titleArea().x,
		model.titleArea().y, titleScale, kTextR, kTextG, kTextB);

	for (const DashboardRow& row : model.rows()) {
		drawRow(device, row, bodyScale, buttonScale);
	}

	drawCorpus(device, model.corpus(), bodyScale, buttonScale);

	// Feedback line: the last launch/stop outcome, or what is missing.
	const std::string& message = model.message();
	if (!message.empty()) {
		const bool bad = message.find("not found") != std::string::npos
			|| message.find("failed") != std::string::npos;
		device.drawText(clip(message, model.messageArea().w, smallScale),
			model.messageArea().x, model.messageArea().y, smallScale,
			bad ? kBadR : kDimR, bad ? kBadG : kDimG, bad ? kBadB : kDimB);
	} else if (model.availableCount() == 0) {
		device.drawText("no applications found beside this executable",
			model.messageArea().x, model.messageArea().y, smallScale,
			kBadR, kBadG, kBadB);
	}
}

} // namespace media
