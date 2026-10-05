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

	// --- additive: the playlist rebuild, which can outlive one frame ---
	/// True while the library is still being walked, so clipCount above is a
	/// PARTIAL count that will grow. It stays a real count so an existing parser
	/// keeps working; a client that wants the final list polls until this is
	/// false.
	bool scanning = false;
	/// Entries examined by the walk so far, for a progress readout.
	std::size_t scanEntries = 0;
	/// True when the walk hit the library's runaway guard and stopped early, so
	/// the list is final but incomplete.
	bool scanTruncated = false;

	/// Absolute path of the current clip, empty when nothing is loaded.
	///
	/// Needed by a host that has to act on the FILE rather than on the decoder:
	/// the Player reads a shader clip's source from here before compiling it, and
	/// there is no other way to ask - clipName is a display name and need not be
	/// unique across merged folders.
	std::string clipPath;
	/// True when the current clip is a generated shader rather than decoded
	/// media. Deliberately NOT reported through `isImage`: a shader is not a
	/// still, and conflating the two would make a third kind of clip
	/// unrepresentable.
	bool isShader = false;
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
	///
	/// `autoplay` is what the caller MEANS, and it has to be said explicitly.
	/// Opening a file and starting it are two different intentions: picking a clip
	/// from a list is "play this", while the backend's own priming wants a still
	/// frame to display. The implementation pauses to get that frame either way,
	/// so a caller that wants playback has to ask for it - and before this
	/// parameter existed, nothing did, which is why every clip opened paused and
	/// the transport looked broken.
	virtual bool open(const MediaClip& clip, bool autoplay) = 0;
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

	/// Open one entry of the playlist.
	///
	/// `autoplay` defaults to true because every caller that names a clip means
	/// "play this one" - the route behind a control bar's clip list, the next and
	/// previous transports, and startup. Passing false opens it and holds the
	/// first frame, which is what a host wants when it is about to issue its own
	/// transport command and does not want a burst of audio first.
	bool openClipAtIndex(std::size_t index, bool autoplay = true);

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
	///
	/// Completes synchronously when the walk fits inside ScanBudget::perRequest()
	/// - which every real corpus does, and which is what keeps the returned count
	/// final for the routes and tests that depend on it. A folder too large for
	/// that budget keeps walking on pollScan() instead of blocking here, and
	/// scanPending() reports it, so a request can never hang the window.
	std::size_t rescan();

	/// Point the library at `directory` and reload. Returns the number of clips
	/// found there.
	///
	/// An empty `directory` means "no folder chosen": the library falls back to
	/// its own default root but nothing is walked, the playlist is empty, and the
	/// Player keeps running with 0 clips - the state the Controller reports as
	/// NO CLIPS.
	///
	/// Like rescan(), a walk too large for one request budget finishes on
	/// pollScan() rather than blocking the caller.
	///
	/// Only meaningful when the clip source is a MediaClipLibrary; any other
	/// source keeps its existing root and is merely rescanned. That keeps the
	/// interface honest for the test double without a dynamic cast here.
	/// Set the folders to merge and reload. An empty list means nothing is
	/// chosen: nothing of the operator's is walked, the playlist is emptied to
	/// just the built-in shader library, and the Player keeps running - the state
	/// the Controller reports as NO CLIPS.
	///
	/// A folder that does not exist is skipped with a warning and the others are
	/// still read. A set far larger than a media corpus is refused whole.
	std::size_t setMediaFolders(const std::vector<std::string>& directories);

	/// Convenience for the single-folder case: replaces the whole list.
	std::size_t setMediaFolder(const std::string& directory);

	/// Advance a playlist walk that did not finish inside rescan() or
	/// setMediaFolder(). Call once per frame from the render loop.
	///
	/// Returns true when this call COMPLETED a walk, which is the moment the
	/// playlist became final and the first clip was opened. Never blocks for more
	/// than one frame's budget: a window that keeps painting cannot be mistaken
	/// for a hang, whatever folder the operator points it at.
	bool pollScan();

	/// Point the library at the startup folder and START walking it, opening
	/// nothing.
	///
	/// For a host whose backend is not initialized yet: all this does is set the
	/// root and hand the walk to the frame loop, which is what stops a large
	/// folder from delaying the first frame. pollScan() opens the first clip when
	/// the walk ends. An empty `directory` means no folder was chosen, and
	/// nothing is walked at all.
	void beginStartupScan(const std::string& directory);

	/// The multi-root form of beginStartupScan(): every folder to merge.
	void beginStartupScan(const std::vector<std::string>& directories);

	/// True while the playlist is still being walked, i.e. getStatus().clipCount
	/// is a partial count.
	bool scanPending() const;

	/// Every folder being merged, in order. Empty means nothing was chosen.
	std::vector<std::string> mediaFolders() const;

	/// The first folder being merged, or empty. For the callers that have exactly
	/// one folder to name - prefer mediaFolders().
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
	/// Begin a walk of the library's current root and take one bounded slice of
	/// it. Shared by rescan() and setMediaFolder() so the two cannot disagree
	/// about when the walk is finished or what finishing it means. Returns true
	/// when the walk completed inside this call.
	bool beginScan();

	IClipSource& clips_;
	IPlaybackBackend* backend_ = nullptr;

	std::size_t currentIndex_ = 0;
	bool loaded_ = false;

	/// A walk is running and the playlist still has to be opened once it ends.
	/// Set by beginScan() when the walk outlives one call; cleared by the
	/// pollScan() that completes it.
	bool scanSetupPending_ = false;

	std::vector<scripts::ScriptFile> scripts_;

	std::string subtitleOverride_;
	std::string subtitleText_;

	ClipChangedHandler clipChangedHandler_ = nullptr;
	void* clipChangedUserData_ = nullptr;
};

} // namespace media
