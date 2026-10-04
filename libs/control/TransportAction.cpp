#include "control/TransportAction.h"

#include "core/AppConfig.h"
#include "core/Log.h"
#include "core/Platform.h"

#include <utility>

namespace media {

TransportExecutor::TransportExecutor(PlayerCommands& player, ControllerModel& model,
	LuaControllerScript* scripts)
	: player_(player), model_(model), scripts_(scripts) {}

std::string TransportExecutor::reloadScript() {
	if (!scripting()) {
		return "no script host";
	}
	std::string error;
	if (scripts_->forceReload(error)) {
		return {};
	}
	return "script: " + error;
}

std::string TransportExecutor::stopScript() {
	if (!scripting()) {
		return "no script host";
	}
	scripts_->stopScript();
	model_.setMessage("script stopped");
	return {};
}

std::string TransportExecutor::chooseMediaFolder() {
	if (!picker_) {
		return "no folder picker in this build";
	}

	const FolderChoice choice = picker_(model_.state().mediaFolder);
	if (choice.cancelled) {
		return {};   // "cancel" is not an error: say nothing
	}
	if (choice.path.empty()) {
		return "no folder chosen";
	}

	std::string error;
	// The Player is the writer of the media folder whenever it is up, because
	// it is the process that scans the corpus: two writers racing over one key
	// is how the displayed folder and the scanned folder start disagreeing.
	if (player_.setMediaFolder(choice.path, error)) {
		return "media folder: " + choice.path;
	}

	// The Player is not answering, so write the choice to mediabus.ini
	// ourselves: it takes effect when the Player next starts, which beats losing
	// the selection. The Player stays the writer when it *is* up.
	config::Config stored;
	config::load(stored);
	stored.mediaFolder = choice.path;
	if (config::save(stored)) {
		return "media folder saved for the next Player start: " + choice.path;
	}
	return "player offline and " + config::configPath() + " could not be written";
}

std::string TransportExecutor::perform(const TransportAction& action) {
	if (!action.valid) {
		return {};
	}

	if (action.chooseFolder) {
		return chooseMediaFolder();
	}
	if (action.reloadScript) {
		return reloadScript();
	}
	if (action.stopScript) {
		return stopScript();
	}
	if (action.restartPlayer) {
		// The launcher owns process lifetime; it handles this itself. Reported
		// here so the value is not silently ignored if it ever arrives.
		LOG_NOTICE("Controller") << "restart requested; the launcher owns that";
		return {};
	}

	std::string error;

	if (action.openingClip) {
		if (player_.openClip(action.clipIndex, error)) {
			return {};
		}
		return "player: " + error;
	}

	if (action.settingVolume) {
		if (player_.setVolume(action.volume, error)) {
			return {};
		}
		return "player: " + error;
	}

	if (action.settingSpeed) {
		if (player_.setSpeed(action.speed, error)) {
			return {};
		}
		return "player: " + error;
	}

	if (action.seeking) {
		if (player_.seekPercent(action.seekPercent, error)) {
			return {};
		}
		return "player: " + error;
	}

	if (action.command != ControlCommand::None) {
		if (player_.send(action.command, 0.0, error)) {
			return {};
		}
		return "player: " + error;
	}

	return {};
}

} // namespace media
