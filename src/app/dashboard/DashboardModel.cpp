#include "app/dashboard/DashboardModel.h"

#include "core/UiScale.h"

#include <algorithm>
#include <cstdio>

namespace media {
namespace {

// Geometry in *text units*: one unit is one glyph cell at the current text
// scale, so every number here grows with the font. The window's default size is
// multiplied by the same factor by dashboard_main.cpp, which is what keeps the
// proportions identical at 100% and at 200% DPI instead of packing a bigger
// font into a fixed card.
constexpr float kPad = 2.0f;
constexpr float kTitleHeight = 3.4f;
constexpr float kRowHeight = 12.0f;
constexpr float kRowGap = 1.7f;
constexpr float kButtonHeight = 4.3f;
constexpr float kButtonWidth = 13.0f;
constexpr float kMessageHeight = 2.6f;
constexpr float kCorpusGap = 2.6f;
constexpr float kCorpusHeight = 10.0f;
constexpr float kCorpusButtonWidth = 19.0f;

} // namespace

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

void DashboardModel::setCorpus(std::string folder, std::size_t clipCount,
	bool playerOnline, std::string playerFolder) {
	corpus_.folder = std::move(folder);
	corpus_.clipCount = clipCount;
	corpus_.playerOnline = playerOnline;
	corpus_.playerFolder = std::move(playerFolder);
}

void DashboardModel::layout(float width, float height, float uiScale) {
	// One unit is one glyph cell at this scale, so a single multiply makes the
	// whole card - not just its text - grow with the font.
	const float scale = ui::sanitizeScale(uiScale);
	const float unit = ui::kGlyphHeight * scale;
	auto u = [unit](float value) { return value * unit; };

	const float pad = u(kPad);
	const float rowGap = u(kRowGap);
	const float buttonHeight = u(kButtonHeight);
	const float buttonWidth = u(kButtonWidth);

	const float innerLeft = pad;
	const float innerRight = std::max(pad, width - pad);
	const float innerWidth = innerRight - innerLeft;

	titleArea_ = {innerLeft, pad, innerWidth, u(kTitleHeight)};
	messageArea_ = {innerLeft, height - pad - u(kMessageHeight), innerWidth,
		u(kMessageHeight)};

	float y = titleArea_.y + titleArea_.h + rowGap;
	for (std::size_t i = 0; i < rows_.size(); ++i) {
		DashboardRow& row = rows_[i];
		row.card = {innerLeft, y, innerWidth, u(kRowHeight)};

		// Buttons hang off the right edge of the card, stop left of launch so
		// the primary action stays the one furthest from the card text.
		const float buttonY = row.card.y + (row.card.h - buttonHeight) * 0.5f;
		row.launchButton = {row.card.x + row.card.w - pad - buttonWidth, buttonY,
			buttonWidth, buttonHeight};
		row.stopButton = {row.launchButton.x - rowGap * 0.6f - buttonWidth, buttonY,
			buttonWidth, buttonHeight};

		y += u(kRowHeight) + rowGap;
	}

	// The corpus panel sits below the application rows and above the message
	// line. Top-down and fixed, like the rows themselves: no centring, because a
	// block that migrates down the window as the height changes is harder to read
	// than one that simply starts at the top.
	const float corpusHeight = u(kCorpusHeight);
	corpus_.card = {innerLeft, y - rowGap + u(kCorpusGap), innerWidth, corpusHeight};

	const float corpusButtonW = u(kCorpusButtonWidth);
	corpus_.chooseButton = {corpus_.card.x + corpus_.card.w - pad - corpusButtonW,
		corpus_.card.y + (corpus_.card.h - buttonHeight) * 0.5f,
		corpusButtonW, buttonHeight};
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
	// Unconditional: choosing the corpus folder is useful precisely when
	// nothing is running, which is when there is nothing to play.
	if (corpus_.chooseButton.hit(x, y)) {
		return DashboardAction::ChooseMediaFolder;
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
