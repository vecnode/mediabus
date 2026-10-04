#include "control/ControllerModel.h"

#include <algorithm>
#include <cmath>

namespace media {
namespace {

/// True when the Player reported no usable folder. Empty is "using the
/// Player's own default", which is a valid corpus, but the Controller has no
/// path to show for it, so both spellings are drawn the same way.
bool folderIsDefault(const std::string& folder) {
	return folder.empty() || folder == "(default)";
}

/// The tail of a path, for the corpus field: the last component, plus its
/// parent when that is available. A full corpus path is far longer than the
/// field, and the tail is what identifies the folder to a person.
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

bool ControllerModel::applyState(const ControllerState& next) {
	// Compare the fields the interface actually draws; the position ticks up on
	// every poll, so a naive whole-struct compare would report "changed" forever.
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

bool ControllerModel::seekBarActive() const {
	// A still image has no timeline (mpv reports seekable=false), and a
	// Controller that offered to scrub one would be lying about the Player.
	return state_.online && state_.loaded && !state_.isImage && state_.seekable;
}

double ControllerModel::seekPercent() const {
	if (state_.duration <= 0.0) {
		return 0.0;
	}
	const double fraction = state_.position / state_.duration;
	return std::max(0.0, std::min(100.0, fraction * 100.0));
}

std::string ControllerModel::titleText() const {
	if (!state_.online) {
		return "Player not running";
	}
	if (!state_.loaded || state_.clipCount == 0) {
		return "No clips";
	}
	std::string text = std::to_string(state_.clipIndex + 1) + " / "
		+ std::to_string(state_.clipCount) + "   " + state_.clipName;
	if (state_.isImage) {
		text += "   [image]";
	} else if (state_.paused) {
		text += "   [paused]";
	} else if (state_.playing) {
		text += "   [playing]";
	} else {
		text += "   [stopped]";
	}
	if (state_.fullscreen) {
		text += "   [fullscreen]";
	}
	return text;
}

bool ControllerModel::corpusChosen() const {
	return state_.online && !folderIsDefault(state_.mediaFolder);
}

std::string ControllerModel::corpusLabel() const {
	return "MEDIA FOLDER";
}

std::string ControllerModel::corpusValue() const {
	if (!state_.online) {
		// No Player means no folder to report. Drawing a stale one would be a
		// lie about what is being played, so the field says what it knows.
		return "start the Player to set it";
	}
	const std::string count = std::to_string(state_.corpusClipCount)
		+ (state_.corpusClipCount == 1 ? " video" : " videos");
	if (!corpusChosen()) {
		// The explicit "nothing chosen" state: no folder, zero videos, no error.
		return "not set - " + count;
	}
	return shortFolder(state_.mediaFolder) + " - " + count;
}

} // namespace media
