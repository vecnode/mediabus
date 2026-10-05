#pragma once

#include "control/AppLauncher.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
	/// Open the folder picker to ADD a folder to the corpus. The corpus is the
	/// merge of every folder listed, so this APPENDS rather than replaces - which
	/// is what makes "media in two places" configurable without moving files
	/// around or filling one folder with shortcuts.
	ChooseMediaFolder,
	/// Drop one folder from the corpus. Which folder travels in
	/// DashboardPanel::Frame::actionFolder, because the action on its own cannot
	/// say which of several rows was clicked.
	RemoveMediaFolder,
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

/// The media corpus panel: which folders the Player reads, and how many clips
/// came out of them. Deliberately not a DashboardRow - it has its own buttons,
/// no "is it running" dot, and a different shape, and expressing those as flags
/// on the app rows would make every one of them conditional.
struct DashboardCorpus {
	/// Every folder the configuration holds, in order. The corpus is the MERGE of
	/// all of them. Empty means nothing has been chosen.
	std::vector<std::string> folders;
	/// How many clips the merged corpus holds.
	std::size_t clipCount = 0;
	/// The Player is up, so the count is live rather than whatever the last
	/// session wrote down.
	bool playerOnline = false;
	/// The folders the Player reported it is actually reading. Kept separately
	/// from `folders` so the panel can say which of the two the count refers to -
	/// someone can edit mediabus.ini while a Player is already running.
	std::vector<std::string> playerFolders;

	/// True when there is at least one folder worth naming. The empty state is
	/// drawn dimmed rather than as a failure: an unset corpus is a normal first
	/// run.
	bool chosen() const { return !folders.empty(); }

	/// The first configured folder, or empty. For the one-line callers that have
	/// a single folder to name; prefer `folders`.
	std::string primaryFolder() const {
		return folders.empty() ? std::string() : folders.front();
	}
	/// The first folder the Player reported, or empty.
	std::string primaryPlayerFolder() const {
		return playerFolders.empty() ? std::string() : playerFolders.front();
	}
};

/// One entry of the playlist the Player reports, for the Corpus tab.
///
/// A trimmed copy rather than `MediaPlayerClipInfo`: the launcher is an HTTP
/// client of the Player, and it should not have to link the media layer to draw a
/// list of names. The view needs exactly these fields and nothing else.
struct CorpusEntry {
	std::size_t index = 0;
	std::string name;
	std::string path;
	/// "image" or "video", as the Player spells it.
	std::string mediaType;
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

	/// Replace the media corpus panel. `folders` is what mediabus.ini holds
	/// (empty when unset); `playerFolders` is what the Player is *actually*
	/// reading, which is empty both when it is down and when nothing is chosen -
	/// `playerOnline` tells those apart.
	void setCorpus(std::vector<std::string> folders, std::size_t clipCount,
		bool playerOnline, std::vector<std::string> playerFolders);

	/// Convenience for the single-folder case, kept because most call sites and
	/// most operators have exactly one folder.
	void setCorpus(const std::string& folder, std::size_t clipCount,
		bool playerOnline, const std::string& playerFolder) {
		setCorpus(folder.empty() ? std::vector<std::string>{}
				: std::vector<std::string>{folder},
			clipCount, playerOnline,
			playerFolder.empty() ? std::vector<std::string>{}
				: std::vector<std::string>{playerFolder});
	}

	const DashboardCorpus& corpus() const { return corpus_; }

	/// Replace the playlist the Corpus tab lists. Filled by the application from
	/// the Player's `/api/clips`, because this class never talks to the network.
	///
	/// An empty list is ambiguous on its own - the Player may be down, or the
	/// folder may simply hold nothing - which is why `corpusStatus()` exists to
	/// say which, and why the tab draws it rather than an empty list.
	void setClipList(std::vector<CorpusEntry> clips) { clips_ = std::move(clips); }
	const std::vector<CorpusEntry>& clipList() const { return clips_; }

	/// One line explaining the state of the list above: how many were read and
	/// when, or why they could not be. Empty shows nothing.
	void setCorpusStatus(std::string status) { corpusStatus_ = std::move(status); }
	const std::string& corpusStatus() const { return corpusStatus_; }

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
	std::vector<CorpusEntry> clips_;
	std::string corpusStatus_;
	std::string message_;
	std::string activityLog_;
};

} // namespace media
