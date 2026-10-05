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
	// An explicit setup supersedes a walk that is still running: whatever it has
	// produced so far is what gets opened, because the caller has said it does not
	// want to wait for the rest.
	scanSetupPending_ = false;
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

bool MediaPlayerController::openClipAtIndex(std::size_t index, bool autoplay) {
	if (index >= clipCount()) {
		LOG_WARN("Controller") << "clip index " << index << " out of range ("
			<< clipCount() << " clips)";
		return false;
	}

	const MediaClip& clip = clips_.clipAt(index);
	const bool shader = clip.mediaType == ClipMediaType::Shader;

	if (backend_ != nullptr) {
		if (shader) {
			// A shader has no decoder, so it must NOT be handed to mpv. mpv would
			// reject a .frag as an unrecognised format, and that failure would
			// present as a broken clip rather than as a clip of a different kind.
			// Closing stops whatever was playing, which is what "open this one"
			// has to mean either way.
			backend_->close();
		} else if (!backend_->open(clip, autoplay)) {
			LOG_WARN("Controller") << "backend refused to open " << clip.displayName;
			return false;
		}
	}

	currentIndex_ = index;
	loaded_ = true;
	syncSubtitleText();
	notifyClipChanged();
	LOG_NOTICE("Controller") << "Opened [" << index << "] " << clip.displayName
		<< " (" << toString(clip.mediaType) << ")"
		<< (shader ? " for the shader renderer"
			: (autoplay ? " and started it" : ", held on its first frame"));
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

bool MediaPlayerController::beginScan() {
	auto* library = dynamic_cast<MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		// A non-disk source (a test double, a future JSON playlist) has no "scan"
		// to ask for. The caller's own source is authoritative, so there is
		// nothing to walk and nothing to wait for.
		scanSetupPending_ = false;
		return true;
	}

	library->scanBegin();
	scanSetupPending_ = true;

	// One bounded slice, synchronously, so a normal corpus behaves exactly as it
	// always did: the count the caller gets back is final and the first clip is
	// already open before the next line runs. Only a folder that cannot finish
	// inside this window - the case that used to freeze the window for minutes -
	// leaves the walk running for pollScan() to carry.
	if (!library->scanStep(MediaClipLibrary::ScanBudget::perRequest())) {
		LOG_NOTICE("Controller") << "scan started: " << library->scanEntries()
			<< " entries so far; the window stays live while it finishes";
		return false;
	}

	scanSetupPending_ = false;
	reloadAfterLibraryChange();
	return true;
}

bool MediaPlayerController::pollScan() {
	auto* library = dynamic_cast<MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		scanSetupPending_ = false;
		return false;
	}

	// One frame's worth of walking. This is the whole reason the walk is
	// incremental: the caller is a render loop, and it must get the frame back.
	if (library->scanning()
		&& !library->scanStep(MediaClipLibrary::ScanBudget::perFrame())) {
		return false;
	}

	if (!scanSetupPending_) {
		return false;   // nothing was waiting on the walk
	}
	scanSetupPending_ = false;
	// The playlist is final (or the folder was empty). Open the first clip, or
	// clear the decoder when there is nothing - the same rule rescan() uses, so
	// the two cannot disagree about what "the library changed" means.
	reloadAfterLibraryChange();
	return true;
}

bool MediaPlayerController::scanPending() const {
	const auto* library = dynamic_cast<const MediaClipLibrary*>(&clips_);
	return library != nullptr && library->scanning();
}

void MediaPlayerController::beginStartupScan(const std::vector<std::string>& directories) {
	auto* library = dynamic_cast<MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		return;
	}
	library->setRoots(directories);
	// Deliberately NOT one synchronous slice the way beginScan() takes one: at
	// startup there is no reason to spend even 250 ms before the first frame, and
	// the first pollScan() is a few milliseconds away. The walk is begun, not
	// done - which is what keeps the window painting.
	library->scanBegin();
	scanSetupPending_ = true;
}

void MediaPlayerController::beginStartupScan(const std::string& directory) {
	beginStartupScan(directory.empty()
		? std::vector<std::string>{}
		: std::vector<std::string>{directory});
}

std::size_t MediaPlayerController::rescan() {
	if (dynamic_cast<MediaClipLibrary*>(&clips_) == nullptr) {
		// A non-disk source (a test double, a future JSON playlist) has no
		// "scan" to ask for; report what it already holds rather than lying.
		return clips_.size();
	}
	if (beginScan()) {
		LOG_NOTICE("Controller") << "rescan: " << clips_.size() << " clip(s)";
	}
	return clips_.size();
}

std::size_t MediaPlayerController::setMediaFolders(const std::vector<std::string>& directories) {
	auto* library = dynamic_cast<MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		// Nothing to re-point and nothing to scan; the caller's own source is
		// authoritative. Reported as unchanged rather than silently ignored.
		return clips_.size();
	}

	// Empty entries are dropped rather than kept: a UI that submits a list with a
	// blank row in it means "two folders", not "two folders and nowhere".
	std::vector<std::string> wanted;
	wanted.reserve(directories.size());
	for (const std::string& folder : directories) {
		if (!folder.empty()) {
			wanted.push_back(folder);
		}
	}
	library->setRoots(wanted);

	if (wanted.empty()) {
		// "No folder chosen" is a real state, not a fallback to the data folder:
		// nothing of the operator's is walked, and what is left is the built-in
		// shader library. See beginStartupScan().
		scanSetupPending_ = false;
		library->scanBegin();
		reloadAfterLibraryChange();
		LOG_NOTICE("Controller") << "media corpus cleared - nothing chosen, "
			<< clips_.size() << " clip(s) from the built-in shader library";
		return clips_.size();
	}

	const bool finished = beginScan();
	LOG_NOTICE("Controller") << "media corpus is now " << wanted.size()
		<< " folder(s), merged - " << clips_.size() << " clip(s)"
		<< (finished ? "" : " (still scanning)");
	for (const std::string& folder : wanted) {
		LOG_NOTICE("Controller") << "  " << folder;
	}
	return clips_.size();
}

std::size_t MediaPlayerController::setMediaFolder(const std::string& directory) {
	return setMediaFolders(directory.empty()
		? std::vector<std::string>{}
		: std::vector<std::string>{directory});
}

std::vector<std::string> MediaPlayerController::mediaFolders() const {
	const auto* library = dynamic_cast<const MediaClipLibrary*>(&clips_);
	if (library == nullptr) {
		return {};
	}
	// Gated on hasRoot() so the library's built-in default is never reported as a
	// folder the operator chose. That distinction is what the empty screen, the
	// Controller's corpus field and the Dashboard row all turn on.
	return library->hasRoot() ? library->roots() : std::vector<std::string>{};
}

std::string MediaPlayerController::mediaFolder() const {
	const std::vector<std::string> all = mediaFolders();
	return all.empty() ? std::string() : all.front();
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

	// A walk that outlives the frame it started in. Reported rather than hidden:
	// clipCount above is a PARTIAL count while this is true, so a client drawing
	// a playlist needs to be able to tell the difference.
	if (const auto* library = dynamic_cast<const MediaClipLibrary*>(&clips_)) {
		status.scanning = library->scanning();
		status.scanEntries = library->scanEntries();
		status.scanTruncated = library->scanTruncated();
	}

	if (loaded_ && currentIndex_ < clipCount()) {
		const MediaClip& clip = clips_.clipAt(currentIndex_);
		status.clipName = clip.displayName;
		status.clipPath = clip.absolutePath;
		status.isImage = clip.mediaType == ClipMediaType::Image;
		status.isShader = clip.mediaType == ClipMediaType::Shader;
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
