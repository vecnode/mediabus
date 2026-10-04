#pragma once

#include "app/dashboard/AppLauncher.h"
#include "app/render/RenderDevice.h"

#include <array>
#include <string>

namespace media {

/// What clicking a Dashboard button does.
enum class DashboardAction {
	None,
	LaunchPlayer,
	LaunchController,
	StopPlayer,
	StopController,
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

/// Dashboard presentation state: layout, hit tests and the small amount of
/// derived text. No GL, no processes, no HTTP — so the whole thing is testable
/// without a window.
class DashboardModel {
public:
	static constexpr int kDefaultWidth = 720;
	static constexpr int kDefaultHeight = 340;

	/// Rows in display order: the Player first, because that is the one that
	/// shows something, then the Controller.
	static constexpr std::size_t kRowCount = 2;

	DashboardModel();

	/// Replace what the probes last reported. `path` empty means the
	/// executable was not found on disk.
	void setStatus(DashboardApp app, const AppStatus& status,
		const std::string& path, int port);

	/// One line of feedback under the rows (last launch/stop result).
	void setMessage(std::string message);
	const std::string& message() const { return message_; }

	/// Recompute geometry for a pixel size. Call on start and on resize.
	void layout(float width, float height);

	const std::array<DashboardRow, kRowCount>& rows() const { return rows_; }
	const Rect& titleArea() const { return titleArea_; }
	const Rect& messageArea() const { return messageArea_; }

	/// Which action is under (x, y), or kNone. A stop button on an app this
	/// Dashboard did not start is present but inert.
	DashboardAction hitTest(float x, float y) const;

	/// Number of rows whose application was found on disk.
	std::size_t availableCount() const;

private:
	DashboardRow& rowFor(DashboardApp app);
	const DashboardRow& rowFor(DashboardApp app) const;

	std::array<DashboardRow, kRowCount> rows_{};
	Rect titleArea_{};
	Rect messageArea_{};
	std::string message_;
};

} // namespace media
