#pragma once

#include "app/render/RenderDevice.h"

#include <cstddef>
#include <string>
#include <vector>

namespace media {

/// Transport actions the Controller bar can issue. Each one maps onto exactly
/// one route of the Player's documented HTTP API — the request-building code
/// and the tests both switch on this enum, so the two can never drift apart
/// silently.
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
	/// has not been reached yet - the corpus rectangle distinguishes the two
	/// using `online`.
	std::string mediaFolder;

	/// How many clips that folder yielded. Needed separately from clipCount
	/// because the two differ in exactly the case that matters: an unset or
	/// empty corpus is 0 videos, which must not look like an error.
	std::size_t corpusClipCount = 0;

	/// Why the last poll failed; empty when online.
	std::string lastError;
};

/// One drawable, clickable button on the bar.
struct ControlButton {
	ControlCommand command = ControlCommand::None;
	std::string label;
	Rect rect;
	/// False when the command makes no sense for the current state (no clip
	/// loaded, an image that cannot be paused, ...). Drawn dimmed, not clicked.
	bool enabled = true;

	bool hit(float x, float y) const {
		return x >= rect.x && x < rect.x + rect.w
			&& y >= rect.y && y < rect.y + rect.h;
	}
};

/// Where the bar puts everything, in pixels.
struct ControllerLayout {
	std::vector<ControlButton> buttons;
	Rect statusChip;
	Rect titleArea;
	Rect seekBar;
	Rect errorStrip;
	Rect volumeArea;
	Rect speedArea;
	/// The "media corpus folder" field. Clicking it opens the OS folder picker.
	/// Drawn whenever there is room, so the folder is visible at a glance and
	/// not only once the Player has been reached.
	Rect corpusArea;
};

/// The Controller's presentation logic: status in, layout and hit tests out.
///
/// Contains no GL, no HTTP and no Lua, which is what makes it unit-testable
/// without a window. The frame loop owns one instance on the main thread.
class ControllerModel {
public:
	/// Default bar size, in framebuffer pixels at 100% DPI. controller_main.cpp
	/// asks for exactly this: the layout is written in text units scaled by the
	/// monitor's content scale, so this is the size that must hold the identity
	/// row, the transport row, the media corpus field and the seek bar *at a
	/// real DPI scale*. The window is not multiplied by that scale - GLFW's
	/// window size is already in DPI-virtualized screen coordinates, so scaling
	/// it too would give a window uiScale times too big for its own contents.
	/// layout() drops the corpus field rather than squeezing the transport
	/// labels if a window is made shorter than that.
	static constexpr int kDefaultWidth = 980;
	static constexpr int kDefaultHeight = 240;

	/// Replace the snapshot. Returns true when anything visible changed, so the
	/// caller can decide whether the bar needs redrawing.
	bool applyState(const ControllerState& next);

	/// Mark the Player unreachable, keeping the reason for the error strip.
	void markOffline(const std::string& reason);

	const ControllerState& state() const { return state_; }

	/// Set the DPI/text scale before the first layout(). Every pixel constant
	/// below is multiplied by it, so the bar grows with the font instead of the
	/// text overflowing a fixed-height row.
	void setUiScale(float scale);
	float uiScale() const { return uiScale_; }

	/// Recompute the bar for a pixel size. Call on start and on every resize.
	void layout(float width, float height);

	const ControllerLayout& layout() const { return layout_; }

	/// Which button is under (x, y), or kNone. Disabled buttons are skipped.
	ControlCommand hitTest(float x, float y) const;

	/// True when (x, y) is inside the corpus field, i.e. the click means
	/// "choose the media folder" rather than a transport command.
	bool corpusHit(float x, float y) const;

	/// Map a point on the seek bar to a 0..100 percentage. Returns false when
	/// the point is outside the bar, so the caller can ignore the click.
	bool seekPercentAt(float x, float y, double& percentOut) const;

	/// True when the current clip can actually be seeked: a still image has no
	/// timeline, so the bar must not pretend to scrub one.
	bool seekBarActive() const;

	/// Human-readable one-liner for the title area.
	std::string titleText() const;

	/// What the corpus field shows. Split in two so the view can draw the fixed
	/// "MEDIA FOLDER:" prefix dim and the state in a colour that carries the
	/// meaning: a path when one is set, the click-to-choose prompt when not.
	std::string corpusLabel() const;
	std::string corpusValue() const;

	/// True when there is a corpus worth drawing a folder name for. False is
	/// the "no folder / 0 videos" state, which is drawn dimmed rather than as
	/// an error.
	bool corpusChosen() const;

	/// One line of feedback the bar shows (the last command's refusal, or what
	/// a script reported). Empty means "show nothing".
	void setMessage(std::string message);
	const std::string& message() const { return message_; }

private:
	ControllerState state_;
	ControllerLayout layout_;
	std::string message_;
	float uiScale_ = 1.0f;
};

} // namespace media
