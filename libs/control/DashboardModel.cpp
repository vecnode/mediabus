#include "control/DashboardModel.h"

#include "core/Platform.h"

#include <cstdio>
#include <utility>

namespace media {

const char* toString(DashboardAction action) {
	switch (action) {
		case DashboardAction::None: return "none";
		case DashboardAction::LaunchPlayer: return "launch-player";
		case DashboardAction::LaunchController: return "launch-controller";
		case DashboardAction::StopPlayer: return "stop-player";
		case DashboardAction::StopController: return "stop-controller";
		case DashboardAction::ChooseMediaFolder: return "choose-media-folder";
	}
	return "none";
}

DashboardModel::DashboardModel() {
	// Fixed row order and fixed actions: the Player shows something, so it comes
	// first; the Controller is the optional extra.
	rows_[0].app = DashboardApp::Player;
	rows_[0].launch = DashboardAction::LaunchPlayer;
	rows_[0].stop = DashboardAction::StopPlayer;
	rows_[0].title = "Player";
	rows_[1].app = DashboardApp::Controller;
	rows_[1].launch = DashboardAction::LaunchController;
	rows_[1].stop = DashboardAction::StopController;
	rows_[1].title = "Controller";
}

DashboardRow& DashboardModel::mutableRowFor(DashboardApp app) {
	return rows_[app == DashboardApp::Controller ? 1u : 0u];
}

const DashboardRow& DashboardModel::rowFor(DashboardApp app) const {
	return rows_[app == DashboardApp::Controller ? 1u : 0u];
}

void DashboardModel::setStatus(DashboardApp app, const AppStatus& status,
	const std::string& path, int port) {
	DashboardRow& row = mutableRowFor(app);
	row.available = !path.empty();
	row.running = status.running();
	row.managed = status.childPid != 0;
	row.path = path;
	row.port = port;

	const char* kind = (app == DashboardApp::Player)
		? "libmpv video and audio" : "ImGui control panel";
	if (!row.available) {
		row.subtitle = std::string("not found next to this launcher (expected ")
			+ dashboardExecutableName(app) + ")";
		return;
	}
	char buffer[192];
	std::snprintf(buffer, sizeof(buffer), "%s - %s - API :%d",
		row.running ? "running" : "stopped", kind, port);
	row.subtitle = buffer;
}

void DashboardModel::setMessage(std::string message) {
	message_ = std::move(message);
}

void DashboardModel::setCorpus(std::string folder, std::size_t clipCount,
	bool playerOnline, std::string playerFolder) {
	corpus_.folder = std::move(folder);
	corpus_.clipCount = clipCount;
	corpus_.playerOnline = playerOnline;
	corpus_.playerFolder = std::move(playerFolder);
}

void DashboardModel::log(std::string line) {
	if (line.empty()) {
		return;
	}
	// A launcher stays up for days, so the log is bounded. Dropping whole lines
	// from the front keeps it readable, which trimming mid-line would not.
	activityLog_ += line;
	activityLog_ += '\n';
	if (activityLog_.size() <= kMaxLogBytes) {
		return;
	}
	const std::size_t overflow = activityLog_.size() - kMaxLogBytes;
	const std::size_t cut = activityLog_.find('\n', overflow);
	if (cut == std::string::npos) {
		activityLog_.clear();
		return;
	}
	activityLog_.erase(0, cut + 1);
}

DashboardAction DashboardModel::launchActionFor(DashboardApp app) const {
	const DashboardRow& row = rowFor(app);
	return row.canLaunch() ? row.launch : DashboardAction::None;
}

DashboardAction DashboardModel::stopActionFor(DashboardApp app) const {
	const DashboardRow& row = rowFor(app);
	return row.canStop() ? row.stop : DashboardAction::None;
}

std::size_t DashboardModel::availableCount() const {
	std::size_t count = 0;
	for (const DashboardRow& row : rows_) {
		if (row.available) {
			++count;
		}
	}
	return count;
}

} // namespace media
