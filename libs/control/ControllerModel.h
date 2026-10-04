#pragma once

#include <cstddef>
#include <string>

namespace media {

/// Transport actions the Controller can issue. Each one maps onto exactly one
/// route of the Player's documented HTTP API - the request-building code and
/// the tests both switch on this enum, so the two can never drift apart
/// silently.
///
/// `PlayPause`, `ToggleHud`, `ToggleFullscreen` and `ToggleSubtitles` are
/// toggles on the wire even though their names read as actions: the Player's
/// protocol carries booleans, so PlayerCommands::send() resolves each of them to
/// an absolute value from the latest snapshot.
enum class ControlCommand {
	None,
	Previous,
	PlayPause,
	Stop,
	Next,
	ToggleHud,
	ToggleFullscreen,
	ToggleSubtitles,
};

/// Stable wire spelling for a command. Used by HTTP clients of the Controller
/// and reported in errors; never localised.
const char* toString(ControlCommand command);

/// Player status as the Controller last saw it.
///
/// This is a value type on purpose: the polling thread produces snapshots and
/// the main thread consumes them, with no shared mutable state between them
/// beyond the one mutex in PlayerClient.
struct ControllerState {
	/// False until a poll has succeeded. A Controller that has never reached a
	/// Player is "offline", not "empty".
	bool online = false;
	bool loaded = false;
	bool playing = false;
	bool paused = false;
	bool isImage = false;
	bool seekable = false;
	std::size_t clipIndex = 0;
	std::size_t clipCount = 0;
	std::string clipName;
	double position = 0.0;
	double duration = 0.0;
	double speed = 1.0;
	double volume = 100.0;
	bool subtitlesEnabled = true;
	/// nullopt in JSON means "this Player has no window"; reported as false.
	bool hudVisible = true;
	bool fullscreen = false;

	/// The folder the Player is reading its corpus from, as it reports on
	/// /api/status. Empty means the Player is using its default data folder, or
	/// has not been reached yet - the interface distinguishes the two using
	/// `online`.
	std::string mediaFolder;

	/// How many clips that folder yielded. Needed separately from clipCount
	/// because the two differ in exactly the case that matters: an unset or
	/// empty corpus is 0 videos, which must not look like a failure.
	std::size_t corpusClipCount = 0;

	/// Why the last poll failed; empty when online.
	std::string lastError;
};

/// The Controller's state, and the small amount of derived text its interface
/// shows.
///
/// Contains no GL, no ImGui, no HTTP and no Lua - and, since the interface
/// became ImGui, no geometry either. Layout used to live here as rectangles
/// hit-tested by hand, because there was no widget toolkit; ImGui lays out its
/// own widgets and reports what the operator did, so this class is now purely
/// the state a panel reads and the labels it shows.
///
/// That is what keeps it unit-testable with no window, and it is why the tests
/// that watched for overlapping rectangles are gone: nothing here can overlap
/// any more.
class ControllerModel {
public:
	/// Replace the snapshot. Returns true when anything visible changed.
	bool applyState(const ControllerState& next);

	/// Mark the Player unreachable, keeping the reason for the error strip.
	void markOffline(const std::string& reason);

	const ControllerState& state() const { return state_; }

	/// True when the current clip can actually be seeked: a still image has no
	/// timeline, so the interface must not pretend to scrub one.
	bool seekBarActive() const;

	/// Where the seek bar should sit, as a percentage of the clip, clamped to
	/// 0..100. The panel feeds this into ImGui and acts only on what the
	/// operator changes.
	double seekPercent() const;

	/// Human-readable one-liner for the status row.
	std::string titleText() const;

	/// What the media-corpus field shows. Split in two so the panel can draw the
	/// fixed "MEDIA FOLDER:" prefix dim and the state in a colour that carries
	/// the meaning: a path when one is set, the click-to-choose prompt when not.
	std::string corpusLabel() const;
	std::string corpusValue() const;

	/// True when there is a corpus worth drawing a folder name for. False is
	/// the "no folder / 0 videos" state, which is drawn dimmed rather than as
	/// an error.
	bool corpusChosen() const;

	/// One line of feedback the interface shows (the last command's refusal, or
	/// what a script reported). Empty means "show nothing".
	void setMessage(std::string message);
	const std::string& message() const { return message_; }

private:
	ControllerState state_;
	std::string message_;
};

} // namespace media
