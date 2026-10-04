#include "app/control/ControllerModel.h"

#include "app/hud/BitmapFont.h"

#include <algorithm>
#include <cmath>

namespace media {
namespace {

// --- bar geometry ---------------------------------------------------------
// Two rows: identity on top, transport below. Every number here is in pixels,
// and the layout is recomputed on resize, so the bar is usable from its
// default size up.
constexpr float kPad = 8.0f;
constexpr float kChipHeight = 14.0f;
constexpr float kChipWidth = 58.0f;
constexpr float kTitleRowHeight = 18.0f;
constexpr float kButtonHeight = 34.0f;
constexpr float kButtonGap = 6.0f;
constexpr float kButtonPadding = 10.0f;
constexpr float kMinButtonWidth = 30.0f;
constexpr float kSeekHeight = 8.0f;
constexpr float kVolumeWidth = 54.0f;
constexpr float kSpeedWidth = 46.0f;
constexpr float kErrorStripHeight = 14.0f;

/// Header text scale and transport text scale. The bitmap font is 5x7 plus a
/// shadow; 2 is legible at 100% DPI without a magnifier, 1 for the small print.
constexpr float kTitleScale = 1.0f;
constexpr float kButtonScale = 2.0f;

/// Width a label needs at `scale`, using the font's own metric.
float labelWidth(const std::string& text, float scale) {
	return hud::textWidth(text, scale);
}

struct ButtonSpec {
	ControlCommand command;
	const char* label;
};

// The transport row, in order. Labels are ASCII and fit the 5x7 alphabet.
const ButtonSpec kButtons[] = {
	{ControlCommand::Previous, "|<"},
	{ControlCommand::PlayPause, "> / ||"},
	{ControlCommand::Stop, "[]"},
	{ControlCommand::Next, ">|"},
	{ControlCommand::ToggleHud, "HUD"},
	{ControlCommand::ToggleFullscreen, "FULL"},
	{ControlCommand::ToggleSubtitles, "SUB"},
};

} // namespace

const char* toString(ControlCommand command) {
	switch (command) {
		case ControlCommand::None: return "none";
		case ControlCommand::Previous: return "previous";
		case ControlCommand::PlayPause: return "play-pause";
		case ControlCommand::Stop: return "stop";
		case ControlCommand::Next: return "next";
		case ControlCommand::ToggleHud: return "toggle-hud";
		case ControlCommand::ToggleFullscreen: return "toggle-fullscreen";
		case ControlCommand::ToggleSubtitles: return "toggle-subtitles";
	}
	return "none";
}

bool ControllerModel::applyState(const ControllerState& next) {
	// Compare the fields the bar actually draws; the position ticks up on every
	// poll, so a naive whole-struct compare would report "changed" forever.
	const ControllerState& prev = state_;
	const bool changed =
		prev.online != next.online || prev.loaded != next.loaded
		|| prev.playing != next.playing || prev.paused != next.paused
		|| prev.isImage != next.isImage || prev.seekable != next.seekable
		|| prev.clipIndex != next.clipIndex || prev.clipCount != next.clipCount
		|| prev.clipName != next.clipName || prev.subtitlesEnabled != next.subtitlesEnabled
		|| prev.hudVisible != next.hudVisible || prev.fullscreen != next.fullscreen
		|| prev.lastError != next.lastError
		|| std::abs(prev.duration - next.duration) > 0.001
		|| std::abs(prev.volume - next.volume) > 0.001
		|| std::abs(prev.speed - next.speed) > 0.001
		|| std::abs(prev.position - next.position) > 0.05;
	state_ = next;
	return changed;
}

void ControllerModel::markOffline(const std::string& reason) {
	state_.online = false;
	state_.playing = false;
	state_.lastError = reason;
}

void ControllerModel::setMessage(std::string message) {
	message_ = std::move(message);
}

void ControllerModel::layout(float width, float height) {
	layout_ = ControllerLayout{};

	const float innerLeft = kPad;
	const float innerRight = std::max(kPad, width - kPad);
	const float innerWidth = innerRight - innerLeft;

	// Top row: status chip on the left, clip identity filling the rest.
	layout_.statusChip = {innerLeft, kPad, kChipWidth, kChipHeight};
	layout_.titleArea = {innerLeft + kChipWidth + kPad, kPad,
		std::max(0.0f, innerWidth - kChipWidth - kPad), kTitleRowHeight};

	// Bottom strip: the error line, only meaningful while offline.
	layout_.errorStrip = {innerLeft, height - kPad - kErrorStripHeight,
		innerWidth, kErrorStripHeight};

	const float secondRowTop = kPad + kTitleRowHeight;
	const float available = std::max(0.0f, innerWidth);
	const bool hasErrorLine = !state_.online;
	const float buttonRowBottom = hasErrorLine
		? layout_.errorStrip.y - kPad
		: height - kPad;
	const float buttonHeight = std::max(kButtonHeight,
		buttonRowBottom - secondRowTop - kSeekHeight - kButtonGap);

	// Reserve the fixed readouts on the right of the transport row.
	const bool compact = available < 640.0f;
	const float volumeW = compact ? 0.0f : kVolumeWidth;
	const float speedW = compact ? 0.0f : kSpeedWidth;

	float buttonsLeft = innerLeft;
	float buttonsRight = innerRight - volumeW - speedW
		- (volumeW > 0.0f ? kButtonGap : 0.0f) - (speedW > 0.0f ? kButtonGap : 0.0f);
	if (buttonsRight < buttonsLeft + kMinButtonWidth) {
		buttonsRight = innerRight;
	}

	// Measure every button, then distribute any spare width evenly rather than
	// left-packing, so the row looks deliberate at any width.
	const std::size_t count = sizeof(kButtons) / sizeof(kButtons[0]);
	float naturalTotal = 0.0f;
	float widths[sizeof(kButtons) / sizeof(kButtons[0])];
	for (std::size_t i = 0; i < count; ++i) {
		const float natural = labelWidth(kButtons[i].label, kButtonScale) + kButtonPadding * 2.0f;
		widths[i] = std::max(kMinButtonWidth, natural);
		naturalTotal += widths[i];
	}
	const float gaps = kButtonGap * static_cast<float>(count - 1);
	const float span = buttonsRight - buttonsLeft;
	float slack = span - naturalTotal - gaps;
	if (slack > 0.0f) {
		const float add = slack / static_cast<float>(count);
		for (std::size_t i = 0; i < count; ++i) {
			widths[i] += add;
		}
		slack = 0.0f;
	}

	// When even the natural widths do not fit, scale them down together.
	float scaleDown = 1.0f;
	const float overflow = (naturalTotal + gaps) - span;
	if (overflow > 0.0f && naturalTotal > 0.0f) {
		scaleDown = std::max(0.35f, (span - gaps) / naturalTotal);
	}

	const float buttonY = secondRowTop;
	float x = buttonsLeft;
	for (std::size_t i = 0; i < count; ++i) {
		const float w = widths[i] * scaleDown;
		ControlButton button;
		button.command = kButtons[i].command;
		button.label = kButtons[i].label;
		button.rect = {x, buttonY, w, buttonHeight};
		layout_.buttons.push_back(button);
		x += w + kButtonGap;
	}

	// Seek bar sits under the transport row, full width up to the readouts.
	const float readoutsW = volumeW + speedW + (volumeW > 0.0f ? kButtonGap : 0.0f)
		+ (speedW > 0.0f ? kButtonGap : 0.0f);
	const float seekWidth = std::max(0.0f, innerWidth - readoutsW);
	layout_.seekBar = {innerLeft, buttonY + buttonHeight + kButtonGap * 0.5f,
		seekWidth, kSeekHeight};

	if (volumeW > 0.0f) {
		layout_.volumeArea = {innerRight - volumeW - speedW - kButtonGap,
			buttonY, volumeW, buttonHeight};
	}
	if (speedW > 0.0f) {
		layout_.speedArea = {innerRight - speedW, buttonY, speedW, buttonHeight};
	}
}

ControlCommand ControllerModel::hitTest(float x, float y) const {
	for (const ControlButton& button : layout_.buttons) {
		if (!button.enabled) {
			continue;
		}
		if (button.hit(x, y)) {
			return button.command;
		}
	}
	return ControlCommand::None;
}

bool ControllerModel::seekPercentAt(float x, float y, double& percentOut) const {
	const Rect& bar = layout_.seekBar;
	if (bar.empty() || !bar.hit(x, y)) {
		return false;
	}
	const double fraction = (static_cast<double>(x) - bar.x) / static_cast<double>(bar.w);
	percentOut = std::max(0.0, std::min(100.0, fraction * 100.0));
	return true;
}

bool ControllerModel::seekBarActive() const {
	// A still image has no timeline (mpv reports seekable=false), and a
	// Controller that offered to scrub one would be lying about the Player.
	return state_.online && state_.loaded && !state_.isImage && state_.seekable;
}

std::string ControllerModel::titleText() const {
	if (!state_.online) {
		return "PLAYER NOT RUNNING";
	}
	if (!state_.loaded || state_.clipCount == 0) {
		return "NO CLIPS";
	}
	std::string text = std::to_string(state_.clipIndex + 1) + "/"
		+ std::to_string(state_.clipCount) + "  " + state_.clipName;
	if (state_.isImage) {
		text += "  [IMAGE]";
	} else if (state_.paused) {
		text += "  [PAUSED]";
	} else if (state_.playing) {
		text += "  [PLAYING]";
	} else {
		text += "  [STOPPED]";
	}
	if (state_.fullscreen) {
		text += "  [FS]";
	}
	return text;
}

} // namespace media
