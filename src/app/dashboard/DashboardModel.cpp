#include "app/dashboard/DashboardModel.h"

#include <algorithm>
#include <cstdio>

namespace media {
namespace {

constexpr float kPad = 14.0f;
constexpr float kTitleHeight = 22.0f;
constexpr float kRowHeight = 96.0f;
constexpr float kRowGap = 12.0f;
constexpr float kButtonHeight = 30.0f;
constexpr float kButtonWidth = 96.0f;
constexpr float kMessageHeight = 18.0f;

} // namespace

const char* toString(DashboardAction action) {
	switch (action) {
		case DashboardAction::None: return "none";
		case DashboardAction::LaunchPlayer: return "launch-player";
		case DashboardAction::LaunchController: return "launch-controller";
		case DashboardAction::StopPlayer: return "stop-player";
		case DashboardAction::StopController: return "stop-controller";
	}
	return "none";
}

DashboardModel::DashboardModel() {
	// Fixed row order and fixed actions: the Player shows something, so it
	// comes first; the Controller is the optional extra.
	rows_[0].app = DashboardApp::Player;
	rows_[0].launch = DashboardAction::LaunchPlayer;
	rows_[0].stop = DashboardAction::StopPlayer;
	rows_[1].app = DashboardApp::Controller;
	rows_[1].launch = DashboardAction::LaunchController;
	rows_[1].stop = DashboardAction::StopController;
	for (DashboardRow& row : rows_) {
		row.title = (row.app == DashboardApp::Player) ? "Player" : "Controller";
	}
}

DashboardRow& DashboardModel::rowFor(DashboardApp app) {
	return rows_[app == DashboardApp::Controller ? 1u : 0u];
}

const DashboardRow& DashboardModel::rowFor(DashboardApp app) const {
	return rows_[app == DashboardApp::Controller ? 1u : 0u];
}

void DashboardModel::setStatus(DashboardApp app, const AppStatus& status,
	const std::string& path, int port) {
	DashboardRow& row = rowFor(app);
	row.available = !path.empty();
	row.running = status.running();
	row.managed = status.childPid != 0;
	row.path = path;
	row.port = port;

	const char* kind = (app == DashboardApp::Player) ? "video + HTTP API" : "control bar";
	char buffer[192];
	if (!row.available) {
		row.subtitle = std::string("not found next to this app (expected ")
			+ dashboardExecutableName(app) + ")";
		return;
	}
	std::snprintf(buffer, sizeof(buffer), "%s - %s :%d",
		row.running ? "running" : "stopped", kind, port);
	row.subtitle = buffer;
}

void DashboardModel::setMessage(std::string message) {
	message_ = std::move(message);
}

void DashboardModel::layout(float width, float height) {
	const float innerLeft = kPad;
	const float innerRight = std::max(kPad, width - kPad);
	const float innerWidth = innerRight - innerLeft;

	titleArea_ = {innerLeft, kPad, innerWidth, kTitleHeight};
	messageArea_ = {innerLeft, height - kPad - kMessageHeight, innerWidth, kMessageHeight};

	float y = titleArea_.y + titleArea_.h + kRowGap;
	for (std::size_t i = 0; i < rows_.size(); ++i) {
		DashboardRow& row = rows_[i];
		row.card = {innerLeft, y, innerWidth, kRowHeight};

		// Buttons hang off the right edge of the card, stop left of launch so
		// the primary action stays the one furthest from the card text.
		const float buttonY = row.card.y + (row.card.h - kButtonHeight) * 0.5f;
		row.launchButton = {row.card.x + row.card.w - kPad - kButtonWidth, buttonY,
			kButtonWidth, kButtonHeight};
		row.stopButton = {row.launchButton.x - kPad * 0.6f - kButtonWidth, buttonY,
			kButtonWidth, kButtonHeight};

		// Inert state is expressed by geometry the hit test can skip, but the
		// rectangle is still reported so the view can draw it dimmed.
		y += kRowHeight + kRowGap;
	}
}

DashboardAction DashboardModel::hitTest(float x, float y) const {
	for (const DashboardRow& row : rows_) {
		if (row.launchButton.hit(x, y) && row.available && !row.running) {
			return row.launch;
		}
		// Stop is only offered for a child this Dashboard started: it must not
		// kill a Player the operator launched from Explorer.
		if (row.stopButton.hit(x, y) && row.running && row.managed) {
			return row.stop;
		}
	}
	return DashboardAction::None;
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
