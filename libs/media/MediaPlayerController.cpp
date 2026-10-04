#include "media/MediaPlayerController.h"

#include "core/Log.h"
#include "media/MediaClipLibrary.h"

#include <cmath>

namespace media {

MediaPlayerController::MediaPlayerController(IClipSource& clips, IPlaybackBackend* backend)
	: clips_(clips), backend_(backend) {}

std::size_t MediaPlayerController::clipCount() const {
	return clips_.size();
}

void MediaPlayerController::setClipChangedHandler(ClipChangedHandler handler, void* userData) {
	clipChangedHandler_ = handler;
	clipChangedUserData_ = userData;
}

void MediaPlayerController::notifyClipChanged() {
	if (clipChangedHandler_ != nullptr) {
		clipChangedHandler_(clipChangedUserData_);
	}
}

void MediaPlayerController::syncSubtitleText() {
	if (!subtitleOverride_.empty()) {
		subtitleText_ = subtitleOverride_;
		return;
	}
	// The on-screen text is whatever the media itself carries: mpv renders
	// embedded and sidecar subtitles directly into the frame, so this status
	// field only ever mirrors an explicit host-supplied override.
	subtitleText_.clear();
}

bool MediaPlayerController::setup() {
	if (clips_.empty()) {
		LOG_ERROR("Controller") << "No clips discovered";
		return false;
	}
	// Scripts must reach the backend before it starts: mpv only accepts the
	// `script` option prior to mpv_initialize().
	if (backend_ != nullptr && !scripts_.empty()) {
		backend_->setScripts(scripts_);
	}
	return openClipAtIndex(0);
}

void MediaPlayerController::setScripts(std::vector<scripts::ScriptFile> scripts) {
	scripts_ = std::move(scripts);
	if (backend_ != nullptr) {
		backend_->setScripts(scripts_);
	}
}

std::vector<std::string> MediaPlayerController::loadedScripts() const {
	if (backend_ != nullptr) {
		return backend_->loadedScripts();
	}
	return {};
}

std::vector<scripts::ScriptFile> MediaPlayerController::rescanScripts() {
	scripts_ = scripts::discover();
	// A script already running cannot be unloaded in place; the new list takes
	// effect on the next start. Report that honestly rather than pretending.
	LOG_NOTICE("Controller") << "script directory now holds " << scripts_.size()
		<< " script(s); restart to load changes";
	return scripts_;
}

bool MediaPlayerController::openClipAtIndex(std::size_t index) {
	if (index >= clipCount()) {
		LOG_WARN("Controller") << "clip index " << index << " out of range ("
			<< clipCount() << " clips)";
		return false;
	}

	if (backend_ != nullptr) {
		const MediaClip& clip = clips_.clipAt(index);
		if (!backend_->open(clip)) {
			LOG_WARN("Controller") << "backend refused to open " << clip.displayName;
			return false;
		}
	}

	currentIndex_ = index;
	loaded_ = true;
	syncSubtitleText();
	notifyClipChanged();
	LOG_NOTICE("Controller") << "Opened [" << index << "] "
		<< clips_.clipAt(index).displayName << " ("
		<< toString(clips_.clipAt(index).mediaType) << ")";
	return true;
}

void MediaPlayerController::reloadAfterLibraryChange() {
	if (clipCount() == 0) {
		// Nothing left to play. Close the decoder rather than leaving the last
		// frame of a clip from the previous folder on screen, which would read
		// as "still playing" while the status says 0 clips.
		if (backend_ != nullptr) {
			backend_->close();
		}
		loaded_ = false;
		currentIndex_ = 0;
		syncSubtitleText();
		notifyClipChanged();
		if (backend_ != nullptr && !scripts_.empty()) {
			// close() may drop the decoder's script state; re-arm it so a
			// folder change does not silently kill the scripting layer.
			backend_->setScripts(scripts_);
		}
		return;
	}
	if (currentIndex_ >= clipCount()) {
		currentIndex_ = 0;
	}
	// Always force a real open, even when the index is unchanged: the file
	// behind it is different now, so "the index did not move" does not mean
	// "the decoder is already showing the right thing".
	loaded_ = false;
	openClipAtIndex(currentIndex_);
}

std::size_t MediaPlayerController::rescan() {
	auto* library = dynamic_cast<MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		// A non-disk source (a test double, a future JSON playlist) has no
		// "scan" to ask for; report what it already holds rather than lying.
		return clips_.size();
	}
	library->scan();
	reloadAfterLibraryChange();
	LOG_NOTICE("Controller") << "rescan: " << clips_.size() << " clip(s)";
	return clips_.size();
}

std::size_t MediaPlayerController::setMediaFolder(const std::string& directory) {
	auto* library = dynamic_cast<MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		// Nothing to re-point and nothing to scan; the caller's own source is
		// authoritative. Reported as unchanged rather than silently ignored.
		return clips_.size();
	}
	// An empty path restores the library's default root (the Player's data
	// directory), which is what "no folder chosen" means.
	library->setRoot(directory);
	library->scan();
	reloadAfterLibraryChange();
	const std::string where = library->root();
	LOG_NOTICE("Controller") << "media folder now "
		<< (where.empty() ? std::string("(default)") : where)
		<< " - " << clips_.size() << " clip(s)";
	return clips_.size();
}

std::string MediaPlayerController::mediaFolder() const {
	if (const auto* library = dynamic_cast<const MediaClipLibrary*>(&clips_)) {
		return library->root();
	}
	return {};
}

void MediaPlayerController::play() {
	if (backend_ != nullptr && loaded_) {
		backend_->play();
	}
}

void MediaPlayerController::stop() {
	if (backend_ != nullptr && loaded_) {
		backend_->stopToPreview();
	}
}

void MediaPlayerController::nextClip() {
	if (clipCount() == 0) {
		return;
	}
	openClipAtIndex(clips_.nextIndex(currentIndex_));
}

void MediaPlayerController::previousClip() {
	if (clipCount() == 0) {
		return;
	}
	openClipAtIndex(clips_.previousIndex(currentIndex_));
}

bool MediaPlayerController::seekAbsolute(double seconds) {
	if (backend_ == nullptr || !loaded_ || !std::isfinite(seconds) || seconds < 0.0) {
		return false;
	}
	backend_->seekAbsolute(seconds);
	return true;
}

bool MediaPlayerController::seekRelative(double seconds) {
	if (backend_ == nullptr || !loaded_ || !std::isfinite(seconds)) {
		return false;
	}
	backend_->seekRelative(seconds);
	return true;
}

bool MediaPlayerController::seekPercent(double percent) {
	if (backend_ == nullptr || !loaded_ || !std::isfinite(percent)
		|| percent < 0.0 || percent > 100.0) {
		return false;
	}
	backend_->seekPercent(percent);
	return true;
}

bool MediaPlayerController::setSpeed(double factor) {
	if (backend_ == nullptr || !loaded_ || !std::isfinite(factor)
		|| factor < 0.01 || factor > 100.0) {
		return false;
	}
	backend_->setSpeed(factor);
	return true;
}

bool MediaPlayerController::setVolume(double percent) {
	if (backend_ == nullptr || !std::isfinite(percent)
		|| percent < 0.0 || percent > 100.0) {
		return false;
	}
	backend_->setVolume(percent);
	return true;
}

bool MediaPlayerController::setPaused(bool paused) {
	if (backend_ == nullptr || !loaded_) {
		return false;
	}
	if (paused) {
		backend_->pause();
	} else {
		backend_->play();
	}
	return true;
}

bool MediaPlayerController::setSubtitlesEnabled(bool enabled) {
	if (backend_ != nullptr) {
		backend_->setSubtitlesEnabled(enabled);
	}
	return true;
}

bool MediaPlayerController::setSubtitleText(const std::string& text) {
	subtitleOverride_ = text;
	syncSubtitleText();
	return true;
}

void MediaPlayerController::clearSubtitleOverride() {
	subtitleOverride_.clear();
	syncSubtitleText();
}

bool MediaPlayerController::isSubtitlesEnabled() const {
	if (backend_ != nullptr) {
		return backend_->state().subtitlesEnabled;
	}
	return true;
}


MediaPlayerStatus MediaPlayerController::getStatus() const {
	MediaPlayerStatus status;
	status.clipCount = clipCount();
	status.clipIndex = currentIndex_;
	status.subtitlesEnabled = isSubtitlesEnabled();
	status.subtitleText = subtitleText_;
	status.loaded = loaded_;

	if (loaded_ && currentIndex_ < clipCount()) {
		const MediaClip& clip = clips_.clipAt(currentIndex_);
		status.clipName = clip.displayName;
		status.isImage = clip.mediaType == ClipMediaType::Image;
	}

	if (backend_ != nullptr) {
		const TransportState transport = backend_->state();
		status.playing = transport.playing;
		status.paused = transport.paused;
		status.position = transport.position;
		status.duration = transport.duration;
		status.seekable = transport.seekable;
		status.speed = transport.speed;
		status.volume = transport.volume;
		status.decoder = transport.decoder;
		status.scriptsLoaded = backend_->loadedScripts();
		// An image cannot be "playing" even if the backend reports transport.
		if (status.isImage) {
			status.playing = false;
		}
	}

	return status;
}

std::string MediaPlayerController::getSubtitleText() const {
	return subtitleText_;
}

std::vector<MediaPlayerClipInfo> MediaPlayerController::getClips() const {
	std::vector<MediaPlayerClipInfo> out;
	out.reserve(clipCount());
	for (std::size_t i = 0; i < clipCount(); ++i) {
		const MediaClip& clip = clips_.clipAt(i);
		MediaPlayerClipInfo info;
		info.index = i;
		info.name = clip.displayName;
		info.path = clip.absolutePath;
		info.mediaType = toString(clip.mediaType);
		out.push_back(std::move(info));
	}
	return out;
}

} // namespace media
