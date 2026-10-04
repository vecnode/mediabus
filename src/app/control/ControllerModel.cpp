#include "app/control/ControllerModel.h"

#include "app/hud/BitmapFont.h"
#include "core/UiScale.h"

#include <algorithm>
#include <cmath>

namespace media {
namespace {

// --- bar geometry ---------------------------------------------------------
// Written in *text units*, not pixels: one unit is the width of one glyph cell
// at the current text scale, so every one of these grows with the font. That is
// what keeps a bigger font from being packed into the same space - the bar's
// own default size is multiplied by the same factor (see controller_main.cpp),
// so the proportions are identical at 100% and at 200% DPI.
//
// Three bands, top to bottom: identity, transport, seek bar. The media corpus
// band is fitted between the transport row and the seek bar when the window is
// tall enough for it, and omitted rather than squeezing the transport labels
// into a sliver when it is not.
constexpr float kPad = 1.0f;
constexpr float kChipHeight = 1.6f;
constexpr float kChipWidth = 7.0f;
constexpr float kTitleRowHeight = 2.0f;
constexpr float kButtonHeight = 3.2f;
constexpr float kButtonGap = 0.7f;
constexpr float kButtonPadding = 1.4f;
constexpr float kMinButtonWidth = 3.5f;
constexpr float kSeekHeight = 0.7f;
constexpr float kVolumeWidth = 8.0f;
constexpr float kSpeedWidth = 7.0f;
constexpr float kErrorStripHeight = 2.0f;
constexpr float kCorpusHeight = 2.2f;

/// What a text scale of 1 draws for each role. These are floored by
/// ui::*Pixels, which is what makes the bar legible on a 4K panel instead of
/// a 5-pixel-wide capital.
constexpr float kTitleRolePixels = 15.0f;
constexpr float kButtonRolePixels = 22.0f;
constexpr float kSmallRolePixels = 14.0f;

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

/// True when the Player reported no usable folder. Empty is "using the
/// Player's own default", which is a valid corpus, but the Controller has no
/// path to show for it, so both spellings are drawn the same way.
bool folderIsDefault(const std::string& folder) {
	return folder.empty() || folder == "(default)";
}

/// The tail of a path, for the bar: the last component, plus its parent when
/// that is available. A full corpus path is far longer than the bar, and the
/// tail is what identifies the folder to a person.
std::string shortFolder(const std::string& folder) {
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
	// "D:\Corpus\Shows\2026" -> "Shows\2026"; the drive root stays whole.
	const std::string tail = text.substr(previous + 1);
	return tail.size() <= 2 ? text : tail;
}

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

void ControllerModel::setUiScale(float scale) {
	uiScale_ = ui::sanitizeScale(scale);
}

bool ControllerModel::applyState(const ControllerState& next) {
	// Compare the fields the bar actually draws; the position ticks up on every
	// poll, so a naive whole-struct compare would report "changed" forever.
	//
	// mediaFolder and corpusClipCount are in this list because the corpus
	// field is drawn from them: leaving them out would mean a folder change
	// made elsewhere never repainted here.
	const ControllerState& prev = state_;
	const bool changed =
		prev.online != next.online || prev.loaded != next.loaded
		|| prev.playing != next.playing || prev.paused != next.paused
		|| prev.isImage != next.isImage || prev.seekable != next.seekable
		|| prev.clipIndex != next.clipIndex || prev.clipCount != next.clipCount
		|| prev.clipName != next.clipName || prev.subtitlesEnabled != next.subtitlesEnabled
		|| prev.hudVisible != next.hudVisible || prev.fullscreen != next.fullscreen
		|| prev.mediaFolder != next.mediaFolder
		|| prev.corpusClipCount != next.corpusClipCount
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

	// One unit is one glyph cell at this scale. Every constant above is in
	// units, so a single multiply here makes the whole bar - not just its text -
	// grow with the font.
	const float unit = ui::kGlyphHeight * uiScale_;
	auto u = [unit](float value) { return value * unit; };

	const float pad = u(kPad);
	const float gap = u(kButtonGap);

	const float innerLeft = pad;
	const float innerRight = std::max(pad, width - pad);
	const float innerWidth = innerRight - innerLeft;

	// Top row: status chip on the left, clip identity filling the rest.
	layout_.statusChip = {innerLeft, pad, u(kChipWidth), u(kChipHeight)};
	layout_.titleArea = {innerLeft + u(kChipWidth) + pad, pad,
		std::max(0.0f, innerWidth - u(kChipWidth) - pad), u(kTitleRowHeight)};

	// Fixed readouts on the right of the transport row.
	const float volumeW = u(kVolumeWidth);
	const float speedW = u(kSpeedWidth);
	const float readoutsW = volumeW + speedW + gap * 2.0f;
	const float readoutWidth = std::max(0.0f, innerWidth - readoutsW);

	// --- the vertical stack -------------------------------------------------
	// Top down: the identity row, the transport row, optionally the media corpus
	// field, then the seek bar. It is built as one block and centred in whatever
	// space is left, so a taller window puts equal air above and below instead of
	// leaving one band stretched and a hole under it.
	//
	// The corpus field is only taken when the transport row would still keep at
	// least its natural height; otherwise the labels would be squeezed into a
	// sliver, which is worse than not showing the folder at all.
	const float titleHeight = u(kTitleRowHeight);
	const float minButtonHeight = u(kButtonHeight);
	const float corpusHeight = u(kCorpusHeight);
	const float seekHeight = u(kSeekHeight);

	// The error strip is where the seek bar's own reading would go, so its band
	// is reserved only while offline (that is when the strip has text in it).
	const float errorHeight = state_.online ? 0.0f : (u(kErrorStripHeight) + gap);
	const float available = std::max(0.0f, height - pad * 2.0f - errorHeight);
	const float bodySpace = std::max(0.0f, available - titleHeight - gap);

	const bool roomForCorpus = bodySpace
		>= minButtonHeight + gap + corpusHeight + gap + seekHeight;
	const float buttonHeight = roomForCorpus
		? std::min(minButtonHeight * 1.2f, bodySpace * 0.4f) : minButtonHeight;
	const float stackHeight = titleHeight + gap + buttonHeight
		+ (roomForCorpus ? gap + corpusHeight : 0.0f) + gap + seekHeight;

	// Extra space becomes margin above and below the block rather than one
	// stretched band. Never negative, so an undersized window just clips.
	const float top = pad + std::max(0.0f, (available - stackHeight) * 0.5f);

	// Top row: status chip on the left, clip identity filling the rest.
	layout_.statusChip = {innerLeft, top, u(kChipWidth), u(kChipHeight)};
	layout_.titleArea = {innerLeft + u(kChipWidth) + pad, top,
		std::max(0.0f, innerWidth - u(kChipWidth) - pad), titleHeight};

	float y = top + titleHeight + gap;
	const float buttonY = y;
	y += buttonHeight;
	if (roomForCorpus) {
		y += gap;
		layout_.corpusArea = {innerLeft, y, readoutWidth, corpusHeight};
		y += corpusHeight;
	}
	y += gap;
	const float seekY = y;

	// Bottom strip: the error line, only laid out while offline so it cannot
	// overlap the seek bar when the bar is online.
	if (!state_.online) {
		layout_.errorStrip = {innerLeft, height - pad - u(kErrorStripHeight),
			innerWidth, u(kErrorStripHeight)};
	}

	// Transport row: left of the readouts.
	float buttonsLeft = innerLeft;
	float buttonsRight = innerRight - readoutsW;
	if (buttonsRight < buttonsLeft + u(kMinButtonWidth)) {
		buttonsRight = innerRight;
	}

	// Measure every button, then distribute any spare width evenly rather than
	// left-packing, so the row looks deliberate at any width.
	const float buttonScale = ui::textScale(uiScale_, kButtonRolePixels);
	const std::size_t count = sizeof(kButtons) / sizeof(kButtons[0]);
	float naturalTotal = 0.0f;
	float widths[sizeof(kButtons) / sizeof(kButtons[0])];
	for (std::size_t i = 0; i < count; ++i) {
		const float natural = labelWidth(kButtons[i].label, buttonScale)
			+ u(kButtonPadding) * 2.0f;
		widths[i] = std::max(u(kMinButtonWidth), natural);
		naturalTotal += widths[i];
	}
	const float gaps = gap * static_cast<float>(count - 1);
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

	float x = buttonsLeft;
	for (std::size_t i = 0; i < count; ++i) {
		const float w = widths[i] * scaleDown;
		ControlButton button;
		button.command = kButtons[i].command;
		button.label = kButtons[i].label;
		button.rect = {x, buttonY, w, buttonHeight};
		layout_.buttons.push_back(button);
		x += w + gap;
	}

	// Seek bar sits under the transport row, full width up to the readouts.
	layout_.seekBar = {innerLeft, seekY, readoutWidth, seekHeight};

	if (volumeW > 0.0f) {
		layout_.volumeArea = {innerRight - volumeW - speedW - gap,
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

bool ControllerModel::corpusHit(float x, float y) const {
	return layout_.corpusArea.hit(x, y);
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

bool ControllerModel::corpusChosen() const {
	return state_.online && !folderIsDefault(state_.mediaFolder);
}

std::string ControllerModel::corpusLabel() const {
	return "MEDIA FOLDER: ";
}

std::string ControllerModel::corpusValue() const {
	if (!state_.online) {
		// No Player means no folder to report. Drawing a stale one would be a
		// lie about what is being played, so the field says what it knows.
		return "START THE PLAYER TO SET IT";
	}
	const std::string count = std::to_string(state_.corpusClipCount)
		+ (state_.corpusClipCount == 1 ? " VIDEO" : " VIDEOS");
	if (!corpusChosen()) {
		// The explicit "nothing chosen" state: black, zero videos, no error.
		return "NOT SET - " + count + " - CLICK TO CHOOSE";
	}
	return shortFolder(state_.mediaFolder) + " - " + count;
}

} // namespace media
