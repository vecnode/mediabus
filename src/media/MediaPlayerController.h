#pragma once

#include "media/IClipSource.h"
#include "media/ScriptHost.h"

#include <cstddef>
#include <string>
#include <vector>

namespace media {

/// Snapshot of playback + UI state, serialised to JSON by the HTTP layer.
///
/// The first block of fields is the original contract and must not change
/// shape: an external controller parses these exact keys. Everything after it
/// is additive.
struct MediaPlayerStatus {
	// --- original contract (frozen) ---
	bool loaded = false;
	bool playing = false;
	bool isImage = false;
	std::size_t clipIndex = 0;
	std::size_t clipCount = 0;
	std::string clipName;
	bool subtitlesEnabled = false;
	std::string subtitleText;

	// --- additive (P2+) ---
	double position = 0.0;
	double duration = 0.0;
	bool seekable = false;
	double speed = 1.0;
	double volume = 100.0;
	bool paused = false;
	std::string decoder;
	std::vector<std::string> scriptsLoaded;
};

/// One entry in the playlist exposed over HTTP.
struct MediaPlayerClipInfo {
	std::size_t index = 0;
	std::string name;
	std::string path;
	std::string mediaType;
};

/// Transport state the controller exposes. The libmpv surface implements the
/// IPlaybackBackend side of this; tests substitute their own backend.
struct TransportState {
	bool loaded = false;
	bool playing = false;
	bool paused = false;
	bool isImage = false;
	bool seekable = false;
	bool subtitlesEnabled = true;
	double position = 0.0;
	double duration = 0.0;
	double speed = 1.0;
	double volume = 100.0;
	std::string decoder;
};

/// Optional playback backend. The controller owns the playlist and the
/// command surface; this is the part that actually decodes.
class IPlaybackBackend {
public:
	virtual ~IPlaybackBackend() = default;

	/// Open `clip` and decode its first frame; return false if it cannot load.
	virtual bool open(const MediaClip& clip) = 0;
	virtual void close() = 0;

	virtual void play() = 0;
	virtual void pause() = 0;
	/// Stop and hold the current frame as a still preview.
	virtual void stopToPreview() = 0;

	virtual TransportState state() const = 0;

	virtual void seekAbsolute(double seconds) = 0;
	virtual void seekRelative(double seconds) = 0;
	virtual void seekPercent(double percent) = 0;
	virtual void setSpeed(double factor) = 0;
	virtual void setVolume(double percent) = 0;
	virtual void setSubtitlesEnabled(bool enabled) = 0;

	/// Scripts to load. MUST be set before the backend starts, because mpv only
	/// accepts the `script` option before mpv_initialize().
	virtual void setScripts(const std::vector<scripts::ScriptFile>& scripts) = 0;

	/// Names of the scripts the backend actually loaded.
	virtual std::vector<std::string> loadedScripts() const = 0;
};

/// Shared command layer for the GUI, HTTP and any future control surface.
///
/// Keeps the original semantics: commands are invoked from the GUI thread (the
/// HTTP layer only enqueues), and the controller never touches a decoder from
/// another thread.
class MediaPlayerController {
public:
	using ClipChangedHandler = void (*)(void* userData);

	MediaPlayerController(IClipSource& clips, IPlaybackBackend* backend);

	/// Rescan the library and open the first clip. Returns false when empty.
	bool setup();

	void play();
	void stop();
	void nextClip();
	void previousClip();
	bool openClipAtIndex(std::size_t index);

	// Transport additions.
	bool seekAbsolute(double seconds);
	bool seekRelative(double seconds);
	bool seekPercent(double percent);
	bool setSpeed(double factor);
	bool setVolume(double percent);
	bool setPaused(bool paused);

	// Subtitles.
	bool setSubtitlesEnabled(bool enabled);
	bool setSubtitleText(const std::string& text);
	void clearSubtitleOverride();
	bool isSubtitlesEnabled() const;

	/// Scripting: hand the discovered scripts to the backend (before setup) and
	/// report which ones loaded.
	void setScripts(std::vector<scripts::ScriptFile> scripts);
	const std::vector<scripts::ScriptFile>& scriptFiles() const { return scripts_; }
	std::vector<std::string> loadedScripts() const;

	/// Re-scan the scripts directory. Reports the new list; scripts already
	/// running keep running, because mpv cannot unload a script in place.
	std::vector<scripts::ScriptFile> rescanScripts();

	MediaPlayerStatus getStatus() const;
	std::string getSubtitleText() const;
	std::vector<MediaPlayerClipInfo> getClips() const;

	/// The playlist source, for callers that own it (e.g. rescanning on demand).
	IClipSource& clipSource() const { return clips_; }

	/// Re-scan the current clip source in place, keeping the folder the library
	/// was already pointed at. Returns the number of clips found.
	std::size_t rescan();

	/// Point the library at `directory` and reload. Returns the number of clips
	/// found there.
	///
	/// An empty `directory` means "back to the library's own default", i.e. the
	/// Player's data folder. A folder that does not exist is not an error: it
	/// scans to nothing, the playlist is cleared below, and the Player keeps
	/// running with 0 clips - the state the Controller reports as NO CLIPS.
	///
	/// Only meaningful when the clip source is a MediaClipLibrary; any other
	/// source keeps its existing root and is merely rescanned. That keeps the
	/// interface honest for the test double without a dynamic cast here.
	std::size_t setMediaFolder(const std::string& directory);

	/// Where media is being read from, for status and diagnostics.
	std::string mediaFolder() const;

	/// Diagnostics: where the playlist came from.
	std::string searchLog() const;

	void setClipChangedHandler(ClipChangedHandler handler, void* userData);

private:
	std::size_t clipCount() const;
	void notifyClipChanged();
	/// Re-open the first clip after the library changed underneath us, or clear
	/// the loaded state when the library is now empty. Shared by rescan() and
	/// setMediaFolder() so the two cannot disagree about what "no clips" means.
	void reloadAfterLibraryChange();
	/// Subtitle text for the current clip: an explicit override wins, otherwise
	/// the backend/embedded track is reported.
	void syncSubtitleText();

	IClipSource& clips_;
	IPlaybackBackend* backend_ = nullptr;

	std::size_t currentIndex_ = 0;
	bool loaded_ = false;

	std::vector<scripts::ScriptFile> scripts_;

	std::string subtitleOverride_;
	std::string subtitleText_;

	ClipChangedHandler clipChangedHandler_ = nullptr;
	void* clipChangedUserData_ = nullptr;
};

} // namespace media
