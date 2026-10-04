#pragma once

#include "control/AppLauncher.h"

#include <array>
#include <cstddef>
#include <string>

namespace media {

/// What clicking a Dashboard button does.
///
/// This is the launcher's whole control surface, and it is unchanged by the move
/// to Dear ImGui: it was a value before there were widgets to produce it, which
/// is why `Dashboard::handle` could be driven equally well by the tray menu and
/// by a hand-drawn button. The panel produces one of these; nothing else does.
enum class DashboardAction {
	None,
	LaunchPlayer,
	LaunchController,
	StopPlayer,
	StopController,
	/// Open the folder picker for the media corpus. This is the setting the
	/// whole point of the launcher is to own: one place to say where the media
	/// is, which both other applications then read.
	ChooseMediaFolder,
};

const char* toString(DashboardAction action);

/// One row on the Dashboard: one application, and what may be done to it.
struct DashboardRow {
	DashboardAction launch = DashboardAction::None;
	DashboardAction stop = DashboardAction::None;
	/// Which application this row describes.
	DashboardApp app = DashboardApp::Player;

	std::string title;
	/// What it is, and whether it is up: "running - video + HTTP API :8080".
	std::string subtitle;
	/// Absolute path of the executable, empty when it was not found.
	std::string path;
	bool available = false;
	bool running = false;
	/// A child this Dashboard started. Only such a process may be stopped from
	/// here: killing a Player the operator launched from Explorer would be
	/// rude, and the distinction is the whole reason this field exists.
	bool managed = false;
	int port = 0;

	/// The LAUNCH button is live only for an application that was found and is
	/// not already up. Presenting it as live when the executable is missing
	/// would produce a button that fails on every press.
	bool canLaunch() const { return available && !running; }
	/// The STOP button is live only for a process this launcher owns.
	bool canStop() const { return running && managed; }
};

/// The media corpus panel: which folder the Player reads, and how many videos
/// are in it. Deliberately not a DashboardRow - it has one button rather than
/// two, no "is it running" dot, and a different shape, and expressing those as
/// flags on the app rows would make every one of them conditional.
struct DashboardCorpus {
	/// What mediabus.ini holds. Empty means the Player's own default.
	std::string folder;
	std::size_t clipCount = 0;
	/// The Player is up, so the count is live rather than whatever the last
	/// session wrote down.
	bool playerOnline = false;
	/// The folder the Player was last seen using, empty when it reported the
	/// default. Kept separately from `folder` so the panel can say which of the
	/// two the count refers to.
	std::string playerFolder;

	/// True when there is a folder worth naming. The empty state is drawn
	/// dimmed rather than as a failure: an unset corpus is a normal first run.
	bool chosen() const { return !folder.empty(); }
};

/// The Dashboard's state and the small amount of derived text its interface
/// shows.
///
/// No GL, no ImGui, no processes and no HTTP - and, since the interface became
/// ImGui, no geometry either. Layout used to live here as rectangles hit-tested
/// by hand; the panel now owns its own widgets, and this class is purely the
/// state it reads. That is what keeps it testable with no window.
class DashboardModel {
public:
	/// Default window size, in pixels.
	///
	/// The application asks GLFW for exactly this. It is not multiplied by the
	/// monitor's content scale: that factor is applied once, when the interface
	/// font is rasterised, so the text is already bigger on a dense display and
	/// scaling the frame as well would grow it twice.
	///
	/// Sized for the interface that is actually here: two application cards with
	/// their buttons, the media folder panel, and - on the other two tabs - an
	/// activity log and a Lua editor, which is why it is taller than the old
	/// launcher card.
	static constexpr int kDefaultWidth = 1040;
	static constexpr int kDefaultHeight = 760;

	/// Rows in display order: the Player first, because that is the one that
	/// shows something, then the Controller.
	static constexpr std::size_t kRowCount = 2;

	DashboardModel();

	/// Replace what the probes last reported. `path` empty means the
	/// executable was not found on disk.
	void setStatus(DashboardApp app, const AppStatus& status,
		const std::string& path, int port);

	/// Replace the media corpus panel. `folder` is what mediabus.ini holds
	/// (empty when unset); `playerFolder` is what the Player is *actually*
	/// using, which is empty both when it is down and when it is on the
	/// default - `playerOnline` tells the two apart.
	void setCorpus(std::string folder, std::size_t clipCount, bool playerOnline,
		std::string playerFolder);

	const DashboardCorpus& corpus() const { return corpus_; }

	/// One line of feedback (last launch/stop result). Empty shows nothing.
	void setMessage(std::string message);
	const std::string& message() const { return message_; }

	/// Append a line to the activity log, newest last. Bounded, because a
	/// launcher that stays up for days must not grow a string without limit.
	void log(std::string line);
	const std::string& activityLog() const { return activityLog_; }
	/// Drop the whole log, for the panel's Clear button.
	void clearLog() { activityLog_.clear(); }

	const std::array<DashboardRow, kRowCount>& rows() const { return rows_; }
	const DashboardRow& rowFor(DashboardApp app) const;

	/// The action a row's LAUNCH button should produce, or None when it is
	/// inert. Kept here rather than in the panel so the enablement rule has one
	/// home and stays testable.
	DashboardAction launchActionFor(DashboardApp app) const;
	DashboardAction stopActionFor(DashboardApp app) const;

	/// Number of rows whose application was found on disk.
	std::size_t availableCount() const;

	/// How many bytes of log text are retained before the oldest line is
	/// dropped. Generous for a session, small enough to be harmless.
	static constexpr std::size_t kMaxLogBytes = 32u * 1024u;

private:
	DashboardRow& mutableRowFor(DashboardApp app);

	std::array<DashboardRow, kRowCount> rows_{};
	DashboardCorpus corpus_;
	std::string message_;
	std::string activityLog_;
};

} // namespace media
