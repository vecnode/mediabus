#pragma once

#include "app/dashboard/AppLauncher.h"
#include "app/render/RenderDevice.h"

#include <array>
#include <cstddef>
#include <string>

namespace media {

/// What clicking a Dashboard button does.
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

/// One clickable row on the Dashboard.
struct DashboardRow {
	DashboardAction launch = DashboardAction::None;
	DashboardAction stop = DashboardAction::None;
	/// Which application this row describes.
	DashboardApp app = DashboardApp::Player;

	std::string title;
	std::string subtitle;
	std::string path;
	bool available = false;
	bool running = false;
	bool managed = false;      ///< A child this Dashboard started.
	int port = 0;

	Rect card;
	Rect launchButton;
	Rect stopButton;
};

/// The media corpus panel: which folder the Player reads, and how many videos
/// are in it. Deliberately not a DashboardRow - it has one button rather than
/// two, no "is it running" dot, and a different height, and expressing those
/// as flags on the app rows would make every one of them conditional.
struct DashboardCorpus {
	std::string folder;
	std::size_t clipCount = 0;
	/// The Player is up, so the count is live rather than whatever the last
	/// session wrote down.
	bool playerOnline = false;
	/// The folder the Player was last seen using, empty when it reported the
	/// default. Kept separately from `folder` so the view can say which of the
	/// two the count refers to.
	std::string playerFolder;

	Rect card;
	Rect chooseButton;
};

/// Dashboard presentation state: layout, hit tests and the small amount of
/// derived text. No GL, no processes, no HTTP — so the whole thing is testable
/// without a window.
class DashboardModel {
public:
	/// Default window size, in framebuffer pixels at 100% DPI. dashboard_main.cpp
	/// scales it by the monitor's content scale to match the layout, which is
	/// written in text units. Sized to hold the title, both application rows,
	/// the corpus panel and the message line, and no more: the layout flows from
	/// the top down, so extra height is empty card, not a taller layout.
	static constexpr int kDefaultWidth = 820;
	static constexpr int kDefaultHeight = 460;

	/// Rows in display order: the Player first, because that is the one that
	/// shows something, then the Controller.
	static constexpr std::size_t kRowCount = 2;

	DashboardModel();

	/// Replace what the probes last reported. `path` empty means the
	/// executable was not found on disk.
	void setStatus(DashboardApp app, const AppStatus& status,
		const std::string& path, int port);

	/// Replace the media corpus panel. `folder` is what mediaplayer.ini holds
	/// (empty when unset); `playerFolder` is what the Player is *actually*
	/// using, which is empty both when it is down and when it is on the
	/// default - `playerOnline` tells the two apart.
	void setCorpus(std::string folder, std::size_t clipCount, bool playerOnline,
		std::string playerFolder);

	const DashboardCorpus& corpus() const { return corpus_; }

	/// One line of feedback under the rows (last launch/stop result).
	void setMessage(std::string message);
	const std::string& message() const { return message_; }

	/// Recompute geometry for a pixel size. Call on start and on resize.
	/// `uiScale` is the monitor's DPI factor; every constant is multiplied by
	/// it, so the card grows with its text rather than clipping it.
	void layout(float width, float height, float uiScale = 1.0f);

	const std::array<DashboardRow, kRowCount>& rows() const { return rows_; }
	const Rect& titleArea() const { return titleArea_; }
	const Rect& messageArea() const { return messageArea_; }

	/// Which action is under (x, y), or kNone. A stop button on an app this
	/// Dashboard did not start is present but inert; the corpus button is
	/// always live, because setting the folder needs no running application.
	DashboardAction hitTest(float x, float y) const;

	/// Number of rows whose application was found on disk.
	std::size_t availableCount() const;

private:
	DashboardRow& rowFor(DashboardApp app);
	const DashboardRow& rowFor(DashboardApp app) const;

	std::array<DashboardRow, kRowCount> rows_{};
	DashboardCorpus corpus_;
	Rect titleArea_{};
	Rect messageArea_{};
	std::string message_;
};

} // namespace media
