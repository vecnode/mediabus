#include "app/control/ControllerView.h"

#include "app/hud/BitmapFont.h"
#include "app/render/RenderDevice.h"
#include "core/UiScale.h"

#include <algorithm>
#include <cstdio>

namespace media {
namespace {

// Palette. Kept in one place so the bar reads as a single surface.
constexpr std::uint8_t kPanelR = 0x1D, kPanelG = 0x22, kPanelB = 0x2B;
constexpr std::uint8_t kEdgeR = 0x2C, kEdgeG = 0x35, kEdgeB = 0x42;
constexpr std::uint8_t kTextR = 0xE6, kTextG = 0xEA, kTextB = 0xF2;
constexpr std::uint8_t kDimR = 0x86, kDimG = 0x96, kDimB = 0xA8;
constexpr std::uint8_t kAccentR = 0x2E, kAccentG = 0x9E, kAccentB = 0xFF;
constexpr std::uint8_t kOkR = 0x3D, kOkG = 0xC8, kOkB = 0x7A;
constexpr std::uint8_t kBadR = 0xE0, kBadG = 0x5A, kBadB = 0x54;

void fill(RenderDevice& device, const Rect& r,
	std::uint8_t x, std::uint8_t y, std::uint8_t z, std::uint8_t a) {
	if (r.empty()) {
		return;
	}
	device.drawSolid(r, x, y, z, a);
}

/// Draw text centred horizontally and vertically inside `r`.
void centredText(RenderDevice& device, const std::string& text, const Rect& r,
	float scale, std::uint8_t x, std::uint8_t y, std::uint8_t z) {
	if (r.empty() || text.empty()) {
		return;
	}
	const float width = hud::textWidth(text, scale);
	const float textX = r.x + std::max(0.0f, (r.w - width) * 0.5f);
	const float textY = r.y + std::max(0.0f, (r.h - hud::kGlyphHeight * scale) * 0.5f);
	device.drawText(text, textX, textY, scale, x, y, z);
}

/// Clip a one-line message to `width` pixels, adding an ellipsis when cut.
/// `scale` matters: the character pitch grows with the font, so a width that
/// fits at scale 1 overflows at scale 2.
std::string clipToWidth(const std::string& text, float width, float scale) {
	const float advance = (hud::kGlyphWidth + 1.0f) * std::max(0.01f, scale);
	const std::size_t maxChars = static_cast<std::size_t>(
		std::max(0.0f, width) / advance);
	if (maxChars == 0 || text.size() <= maxChars) {
		return text;
	}
	if (maxChars <= 3) {
		return text.substr(0, maxChars);
	}
	return text.substr(0, maxChars - 3) + "...";
}

/// "M:SS" for a duration in seconds; "--:--" when there is no timeline.
std::string clockText(double seconds) {
	if (!(seconds > 0.0)) {
		return "--:--";
	}
	const int total = static_cast<int>(seconds + 0.5);
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
	return buffer;
}

} // namespace

void ControllerView::drawButton(RenderDevice& device, const ControlButton& button,
	float textScale) const {
	const bool dim = !button.enabled;

	fill(device, button.rect, kPanelR, kPanelG, kPanelB, dim ? 0x80 : 0xFF);
	device.drawOutline(button.rect, 1.0f, kEdgeR, kEdgeG, kEdgeB, dim ? 0x80 : 0xFF);

	const std::uint8_t textR = dim ? kDimR : kTextR;
	const std::uint8_t textG = dim ? kDimG : kTextG;
	const std::uint8_t textB = dim ? kDimB : kTextB;
	centredText(device, button.label, button.rect, textScale, textR, textG, textB);
}

void ControllerView::draw(RenderDevice& device, const ControllerModel& model) const {
	const ControllerLayout& layout = model.layout();
	const ControllerState& state = model.state();
	const bool seekActive = model.seekBarActive();

	// Every text size comes from the model's DPI scale through the shared
	// helper, so the three windows cannot disagree about how big "body text"
	// is. Scales are distinct per role: a transport label is bigger than the
	// status chip, which is bigger than a folder path.
	const float uiScale = model.uiScale();
	const float titleScale = ui::textScale(uiScale, 15.0f);
	const float smallScale = ui::textScale(uiScale, 14.0f);
	const float buttonScale = ui::textScale(uiScale, 22.0f);
	const float corpusScale = ui::textScale(uiScale, 15.0f);
	const float s = uiScale;

	// ---- status chip -----------------------------------------------------
	const Rect& chip = layout.statusChip;
	if (!chip.empty()) {
		const bool online = state.online;
		fill(device, chip, online ? kOkR : kBadR, online ? kOkG : kBadG,
			online ? kOkB : kBadB, 0xFF);
		centredText(device, online ? "ONLINE" : "OFFLINE", chip, smallScale,
			0x08, 0x0A, 0x0C);
	}

	// ---- title -----------------------------------------------------------
	if (!layout.titleArea.empty()) {
		device.drawText(clipToWidth(model.titleText(), layout.titleArea.w, titleScale),
			layout.titleArea.x, layout.titleArea.y + 2.0f * s, titleScale,
			kTextR, kTextG, kTextB);
	}

	// ---- media corpus folder ---------------------------------------------
	// Always visible while the Player is up, whether or not a folder has been
	// chosen: "NOT SET - 0 VIDEOS" is a state the operator needs to see, and
	// the field is also the control that fixes it.
	const Rect& corpus = layout.corpusArea;
	if (!corpus.empty()) {
		const bool chosen = model.corpusChosen();
		// An empty corpus is drawn as a normal panel, not as an error: nothing
		// has gone wrong, there is simply no folder selected yet.
		std::uint8_t bgR = kPanelR;
		std::uint8_t bgG = kPanelG;
		std::uint8_t bgB = kPanelB;
		std::uint8_t edgeR = kEdgeR;
		std::uint8_t edgeG = kEdgeG;
		std::uint8_t edgeB = kEdgeB;
		std::uint8_t edgeA = 0xFF;
		if (!chosen) {
			bgR = 0x14; bgG = 0x17; bgB = 0x1D;
			edgeR = kAccentR; edgeG = kAccentG; edgeB = kAccentB; edgeA = 0xC0;
		}
		fill(device, corpus, bgR, bgG, bgB, 0xFF);
		device.drawOutline(corpus, 1.0f, edgeR, edgeG, edgeB, edgeA);

		const float padX = 8.0f * s;
		const float textY = corpus.y
			+ std::max(0.0f, (corpus.h - hud::kGlyphHeight * corpusScale) * 0.5f);
		const std::string label = model.corpusLabel();
		const std::string value = model.corpusValue();
		const float labelWidth = hud::textWidth(label, corpusScale);

		// The label is a fixed dim prefix; the value carries the state, so its
		// colour is what tells "a folder is set" from "nothing chosen yet".
		device.drawText(label, corpus.x + padX, textY, corpusScale,
			kDimR, kDimG, kDimB);
		const std::uint8_t bodyR = chosen ? kTextR : kAccentR;
		const std::uint8_t bodyG = chosen ? kTextG : kAccentG;
		const std::uint8_t bodyB = chosen ? kTextB : kAccentB;
		const float remaining = std::max(0.0f,
			corpus.w - padX * 2.0f - labelWidth - hud::kGlyphWidth * corpusScale);
		device.drawText(clipToWidth(value, remaining, corpusScale),
			corpus.x + padX + labelWidth, textY, corpusScale,
			bodyR, bodyG, bodyB);
	}

	// ---- transport row ---------------------------------------------------
	// Two buttons show live state rather than a fixed label: play/pause flips
	// with the transport, and HUD dims while the bar is hidden.
	for (const ControlButton& button : layout.buttons) {
		if (button.command == ControlCommand::PlayPause) {
			ControlButton dynamicButton = button;
			dynamicButton.label = (state.playing && !state.paused) ? "||" : ">";
			drawButton(device, dynamicButton, buttonScale);
			continue;
		}
		if (button.command == ControlCommand::ToggleHud && !state.hudVisible) {
			ControlButton dimmed = button;
			dimmed.enabled = false;
			drawButton(device, dimmed, buttonScale);
			continue;
		}
		if (button.command == ControlCommand::ToggleSubtitles && !state.subtitlesEnabled) {
			ControlButton dimmed = button;
			dimmed.enabled = false;
			drawButton(device, dimmed, buttonScale);
			continue;
		}
		drawButton(device, button, buttonScale);
	}

	// ---- seek bar --------------------------------------------------------
	const Rect& bar = layout.seekBar;
	if (!bar.empty()) {
		fill(device, bar, kPanelR, kPanelG, kPanelB, 0xFF);
		double fraction = 0.0;
		if (seekActive && state.duration > 0.0) {
			fraction = state.position / state.duration;
		} else if (state.clipCount > 0) {
			// No timeline (a still image): show playlist position so the bar
			// still reports something true instead of sitting empty.
			fraction = static_cast<double>(state.clipIndex + 1)
				/ static_cast<double>(state.clipCount);
		}
		fraction = std::max(0.0, std::min(1.0, fraction));
		const Rect filled{bar.x, bar.y, bar.w * static_cast<float>(fraction), bar.h};
		fill(device, filled, seekActive ? kAccentR : kDimR, seekActive ? kAccentG : kDimG,
			seekActive ? kAccentB : kDimB, 0xFF);
	}

	// ---- readouts --------------------------------------------------------
	if (!layout.volumeArea.empty()) {
		centredText(device, "VOL " + std::to_string(static_cast<int>(state.volume + 0.5)),
			layout.volumeArea, smallScale, kDimR, kDimG, kDimB);
	}
	if (!layout.speedArea.empty()) {
		char buffer[24];
		std::snprintf(buffer, sizeof(buffer), "%.2fX", state.speed);
		centredText(device, buffer, layout.speedArea, smallScale, kDimR, kDimG, kDimB);
	}

	// Position/duration rides just above the seek bar when a timeline exists.
	if (seekActive) {
		const std::string clock = clockText(state.position) + " / " + clockText(state.duration);
		const float width = hud::textWidth(clock, smallScale);
		const float x = std::max(bar.x, bar.x + bar.w - width);
		device.drawText(clock, x, bar.y - hud::kGlyphHeight * smallScale - 2.0f * s,
			smallScale, kDimR, kDimG, kDimB);
	}

	// ---- message / error strip -------------------------------------------
	// One line, either the last refusal or the last thing a script reported.
	if (!layout.errorStrip.empty()) {
		std::string message = model.message();
		if (message.empty() && !state.online) {
			message = state.lastError;
		}
		if (message.empty() && !state.online) {
			message = "start the player to control it";
		}
		if (!message.empty()) {
			device.drawText(clipToWidth(message, layout.errorStrip.w, smallScale),
				layout.errorStrip.x, layout.errorStrip.y, smallScale,
				state.online ? kDimR : kBadR, state.online ? kDimG : kBadG,
				state.online ? kDimB : kBadB);
		}
	}
}

} // namespace media
